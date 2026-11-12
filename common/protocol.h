#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "crypto.h"

// Límites de esta etapa local; no son requisitos del proyecto completo.
#define MAX_FILE_SIZE UINT64_C(1073741824)
#define MAX_TRANSFER_SIZE (MAX_FILE_SIZE + ENCRYPTION_OVERHEAD)
#define TRANSFER_BLOCK_SIZE 16384
#define SOCKET_TIMEOUT_SECONDS 10
#define TRANSFER_HEADER_SIZE 12

typedef enum {
    TRANSFER_SUCCESS = 0,
    TRANSFER_READY = 1,
    TRANSFER_INVALID = 2,
    TRANSFER_IO_ERROR = 3,
    TRANSFER_INCOMPLETE = 4
} TransferStatus;

/**
 * @brief Valida una dirección IPv4 de loopback y un puerto decimal entre 1 y 65535.
 * @param address Texto de la dirección; no puede ser NULL.
 * @param port Texto del puerto; no puede ser NULL.
 * @param endpoint Recibe la dirección; no puede ser NULL.
 * @return 0 si es válido, -1 con errno si no lo es.
 */
int parseEndpoint(const char *address, const char *port, struct sockaddr_in *endpoint);

/**
 * @brief Limita a diez segundos cada espera de E/S de un socket conectado.
 * @param socketFd Descriptor válido, cuya propiedad conserva el llamador.
 * @return 0 si configura ambos límites, -1 con errno si falla.
 */
int configureSocket(int socketFd);

/**
 * @brief Envía todos los bytes y reintenta interrupciones y envíos parciales.
 * @param socketFd Socket conectado; no se cierra.
 * @param buffer Bytes de origen; NULL solo si length es cero.
 * @param length Cantidad de bytes, como máximo TRANSFER_BLOCK_SIZE.
 * @return 0 al completar, -1 con errno si falla, sin generar SIGPIPE.
 */
int sendAll(int socketFd, const void *buffer, size_t length);

/**
 * @brief Recibe exactamente los bytes solicitados, reintentando interrupciones.
 * @param socketFd Socket conectado; no se cierra.
 * @param buffer Destino; NULL solo si length es cero.
 * @param length Cantidad de bytes, como máximo TRANSFER_BLOCK_SIZE.
 * @return 0 al completar, -1 con errno; EOF prematuro produce ECONNRESET.
 */
int receiveAll(int socketFd, void *buffer, size_t length);

/**
 * @brief Envía la cabecera DTP2 y el tamaño del archivo cifrado en orden big endian.
 * @param socketFd Socket conectado.
 * @param fileSize Tamaño entre ENCRYPTION_OVERHEAD y MAX_TRANSFER_SIZE.
 * @return 0 al completar, -1 con errno si falla.
 */
int sendHeader(int socketFd, uint64_t fileSize);

/**
 * @brief Recibe y valida la cabecera y el límite de tamaño antes de usarlo.
 * @param socketFd Socket conectado.
 * @param fileSize Recibe el tamaño; no puede ser NULL.
 * @return 0 al completar, -1 con errno (EPROTO o EFBIG para cabeceras inválidas).
 */
int receiveHeader(int socketFd, uint64_t *fileSize);

/**
 * @brief Envía un estado del protocolo en un byte.
 * @param socketFd Socket conectado.
 * @param status Estado definido por TransferStatus.
 * @return 0 al completar, -1 con errno si falla.
 */
int sendStatus(int socketFd, TransferStatus status);

/**
 * @brief Recibe un estado y rechaza valores desconocidos.
 * @param socketFd Socket conectado.
 * @param status Destino del estado; no puede ser NULL.
 * @return 0 al completar, -1 con errno si falla.
 */
int receiveStatus(int socketFd, TransferStatus *status);

/**
 * @brief Transmite un archivo por bloques y comprueba que no haya crecido.
 * @param socketFd Socket conectado.
 * @param input Archivo abierto en lectura al inicio; conserva su propiedad el llamador.
 * @param fileSize Tamaño previamente validado, hasta MAX_TRANSFER_SIZE.
 * @return 0 al completar, -1 con errno ante errores o cambios de longitud.
 */
int sendFile(int socketFd, FILE *input, uint64_t fileSize);

/**
 * @brief Recibe el contenido por bloques sin cargarlo completo en memoria.
 * @param socketFd Socket conectado.
 * @param output Archivo abierto para escritura; no se cierra ni se vacía aquí.
 * @param fileSize Tamaño previamente validado, hasta MAX_TRANSFER_SIZE.
 * @return 0 al completar, -1 con errno ante errores de red o escritura.
 */
int receiveFile(int socketFd, FILE *output, uint64_t fileSize);

/**
 * @brief Exige EOF después del contenido para detectar bytes adicionales.
 * @param socketFd Socket conectado; el emisor debe haber ejecutado shutdown(SHUT_WR).
 * @return 0 al recibir EOF, -1 con errno (EPROTO si hay bytes adicionales).
 */
int expectStreamEnd(int socketFd);

#endif
