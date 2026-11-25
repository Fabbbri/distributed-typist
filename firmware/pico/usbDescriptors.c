#include "pico/unique_id.h"
#include "tusb.h"
#include "typistUsbProtocol.h"

#include <string.h>

#define USB_STRING_LANGUAGE 0
#define USB_STRING_MANUFACTURER 1
#define USB_STRING_PRODUCT 2
#define USB_STRING_SERIAL 3
#define USB_STRING_CAPACITY 32
#define USB_ENDPOINT_OUT 0x01
#define USB_ENDPOINT_IN 0x81
#define USB_CONFIGURATION_LENGTH (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)

// Los IDs son parámetros obligatorios de construcción; no se apropia un VID/PID ajeno por defecto.
static const tusb_desc_device_t deviceDescriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = TYPIST_USB_VID,
    .idProduct = TYPIST_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = USB_STRING_MANUFACTURER,
    .iProduct = USB_STRING_PRODUCT,
    .iSerialNumber = USB_STRING_SERIAL,
    .bNumConfigurations = 1,
};

// TinyUSB registra la interfaz vendor con sus endpoints bulk; el protocolo v1 solo utiliza EP0.
static const uint8_t configurationDescriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, USB_CONFIGURATION_LENGTH, 0, TYPIST_USB_POWER_MA),
    TUD_VENDOR_DESCRIPTOR(TYPIST_USB_INTERFACE, 0, USB_ENDPOINT_OUT, USB_ENDPOINT_IN,
                          CFG_TUD_VENDOR_EPSIZE),
};

/**
 * @brief Proporciona a TinyUSB el descriptor permanente del dispositivo Pico H.
 * @return Descriptor estático que no debe modificarse ni liberarse.
 */
const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&deviceDescriptor;
}

/**
 * @brief Expone la única configuración de la interfaz de fabricante.
 * @param index Índice de configuración, que debe ser cero.
 * @return Descriptor estático, o NULL para un índice inexistente.
 */
const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    return index == 0 ? configurationDescriptor : NULL;
}

/**
 * @brief Construye un descriptor UTF-16 con cadenas ASCII y el identificador real de la placa.
 * @param index Índice de cadena solicitado por el host.
 * @param language Idioma solicitado; las cadenas de esta versión son independientes del idioma.
 * @return Buffer estático de TinyUSB, o NULL si la cadena no existe o excede su capacidad.
 */
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t language) {
    static uint16_t descriptor[USB_STRING_CAPACITY];
    static char serial[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
    const char *text = NULL;
    size_t characterCount = 0;

    (void)language;
    if (index == USB_STRING_LANGUAGE) {
        descriptor[0] = (TUSB_DESC_STRING << 8) | 4;
        descriptor[1] = 0x0409;
        return descriptor;
    }
    // El número de serie distingue placas reales sin inventar un identificador constante.
    switch (index) {
        case USB_STRING_MANUFACTURER:
            text = "distributed-typist";
            break;
        case USB_STRING_PRODUCT:
            text = "Pico H Typist USB";
            break;
        case USB_STRING_SERIAL:
            pico_get_unique_board_id_string(serial, sizeof(serial));
            text = serial;
            break;
        default:
            return NULL;
    }
    characterCount = strlen(text);
    if (characterCount >= USB_STRING_CAPACITY) {
        return NULL;
    }
    for (size_t position = 0; position < characterCount; position++) {
        descriptor[position + 1] = (unsigned char)text[position];
    }
    descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * characterCount + 2));
    return descriptor;
}
