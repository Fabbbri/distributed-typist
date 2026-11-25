// SPDX-License-Identifier: GPL-2.0-only
#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/usb.h>

#include "typistIoctl.h"

#define TYPIST_MINOR_BASE 192
#define TYPIST_USB_TIMEOUT_MS 1000

typedef struct {
    struct usb_device *usbDevice;
    struct mutex ioMutex;
    struct kref reference;
    TypistStatusFrame *response;
    bool disconnected;
} TypistDevice;

static unsigned short vendorId;
static unsigned short productId;
module_param(vendorId, ushort, 0444);
module_param(productId, ushort, 0444);
MODULE_PARM_DESC(vendorId, "Explicit USB vendor ID matching the project firmware");
MODULE_PARM_DESC(productId, "Explicit USB product ID matching the project firmware");

// Los identificadores se completan al cargar; no se publica un alias USB universal.
static struct usb_device_id deviceIds[] = {
    {.match_flags = USB_DEVICE_ID_MATCH_DEVICE | USB_DEVICE_ID_MATCH_INT_INFO,
     .bInterfaceClass = TYPIST_USB_CLASS,
     .bInterfaceSubClass = TYPIST_USB_SUBCLASS,
     .bInterfaceProtocol = TYPIST_USB_INTERFACE_PROTOCOL},
    {}
};
static struct usb_driver typistDriver;

/**
 * @brief Libera el dispositivo después de la desconexión y del cierre del último descriptor.
 * @param reference Referencia embebida que alcanzó cero; no NULL.
 */
static void deleteDevice(struct kref *reference) {
    TypistDevice *device = container_of(reference, TypistDevice, reference);

    usb_put_dev(device->usbDevice);
    kfree(device->response);
    kfree(device);
}

/**
 * @brief Consulta EP0 y valida ocho bytes del protocolo propio.
 * @param device Dispositivo conectado, con ioMutex adquirido o aún dentro de probe.
 * @return 0 con respuesta válida, o un errno negativo de USB/protocolo.
 */
static int queryStatus(TypistDevice *device) {
    int result;

    // El buffer pertenece a kmalloc: USB no recibe un puntero al stack del kernel.
    memset(device->response, 0, sizeof(*device->response));
    result = usb_control_msg(device->usbDevice, usb_rcvctrlpipe(device->usbDevice, 0),
                             TYPIST_USB_REQUEST_STATUS, TYPIST_USB_REQUEST_TYPE,
                             TYPIST_PROTOCOL_VERSION, TYPIST_USB_INTERFACE,
                             device->response, sizeof(*device->response), TYPIST_USB_TIMEOUT_MS);
    if (result < 0) {
        return result;
    }
    if (result != sizeof(*device->response) || !typistStatusIsValid(device->response)) {
        return -EPROTO;
    }
    return 0;
}

/**
 * @brief Adquiere una referencia para un descriptor abierto del dispositivo registrado por USB.
 * @param inode Inodo del nodo de dispositivo, no NULL.
 * @param file Descriptor en apertura; recibe el contexto privado.
 * @return 0 si adquiere el dispositivo, -ENODEV si ya no está disponible.
 */
static int openDevice(struct inode *inode, struct file *file) {
    struct usb_interface *interface;
    TypistDevice *device;

    // usb_register_dev protege esta apertura frente a usb_deregister_dev en disconnect.
    interface = usb_find_interface(&typistDriver, iminor(inode));
    if (interface == NULL) {
        return -ENODEV;
    }
    device = usb_get_intfdata(interface);
    if (device == NULL) {
        return -ENODEV;
    }
    kref_get(&device->reference);
    file->private_data = device;
    return nonseekable_open(inode, file);
}

/**
 * @brief Devuelve la referencia del descriptor sin liberar un contexto todavía utilizado.
 * @param inode Inodo del dispositivo, no se utiliza.
 * @param file Descriptor que conserva el contexto adquirido en openDevice.
 * @return 0 después de devolver la referencia.
 */
static int releaseDevice(struct inode *inode, struct file *file) {
    TypistDevice *device = file->private_data;

    (void)inode;
    kref_put(&device->reference, deleteDevice);
    return 0;
}

/**
 * @brief Ejecuta exclusivamente la consulta de estado de la ABI propia del módulo.
 * @param file Descriptor con referencia al dispositivo, no NULL.
 * @param command TYPIST_IOCTL_STATUS; cualquier otro comando se rechaza.
 * @param argument Dirección de usuario que recibirá una respuesta completa.
 * @return 0 si copia el estado, o errno negativo ante fallo, desconexión o ABI desconocida.
 */
