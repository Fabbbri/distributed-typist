#include "server.h"
#include "crypto.h"
#include "protocol.h"
#include "wordCount.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <mpi.h>
#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CLUSTER_PROCESS_COUNT 3
#define CLUSTER_MESSAGE_TAG 41
#define TASK_STOP 0
#define TASK_PROCESS 1
#define RESULT_ERROR 0
#define RESULT_SUCCESS 1

typedef struct {
    unsigned char key[ENCRYPTION_KEY_SIZE];
    const char *plainPath;
    const char *countsPath;
    int rank;
    char hostname[MPI_MAX_PROCESSOR_NAME + 1];
} ClusterContext;

/**
 * @brief Aborta el grupo ante fallos de transporte o recursos que impiden completar un mensaje.
 * @param operation Contexto técnico del error, no NULL.
 */
static void abortCluster(const char *operation) {
    fprintf(stderr, "ERROR: cluster transport/resource failure: %s\n", operation);
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    exit(EXIT_FAILURE);
}

/**
 * @brief Comprueba una llamada MPI y evita dejar otros rangos bloqueados tras un fallo.
 * @param status Código devuelto por MPI después de inicializar el entorno.
 * @param operation Nombre de la operación, no NULL.
 */
static void checkMpi(int status, const char *operation) {
    if (status != MPI_SUCCESS) {
        abortCluster(operation);
    }
}

/**
 * @brief Cierra un temporal y registra fallos sin ocultar el errno de la operación principal.
 * @param stream Temporal propio, o NULL si no llegó a abrirse.
 */
static void closeTemporary(FILE *stream) {
    int savedError = errno;

    if (stream != NULL && fclose(stream) == EOF) {
        perror("WARN: close cluster temporary file");
    }
    errno = savedError;
}

/**
 * @brief Obtiene la longitud de un archivo temporal con límite del protocolo existente.
 * @param stream Archivo abierto, no NULL; se vacía antes de consultar su tamaño.
 * @return Tamaño no negativo; aborta el grupo si el recurso es inválido.
 */
static uint64_t fileLength(FILE *stream) {
    struct stat fileInfo;

    // Consultar el descriptor evita depender de la posición actual del flujo.
    if (fflush(stream) == EOF || fstat(fileno(stream), &fileInfo) == -1 ||
        fileInfo.st_size < 0 || (uint64_t)fileInfo.st_size > MAX_TRANSFER_SIZE) {
        abortCluster("file length");
    }
    return (uint64_t)fileInfo.st_size;
}

/**
 * @brief Cifra un temporal completo como contenedor DTG1 independiente.
 * @param plain Temporal de origen, no NULL; se rebobina y no se cierra.
 * @param context Contexto con clave local, no NULL.
 * @return Nuevo temporal cifrado al inicio, propiedad del llamador, o NULL con errno.
 */
static FILE *encryptTemporary(FILE *plain, const ClusterContext *context) {
    uint64_t plainSize = fileLength(plain);
    FILE *encrypted = tmpfile();

    // Cada tarea y cada resultado adquieren un nonce y una etiqueta propios.
    if (encrypted == NULL || fseeko(plain, 0, SEEK_SET) == -1 ||
        encryptFile(plain, encrypted, plainSize, context->key) == -1 ||
        fseeko(encrypted, 0, SEEK_SET) == -1) {
        closeTemporary(encrypted);
        return NULL;
    }
    return encrypted;
}

/**
 * @brief Recupera un contenedor y entrega texto únicamente después de autenticarlo.
 * @param encrypted Contenedor al inicio, no NULL; no se cierra.
 * @param context Contexto con clave local, no NULL.
 * @return Temporal autenticado al inicio, propiedad del llamador, o NULL con errno.
 */
static FILE *decryptTemporary(FILE *encrypted, const ClusterContext *context) {
    FILE *plain = tmpfile();

    if (plain == NULL || decryptFile(encrypted, plain, context->key) == -1) {
        closeTemporary(plain);
        return NULL;
    }
    return plain;
}

