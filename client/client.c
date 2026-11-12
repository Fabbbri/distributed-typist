#include "client.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define COMMAND_BUFFER_SIZE (PATH_MAX + 32)

typedef enum {
    COMMAND_READ_ERROR = -1,
    COMMAND_END = 0,
    COMMAND_READY = 1,
    COMMAND_INVALID = 2
} CommandReadResult;

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
    // Consulta el descriptor abierto para validar el archivo que realmente se enviará.
    if (fstat(inputFd, &fileInfo) == -1) {
        // La limpieza no debe sustituir el error que impidió obtener los metadatos.
        int savedError = errno;
        if (close(inputFd) == -1) {
            perror("WARN: close input");
        }
        errno = savedError;
        return NULL;
    }
    // Descarta fuentes sin longitud utilizable y archivos que excedan el protocolo.
    if (!S_ISREG(fileInfo.st_mode) || fileInfo.st_size < 0 ||
        (uint64_t)fileInfo.st_size > MAX_FILE_SIZE) {
        if (close(inputFd) == -1) {
            perror("WARN: close input");
        }
        errno = EINVAL;
        return NULL;
    }
    // Desde fdopen, el flujo FILE asume el descriptor; si falla, se cierra aquí.
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
    // Un estado válido también es un error si no corresponde a la fase actual.
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

    // Valida la configuración y el origen antes de adquirir recursos de red.
    if (filePath == NULL || *filePath == '\0' || parseEndpoint(address, port, &endpoint) == -1) {
        fprintf(stderr, "ERROR: expected loopback IPv4, port 1..65535 and file path\n");
        return -1;
    }
    input = openInput(filePath, &fileSize);
    if (input == NULL) {
        perror("ERROR: open regular input file (maximum 1 GiB)");
        return -1;
    }
    // Establece la conexión con esperas acotadas para evitar bloqueos indefinidos de E/S.
    socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd == -1 || configureSocket(socketFd) == -1 ||
        connect(socketFd, (struct sockaddr *)&endpoint, sizeof(endpoint)) == -1) {
        perror("ERROR: connect to server");
        goto cleanup;
    }
    // Anuncia el tamaño y espera que el servidor prepare la salida antes de enviar el cuerpo.
    if (sendHeader(socketFd, fileSize) == -1) {
        perror("ERROR: send header");
        goto cleanup;
    }
    if (checkStatus(socketFd, TRANSFER_READY) == -1) {
        goto cleanup;
    }
    // El envío por bloques y el cierre del origen deben terminar antes de señalar el EOF.
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
    // Cierra solo el sentido de envío: aún necesita recibir la confirmación final.
    if (shutdown(socketFd, SHUT_WR) == -1) {
        perror("ERROR: finish outgoing stream");
        goto cleanup;
    }
    // Haber enviado todos los bytes no basta: el servidor debe confirmar su almacenamiento.
    if (checkStatus(socketFd, TRANSFER_SUCCESS) == -1) {
        goto cleanup;
    }
    printf("INFO: transfer confirmed (%" PRIu64 " bytes)\n", fileSize);
    result = 0;

cleanup:
    // Todas las salidas posteriores a la apertura liberan únicamente los recursos adquiridos.
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
 * @brief Lee una línea acotada y descarta por completo comandos excesivos o con bytes nulos.
 * @param command Buffer de destino, no NULL.
 * @param capacity Capacidad del buffer, como mínimo dos bytes.
 * @return COMMAND_READY con una línea, COMMAND_END con EOF sin datos, COMMAND_INVALID
 * ante una línea descartada o COMMAND_READ_ERROR con errno ante errores de lectura.
 */
static CommandReadResult readCommand(char *command, size_t capacity) {
    size_t commandLength = 0;
    int nextCharacter = 0;
    int invalidCommand = 0;

    if (command == NULL || capacity < 2) {
        errno = EINVAL;
        return COMMAND_READ_ERROR;
    }
    // Consume hasta el fin de línea incluso tras un error de formato, sin dividir un comando.
    while ((nextCharacter = fgetc(stdin)) != EOF && nextCharacter != '\n') {
        if (nextCharacter == '\0' || commandLength >= capacity - 1) {
            invalidCommand = 1;
        } else if (!invalidCommand) {
            command[commandLength++] = (char)nextCharacter;
        }
    }
    command[commandLength] = '\0';
    if (ferror(stdin)) {
        return COMMAND_READ_ERROR;
    }
    if (invalidCommand) {
        return COMMAND_INVALID;
    }
    // Permite procesar la última línea aunque no tenga un salto de línea antes del EOF.
    return nextCharacter == EOF && commandLength == 0 ? COMMAND_END : COMMAND_READY;
}

