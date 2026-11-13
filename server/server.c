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

// La señal solo modifica este indicador; la limpieza ocurre en el flujo normal.
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

    // Una ruta truncada no identifica un temporal válido y no debe usarse en la limpieza.
    if (pathLength < 0 || pathLength >= PATH_MAX) {
        stagingPath[0] = '\0';
        errno = ENAMETOOLONG;
        return NULL;
    }
    // Crea el temporal junto al destino para que rename opere en el mismo sistema de archivos.
    outputFd = mkstemp(stagingPath);
    if (outputFd == -1) {
        stagingPath[0] = '\0';
        return NULL;
    }
    // Si no se puede asociar el flujo, revierte la creación conservando el error original.
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
 * @brief Recibe, publica y procesa opcionalmente antes de confirmar la transferencia.
 * @param socketFd Socket conectado, cuya propiedad conserva el llamador.
 * @param outputPath Ruta final previamente validada.
 * @param processor Procesador opcional del archivo cifrado publicado.
 * @param context Contexto prestado del procesador.
 */
static void handleTransfer(int socketFd, const char *outputPath,
                           FileProcessor processor, void *context) {
    uint64_t fileSize = 0;
    char stagingPath[PATH_MAX] = "";
    FILE *output = NULL;
    TransferStatus status = TRANSFER_INVALID;

    // Acota las esperas y valida la cabecera antes de crear archivos para esta conexión.
    if (configureSocket(socketFd) == -1) {
        perror("ERROR: configure connection");
        return;
    }
    if (receiveHeader(socketFd, &fileSize) == -1) {
        perror("ERROR: invalid or incomplete header");
        goto cleanup;
    }
    // READY se envía solo después de preparar el temporal, sin tocar aún el destino anterior.
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
    // Recibe la longitud anunciada y distingue fallos de recepción de errores de E/S.
    status = TRANSFER_INCOMPLETE;
    if (receiveFile(socketFd, output, fileSize) == -1) {
        if (errno != ECONNRESET && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            status = TRANSFER_IO_ERROR;
        }
        perror("ERROR: receive file");
        goto cleanup;
    }
    // Exige el cierre de envío del cliente para detectar contenido sobrante.
    if (expectStreamEnd(socketFd) == -1) {
        if (errno == EPROTO) {
            status = TRANSFER_INVALID;
        }
        perror("ERROR: expected end of outgoing stream");
        goto cleanup;
    }
    // fclose comprueba el vaciado pendiente; solo después se reemplaza el destino.
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
    // El temporal ya es el archivo final: deja de ser un recurso pendiente de eliminación.
    stagingPath[0] = '\0';
    printf("INFO: stored encrypted container (%" PRIu64 " bytes) in %s\n", fileSize, outputPath);
    // El modo distribuido solo confirma éxito cuando los tres rangos completan su trabajo.
    if (processor != NULL && processor(outputPath, context) == -1) {
        perror("ERROR: process received file");
        goto cleanup;
    }
    status = TRANSFER_SUCCESS;

cleanup:
    // Un fallo previo a rename elimina el temporal y conserva la salida anterior.
    if (output != NULL && fclose(output) == EOF) {
        perror("WARN: close staging file");
    }
    if (stagingPath[0] != '\0' && unlink(stagingPath) == -1) {
        perror("WARN: remove incomplete file");
    }
    // Intenta comunicar el resultado incluso tras un error; el par puede estar desconectado.
    if (sendStatus(socketFd, status) == -1) {
        perror("WARN: transfer status could not reach client");
    }
}

/**
 * @brief Espera conexiones y conserva el servicio después de transferencias fallidas.
 * @param listenerFd Socket de escucha; no se cierra aquí.
 * @param outputPath Ruta final previamente validada.
 * @param processor Procesador opcional del archivo recibido.
 * @param context Contexto prestado del procesador.
 * @return 0 al solicitar parada, -1 si falla la escucha.
 */
static int acceptConnections(int listenerFd, const char *outputPath,
                              FileProcessor processor, void *context) {
    struct pollfd listenerPoll = {listenerFd, POLLIN, 0};

    // La espera periódica permite revisar la solicitud de parada sin una espera activa.
    while (!stopRequested) {
        int pollResult = poll(&listenerPoll, 1, ACCEPT_POLL_MILLISECONDS);
        int connectionFd = -1;

        // Una señal interrumpe la espera: vuelve al inicio para comprobar si se debe salir.
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
        // Aunque poll anuncie actividad, accept puede no encontrar ya una conexión pendiente.
        connectionFd = accept(listenerFd, NULL, NULL);
        if (connectionFd == -1) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            perror("ERROR: accept connection");
            return -1;
        }
        // Cada cliente se atiende y cierra antes del siguiente; un fallo no detiene la escucha.
        handleTransfer(connectionFd, outputPath, processor, context);
        if (close(connectionFd) == -1) {
            perror("WARN: close client connection");
        }
    }
    return 0;
}

int runServerWithProcessor(const char *address, const char *port, const char *outputPath,
                           FileProcessor processor, void *context) {
    struct sockaddr_in endpoint;
    struct sigaction stopAction = {0};
    const int reuseAddress = 1;
    int listenerFd = -1;
    int result = -1;

    // Rechaza configuraciones inválidas antes de instalar señales o crear el socket.
    if (outputPath == NULL || *outputPath == '\0' ||
        parseEndpoint(address, port, &endpoint) == -1) {
        fprintf(stderr, "ERROR: expected loopback IPv4, port 1..65535 and output path\n");
        return -1;
    }
    // Los manejadores solo solicitan la parada; el flujo normal queda a cargo de la limpieza.
    stopRequested = 0;
    stopAction.sa_handler = requestStop;
    if (sigemptyset(&stopAction.sa_mask) == -1 ||
        sigaction(SIGINT, &stopAction, NULL) == -1 ||
        sigaction(SIGTERM, &stopAction, NULL) == -1) {
        perror("ERROR: configure stop signals");
        return -1;
    }
    // La escucha no bloqueante evita quedar en accept si la conexión desaparece tras poll.
    listenerFd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listenerFd == -1 ||
        setsockopt(listenerFd, SOL_SOCKET, SO_REUSEADDR, &reuseAddress,
                   sizeof(reuseAddress)) == -1 ||
        bind(listenerFd, (struct sockaddr *)&endpoint, sizeof(endpoint)) == -1 ||
        listen(listenerFd, CONNECTION_BACKLOG) == -1) {
        perror("ERROR: start TCP listener");
        goto cleanup;
    }
    // Publica el aviso de disponibilidad incluso con stdout redirigido antes de atender.
    printf("INFO: listening on %s:%s (%s)\n", address, port,
           processor != NULL ? "local TCP + three MPI workers" : "local encrypted file storage");
    if (fflush(stdout) == EOF) {
        perror("ERROR: flush startup message");
        goto cleanup;
    }
    result = acceptConnections(listenerFd, outputPath, processor, context);

cleanup:
    // La conexión de cada cliente ya se cierra en el bucle; aquí se libera la escucha.
    if (listenerFd != -1 && close(listenerFd) == -1) {
        perror("ERROR: close listener");
        result = -1;
    }
    return result;
}

int runServer(const char *address, const char *port, const char *outputPath) {
    // Conserva el servicio de almacenamiento y sus contratos para las pruebas existentes.
    return runServerWithProcessor(address, port, outputPath, NULL, NULL);
}
