/*
 * QEMU Force eXecution PCI device
 * 2021 Lorenzo Susini
*/

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/hw.h"
#include "hw/pci/msi.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "qemu/main-loop.h" /* iothread mutex */
#include "qemu/module.h"
#include "qapi/visitor.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/random.h>

#define TYPE_PCI_FXPCI_DEVICE "fx"
typedef struct FxState FxState;
DECLARE_INSTANCE_CHECKER(FxState, FX,
                         TYPE_PCI_FXPCI_DEVICE)

#define ID_REGISTER                 0x00
#define CARD_LIVENESS_REGISTER      0x04
#define SCHEDULE_NEXT_REGISTER      0x08
#define INTERRUPT_STATUS_REGISTER   0x24
#define START_THREAD_REGISTER       0x30
#define INTERRUPT_RAISE_REGISTER    0x60
#define INTERRUPT_ACK_REGISTER      0x64

#define CONF_INTERVAL_DEFAULT       10
#define CONF_SERVER_PORT            3333

struct FxState {
    PCIDevice pdev;
    MemoryRegion mmio;
    MemoryRegion idt;

    QemuThread thread;
    QemuMutex thr_mutex;
    QemuCond thr_cond; 
    bool stopping;

    uint32_t irq_status;
    uint32_t card_liveness;

    QemuMutex conf_mutex;
    unsigned int conf_sleep_interval;
    int listen_fd;
    int conn_fd;

};

static bool fx_msi_enabled(FxState *);
static void fx_raise_irq(FxState *, uint32_t);
static void fx_lower_irq(FxState *, uint32_t);
static uint64_t fx_mmio_read(void *, hwaddr, unsigned);
static void fx_mmio_write(void *, hwaddr, uint64_t, unsigned);
static void *fx_forcer_thread(void *);
static void pci_fx_realize(PCIDevice *, Error **);
static void pci_fx_uninit(PCIDevice *);
static void fx_instance_init(Object *);
static void fx_class_init(ObjectClass *, void *);
static void pci_fx_register_types(void);
static void conf_server_init(void *);
static void conf_server_uninit(void *);
static void accept_conf_server_callback(void *);
static void read_conf_server_callback(void *);


static const MemoryRegionOps fx_mmio_ops = {
    .read = fx_mmio_read,
    .write = fx_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static bool fx_msi_enabled(FxState *fx)
{
    return msi_enabled(&fx->pdev);
}

static void fx_raise_irq(FxState *fx, uint32_t val)
{
    fx->irq_status |= val;
    if(fx->irq_status){
        if (fx_msi_enabled(fx)) {
            msi_notify(&fx->pdev, 0);
        } else {
            pci_set_irq(&fx->pdev, 1);
        }
    }
}

static void fx_lower_irq(FxState *fx, uint32_t val)
{
    fx->irq_status &= ~val;

    if (!fx->irq_status && !fx_msi_enabled(fx)) {
        pci_set_irq(&fx->pdev, 0);
    }
}

static uint64_t fx_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    FxState *fx = opaque;
    uint64_t val = ~0ULL;

    if(size != 4)
        return val;

    switch (addr) {
        case ID_REGISTER:
        /* let the device driver check version. 0xMMmm0edu */
            val = 0x01000edu;
            break;
        /* card liveness for sanity checks */
        case CARD_LIVENESS_REGISTER:
            val = fx->card_liveness;
            break;
        case INTERRUPT_STATUS_REGISTER:
            val = fx->irq_status;
            break;
        default:
            break;
        }

    return val;
}

static void fx_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                unsigned size)
{
    FxState *fx = opaque;

    if(size != 4)
        return;

    switch (addr) {
    case START_THREAD_REGISTER:
        qemu_mutex_lock(&fx->thr_mutex);
        qemu_cond_signal(&fx->thr_cond);
        qemu_mutex_unlock(&fx->thr_mutex);
        break;
    case SCHEDULE_NEXT_REGISTER:
        qemu_mutex_lock(&fx->thr_mutex);
        qemu_cond_signal(&fx->thr_cond);
        qemu_mutex_unlock(&fx->thr_mutex);
        break;
    case INTERRUPT_RAISE_REGISTER:
    //  fx_raise_irq(fx, val);
        break;
    case INTERRUPT_ACK_REGISTER:
        fx_lower_irq(fx, val);
        break;        
    default:
        break;
    }
}

static void *wait_device_driver(void *opaque)
{
    FxState *fx = opaque;
    qemu_mutex_lock(&fx->thr_mutex);
    qemu_cond_wait(&fx->thr_cond, &fx->thr_mutex);
    qemu_mutex_unlock(&fx->thr_mutex);
    return fx_forcer_thread(opaque);
}