/**
 * @brief Envía un contenedor cifrado por bloques sin compartir directorios ni claves por MPI.
 * @param encrypted Archivo DTG1 propio, no NULL; se rebobina y no se cierra.
 * @param destination Rango receptor.
 */
static void sendContainer(FILE *encrypted, int destination) {
    unsigned char block[TRANSFER_BLOCK_SIZE];
    uint64_t length = fileLength(encrypted);
    uint64_t remainingBytes = length;

    // Solo la longitud estructural viaja fuera del contenedor; el contenido ya está cifrado.
    if (length < ENCRYPTION_OVERHEAD || fseeko(encrypted, 0, SEEK_SET) == -1) {
        abortCluster("rewind encrypted message");
    }
    checkMpi(MPI_Send(&length, 1, MPI_UINT64_T, destination, CLUSTER_MESSAGE_TAG,
                      MPI_COMM_WORLD), "send message length");
    while (remainingBytes > 0) {
        size_t blockSize = remainingBytes < sizeof(block) ?
            (size_t)remainingBytes : sizeof(block);

        if (fread(block, 1, blockSize, encrypted) != blockSize) {
            abortCluster("read encrypted message");
        }
        checkMpi(MPI_Send(block, (int)blockSize, MPI_BYTE, destination, CLUSTER_MESSAGE_TAG,
                          MPI_COMM_WORLD), "send encrypted block");
        remainingBytes -= blockSize;
    }
}

/**
 * @brief Recibe exactamente el tamaño anunciado, usando un temporal privado del rango.
 * @param source Rango emisor conocido.
 * @return Contenedor al inicio, propiedad del llamador; aborta ante fallos del transporte.
 */
static FILE *receiveContainer(int source) {
    unsigned char block[TRANSFER_BLOCK_SIZE];
    uint64_t length = 0;
    uint64_t remainingBytes = 0;
    FILE *encrypted = tmpfile();
    MPI_Status status;
    int receivedCount = 0;

    if (encrypted == NULL) {
        abortCluster("create received container");
    }
    // Comprueba tanto la longitud total como cada mensaje antes de usar sus bytes.
    checkMpi(MPI_Recv(&length, 1, MPI_UINT64_T, source, CLUSTER_MESSAGE_TAG,
                      MPI_COMM_WORLD, &status), "receive message length");
    checkMpi(MPI_Get_count(&status, MPI_UINT64_T, &receivedCount), "length element count");
    if (receivedCount != 1 || length < ENCRYPTION_OVERHEAD || length > MAX_TRANSFER_SIZE) {
        abortCluster("invalid encrypted message length");
    }
    remainingBytes = length;
    while (remainingBytes > 0) {
        int blockSize = remainingBytes < sizeof(block) ?
            (int)remainingBytes : (int)sizeof(block);

        checkMpi(MPI_Recv(block, blockSize, MPI_BYTE, source, CLUSTER_MESSAGE_TAG,
                          MPI_COMM_WORLD, &status), "receive encrypted block");
        checkMpi(MPI_Get_count(&status, MPI_BYTE, &receivedCount), "block byte count");
        if (receivedCount != blockSize ||
            fwrite(block, 1, (size_t)blockSize, encrypted) != (size_t)blockSize) {
            abortCluster("incomplete encrypted block or temporary write failure");
        }
        remainingBytes -= (uint64_t)blockSize;
    }
    if (fflush(encrypted) == EOF || fseeko(encrypted, 0, SEEK_SET) == -1) {
        abortCluster("rewind received container");
    }
    return encrypted;
}

/**
 * @brief Divide texto autenticado cerca de tercios, siempre después de un separador.
 * @param input Texto al inicio, no NULL; no se cierra.
 * @param tasks Arreglo de tres temporales cifrados que adquiere el llamador; inicia con NULL.
 * @param context Contexto con clave, no NULL.
 * @return 0 al completar, -1 con errno; el llamador cierra también las salidas parciales.
 */
