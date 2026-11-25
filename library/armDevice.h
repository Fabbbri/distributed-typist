#ifndef ARM_DEVICE_H
#define ARM_DEVICE_H

typedef struct {
    unsigned char protocolVersion;
    unsigned char state;
    unsigned char capabilities;
} ArmStatus;

/**
 * @brief Abre el dispositivo del driver propio de Linux, sin utilizar puertos seriales.
 * @param devicePath Ruta no vacía, normalmente /dev/typist0; no NULL.
 * @return Descriptor propio del llamador que debe cerrar con armClose, o -1 con errno.
 */
int armOpen(const char *devicePath);

/**
 * @brief Consulta y valida el estado real publicado por el firmware a través del módulo.
 * @param deviceFd Descriptor abierto por armOpen; no se cierra.
 * @param status Destino no NULL; permanece intacto ante cualquier error.
 * @return 0 si consulta, -1 con errno del driver o EPROTO para una respuesta inválida.
 */
int armGetStatus(int deviceFd, ArmStatus *status);

/**
 * @brief Cierra un descriptor del driver; no reintenta close después de una interrupción.
 * @param deviceFd Descriptor propio válido, que deja de utilizar el llamador.
 * @return 0 si cierra, -1 con errno si falla.
 */
int armClose(int deviceFd);

#endif
