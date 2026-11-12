#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

int parseEndpoint(const char *address, const char *port, struct sockaddr_in *endpoint) {
    unsigned long portNumber = 0;

    // Exige texto decimal estricto: strtoul por sí solo también admitiría signos y espacios.
    if (address == NULL || port == NULL || endpoint == NULL || *port == '\0') {
        errno = EINVAL;
        return -1;
    }
    for (const char *digit = port; *digit != '\0'; digit++) {
        if (*digit < '0' || *digit > '9') {
            errno = EINVAL;
            return -1;
        }
    }
    // Comprueba tanto el desbordamiento de la conversión como el rango de puertos utilizables.
    errno = 0;
    portNumber = strtoul(port, NULL, 10);
    if (errno != 0 || portNumber == 0 || portNumber > UINT16_MAX) {
        errno = EINVAL;
        return -1;
    }
    // Construye la dirección en orden de red y limita esta etapa sin cifrado a 127.0.0.0/8.
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->sin_family = AF_INET;
    endpoint->sin_port = htons((uint16_t)portNumber);
    if (inet_pton(AF_INET, address, &endpoint->sin_addr) != 1 ||
        (ntohl(endpoint->sin_addr.s_addr) >> 24) != 127) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int configureSocket(int socketFd) {
    const struct timeval timeout = {SOCKET_TIMEOUT_SECONDS, 0};

    // Acota ambos sentidos de E/S; estos límites no representan un plazo total del archivo.
    if (setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 ||
        setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1) {
        return -1;
    }
    return 0;
}

int sendAll(int socketFd, const void *buffer, size_t length) {
    const unsigned char *bytes = buffer;
    size_t sentBytes = 0;

    if ((buffer == NULL && length != 0) || length > TRANSFER_BLOCK_SIZE) {
        errno = EINVAL;
        return -1;
    }
    // Reenvía solo el tramo pendiente; MSG_NOSIGNAL convierte la desconexión en un error.
    while (sentBytes < length) {
        ssize_t result = send(socketFd, bytes + sentBytes, length - sentBytes, MSG_NOSIGNAL);

        // Una interrupción sin bytes enviados permite reintentar sin avanzar el desplazamiento.
        if (result == -1 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            if (result == 0) {
                errno = EPIPE;
            }
            return -1;
        }
        sentBytes += (size_t)result;
    }
    return 0;
}

int receiveAll(int socketFd, void *buffer, size_t length) {
    unsigned char *bytes = buffer;
    size_t receivedBytes = 0;

    if ((buffer == NULL && length != 0) || length > TRANSFER_BLOCK_SIZE) {
        errno = EINVAL;
        return -1;
    }
    // Acumula las entregas de TCP hasta completar la longitud requerida por el protocolo.
    while (receivedBytes < length) {
        ssize_t result = recv(socketFd, bytes + receivedBytes, length - receivedBytes, 0);

        if (result == -1 && errno == EINTR) {
            continue;
        }
        // EOF antes de completar el bloque indica una transferencia truncada, no éxito.
        if (result <= 0) {
            if (result == 0) {
                errno = ECONNRESET;
            }
            return -1;
        }
        receivedBytes += (size_t)result;
    }
    return 0;
}

int sendHeader(int socketFd, uint64_t fileSize) {
    unsigned char header[TRANSFER_HEADER_SIZE] = {'D', 'T', 'P', '1'};

    if (fileSize > MAX_FILE_SIZE) {
        errno = EFBIG;
        return -1;
    }
    // Coloca los bytes del tamaño desde el final, dejando el más significativo primero.
    for (size_t index = 0; index < sizeof(fileSize); index++) {
        header[sizeof(header) - 1 - index] = (unsigned char)(fileSize >> (index * 8));
    }
    return sendAll(socketFd, header, sizeof(header));
}

int receiveHeader(int socketFd, uint64_t *fileSize) {
    unsigned char header[TRANSFER_HEADER_SIZE];
    uint64_t decodedSize = 0;

    if (fileSize == NULL) {
        errno = EINVAL;
        return -1;
    }
    // No interpreta campos hasta completar la cabecera y reconocer la versión del protocolo.
    if (receiveAll(socketFd, header, sizeof(header)) == -1) {
        return -1;
    }
    if (memcmp(header, "DTP1", 4) != 0) {
        errno = EPROTO;
        return -1;
    }
    // Reconstruye el entero big endian sin depender del orden de bytes del equipo.
    for (size_t index = 4; index < sizeof(header); index++) {
        decodedSize = (decodedSize << 8) | header[index];
    }
    // Publica el tamaño al llamador únicamente cuando cumple el límite de recepción.
    if (decodedSize > MAX_FILE_SIZE) {
        errno = EFBIG;
        return -1;
    }
    *fileSize = decodedSize;
    return 0;
}

int sendStatus(int socketFd, TransferStatus status) {
    unsigned char statusByte = (unsigned char)status;

    // Valida el valor original antes de transmitir su representación de un byte.
    if (status < TRANSFER_SUCCESS || status > TRANSFER_INCOMPLETE) {
        errno = EINVAL;
        return -1;
    }
    return sendAll(socketFd, &statusByte, sizeof(statusByte));
}

int receiveStatus(int socketFd, TransferStatus *status) {
    unsigned char statusByte = 0;

    if (status == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (receiveAll(socketFd, &statusByte, sizeof(statusByte)) == -1) {
        return -1;
    }
    // Solo convierte a la enumeración los códigos que el protocolo reconoce.
    if (statusByte > TRANSFER_INCOMPLETE) {
        errno = EPROTO;
        return -1;
    }
    *status = (TransferStatus)statusByte;
    return 0;
}

int sendFile(int socketFd, FILE *input, uint64_t fileSize) {
    unsigned char buffer[TRANSFER_BLOCK_SIZE];
    uint64_t remainingBytes = fileSize;

    if (input == NULL || fileSize > MAX_FILE_SIZE) {
        errno = EINVAL;
        return -1;
    }
    // Ajusta el último bloque al resto anunciado para no enviar bytes fuera del tamaño.
    while (remainingBytes > 0) {
        size_t blockSize = remainingBytes < sizeof(buffer) ?
            (size_t)remainingBytes : sizeof(buffer);

        // Una lectura corta puede indicar que el origen se redujo; nunca envía ese bloque.
        errno = 0;
        if (fread(buffer, 1, blockSize, input) != blockSize) {
            if (errno == 0) {
                errno = EIO;
            }
            return -1;
        }
        if (sendAll(socketFd, buffer, blockSize) == -1) {
            return -1;
        }
        remainingBytes -= blockSize;
    }
    // Tras la longitud anunciada, un byte adicional revela crecimiento del archivo.
    errno = 0;
    if (fgetc(input) != EOF || ferror(input)) {
        if (errno == 0) {
            errno = EIO;
        }
        return -1;
    }
    return 0;
}

int receiveFile(int socketFd, FILE *output, uint64_t fileSize) {
    unsigned char buffer[TRANSFER_BLOCK_SIZE];
    uint64_t remainingBytes = fileSize;

    if (output == NULL || fileSize > MAX_FILE_SIZE) {
        errno = EINVAL;
        return -1;
    }
    // Mantiene memoria acotada y no consume más bytes que los declarados en la cabecera.
    while (remainingBytes > 0) {
        size_t blockSize = remainingBytes < sizeof(buffer) ?
            (size_t)remainingBytes : sizeof(buffer);

        // Completa cada bloque de red antes de escribirlo; el llamador cerrará el archivo.
        if (receiveAll(socketFd, buffer, blockSize) == -1) {
            return -1;
        }
        errno = 0;
        if (fwrite(buffer, 1, blockSize, output) != blockSize) {
            if (errno == 0) {
                errno = EIO;
            }
            return -1;
        }
        remainingBytes -= blockSize;
    }
    return 0;
}

int expectStreamEnd(int socketFd) {
    unsigned char extraByte = 0;
    ssize_t result = 0;

    // Después del cuerpo solo se admite EOF; leer un byte basta para detectar un exceso.
    do {
        result = recv(socketFd, &extraByte, sizeof(extraByte), 0);
    } while (result == -1 && errno == EINTR);
    if (result == 0) {
        return 0;
    }
    if (result > 0) {
        errno = EPROTO;
    }
    return -1;
}
