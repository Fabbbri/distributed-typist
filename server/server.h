#ifndef SERVER_H
#define SERVER_H

/** Procesador opcional: recibe la ruta cifrada publicada y devuelve 0 o -1. */
typedef int (*FileProcessor)(const char *encryptedPath, void *context);

/**
 * @brief Atiende transferencias locales secuenciales hasta SIGINT o SIGTERM.
 * @param address Dirección IPv4 numérica de loopback, no NULL.
 * @param port Puerto decimal entre 1 y 65535, no NULL.
 * @param outputPath Ruta de salida no vacía, no NULL; se reemplaza solo tras recepción completa.
 * @return 0 al detenerse normalmente, -1 ante un error del servicio, ya registrado.
 */
int runServer(const char *address, const char *port, const char *outputPath);

/**
 * @brief Atiende transferencias y confirma éxito solo después del procesamiento opcional.
 * @param address Dirección IPv4 de loopback, no NULL.
 * @param port Puerto decimal válido, no NULL.
 * @param outputPath Ruta cifrada no vacía; cada recepción completa reemplaza el archivo.
 * @param processor Función opcional, NULL para solo almacenar; no debe cerrar el servicio.
 * @param context Contexto prestado que se entrega a processor.
 * @return 0 al detenerse, -1 ante errores del servicio.
 * Un fallo del procesador conserva el cifrado recibido y se informa al cliente como error.
 */
int runServerWithProcessor(const char *address, const char *port, const char *outputPath,
                           FileProcessor processor, void *context);

#endif
