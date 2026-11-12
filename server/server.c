#include "server.h"
#include "protocol.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CONNECTION_BACKLOG 8
#define ACCEPT_POLL_MILLISECONDS 250

/* La señal solo modifica este indicador; la limpieza ocurre en el flujo normal. */
static volatile sig_atomic_t stopRequested = 0;

/**
 * @brief Solicita terminar el servicio sin ejecutar E/S desde una señal.
 * @param signalNumber Señal recibida, SIGINT o SIGTERM.
 */
static void requestStop(int signalNumber) {
    (void)signalNumber;
    stopRequested = 1;
}

/**
 * @brief Crea un archivo temporal junto al destino para permitir rename atómico.
 * @param outputPath Ruta final previamente validada.
 * @param stagingPath Buffer de PATH_MAX bytes para la ruta temporal generada.
 * @return Archivo abierto que debe cerrar el llamador, o NULL con errno.
 */
static FILE *openStagingFile(const char *outputPath, char *stagingPath) {
    int pathLength = snprintf(stagingPath, PATH_MAX, "%s.part.XXXXXX", outputPath);
    int outputFd = -1;
    FILE *output = NULL;

    if (pathLength < 0 || pathLength >= PATH_MAX) {
        stagingPath[0] = '\0';
        errno = ENAMETOOLONG;
        return NULL;
    }
    outputFd = mkstemp(stagingPath);
    if (outputFd == -1) {
        stagingPath[0] = '\0';
        return NULL;
    }
    output = fdopen(outputFd, "wb");
    if (output == NULL) {
        int savedError = errno;
        if (close(outputFd) == -1) {
            perror("WARN: close staging file");
        }
        if (unlink(stagingPath) == -1) {
            perror("WARN: remove staging file");
        }
        stagingPath[0] = '\0';
        errno = savedError;
    }
    return output;
}

/**
 * @brief Recibe una transferencia y confirma únicamente tras cerrar y publicar el archivo.
 * @param socketFd Socket conectado, cuya propiedad conserva el llamador.
 * @param outputPath Ruta final previamente validada.
 */
static void handleTransfer(int socketFd, const char *outputPath) {
    uint64_t fileSize = 0;
    char stagingPath[PATH_MAX] = "";
    FILE *output = NULL;
    TransferStatus status = TRANSFER_INVALID;

    if (configureSocket(socketFd) == -1) {
        perror("ERROR: configure connection");
        return;
    }
    if (receiveHeader(socketFd, &fileSize) == -1) {
        perror("ERROR: invalid or incomplete header");
        goto cleanup;
    }
    status = TRANSFER_IO_ERROR;
    output = openStagingFile(outputPath, stagingPath);
    if (output == NULL) {
        perror("ERROR: create staging file");
        goto cleanup;
    }
    if (sendStatus(socketFd, TRANSFER_READY) == -1) {
        perror("ERROR: send ready status");
        goto cleanup;
    }
    status = TRANSFER_INCOMPLETE;
    if (receiveFile(socketFd, output, fileSize) == -1) {
        if (errno != ECONNRESET && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            status = TRANSFER_IO_ERROR;
        }
        perror("ERROR: receive file");
        goto cleanup;
    }
    if (expectStreamEnd(socketFd) == -1) {
        if (errno == EPROTO) {
            status = TRANSFER_INVALID;
        }
        perror("ERROR: expected end of outgoing stream");
        goto cleanup;
    }
    status = TRANSFER_IO_ERROR;
    if (fclose(output) == EOF) {
        output = NULL;
        perror("ERROR: close received file");
        goto cleanup;
    }
    output = NULL;
    if (rename(stagingPath, outputPath) == -1) {
        perror("ERROR: publish received file");
        goto cleanup;
    }
    stagingPath[0] = '\0';
    status = TRANSFER_SUCCESS;
    printf("INFO: stored %" PRIu64 " bytes in %s\n", fileSize, outputPath);

cleanup:
    if (output != NULL && fclose(output) == EOF) {
        perror("WARN: close staging file");
    }
    if (stagingPath[0] != '\0' && unlink(stagingPath) == -1) {
        perror("WARN: remove incomplete file");
    }
    if (sendStatus(socketFd, status) == -1) {
        perror("WARN: transfer status could not reach client");
    }
}

