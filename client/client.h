#ifndef CLIENT_H
#define CLIENT_H

/**
 * @brief Envía un archivo regular y espera confirmación de almacenamiento del servidor.
 * @param address Dirección IPv4 numérica de loopback, no NULL.
 * @param port Puerto decimal entre 1 y 65535, no NULL.
 * @param filePath Ruta del archivo, no NULL; no debe modificarse durante el envío.
 * @return 0 si el servidor confirmó éxito, -1 si hubo un error, ya registrado.
 */
int runClient(const char *address, const char *port, const char *filePath);

#endif