static long ioctlDevice(struct file *file, unsigned int command, unsigned long argument) {
    TypistDevice *device = file->private_data;
    int result;

    if (command != TYPIST_IOCTL_STATUS) {
        return -ENOTTY;
    }
    result = mutex_lock_interruptible(&device->ioMutex);
    if (result != 0) {
        return result;
    }
    // Serializa las consultas y la desconexión para no reutilizar un dispositivo retirado.
    result = device->disconnected ? -ENODEV : queryStatus(device);
    if (result == 0 && copy_to_user((void __user *)argument, device->response,
                                    sizeof(*device->response)) != 0) {
        result = -EFAULT;
    }
    mutex_unlock(&device->ioMutex);
    return result;
}

static const struct file_operations deviceOperations = {
    .owner = THIS_MODULE,
    .open = openDevice,
    .release = releaseDevice,
    .unlocked_ioctl = ioctlDevice,
#ifdef CONFIG_COMPAT
    .compat_ioctl = compat_ptr_ioctl,
#endif
};

static struct usb_class_driver deviceClass = {
    .name = "typist%d",
    .fops = &deviceOperations,
    .minor_base = TYPIST_MINOR_BASE,
};

/**
 * @brief Reconoce la interfaz configurada, comprueba el firmware y registra su nodo propio.
 * @param interface Interfaz USB candidata, propiedad del núcleo USB.
 * @param identity Entrada que coincide con VID/PID y clase; no se modifica.
 * @return 0 tras registrar, o errno negativo sin dejar recursos adquiridos.
 */
static int probeDevice(struct usb_interface *interface, const struct usb_device_id *identity) {
    TypistDevice *device;
    int result;

    (void)identity;
    if (interface->cur_altsetting->desc.bInterfaceNumber != TYPIST_USB_INTERFACE ||
        interface->cur_altsetting->desc.bAlternateSetting != 0) {
        return -ENODEV;
    }
    device = kzalloc(sizeof(*device), GFP_KERNEL);
    if (device == NULL) {
        return -ENOMEM;
    }
    device->usbDevice = usb_get_dev(interface_to_usbdev(interface));
    mutex_init(&device->ioMutex);
    kref_init(&device->reference);
    device->response = kmalloc(sizeof(*device->response), GFP_KERNEL);
    if (device->response == NULL) {
        result = -ENOMEM;
        goto cleanup;
    }

    // El VID/PID por sí solo no basta: exige la firma y versión de nuestro firmware.
    result = queryStatus(device);
    if (result != 0) {
        goto cleanup;
    }
    usb_set_intfdata(interface, device);
    result = usb_register_dev(interface, &deviceClass);
    if (result != 0) {
        usb_set_intfdata(interface, NULL);
        goto cleanup;
    }
    dev_info(&interface->dev, "typist USB status device registered, minor=%d\n", interface->minor);
    return 0;

cleanup:
    kref_put(&device->reference, deleteDevice);
    return result;
}

/**
 * @brief Retira el nodo y conserva el contexto hasta que se cierren los descriptores existentes.
 * @param interface Interfaz que se está desconectando, no NULL.
 */
static void disconnectDevice(struct usb_interface *interface) {
    TypistDevice *device = usb_get_intfdata(interface);

    // Primero impide nuevas aperturas y espera las que USB ya tiene en curso.
    usb_deregister_dev(interface, &deviceClass);
    mutex_lock(&device->ioMutex);
    device->disconnected = true;
    usb_set_intfdata(interface, NULL);
    mutex_unlock(&device->ioMutex);
    kref_put(&device->reference, deleteDevice);
    dev_info(&interface->dev, "typist USB device disconnected\n");
}

static struct usb_driver typistDriver = {
    .name = "typist_usb",
    .probe = probeDevice,
    .disconnect = disconnectDevice,
    .id_table = deviceIds,
};

/**
 * @brief Registra el módulo únicamente con VID y PID explícitos del firmware del proyecto.
 * @return 0 si registra, -EINVAL con IDs ausentes/reservados, u otro errno de USB.
 */
static int __init initializeDriver(void) {
    if (vendorId == 0 || vendorId == 0xffff || productId == 0 || productId == 0xffff) {
        pr_err("typist_usb: specify vendorId and productId matching the firmware\n");
        return -EINVAL;
    }
    // Los valores de solo lectura permanecen constantes durante toda la vida del módulo.
    deviceIds[0].idVendor = vendorId;
    deviceIds[0].idProduct = productId;
    return usb_register(&typistDriver);
}

/**
 * @brief Retira el driver y deja que USB desconecte sus interfaces antes de terminar el módulo.
 */
static void __exit terminateDriver(void) {
    usb_deregister(&typistDriver);
}

module_init(initializeDriver);
module_exit(terminateDriver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Project-specific Pico H USB status driver; no servo motion yet");
