#include "protocol.h"

#include <errno.h>
#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Comprueba un estado recibido durante una prueba.
 * @param socketFd Conexión bajo prueba.
 * @param expectedStatus Estado requerido.
 * @return 0 si coincide, -1 si falla o no coincide.
 */
static int expectStatus(int socketFd, TransferStatus expectedStatus) {
    TransferStatus actualStatus = TRANSFER_INVALID;

    if (receiveStatus(socketFd, &actualStatus) == -1 || actualStatus != expectedStatus) {
        fprintf(stderr, "ERROR: peer expected status %u\n", (unsigned int)expectedStatus);
        return -1;
    }
    return 0;
}

/**
 * @brief Envía cada byte por separado con una pausa para fragmentar el flujo.
 * @param socketFd Conexión bajo prueba.
 * @param bytes Origen no nulo.
 * @param length Cantidad de bytes.
 * @return 0 al completar, -1 si falla una operación.
 */
static int sendFragmented(int socketFd, const unsigned char *bytes, size_t length) {
    const struct timespec delay = {0, 2000000};

    // Las pausas favorecen entregas parciales sin suponer límites de mensajes en TCP.
    for (size_t index = 0; index < length; index++) {
        if (sendAll(socketFd, bytes + index, 1) == -1 || nanosleep(&delay, NULL) == -1) {
            return -1;
        }
    }
    return 0;
}

/**
 * @brief Construye un contenedor válido pequeño y lo envía byte a byte junto a su cabecera.
 * @param socketFd Conexión al servidor real.
 * @param message Texto de prueba terminado en cero, no NULL.
 * @return 0 al enviar, -1 ante errores del auxiliar.
 */
static int sendEncryptedFragmented(int socketFd, const char *message) {
    unsigned char key[ENCRYPTION_KEY_SIZE] = {0};
    unsigned char header[TRANSFER_HEADER_SIZE] = {'D', 'T', 'P', '2'};
    unsigned char envelope[TRANSFER_BLOCK_SIZE];
    size_t messageLength = strlen(message);
    size_t envelopeSize = messageLength + ENCRYPTION_OVERHEAD;
    FILE *plainInput = tmpfile();
    FILE *encrypted = tmpfile();
    int result = -1;

    // El tamaño elegido cabe en un byte; la misma clave efímera de la suite cifra el mensaje.
    if (plainInput == NULL || encrypted == NULL || envelopeSize > UINT8_MAX ||
        loadEncryptionKey(getenv("TYPIST_KEY_FILE"), key) == -1 ||
        fwrite(message, 1, messageLength, plainInput) != messageLength ||
        fseeko(plainInput, 0, SEEK_SET) == -1 ||
        encryptFile(plainInput, encrypted, messageLength, key) == -1 ||
        fseeko(encrypted, 0, SEEK_SET) == -1 ||
        fread(envelope, 1, envelopeSize, encrypted) != envelopeSize) {
        goto cleanup;
    }
    header[TRANSFER_HEADER_SIZE - 1] = (unsigned char)envelopeSize;
    if (sendFragmented(socketFd, header, sizeof(header)) == -1 ||
        expectStatus(socketFd, TRANSFER_READY) == -1 ||
        sendFragmented(socketFd, envelope, envelopeSize) == -1) {
        goto cleanup;
    }
    result = 0;

cleanup:
    OPENSSL_cleanse(key, sizeof(key));
    if (plainInput != NULL && fclose(plainInput) == EOF) {
        result = -1;
    }
    if (encrypted != NULL && fclose(encrypted) == EOF) {
        result = -1;
    }
    return result;
}

/**
 * @brief Ejecuta casos de protocolo inválido o fragmentado contra el servidor real.
 * @param socketFd Socket conectado que conserva el llamador.
 * @param mode Nombre del caso seleccionado.
 * @return 0 si se observa el comportamiento esperado, -1 si falla.
 */
