#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define REQUIRED_PROCESS_COUNT 3
#define EXPECTED_TOTAL 6

/**
 * @brief Interrumpe el diagnóstico completo si falla una operación MPI.
 * @param status Código devuelto por MPI después de inicializar el entorno.
 * @param operation Nombre de la operación para el mensaje técnico.
 */
static void checkMpi(int status, const char *operation) {
    if (status != MPI_SUCCESS) {
        // Aborta el grupo para que otros procesos no queden esperando una colectiva.
        fprintf(stderr, "ERROR: %s failed (MPI status %d)\n", operation, status);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief Comprueba que exactamente tres procesos participan en una suma colectiva.
 * @param argumentCount Cantidad de argumentos, entregada a MPI_Init.
 * @param arguments Argumentos del ejecutable, entregados a MPI_Init.
 * @return EXIT_SUCCESS si todos obtienen 6; EXIT_FAILURE ante errores.
 * Solo intercambia números sintéticos; no cifra ni procesa documentos del proyecto.
 */
int main(int argumentCount, char *arguments[]) {
    int rank = 0;
    int processCount = 0;
    int processorNameLength = 0;
    int contribution = 0;
    int total = 0;
    int result = EXIT_FAILURE;
    char processorName[MPI_MAX_PROCESSOR_NAME + 1] = {0};

    // Inicializa MPI antes de consultar la identidad y distribución de los procesos.
    if (MPI_Init(&argumentCount, &arguments) != MPI_SUCCESS) {
        fprintf(stderr, "ERROR: MPI_Init failed\n");
        return EXIT_FAILURE;
    }
    checkMpi(MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN),
             "MPI_Comm_set_errhandler");
    checkMpi(MPI_Comm_rank(MPI_COMM_WORLD, &rank), "MPI_Comm_rank");
    checkMpi(MPI_Comm_size(MPI_COMM_WORLD, &processCount), "MPI_Comm_size");

    // Todos observan el mismo tamaño y finalizan sin entrar en la suma si es incorrecto.
    if (processCount != REQUIRED_PROCESS_COUNT) {
        if (rank == 0) {
            fprintf(stderr, "ERROR: expected exactly %d MPI processes, got %d\n",
                    REQUIRED_PROCESS_COUNT, processCount);
        }
        goto cleanup;
    }
    checkMpi(MPI_Get_processor_name(processorName, &processorNameLength),
             "MPI_Get_processor_name");
    if (processorNameLength < 0 || processorNameLength > MPI_MAX_PROCESSOR_NAME) {
        fprintf(stderr, "ERROR: invalid MPI processor name length\n");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        exit(EXIT_FAILURE);
    }
    processorName[processorNameLength] = '\0';

    // Cada rango aporta un valor distinto; Allreduce devuelve la suma a los tres.
    contribution = rank + 1;
    checkMpi(MPI_Allreduce(&contribution, &total, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD),
             "MPI_Allreduce");
    printf("INFO: rank=%d host=%s pid=%ld contribution=%d total=%d\n",
           rank, processorName, (long)getpid(), contribution, total);
    if (total != EXPECTED_TOTAL) {
        fprintf(stderr, "ERROR: rank %d expected total %d, got %d\n",
                rank, EXPECTED_TOTAL, total);
        goto cleanup;
    }
    result = EXIT_SUCCESS;
    if (rank == 0) {
        printf("PASS: all three MPI processes participated (1 + 2 + 3 = 6)\n");
    }

cleanup:
    // La finalización colectiva permite cerrar limpiamente tanto éxito como error de uso.
    if (MPI_Finalize() != MPI_SUCCESS) {
        fprintf(stderr, "ERROR: MPI_Finalize failed\n");
        result = EXIT_FAILURE;
    }
    return result;
}
