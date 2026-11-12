#ifndef CLIENT_H
#define CLIENT_H

/**
 * @brief Cifra un archivo regular y espera confirmación de almacenamiento del contenedor GCM.
 * @param address Dirección IPv4 numérica de loopback, no NULL.
 * @param port Puerto decimal entre 1 y 65535, no NULL.
 * @param filePath Ruta del archivo, no NULL; no debe modificarse durante el envío.
 * La clave se carga desde TYPIST_KEY_FILE, o keys/client.key si no está definida la variable.
 * @return 0 si el servidor confirmó éxito, -1 si hubo un error, ya registrado.
 */
int runClient(const char *address, const char *port, const char *filePath);

/**
 * @brief Atiende comandos enviar y salir desde stdin, reutilizando el envío de archivos.
 * @param address Dirección IPv4 numérica de loopback, no NULL.
 * @param port Puerto decimal entre 1 y 65535, no NULL.
 * @param inputDirectory Directorio para nombres sin ruta, no vacío ni NULL.
 * @return 0 al recibir salir o EOF; -1 ante configuración inválida o errores de la consola.
 * Los errores de comandos y transferencias se registran sin terminar la sesión.
 */
int runInteractiveClient(const char *address, const char *port, const char *inputDirectory);

#endif
