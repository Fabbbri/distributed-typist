#ifndef TYPIST_USB_PROTOCOL_H
#define TYPIST_USB_PROTOCOL_H

// Contrato USB inicial: consulta de estado; todavía no hay órdenes de movimiento.
#define TYPIST_USB_INTERFACE 0
#define TYPIST_USB_CLASS 0xff
#define TYPIST_USB_SUBCLASS 0
#define TYPIST_USB_INTERFACE_PROTOCOL 0
#define TYPIST_USB_REQUEST_STATUS 0x01
#define TYPIST_USB_REQUEST_TYPE 0xc1
#define TYPIST_PROTOCOL_VERSION 1
#define TYPIST_STATE_UNCONFIGURED 1
#define TYPIST_CAP_STATUS 0x01
#define TYPIST_STATUS_SIZE 8
#define TYPIST_STATUS_INITIALIZER {{'T', 'P', 'H', '1', TYPIST_PROTOCOL_VERSION, \
    TYPIST_STATE_UNCONFIGURED, TYPIST_CAP_STATUS, 0}}

/** Respuesta de ocho bytes, idéntica en Linux, espacio de usuario y RP2040. */
typedef struct {
    unsigned char bytes[TYPIST_STATUS_SIZE];
} TypistStatusFrame;

/**
 * @brief Valida firma, versión y estado del protocolo inicial sin depender de la plataforma.
 * @param frame Respuesta completa, o NULL para rechazarla.
 * @return 1 si es válida y declara el hardware sin configurar; 0 en otro caso.
 * Este contrato no permite anunciar movimiento ni calibración que aún no existen.
 */
static inline int typistStatusIsValid(const TypistStatusFrame *frame) {
    return frame != 0 && frame->bytes[0] == 'T' && frame->bytes[1] == 'P' &&
        frame->bytes[2] == 'H' && frame->bytes[3] == '1' &&
        frame->bytes[4] == TYPIST_PROTOCOL_VERSION &&
        frame->bytes[5] == TYPIST_STATE_UNCONFIGURED &&
        frame->bytes[6] == TYPIST_CAP_STATUS && frame->bytes[7] == 0;
}

#endif