/**
 * @brief Interpreta una orden y ejecuta una transferencia sin cerrar la sesión por fallos.
 * @param command Línea modificable terminada en cero, no NULL.
 * @param address Dirección validada del servidor.
 * @param port Puerto validado del servidor.
 * @param inputDirectory Directorio de entrada validado.
 * @return 0 si se solicita salir sin argumentos, 1 para continuar la sesión.
 */
static int handleCommand(char *command, const char *address, const char *port,
                         const char *inputDirectory) {
    const char *whitespace = " \t\r\n";
    char *commandName = command + strspn(command, whitespace);
    char *fileName = commandName + strcspn(commandName, whitespace);
    char filePath[PATH_MAX];
    size_t nameLength = 0;
    int pathLength = 0;

    // Separa solo la orden; el resto conserva espacios internos como parte del nombre.
    if (*fileName != '\0') {
        *fileName++ = '\0';
        fileName += strspn(fileName, whitespace);
    }
    nameLength = strlen(fileName);
    while (nameLength > 0 && strchr(whitespace, fileName[nameLength - 1]) != NULL) {
        fileName[--nameLength] = '\0';
    }
    if (*commandName == '\0') {
        return 1;
    }
    if (strcmp(commandName, "salir") == 0 && *fileName == '\0') {
        return 0;
    }
    if (strcmp(commandName, "enviar") != 0 || *fileName == '\0') {
        fprintf(stderr, "ERROR: expected enviar <file> or salir\n");
        return 1;
    }
    // Los nombres simples usan la carpeta de entrada; las rutas explícitas se respetan.
    pathLength = strchr(fileName, '/') == NULL ?
        snprintf(filePath, sizeof(filePath), "%s/%s", inputDirectory, fileName) :
        snprintf(filePath, sizeof(filePath), "%s", fileName);
    if (pathLength < 0 || (size_t)pathLength >= sizeof(filePath)) {
        fprintf(stderr, "ERROR: input file path is too long\n");
        return 1;
    }
    // Cada orden usa el transporte existente y deja la consola disponible si falla.
    if (runClient(address, port, filePath) == -1) {
        fprintf(stderr, "WARN: transfer failed; session remains active\n");
    }
    return 1;
}

int runInteractiveClient(const char *address, const char *port, const char *inputDirectory) {
    struct sockaddr_in endpoint;
    char command[COMMAND_BUFFER_SIZE];
    const int showPrompt = isatty(STDIN_FILENO);

    // Valida la configuración una vez; la conexión se establece solo al solicitar un envío.
    if (inputDirectory == NULL || *inputDirectory == '\0' ||
        parseEndpoint(address, port, &endpoint) == -1) {
        fprintf(stderr, "ERROR: expected loopback IPv4, port 1..65535 and input directory\n");
        return -1;
    }
    if (printf("INFO: commands: enviar <file>, salir\nINFO: input directory: %s\n",
               inputDirectory) < 0) {
        return -1;
    }
    // Conserva una sola consola para varias transferencias, con memoria de entrada acotada.
    while (1) {
        CommandReadResult readResult;

        if (showPrompt && (fputs("client> ", stdout) == EOF || fflush(stdout) == EOF)) {
            perror("ERROR: write client prompt");
            return -1;
        }
        readResult = readCommand(command, sizeof(command));
        if (readResult == COMMAND_READ_ERROR) {
            perror("ERROR: read client command");
            return -1;
        }
        if (readResult == COMMAND_END) {
            return 0;
        }
        if (readResult == COMMAND_INVALID) {
            fprintf(stderr, "ERROR: command too long or contains a null byte\n");
            continue;
        }
        // Solo una orden salir válida termina el bucle; los demás casos permiten reintentar.
        if (handleCommand(command, address, port, inputDirectory) == 0) {
            return 0;
        }
    }
}

/**
 * @brief Selecciona una transferencia individual o la consola interactiva por argumentos.
 * @param argumentCount Cantidad de argumentos, incluido el ejecutable.
 * @param arguments Opciones, dirección, puerto y archivo o directorio de entrada.
 * @return EXIT_SUCCESS al completar el modo elegido, EXIT_FAILURE ante errores fatales.
 */
int main(int argumentCount, char *arguments[]) {
    // El modo explícito conserva la interfaz anterior para envíos individuales y pruebas.
    if (argumentCount == 5 && strcmp(arguments[1], "--interactive") == 0) {
        return runInteractiveClient(arguments[2], arguments[3], arguments[4]) == 0 ?
            EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argumentCount != 4) {
        fprintf(stderr, "Usage: %s <loopback-ipv4> <port> <input-file>\n", arguments[0]);
        fprintf(stderr, "       %s --interactive <loopback-ipv4> <port> <input-directory>\n",
                arguments[0]);
        return EXIT_FAILURE;
    }
    return runClient(arguments[1], arguments[2], arguments[3]) == 0 ?
        EXIT_SUCCESS : EXIT_FAILURE;
}
