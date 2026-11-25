#include "armDevice.h"

#include <stdio.h>
#include <stdlib.h>

/**
 * @brief Comprueba la comunicación USB usando únicamente la biblioteca estática del proyecto.
 * @param argumentCount Cantidad de argumentos, incluido el ejecutable.
 * @param arguments Ejecutable y ruta opcional del dispositivo del módulo propio.
 * @return EXIT_SUCCESS con estado válido, EXIT_FAILURE ante errores; no mueve servos.
 */
int main(int argumentCount, char *arguments[]) {
    ArmStatus status = {0};
    int deviceFd = -1;
    int result = EXIT_FAILURE;

    if (argumentCount > 2) {
        fprintf(stderr, "Usage: %s [/dev/typistN]\n", arguments[0]);
        return EXIT_FAILURE;
    }
    deviceFd = armOpen(argumentCount == 2 ? arguments[1] : "/dev/typist0");
    if (deviceFd == -1) {
        perror("ERROR: open typist kernel device");
        return EXIT_FAILURE;
    }
    // Esta versión confirma comunicación, no calibración ni pulsaciones del teclado.
    if (armGetStatus(deviceFd, &status) == -1) {
        perror("ERROR: query Pico H status");
    } else {
        if (printf("INFO: Pico H protocol=%u state=UNCONFIGURED capabilities=STATUS_ONLY\n",
                   (unsigned int)status.protocolVersion) < 0) {
            perror("ERROR: write device status");
        } else {
            result = EXIT_SUCCESS;
        }
    }
    if (armClose(deviceFd) == -1) {
        perror("ERROR: close typist kernel device");
        result = EXIT_FAILURE;
    }
    return result;
}
