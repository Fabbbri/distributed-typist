#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>
#include <stdio.h>

#define ENCRYPTION_KEY_SIZE 32
#define ENCRYPTION_NONCE_SIZE 12
#define ENCRYPTION_TAG_SIZE 16
#define ENCRYPTION_HEADER_SIZE 16
#define ENCRYPTION_OVERHEAD (ENCRYPTION_HEADER_SIZE + ENCRYPTION_TAG_SIZE)
#define ENCRYPTION_MAGIC "DTG1"

/**
 * @brief Carga una clave binaria AES-256 desde un archivo regular privado de 32 bytes.
 * @param keyPath Ruta no vacía, no NULL; el archivo no debe tener permisos para grupo u otros.
 * @param key Buffer de ENCRYPTION_KEY_SIZE bytes, no NULL; se borra si falla la carga.
 * @return 0 si carga y cierra el archivo, -1 con errno si falla.
 * El llamador debe borrar la clave de memoria mediante OPENSSL_cleanse después de usarla.
 */
int loadEncryptionKey(const char *keyPath, unsigned char *key);

/**
 * @brief Cifra un archivo mediante AES-256-GCM incremental con nonce aleatorio y AAD.
 * @param input Origen regular al inicio, no NULL; no se cierra ni debe cambiar durante el cifrado.
 * @param output Destino vacío, no NULL; recibe DTG1, nonce, contenido cifrado y etiqueta GCM.
 * @param fileSize Tamaño esperado del origen, hasta MAX_FILE_SIZE.
 * @param key Clave de ENCRYPTION_KEY_SIZE bytes, no NULL; conserva su propiedad el llamador.
 * @return 0 si completa y vacía la salida, -1 con errno; la salida parcial debe descartarse.
 * La memoria utilizada es acotada; ningún texto claro se escribe en el archivo de salida.
 */
int encryptFile(FILE *input, FILE *output, uint64_t fileSize, const unsigned char *key);

#endif