static void *fx_forcer_thread(void *opaque)
{
    FxState *fx = opaque;
    unsigned int interval;
    char *buf;

    buf = g_malloc0(sizeof(unsigned int));

    while (1) {

        /* get random bytes from urandom. */
        getrandom(buf, sizeof(unsigned int), 0);

        qemu_mutex_lock(&fx->conf_mutex);
        interval = fx->conf_sleep_interval;
        qemu_mutex_unlock(&fx->conf_mutex);

        g_usleep(
            (interval * G_USEC_PER_SEC / 10) + 
            (*(unsigned int *)buf % (G_USEC_PER_SEC / 100))
        );
        
        qemu_mutex_lock(&fx->thr_mutex);
        fx_raise_irq(fx, 0x1);

        qemu_cond_wait(&fx->thr_cond, &fx->thr_mutex);

        if(fx->stopping){
            qemu_mutex_unlock(&fx->thr_mutex);

            break;
        }
        qemu_mutex_unlock(&fx->thr_mutex);

    }

    g_free(buf);
    return NULL;
}


static void conf_server_init(void *opaque)
{
    FxState *fx = opaque;
    struct sockaddr_in serv_addr;

    qemu_mutex_init(&fx->conf_mutex);
    fx->conf_sleep_interval = CONF_INTERVAL_DEFAULT;
    fx->listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    memset(&serv_addr, 0, sizeof(struct sockaddr_in));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(CONF_SERVER_PORT); 

    bind(
        fx->listen_fd, 
        (struct sockaddr*)&serv_addr, 
        sizeof(serv_addr)
    ); 
    listen(fx->listen_fd, 10); 

    // add listen_fd to the set of fds monitored by iothread. Once it becomes
    //    ready, it is possible to accept the connection without blocking 
    qemu_set_fd_handler(
        fx->listen_fd, 
        accept_conf_server_callback, 
        NULL, 
        opaque
    );
}

static void conf_server_uninit(void *opaque)
{
    FxState *fx = opaque;
    qemu_mutex_destroy(&fx->conf_mutex);
    close(fx->listen_fd);
}

static void accept_conf_server_callback(void *opaque)
{
    FxState *fx = opaque;

    fx->conn_fd = accept(fx->listen_fd, NULL, NULL);
    printf("Accepted connection \n");

    qemu_set_fd_handler(
        fx->conn_fd, 
        read_conf_server_callback, 
        NULL, 
        opaque
    );

}

static void read_conf_server_callback(void *opaque)
{
    unsigned int interval;
    FxState *fx = opaque;

    printf("read callback\n");
    read(fx->conn_fd, &interval, sizeof(unsigned int));

    qemu_mutex_lock(&fx->conf_mutex);
    fx->conf_sleep_interval = interval;
    qemu_mutex_unlock(&fx->conf_mutex);

    printf("Received new conf interval: %u\n", interval);

    // remove itself from set
    qemu_set_fd_handler(fx->conn_fd, NULL, NULL, NULL);  
    close(fx->conn_fd);
}


static void pci_fx_realize(PCIDevice *pdev, Error **errp)
{
    FxState *fx = FX(pdev);
    uint8_t *pci_conf = pdev->config;

    pci_config_set_interrupt_pin(pci_conf, 1);

    if (msi_init(pdev, 0, 1, true, false, errp)) {
        return;
    }

    qemu_mutex_init(&fx->thr_mutex);
    qemu_cond_init(&fx->thr_cond);
    qemu_thread_create(&fx->thread, "fx", wait_device_driver,
                       fx, QEMU_THREAD_JOINABLE);

    memory_region_init_io(&fx->mmio, OBJECT(fx), &fx_mmio_ops, fx,
                    "fx-mmio", 1 * KiB);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &fx->mmio);

    conf_server_init((void *)fx);
}

static void pci_fx_uninit(PCIDevice *pdev)
{
    FxState *fx = FX(pdev);

    qemu_mutex_lock(&fx->thr_mutex);
    fx->stopping = true;
    qemu_mutex_unlock(&fx->thr_mutex);
    qemu_cond_signal(&fx->thr_cond);
    qemu_thread_join(&fx->thread);

    qemu_cond_destroy(&fx->thr_cond);
    qemu_mutex_destroy(&fx->thr_mutex);

    conf_server_uninit((void *)fx);

    msi_uninit(pdev);
}

static void fx_instance_init(Object *obj)
{
    FxState *fx = FX(obj);
    fx->card_liveness = 0xdeadbeef;
}

static void fx_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

    k->realize = pci_fx_realize;
    k->exit = pci_fx_uninit;
    k->vendor_id = PCI_VENDOR_ID_QEMU;
    k->device_id = 0x0609;
    k->revision = 0x10;
    k->class_id = PCI_CLASS_OTHERS;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static void pci_fx_register_types(void)
{
    static InterfaceInfo interfaces[] = {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    };
    static const TypeInfo fx_info = {
        .name          = TYPE_PCI_FXPCI_DEVICE,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(FxState),
        .instance_init = fx_instance_init,
        .class_init    = fx_class_init,
        .interfaces    = interfaces,
    };

    type_register_static(&fx_info);
}
type_init(pci_fx_register_types)
