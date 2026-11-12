#ifndef SERVER_H
#define SERVER_H

/**
 * @brief Atiende transferencias locales secuenciales hasta SIGINT o SIGTERM.
 * @param address Dirección IPv4 numérica de loopback, no NULL.
 * @param port Puerto decimal entre 1 y 65535, no NULL.
 * @param outputPath Ruta de salida no vacía, no NULL; se reemplaza solo tras recepción completa.
 * @return 0 al detenerse normalmente, -1 ante un error del servicio, ya registrado.
 */
int runServer(const char *address, const char *port, const char *outputPath);

#endif
