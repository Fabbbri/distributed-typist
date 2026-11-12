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
 * @brief Lee y cifra cada carácter de C por separado, validando la longitud del origen.
 * @param context Contexto GCM inicializado con clave, nonce y AAD, no NULL.
 * @param input Archivo de origen abierto, no NULL.
 * @param output Archivo de destino abierto, no NULL.
 * @param fileSize Cantidad de bytes que deben procesarse.
 * @return 0 si completa el contenido, -1 con errno si falla la lectura, el cifrado o la escritura.
 */
static int encryptContent(EVP_CIPHER_CTX *context, FILE *input, FILE *output,
                          uint64_t fileSize) {
    unsigned char plainCharacter = 0;
    unsigned char cipherCharacter[1 + EVP_MAX_BLOCK_LENGTH];
    uint64_t remainingBytes = fileSize;
    int nextCharacter = EOF;
    int result = -1;

    // Cada fgetc y cada actualización GCM procesan un char de C, conservando el contexto.
    while (remainingBytes > 0) {
        int cipherLength = 0;

        // fgetc devuelve int: comprueba EOF antes de convertir para no perder el byte 0xFF.
        errno = 0;
        nextCharacter = fgetc(input);
        if (nextCharacter == EOF) {
            errno = errno != 0 ? errno : EIO;
            goto cleanup;
        }
        plainCharacter = (unsigned char)nextCharacter;
        if (EVP_EncryptUpdate(context, cipherCharacter, &cipherLength,
                              &plainCharacter, sizeof(plainCharacter)) != 1 ||
            cipherLength != sizeof(plainCharacter)) {
            errno = EIO;
            goto cleanup;
        }
        // Escribe el carácter cifrado; stdio conserva su buffering de E/S habitual.
        if (fputc(cipherCharacter[0], output) == EOF) {
            errno = errno != 0 ? errno : EIO;
            goto cleanup;
        }
        remainingBytes--;
    }
    // No finaliza ni emite la etiqueta si el origen creció respecto al tamaño anunciado.
    errno = 0;
    if (fgetc(input) != EOF || ferror(input)) {
        errno = errno != 0 ? errno : EIO;
        goto cleanup;
    }
    result = 0;

cleanup:
    OPENSSL_cleanse(&plainCharacter, sizeof(plainCharacter));
    OPENSSL_cleanse(&nextCharacter, sizeof(nextCharacter));
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

int decryptFile(FILE *input, FILE *output, const unsigned char *key) {
    EVP_CIPHER_CTX *context = NULL;
    struct stat inputInfo;
    unsigned char header[ENCRYPTION_HEADER_SIZE];
    unsigned char tag[ENCRYPTION_TAG_SIZE];
    unsigned char cipherBlock[TRANSFER_BLOCK_SIZE];
    unsigned char plainBlock[TRANSFER_BLOCK_SIZE + EVP_MAX_BLOCK_LENGTH];
    uint64_t remainingBytes = 0;
    int plainLength = 0;
    int result = -1;
    int savedError = 0;

    // Valida el contenedor completo antes de reservar el contexto criptográfico.
    if (input == NULL || output == NULL || input == output || key == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (fstat(fileno(input), &inputInfo) == -1) {
        return -1;
    }
    if (!S_ISREG(inputInfo.st_mode) || inputInfo.st_size < ENCRYPTION_OVERHEAD ||
        (uint64_t)inputInfo.st_size > MAX_TRANSFER_SIZE ||
        fread(header, 1, sizeof(header), input) != sizeof(header) ||
        memcmp(header, ENCRYPTION_MAGIC, sizeof(ENCRYPTION_MAGIC) - 1) != 0) {
        errno = EBADMSG;
        return -1;
    }
    remainingBytes = (uint64_t)inputInfo.st_size - ENCRYPTION_OVERHEAD;
    context = EVP_CIPHER_CTX_new();
    if (context == NULL) {
        errno = ENOMEM;
        return -1;
    }
    // Reproduce el nonce y el AAD; el resultado todavía no está autenticado.
    if (EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), NULL, key,
                           header + sizeof(ENCRYPTION_MAGIC) - 1) != 1 ||
        EVP_DecryptUpdate(context, NULL, &plainLength, header, sizeof(header)) != 1) {
        errno = EIO;
        goto cleanup;
    }
    while (remainingBytes > 0) {
        size_t blockSize = remainingBytes < sizeof(cipherBlock) ?
            (size_t)remainingBytes : sizeof(cipherBlock);

        // Solo escribe en el temporal del llamador; una etiqueta inválida lo descarta.
        if (fread(cipherBlock, 1, blockSize, input) != blockSize ||
            EVP_DecryptUpdate(context, plainBlock, &plainLength,
                              cipherBlock, (int)blockSize) != 1 ||
            plainLength != (int)blockSize ||
            fwrite(plainBlock, 1, (size_t)plainLength, output) != (size_t)plainLength) {
            errno = EIO;
            goto cleanup;
        }
        remainingBytes -= blockSize;
    }
    // Ningún consumidor puede contar palabras ni publicar texto sin verificar este tag.
    if (fread(tag, 1, sizeof(tag), input) != sizeof(tag) ||
        fgetc(input) != EOF || ferror(input) ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, sizeof(tag), tag) != 1 ||
        EVP_DecryptFinal_ex(context, plainBlock, &plainLength) != 1 || plainLength != 0) {
        errno = EBADMSG;
        goto cleanup;
    }
    if (fflush(output) == EOF || fseeko(output, 0, SEEK_SET) == -1) {
        goto cleanup;
    }
    result = 0;

cleanup:
    savedError = errno;
    OPENSSL_cleanse(plainBlock, sizeof(plainBlock));
    EVP_CIPHER_CTX_free(context);
    errno = savedError;
    return result;
}
