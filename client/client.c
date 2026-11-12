#include "client.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * @brief Abre y valida un archivo regular sin bloquear ante una FIFO.
 * @param filePath Ruta no nula del archivo.
 * @param fileSize Recibe su tamaño validado; no NULL.
 * @return Archivo que debe cerrar el llamador, o NULL con errno si falla.
 */
static FILE *openInput(const char *filePath, uint64_t *fileSize) {
    struct stat fileInfo;
    int inputFd = open(filePath, O_RDONLY | O_NONBLOCK);
    FILE *input = NULL;

    if (inputFd == -1) {
        return NULL;
    }
    if (fstat(inputFd, &fileInfo) == -1) {
        int savedError = errno;
        if (close(inputFd) == -1) {
            perror("WARN: close input");
        }
        errno = savedError;
        return NULL;
    }
    if (!S_ISREG(fileInfo.st_mode) || fileInfo.st_size < 0 ||
        (uint64_t)fileInfo.st_size > MAX_FILE_SIZE) {
        if (close(inputFd) == -1) {
            perror("WARN: close input");
        }
        errno = EINVAL;
        return NULL;
    }
    input = fdopen(inputFd, "rb");
    if (input == NULL) {
        int savedError = errno;
        if (close(inputFd) == -1) {
            perror("WARN: close input");
        }
        errno = savedError;
        return NULL;
    }
    *fileSize = (uint64_t)fileInfo.st_size;
    return input;
}

/**
 * @brief Exige el estado esperado y registra rechazos o pérdida de confirmación.
 * @param socketFd Socket conectado.
 * @param expectedStatus Estado requerido en esta fase.
 * @return 0 si coincide, -1 ante errores o rechazo.
 */
static int checkStatus(int socketFd, TransferStatus expectedStatus) {
    TransferStatus status = TRANSFER_INVALID;

    if (receiveStatus(socketFd, &status) == -1) {
        perror("ERROR: receive server status; transfer not confirmed");
        return -1;
    }
    if (status != expectedStatus) {
        fprintf(stderr, "ERROR: unexpected server status %u (expected %u)\n",
                (unsigned int)status, (unsigned int)expectedStatus);
        return -1;
    }
    return 0;
}

int runClient(const char *address, const char *port, const char *filePath) {
    struct sockaddr_in endpoint;
    uint64_t fileSize = 0;
    FILE *input = NULL;
    int socketFd = -1;
    int result = -1;

    if (filePath == NULL || *filePath == '\0' || parseEndpoint(address, port, &endpoint) == -1) {
        fprintf(stderr, "ERROR: expected loopback IPv4, port 1..65535 and file path\n");
        return -1;
    }
    input = openInput(filePath, &fileSize);
    if (input == NULL) {
        perror("ERROR: open regular input file (maximum 1 GiB)");
        return -1;
    }
    socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd == -1 || configureSocket(socketFd) == -1 ||
        connect(socketFd, (struct sockaddr *)&endpoint, sizeof(endpoint)) == -1) {
        perror("ERROR: connect to server");
        goto cleanup;
    }
    if (sendHeader(socketFd, fileSize) == -1) {
        perror("ERROR: send header");
        goto cleanup;
    }
    if (checkStatus(socketFd, TRANSFER_READY) == -1) {
        goto cleanup;
    }
    if (sendFile(socketFd, input, fileSize) == -1) {
        perror("ERROR: send file");
        goto cleanup;
    }
    if (fclose(input) == EOF) {
        input = NULL;
        perror("ERROR: close input");
        goto cleanup;
    }
    input = NULL;
    if (shutdown(socketFd, SHUT_WR) == -1) {
        perror("ERROR: finish outgoing stream");
        goto cleanup;
    }
    if (checkStatus(socketFd, TRANSFER_SUCCESS) == -1) {
        goto cleanup;
    }
    printf("INFO: transfer confirmed (%" PRIu64 " bytes)\n", fileSize);
    result = 0;

cleanup:
    if (input != NULL && fclose(input) == EOF) {
        perror("ERROR: close input");
        result = -1;
    }
    if (socketFd != -1 && close(socketFd) == -1) {
        perror("ERROR: close connection");
        result = -1;
    }
    return result;
}

/**
 * @brief Ejecuta una transferencia local configurada por argumentos.
 * @param argumentCount Cantidad de argumentos, incluido el ejecutable.
 * @param arguments Ejecutable, dirección, puerto y ruta de origen.
 * @return EXIT_SUCCESS con confirmación, EXIT_FAILURE ante cualquier error.
 */
int main(int argumentCount, char *arguments[]) {
    if (argumentCount != 4) {
        fprintf(stderr, "Usage: %s <loopback-ipv4> <port> <input-file>\n", arguments[0]);
        return EXIT_FAILURE;
    }
    return runClient(arguments[1], arguments[2], arguments[3]) == 0 ?
        EXIT_SUCCESS : EXIT_FAILURE;
}
