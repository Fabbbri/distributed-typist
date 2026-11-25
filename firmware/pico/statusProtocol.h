#ifndef PICO_STATUS_PROTOCOL_H
#define PICO_STATUS_PROTOCOL_H

#include "typistUsbProtocol.h"

/**
 * @brief Valida la petición USB propia y construye un estado sin movimientos habilitados.
 * @param requestType bmRequestType: entrada, fabricante, destinatario interfaz.
 * @param request Código de consulta de estado.
 * @param value Versión requerida del protocolo.
 * @param index Número de interfaz USB.
 * @param length Longitud exacta requerida para la respuesta.
 * @param response Destino no NULL; permanece intacto al rechazar una petición.
 * @return 1 si acepta y escribe ocho bytes; 0 si debe rechazar mediante STALL.
 */
int picoStatusReply(unsigned char requestType, unsigned char request, unsigned short value,
                    unsigned short index, unsigned short length, TypistStatusFrame *response);

#endif