static int prepareTasks(FILE *input, FILE **tasks, const ClusterContext *context) {
    FILE *parts[CLUSTER_PROCESS_COUNT] = {NULL};
    uint64_t inputSize = fileLength(input);
    uint64_t position = 0;
    int partIndex = 0;
    int character = EOF;
    int result = -1;

    // El marcador de tarea también queda protegido dentro de GCM.
    for (int index = 0; index < CLUSTER_PROCESS_COUNT; index++) {
        parts[index] = tmpfile();
        if (parts[index] == NULL || fputc(TASK_PROCESS, parts[index]) == EOF) {
            goto cleanup;
        }
    }
    while ((character = fgetc(input)) != EOF) {
        if (fputc(character, parts[partIndex]) == EOF) {
            goto cleanup;
        }
        position++;
        // Un token UTF-8 tampoco se divide: todos sus bytes altos se consideran de palabra.
        if (partIndex < CLUSTER_PROCESS_COUNT - 1 && !isWordByte(character) &&
            position >= inputSize * (uint64_t)(partIndex + 1) / CLUSTER_PROCESS_COUNT) {
            partIndex++;
        }
    }
    if (ferror(input)) {
        goto cleanup;
    }
    for (int index = 0; index < CLUSTER_PROCESS_COUNT; index++) {
        tasks[index] = encryptTemporary(parts[index], context);
        if (tasks[index] == NULL) {
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    for (int index = 0; index < CLUSTER_PROCESS_COUNT; index++) {
        closeTemporary(parts[index]);
    }
    return result;
}

/**
 * @brief Descifra una tarea, cuenta su fragmento y cifra todos los conteos locales.
 * @param encrypted Tarea al inicio, no NULL; conserva su propiedad el llamador.
 * @param context Identidad y clave del rango, no NULL.
 * @param stop Recibe 1 si es una orden autenticada de parada, 0 en otro caso.
 * @return Resultado cifrado propio o NULL si se solicita parar; aborta sin recursos de respuesta.
 */
static FILE *processTask(FILE *encrypted, const ClusterContext *context, int *stop) {
    WordCounts counts = {0};
    FILE *plain = decryptTemporary(encrypted, context);
    FILE *response = tmpfile();
    FILE *result = NULL;
    int taskKind = plain != NULL ? fgetc(plain) : EOF;
    int success = 0;

    *stop = 0;
    // Una parada solo es válida si su contenedor autentica y no contiene bytes adicionales.
    if (taskKind == TASK_STOP && fgetc(plain) == EOF && !ferror(plain)) {
        *stop = 1;
        goto cleanup;
    }
    if (response == NULL) {
        abortCluster("create worker response");
    }
    if (taskKind == TASK_PROCESS && countWords(plain, &counts) == 0) {
        success = 1;
        printf("INFO: worker rank=%d host=%s pid=%ld bytes=%" PRIu64
               " words=%" PRIu64 " unique=%zu\n", context->rank, context->hostname,
               (long)getpid(), fileLength(plain) - 1, counts.totalCount, counts.uniqueCount);
        if (fflush(stdout) == EOF) {
            abortCluster("flush worker log");
        }
    } else {
        if (taskKind != TASK_PROCESS) {
            errno = EBADMSG;
        }
        perror("ERROR: authenticate or count worker task");
    }
    // Incluso los errores de procesamiento regresan cifrados y permiten atender otro archivo.
    if (fputc(success ? RESULT_SUCCESS : RESULT_ERROR, response) == EOF ||
        (success && writeWordCounts(response, &counts) == -1)) {
        abortCluster("serialize worker response");
    }
    result = encryptTemporary(response, context);
    if (result == NULL) {
        abortCluster("encrypt worker response");
    }

cleanup:
    clearWordCounts(&counts);
    closeTemporary(plain);
    closeTemporary(response);
    return result;
}

/**
 * @brief Autentica un resultado antes de sumar el diccionario completo al agregado.
 * @param response Resultado cifrado al inicio, no NULL; no se cierra.
 * @param counts Tabla agregada inicializada, no NULL.
 * @param context Contexto con clave, no NULL.
 * @return 0 con un resultado válido, -1 con errno si falla.
 */
static int mergeResponse(FILE *response, WordCounts *counts, const ClusterContext *context) {
    FILE *plain = decryptTemporary(response, context);
    int result = -1;

    if (plain != NULL && fgetc(plain) == RESULT_SUCCESS) {
        result = mergeWordCounts(plain, counts);
    } else {
        errno = EBADMSG;
    }
    closeTemporary(plain);
    return result;
}

/**
 * @brief Publica una copia completa mediante temporal vecino y rename atómico.
 * @param input Temporal autenticado o resultado agregado, no NULL; se rebobina.
 * @param outputPath Ruta no vacía, no NULL; se reemplaza al completar la copia.
 * @return 0 si publica, -1 con errno; un fallo previo a rename conserva la salida anterior.
 */
static int publishFile(FILE *input, const char *outputPath) {
    char stagingPath[PATH_MAX];
    unsigned char block[TRANSFER_BLOCK_SIZE];
    int pathLength = snprintf(stagingPath, sizeof(stagingPath), "%s.part.XXXXXX", outputPath);
    int outputFd = -1;
    FILE *output = NULL;
    size_t blockSize = 0;
    int result = -1;
    int savedError = 0;

    if (pathLength < 0 || (size_t)pathLength >= sizeof(stagingPath)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (fseeko(input, 0, SEEK_SET) == -1 || (outputFd = mkstemp(stagingPath)) == -1) {
        return -1;
    }
    output = fdopen(outputFd, "wb");
    if (output == NULL) {
        savedError = errno;
        if (close(outputFd) == -1) {
            perror("WARN: close cluster output");
        }
        errno = savedError;
        goto cleanup;
    }
    // El archivo final permanece intacto hasta terminar la copia y comprobar fclose.
    while ((blockSize = fread(block, 1, sizeof(block), input)) > 0) {
        if (fwrite(block, 1, blockSize, output) != blockSize) {
            goto cleanup;
        }
    }
    if (ferror(input)) {
        goto cleanup;
    }
    if (fclose(output) == EOF) {
        output = NULL;
        goto cleanup;
    }
    output = NULL;
    result = rename(stagingPath, outputPath);

cleanup:
    savedError = errno;
    closeTemporary(output);
    if (result == -1 && unlink(stagingPath) == -1) {
        perror("WARN: remove cluster staging file");
    }
    OPENSSL_cleanse(block, sizeof(block));
    errno = savedError;
    return result;
}

/**
 * @brief Procesa un archivo recibido con los tres rangos y publica texto y conteos globales.
 * @param encryptedPath Ruta cifrada publicada por el servidor TCP, no NULL.
 * @param opaqueContext ClusterContext prestado del rango 0.
 * @return 0 tras completar todo el procesamiento, -1 con errno ante un fallo de este archivo.
 */
static int processReceivedFile(const char *encryptedPath, void *opaqueContext) {
    ClusterContext *context = opaqueContext;
    WordCounts counts = {0};
    FILE *input = fopen(encryptedPath, "rb");
    FILE *plain = NULL;
    FILE *tasks[CLUSTER_PROCESS_COUNT] = {NULL};
    FILE *summary = NULL;
    int failed = 0;
    int result = -1;

    // DTG1 autentica el documento completo: valida antes de repartir contenedores independientes.
    if (input == NULL || (plain = decryptTemporary(input, context)) == NULL ||
        prepareTasks(plain, tasks, context) == -1) {
        goto cleanup;
    }
    for (int rank = 1; rank < CLUSTER_PROCESS_COUNT; rank++) {
        sendContainer(tasks[rank], rank);
    }
    // El coordinador también descifra y cuenta, por lo que hay tres procesadores efectivos.
    for (int rank = 0; rank < CLUSTER_PROCESS_COUNT; rank++) {
        int stop = 0;
        FILE *response = rank == 0 ? processTask(tasks[0], context, &stop) :
            receiveContainer(rank);

        if (response == NULL || mergeResponse(response, &counts, context) == -1) {
            failed = 1;
        }
        closeTemporary(response);
        // Siempre drena las dos respuestas remotas, incluso si falló un resultado anterior.
    }
    if (failed) {
        errno = EBADMSG;
        goto cleanup;
    }
    summary = tmpfile();
    if (summary == NULL || writeWordCounts(summary, &counts) == -1 ||
        publishFile(summary, context->countsPath) == -1 ||
        publishFile(plain, context->plainPath) == -1) {
        goto cleanup;
    }
    const WordEntry *winner = mostFrequentWord(&counts);
    if (winner != NULL) {
        printf("INFO: cluster result word=%s count=%" PRIu64 " totalWords=%" PRIu64 "\n",
               winner->word, winner->count, counts.totalCount);
    } else {
        printf("INFO: cluster result: no words\n");
    }
    if (fflush(stdout) == EOF) {
        goto cleanup;
    }
    result = 0;

cleanup:
    clearWordCounts(&counts);
    closeTemporary(input);
    closeTemporary(plain);
    closeTemporary(summary);
    for (int index = 0; index < CLUSTER_PROCESS_COUNT; index++) {
        closeTemporary(tasks[index]);
    }
    return result;
}

/**
 * @brief Atiende tareas cifradas hasta recibir una parada autenticada del coordinador.
 * @param context Identidad y clave local del trabajador, no NULL.
 */
static void runWorker(const ClusterContext *context) {
    // Cada mensaje se descifra localmente; nunca se abre un archivo del equipo coordinador.
    for (;;) {
        FILE *task = receiveContainer(0);
        int stop = 0;
        FILE *response = processTask(task, context, &stop);

        closeTemporary(task);
        if (stop) {
            return;
        }
        sendContainer(response, 0);
        closeTemporary(response);
    }
}

/**
 * @brief Construye una orden cifrada de parada, también útil para comprobar las claves iniciales.
 * @param context Contexto con clave del coordinador, no NULL.
 * @return Temporal cifrado propio; aborta si no puede crear una orden completa.
 */
static FILE *createStopCommand(const ClusterContext *context) {
    FILE *plain = tmpfile();
    FILE *encrypted = NULL;

    if (plain == NULL || fputc(TASK_STOP, plain) == EOF ||
        (encrypted = encryptTemporary(plain, context)) == NULL) {
        abortCluster("prepare stop command");
    }
    closeTemporary(plain);
    return encrypted;
}

/**
 * @brief Comprueba que todos puedan autenticar una prueba sin transmitir la clave secreta.
 * @param context Identidad y clave local, no NULL.
 * @return 1 si los tres comparten la clave, 0 si alguna autenticación falla.
 */
static int verifyClusterKeys(const ClusterContext *context) {
    FILE *encrypted = NULL;
    FILE *plain = NULL;
    int ready = 1;
    int allReady = 0;

    // La prueba posee nonce nuevo; solo quienes conocen la misma clave pueden autenticarla.
    if (context->rank == 0) {
        encrypted = createStopCommand(context);
        for (int rank = 1; rank < CLUSTER_PROCESS_COUNT; rank++) {
            sendContainer(encrypted, rank);
        }
    } else {
        encrypted = receiveContainer(0);
        plain = decryptTemporary(encrypted, context);
        ready = plain != NULL && fgetc(plain) == TASK_STOP &&
            fgetc(plain) == EOF && !ferror(plain);
    }
    checkMpi(MPI_Allreduce(&ready, &allReady, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD),
             "key authentication");
    if (!ready) {
        fprintf(stderr, "ERROR: rank %d cannot authenticate the coordinator key\n", context->rank);
    }
    closeTemporary(plain);
    closeTemporary(encrypted);
    return allReady;
}

/**
 * @brief Detiene a los dos trabajadores con una orden cifrada independiente de los documentos.
 * @param context Contexto con clave del coordinador, no NULL.
 */
static void stopWorkers(const ClusterContext *context) {
    FILE *encrypted = createStopCommand(context);

    // Todos los receptores autentican la orden antes de salir de su bucle.
    for (int rank = 1; rank < CLUSTER_PROCESS_COUNT; rank++) {
        sendContainer(encrypted, rank);
    }
    closeTemporary(encrypted);
}

/**
 * @brief Inicia exactamente tres rangos, uno servidor/trabajador y dos trabajadores remotos.
 * @param argumentCount Cantidad de argumentos, incluido el ejecutable.
 * @param arguments Ejecutable, IPv4 local, puerto, cifrado, descifrado, conteos y clave local.
 * @return EXIT_SUCCESS al detenerse, EXIT_FAILURE ante errores de configuración o servicio.
 */
int main(int argumentCount, char *arguments[]) {
    ClusterContext context = {0};
    int processCount = 0;
    int hostnameLength = 0;
    int ready = 0;
    int allReady = 0;
    int result = EXIT_FAILURE;

    if (MPI_Init(&argumentCount, &arguments) != MPI_SUCCESS) {
        fprintf(stderr, "ERROR: MPI_Init failed\n");
        return EXIT_FAILURE;
    }
    checkMpi(MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN), "error handler");
    checkMpi(MPI_Comm_rank(MPI_COMM_WORLD, &context.rank), "rank");
    checkMpi(MPI_Comm_size(MPI_COMM_WORLD, &processCount), "size");
    // La validación colectiva también cubre argumentos distintos en un arranque MPMD.
    ready = argumentCount == 7 && processCount == CLUSTER_PROCESS_COUNT;
    checkMpi(MPI_Allreduce(&ready, &allReady, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD),
             "argument validation");
    if (!allReady) {
        if (context.rank == 0) {
            fprintf(stderr, "Usage: mpirun -np 3 %s <loopback-ipv4> <port> "
                    "<encrypted-file> <plain-file> <counts-file> <key-file>\n", arguments[0]);
        }
        goto cleanup;
    }
    context.plainPath = arguments[4];
    context.countsPath = arguments[5];
    // Cada equipo recibe su clave mediante provisión local, nunca mediante un broadcast en claro.
    ready = *arguments[3] != '\0' && *arguments[4] != '\0' && *arguments[5] != '\0' &&
        strcmp(arguments[3], arguments[4]) != 0 && strcmp(arguments[3], arguments[5]) != 0 &&
        strcmp(arguments[4], arguments[5]) != 0 &&
        loadEncryptionKey(arguments[6], context.key) == 0;
    if (!ready) {
        fprintf(stderr, "ERROR: rank %d requires distinct output paths and a private 32-byte key\n",
                context.rank);
    }
    checkMpi(MPI_Allreduce(&ready, &allReady, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD), "startup");
    if (!allReady || !verifyClusterKeys(&context)) {
        goto cleanup;
    }
    checkMpi(MPI_Get_processor_name(context.hostname, &hostnameLength), "processor name");
    if (hostnameLength < 0 || hostnameLength > MPI_MAX_PROCESSOR_NAME) {
        abortCluster("invalid processor name");
    }
    context.hostname[hostnameLength] = '\0';
    if (context.rank == 0) {
        printf("INFO: cluster coordinator pid=%ld processes=%d\n",
               (long)getpid(), processCount);
        result = runServerWithProcessor(arguments[1], arguments[2], arguments[3],
                                         processReceivedFile, &context) == 0 ?
            EXIT_SUCCESS : EXIT_FAILURE;
        stopWorkers(&context);
    } else {
        runWorker(&context);
        result = EXIT_SUCCESS;
    }

cleanup:
    // Todos los rangos liberan la clave y finalizan después de cerrar mensajes y temporales.
    OPENSSL_cleanse(context.key, sizeof(context.key));
    if (MPI_Finalize() != MPI_SUCCESS) {
        result = EXIT_FAILURE;
    }
    return result;
}
