#include "server.h"

#include <stdio.h>
#include <stdlib.h>

/**
 * @brief Inicia el servidor de almacenamiento cifrado en loopback.
 * @param argumentCount Cantidad de argumentos, incluido el ejecutable.
 * @param arguments Ejecutable, dirección, puerto y ruta cifrada.
 * @return EXIT_SUCCESS al detenerse, EXIT_FAILURE ante errores.
 */
int main(int argumentCount, char *arguments[]) {
    if (argumentCount != 4) {
        fprintf(stderr, "Usage: %s <loopback-ipv4> <port> <output-file>\n", arguments[0]);
        return EXIT_FAILURE;
    }
    return runServer(arguments[1], arguments[2], arguments[3]) == 0 ?
        EXIT_SUCCESS : EXIT_FAILURE;
}