/**
 * @brief Espera conexiones y conserva el servicio después de transferencias fallidas.
 * @param listenerFd Socket de escucha; no se cierra aquí.
 * @param outputPath Ruta final previamente validada.
 * @return 0 al solicitar parada, -1 si falla la escucha.
 */
static int acceptConnections(int listenerFd, const char *outputPath) {
    struct pollfd listenerPoll = {listenerFd, POLLIN, 0};

    while (!stopRequested) {
        int pollResult = poll(&listenerPoll, 1, ACCEPT_POLL_MILLISECONDS);
        int connectionFd = -1;

        if (pollResult == -1 && errno == EINTR) {
            continue;
        }
        if (pollResult == -1 || (listenerPoll.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            fprintf(stderr, "ERROR: listener poll failed\n");
            return -1;
        }
        if (pollResult == 0 || stopRequested) {
            continue;
        }
        connectionFd = accept(listenerFd, NULL, NULL);
        if (connectionFd == -1) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            perror("ERROR: accept connection");
            return -1;
        }
        handleTransfer(connectionFd, outputPath);
        if (close(connectionFd) == -1) {
            perror("WARN: close client connection");
        }
    }
    return 0;
}

int runServer(const char *address, const char *port, const char *outputPath) {
    struct sockaddr_in endpoint;
    struct sigaction stopAction = {0};
    const int reuseAddress = 1;
    int listenerFd = -1;
    int result = -1;

    if (outputPath == NULL || *outputPath == '\0' ||
        parseEndpoint(address, port, &endpoint) == -1) {
        fprintf(stderr, "ERROR: expected loopback IPv4, port 1..65535 and output path\n");
        return -1;
    }
    stopRequested = 0;
    stopAction.sa_handler = requestStop;
    if (sigemptyset(&stopAction.sa_mask) == -1 ||
        sigaction(SIGINT, &stopAction, NULL) == -1 ||
        sigaction(SIGTERM, &stopAction, NULL) == -1) {
        perror("ERROR: configure stop signals");
        return -1;
    }
    /* O_NONBLOCK evita quedar en accept si una conexión desaparece después de poll. */
    listenerFd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listenerFd == -1 ||
        setsockopt(listenerFd, SOL_SOCKET, SO_REUSEADDR, &reuseAddress,
                   sizeof(reuseAddress)) == -1 ||
        bind(listenerFd, (struct sockaddr *)&endpoint, sizeof(endpoint)) == -1 ||
        listen(listenerFd, CONNECTION_BACKLOG) == -1) {
        perror("ERROR: start TCP listener");
        goto cleanup;
    }
    printf("INFO: listening on %s:%s (local plaintext tests only)\n", address, port);
    if (fflush(stdout) == EOF) {
        perror("ERROR: flush startup message");
        goto cleanup;
    }
    result = acceptConnections(listenerFd, outputPath);

cleanup:
    if (listenerFd != -1 && close(listenerFd) == -1) {
        perror("ERROR: close listener");
        result = -1;
    }
    return result;
}

/**
 * @brief Inicia el servidor secuencial para pruebas TCP locales sin cifrado.
 * @param argumentCount Cantidad de argumentos, incluido el ejecutable.
 * @param arguments Ejecutable, dirección, puerto y ruta de destino.
 * @return EXIT_SUCCESS al detenerse normalmente, EXIT_FAILURE ante errores de servicio.
 */
int main(int argumentCount, char *arguments[]) {
    if (argumentCount != 4) {
        fprintf(stderr, "Usage: %s <loopback-ipv4> <port> <output-file>\n", arguments[0]);
        return EXIT_FAILURE;
    }
    return runServer(arguments[1], arguments[2], arguments[3]) == 0 ?
        EXIT_SUCCESS : EXIT_FAILURE;
}
