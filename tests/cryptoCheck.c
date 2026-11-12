#include "crypto.h"
#include "protocol.h"

#include <errno.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/**
 * @brief Descifra el contenido a un temporal y valida la etiqueta antes de permitir su uso.
 * @param encrypted Archivo de contenedor al inicio, no NULL.
 * @param recovered Temporal vacío de pruebas, no NULL; se descarta si falla la autenticación.
 * @param key Clave de 32 bytes, no NULL.
 * @return 0 si autentica todo el contenedor, -1 con errno si falla.
 */
static int recoverAuthenticatedFile(FILE *encrypted, FILE *recovered, const unsigned char *key) {
    EVP_CIPHER_CTX *context = NULL;
    struct stat encryptedInfo;
    unsigned char header[ENCRYPTION_HEADER_SIZE];
    unsigned char tag[ENCRYPTION_TAG_SIZE];
    unsigned char cipherBlock[TRANSFER_BLOCK_SIZE];
    unsigned char plainBlock[TRANSFER_BLOCK_SIZE + EVP_MAX_BLOCK_LENGTH];
    uint64_t remainingBytes = 0;
    int plainLength = 0;
    int result = -1;

    // Obtiene la longitud del cuerpo sin confiar en cadenas ni cargar el contenedor completo.
    if (fstat(fileno(encrypted), &encryptedInfo) == -1) {
        return -1;
    }
    if (!S_ISREG(encryptedInfo.st_mode) || encryptedInfo.st_size < ENCRYPTION_OVERHEAD ||
        (uint64_t)encryptedInfo.st_size > MAX_TRANSFER_SIZE ||
        fread(header, 1, sizeof(header), encrypted) != sizeof(header) ||
        memcmp(header, ENCRYPTION_MAGIC, sizeof(ENCRYPTION_MAGIC) - 1) != 0) {
        errno = EINVAL;
        return -1;
    }
    remainingBytes = (uint64_t)encryptedInfo.st_size - ENCRYPTION_OVERHEAD;
    context = EVP_CIPHER_CTX_new();
    if (context == NULL) {
        errno = ENOMEM;
        return -1;
    }
    // Reproduce el AAD especificado por el formato, sin reutilizar la implementación de cifrado.
    if (EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), NULL, key,
                           header + sizeof(ENCRYPTION_MAGIC) - 1) != 1 ||
        EVP_DecryptUpdate(context, NULL, &plainLength, header, sizeof(header)) != 1) {
        errno = EIO;
        goto cleanup;
    }
    while (remainingBytes > 0) {
        size_t blockSize = remainingBytes < sizeof(cipherBlock) ?
            (size_t)remainingBytes : sizeof(cipherBlock);

        if (fread(cipherBlock, 1, blockSize, encrypted) != blockSize ||
            EVP_DecryptUpdate(context, plainBlock, &plainLength,
                              cipherBlock, (int)blockSize) != 1 ||
            plainLength != (int)blockSize ||
            fwrite(plainBlock, 1, (size_t)plainLength, recovered) != (size_t)plainLength) {
            errno = EIO;
            goto cleanup;
        }
        remainingBytes -= blockSize;
    }
    // Hasta validar el tag, el texto recuperado permanece en un temporal no publicado.
    if (fread(tag, 1, sizeof(tag), encrypted) != sizeof(tag) ||
        fgetc(encrypted) != EOF || ferror(encrypted) ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, sizeof(tag), tag) != 1 ||
        EVP_DecryptFinal_ex(context, plainBlock, &plainLength) != 1 || plainLength != 0) {
        errno = EBADMSG;
        goto cleanup;
    }
    if (fflush(recovered) == EOF || fseeko(recovered, 0, SEEK_SET) == -1) {
        goto cleanup;
    }
    result = 0;

cleanup:
    OPENSSL_cleanse(plainBlock, sizeof(plainBlock));
    EVP_CIPHER_CTX_free(context);
    return result;
}

/**
 * @brief Copia un resultado ya autenticado para compararlo con herramientas de Linux.
 * @param recovered Temporal autenticado al inicio, no NULL.
 * @param outputPath Ruta de salida no NULL, utilizada solo por la suite de pruebas.
 * @return 0 si completa y cierra la salida, -1 si falla.
 */
static int saveRecoveredFile(FILE *recovered, const char *outputPath) {
    unsigned char plainBlock[TRANSFER_BLOCK_SIZE];
    FILE *output = fopen(outputPath, "wb");
    size_t blockSize = 0;
    int result = 0;

    if (output == NULL) {
        return -1;
    }
    // El llamador solo llega aquí tras validar la autenticación completa del archivo.
    while ((blockSize = fread(plainBlock, 1, sizeof(plainBlock), recovered)) > 0) {
        if (fwrite(plainBlock, 1, blockSize, output) != blockSize) {
            result = -1;
            break;
        }
    }
    if (ferror(recovered)) {
        result = -1;
    }
    if (fclose(output) == EOF) {
        result = -1;
    }
    OPENSSL_cleanse(plainBlock, sizeof(plainBlock));
    return result;
}

/**
 * @brief Verifica un contenedor GCM y exporta texto claro exclusivamente para pruebas.
 * @param argumentCount Cantidad de argumentos, incluido el ejecutable.
 * @param arguments Ejecutable, contenedor cifrado, archivo de clave y salida de pruebas.
 * @return EXIT_SUCCESS con autenticación válida, EXIT_FAILURE ante cualquier error.
 */
int main(int argumentCount, char *arguments[]) {
    unsigned char key[ENCRYPTION_KEY_SIZE] = {0};
    FILE *encrypted = NULL;
    FILE *recovered = NULL;
    int result = EXIT_FAILURE;

    if (argumentCount != 4) {
        fprintf(stderr, "Usage: %s <encrypted-file> <key-file> <test-output>\n", arguments[0]);
        return EXIT_FAILURE;
    }
    // El verificador es independiente del servidor: este no descifra ni necesita la clave.
    if (loadEncryptionKey(arguments[2], key) == -1) {
        perror("ERROR: test key");
        goto cleanup;
    }
    encrypted = fopen(arguments[1], "rb");
    recovered = tmpfile();
    if (encrypted == NULL || recovered == NULL ||
        recoverAuthenticatedFile(encrypted, recovered, key) == -1 ||
        saveRecoveredFile(recovered, arguments[3]) == -1) {
        perror("ERROR: authenticated test recovery");
        goto cleanup;
    }
    result = EXIT_SUCCESS;

cleanup:
    OPENSSL_cleanse(key, sizeof(key));
    if (encrypted != NULL && fclose(encrypted) == EOF) {
        result = EXIT_FAILURE;
    }
    if (recovered != NULL && fclose(recovered) == EOF) {
        result = EXIT_FAILURE;
    }
    return result;
}