static int exerciseServer(int socketFd, const char *mode) {
    unsigned char header[TRANSFER_HEADER_SIZE] = {'D', 'T', 'P', '2'};
    const char *message = "fragmented\n";
    const unsigned char incompleteBody[ENCRYPTION_OVERHEAD + 1] = {0};
    TransferStatus expectedStatus = TRANSFER_INVALID;

    if (strcmp(mode, "partialHeader") == 0) {
        // El EOF posterior dejará incompleta la cabecera, antes de preparar la salida.
        if (sendAll(socketFd, header, 5) == -1) {
            return -1;
        }
    } else if (strcmp(mode, "badMagic") == 0 || strcmp(mode, "oversize") == 0 ||
               strcmp(mode, "legacy") == 0 || strcmp(mode, "belowMinimum") == 0) {
        // Altera la versión o anuncia UINT64_MAX para comprobar el rechazo de la cabecera.
        if (strcmp(mode, "badMagic") == 0) {
            header[0] = 'X';
        } else if (strcmp(mode, "oversize") == 0) {
            memset(header + 4, 255, 8);
        } else if (strcmp(mode, "legacy") == 0) {
            header[3] = '1';
        }
        if (sendAll(socketFd, header, sizeof(header)) == -1) {
            return -1;
        }
    } else if (strcmp(mode, "fragmented") == 0) {
        // Tanto la cabecera TCP como el contenedor GCM se fragmentan en envíos de un byte.
        if (sendEncryptedFragmented(socketFd, message) == -1) {
            return -1;
        }
        expectedStatus = TRANSFER_SUCCESS;
    } else if (strcmp(mode, "truncated") == 0 || strcmp(mode, "reset") == 0 ||
               strcmp(mode, "extra") == 0 || strcmp(mode, "stall") == 0) {
        uint64_t declaredSize = strcmp(mode, "extra") == 0 ? ENCRYPTION_OVERHEAD : 64;

        // Primero completa la negociación para aislar los errores de la fase del cuerpo.
        if (sendHeader(socketFd, declaredSize) == -1 ||
            expectStatus(socketFd, TRANSFER_READY) == -1) {
            return -1;
        }
        if (strcmp(mode, "reset") == 0) {
            // El cierre que realiza main enviará RST en lugar de un EOF normal.
            const struct linger resetConnection = {1, 0};
            return setsockopt(socketFd, SOL_SOCKET, SO_LINGER,
                              &resetConnection, sizeof(resetConnection));
        }
        if (strcmp(mode, "stall") == 0) {
            // Mantiene la conexión abierta sin datos hasta superar la espera del servidor.
            const struct timespec delay = {SOCKET_TIMEOUT_SECONDS + 2, 0};
            if (nanosleep(&delay, NULL) == -1) {
                return -1;
            }
            return expectStatus(socketFd, TRANSFER_INCOMPLETE);
        }
        // El cuerpo excede el mínimo declarado o queda corto respecto a los 64 bytes.
        if (sendAll(socketFd, incompleteBody, sizeof(incompleteBody)) == -1) {
            return -1;
        }
        expectedStatus = declaredSize == ENCRYPTION_OVERHEAD ?
            TRANSFER_INVALID : TRANSFER_INCOMPLETE;
    } else {
        fprintf(stderr, "ERROR: unknown peer mode\n");
        return -1;
    }
    // Señala fin del envío y conserva la recepción para comprobar el resultado del servidor.
    if (shutdown(socketFd, SHUT_WR) == -1) {
        return -1;
    }
    return expectStatus(socketFd, expectedStatus);
}

/**
 * @brief Atiende una conexión para probar desconexiones y confirmaciones del cliente real.
 * @param listenerFd Socket de escucha que conserva el llamador.
 * @param mode drop, noAck o invalidStatus.
 * @return 0 si se completó la situación de prueba, -1 ante un error del auxiliar.
 */
