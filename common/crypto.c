#include "crypto.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int loadEncryptionKey(const char *keyPath, unsigned char *key) {
    struct stat keyInfo;
    FILE *keyFile = NULL;
    int keyFd = -1;
    int result = -1;
    int savedError = EINVAL;

    if (key == NULL) {
        errno = EINVAL;
        return -1;
    }
    OPENSSL_cleanse(key, ENCRYPTION_KEY_SIZE);
    if (keyPath == NULL || *keyPath == '\0') {
        errno = EINVAL;
        return -1;
    }
    // Abre sin bloquear ante una FIFO y comprueba el archivo real antes de leer secretos.
    keyFd = open(keyPath, O_RDONLY | O_NONBLOCK);
    if (keyFd == -1) {
        return -1;
    }
    if (fstat(keyFd, &keyInfo) == -1) {
        savedError = errno;
        goto cleanup;
    }
    if (!S_ISREG(keyInfo.st_mode) || keyInfo.st_size != ENCRYPTION_KEY_SIZE) {
        goto cleanup;
    }
    if ((keyInfo.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        savedError = EACCES;
        goto cleanup;
    }
    keyFile = fdopen(keyFd, "rb");
    if (keyFile == NULL) {
        savedError = errno;
        goto cleanup;
    }
    keyFd = -1;
    // Exige exactamente 256 bits; no acepta contraseñas, hexadecimal ni bytes sobrantes.
    errno = 0;
    if (fread(key, 1, ENCRYPTION_KEY_SIZE, keyFile) != ENCRYPTION_KEY_SIZE ||
        fgetc(keyFile) != EOF || ferror(keyFile)) {
        savedError = errno != 0 ? errno : EINVAL;
        goto cleanup;
    }
    result = 0;

cleanup:
    // Conserva el error original y elimina todo material de clave si la operación falla.
    if (keyFile != NULL && fclose(keyFile) == EOF && result == 0) {
        savedError = errno;
        result = -1;
    }
    if (keyFd != -1 && close(keyFd) == -1 && result == 0) {
        savedError = errno;
        result = -1;
    }
    if (result == -1) {
        OPENSSL_cleanse(key, ENCRYPTION_KEY_SIZE);
        errno = savedError;
    }
    return result;
}

/**
 * @brief Cifra el contenido esperado y rechaza cambios de longitud del origen.
 * @param context Contexto GCM inicializado con clave, nonce y AAD, no NULL.
 * @param input Archivo de origen abierto, no NULL.
 * @param output Archivo de destino abierto, no NULL.
 * @param fileSize Cantidad de bytes que deben procesarse.
 * @return 0 si completa el contenido, -1 con errno si falla la lectura, el cifrado o la escritura.
 */
static int encryptContent(EVP_CIPHER_CTX *context, FILE *input, FILE *output,
                          uint64_t fileSize) {
    unsigned char plainBlock[TRANSFER_BLOCK_SIZE];
    unsigned char cipherBlock[TRANSFER_BLOCK_SIZE + EVP_MAX_BLOCK_LENGTH];
    uint64_t remainingBytes = fileSize;
    int result = -1;

    // Mantiene un único contexto GCM entre bloques; cada byte pertenece al mismo mensaje.
    while (remainingBytes > 0) {
        size_t blockSize = remainingBytes < sizeof(plainBlock) ?
            (size_t)remainingBytes : sizeof(plainBlock);
        int cipherLength = 0;

        errno = 0;
        if (fread(plainBlock, 1, blockSize, input) != blockSize) {
            errno = errno != 0 ? errno : EIO;
            goto cleanup;
        }
        if (EVP_EncryptUpdate(context, cipherBlock, &cipherLength,
                              plainBlock, (int)blockSize) != 1 ||
            cipherLength != (int)blockSize) {
            errno = EIO;
            goto cleanup;
        }
        if (fwrite(cipherBlock, 1, (size_t)cipherLength, output) != (size_t)cipherLength) {
            errno = errno != 0 ? errno : EIO;
            goto cleanup;
        }
        remainingBytes -= blockSize;
    }
    // No finaliza ni emite la etiqueta si el origen creció respecto al tamaño anunciado.
    errno = 0;
    if (fgetc(input) != EOF || ferror(input)) {
        errno = errno != 0 ? errno : EIO;
        goto cleanup;
    }
    result = 0;

cleanup:
    OPENSSL_cleanse(plainBlock, sizeof(plainBlock));
    return result;
}

int encryptFile(FILE *input, FILE *output, uint64_t fileSize, const unsigned char *key) {
    EVP_CIPHER_CTX *context = NULL;
    unsigned char header[ENCRYPTION_HEADER_SIZE] = ENCRYPTION_MAGIC;
    unsigned char tag[ENCRYPTION_TAG_SIZE];
    unsigned char finalBlock[EVP_MAX_BLOCK_LENGTH];
    int finalLength = 0;
    int aadLength = 0;
    int result = -1;
    int savedError = 0;

    if (input == NULL || output == NULL || input == output || key == NULL ||
        fileSize > MAX_FILE_SIZE) {
        errno = EINVAL;
        return -1;
    }
    // Un nonce nuevo por archivo evita reutilizar el flujo GCM con la misma clave.
    if (RAND_bytes(header + sizeof(ENCRYPTION_MAGIC) - 1, ENCRYPTION_NONCE_SIZE) != 1) {
        errno = EIO;
        return -1;
    }
    context = EVP_CIPHER_CTX_new();
    if (context == NULL) {
        errno = ENOMEM;
        return -1;
    }
    // Autentica también el identificador de formato y el nonce mediante AAD.
    if (EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), NULL, key,
                           header + sizeof(ENCRYPTION_MAGIC) - 1) != 1 ||
        EVP_EncryptUpdate(context, NULL, &aadLength, header, sizeof(header)) != 1) {
        errno = EIO;
        goto cleanup;
    }
    errno = 0;
    if (fwrite(header, 1, sizeof(header), output) != sizeof(header)) {
        errno = errno != 0 ? errno : EIO;
        goto cleanup;
    }
    if (encryptContent(context, input, output, fileSize) == -1) {
        goto cleanup;
    }
    // GCM no añade relleno; la etiqueta de 128 bits se almacena después del contenido.
    if (EVP_EncryptFinal_ex(context, finalBlock, &finalLength) != 1 || finalLength != 0 ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, sizeof(tag), tag) != 1) {
        errno = EIO;
        goto cleanup;
    }
    errno = 0;
    if (fwrite(tag, 1, sizeof(tag), output) != sizeof(tag) || fflush(output) == EOF) {
        errno = errno != 0 ? errno : EIO;
        goto cleanup;
    }
    result = 0;

cleanup:
    savedError = errno;
    EVP_CIPHER_CTX_free(context);
    errno = savedError;
    return result;
}