static int exerciseClient(int listenerFd, const char *mode) {
    int connectionFd = -1;
    int result = -1;
    uint64_t fileSize = 0;
    FILE *discardFile = NULL;

    // El coordinador espera este aviso antes de iniciar el cliente real.
    if (puts("INFO: listening") == EOF || fflush(stdout) == EOF) {
        return -1;
    }
    connectionFd = accept(listenerFd, NULL, NULL);
    if (connectionFd == -1) {
        return -1;
    }
    // Recibe una cabecera real antes de provocar el fallo seleccionado.
    if (configureSocket(connectionFd) == -1 || receiveHeader(connectionFd, &fileSize) == -1) {
        goto cleanup;
    }
    if (strcmp(mode, "invalidStatus") == 0) {
        // Comprueba que el cliente rechace un código ajeno al protocolo.
        const unsigned char invalidStatus = 255;
        result = sendAll(connectionFd, &invalidStatus, 1);
        goto cleanup;
    }
    if (sendStatus(connectionFd, TRANSFER_READY) == -1) {
        goto cleanup;
    }
    if (strcmp(mode, "drop") == 0) {
        // Espera a que empiece el cuerpo y prepara un cierre abrupto durante la transferencia.
        const struct linger resetConnection = {1, 0};
        unsigned char firstByte = 0;

        if (receiveAll(connectionFd, &firstByte, 1) == -1) {
            goto cleanup;
        }
        result = setsockopt(connectionFd, SOL_SOCKET, SO_LINGER,
                            &resetConnection, sizeof(resetConnection));
    } else {
        // Consume todo el archivo, pero cierra sin confirmar para ejercitar la pérdida del ACK.
        discardFile = tmpfile();
        if (discardFile == NULL || receiveFile(connectionFd, discardFile, fileSize) == -1 ||
            expectStreamEnd(connectionFd) == -1) {
            goto cleanup;
        }
        result = 0;
    }

cleanup:
    // Tanto los fallos provocados como los del auxiliar convergen en la misma liberación.
    if (discardFile != NULL && fclose(discardFile) == EOF) {
        result = -1;
    }
    if (close(connectionFd) == -1) {
        result = -1;
    }
    return result;
}

/**
 * @brief Ejecuta un auxiliar de pruebas TCP; no forma parte de la aplicación.
 * @param argumentCount Cantidad de argumentos.
 * @param arguments Ejecutable, modo y puerto opcional; el modo port busca uno libre.
 * @return EXIT_SUCCESS al cumplir el caso, EXIT_FAILURE ante errores.
 */
int main(int argumentCount, char *arguments[]) {
    struct sockaddr_in endpoint;
    int socketFd = -1;
    int result = -1;
    int reuseAddress = 1;
    int isListener = 0;

    // Prepara un extremo local; el puerto provisional solo se usa para el modo de búsqueda.
    if (argumentCount < 2 || argumentCount > 3) {
        return EXIT_FAILURE;
    }
    if (parseEndpoint("127.0.0.1", argumentCount == 3 ? arguments[2] : "1", &endpoint) == -1) {
        return EXIT_FAILURE;
    }
    socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd == -1 || configureSocket(socketFd) == -1) {
        perror("ERROR: peer socket");
        goto cleanup;
    }
    if (strcmp(arguments[1], "port") == 0) {
        // El puerto cero solicita uno libre al kernel; se informa y se libera al salir.
        socklen_t endpointSize = sizeof(endpoint);
        endpoint.sin_port = 0;
        if (bind(socketFd, (struct sockaddr *)&endpoint, sizeof(endpoint)) == 0 &&
            getsockname(socketFd, (struct sockaddr *)&endpoint, &endpointSize) == 0) {
            result = printf("%u\n", (unsigned int)ntohs(endpoint.sin_port)) > 0 ? 0 : -1;
        }
        goto cleanup;
    }
    // Selecciona el papel del auxiliar según se pruebe el cliente o el servidor real.
    isListener = strcmp(arguments[1], "drop") == 0 || strcmp(arguments[1], "noAck") == 0 ||
        strcmp(arguments[1], "invalidStatus") == 0;
    if (isListener) {
        if (setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR,
                       &reuseAddress, sizeof(reuseAddress)) == -1 ||
            bind(socketFd, (struct sockaddr *)&endpoint, sizeof(endpoint)) == -1 ||
            listen(socketFd, 1) == -1) {
            goto cleanup;
        }
        result = exerciseClient(socketFd, arguments[1]);
    } else if (connect(socketFd, (struct sockaddr *)&endpoint, sizeof(endpoint)) == 0) {
        result = exerciseServer(socketFd, arguments[1]);
    }

cleanup:
    // El cierre también materializa el RST cuando el caso de prueba configuró SO_LINGER.
    if (result == -1) {
        perror("ERROR: TCP test peer");
    }
    if (socketFd != -1 && close(socketFd) == -1) {
        result = -1;
    }
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
