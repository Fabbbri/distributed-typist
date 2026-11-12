#include "crypto.h"
#include "protocol.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdlib.h>

// El observador de pruebas no recibe parámetros propios a través de la interfaz EVP.
static size_t characterCallCount = 0;
static int groupedInputDetected = 0;

/**
 * @brief Declara la función real que resolverá el enlazador mediante --wrap.
 * @param context Contexto EVP del cifrado.
 * @param output Destino cifrado, o NULL para AAD.
 * @param outputLength Recibe la longitud producida.
 * @param input Bytes de entrada.
 * @param inputLength Longitud de la entrada.
 * @return Resultado de la función original de OpenSSL.
 */
int __real_EVP_EncryptUpdate(EVP_CIPHER_CTX *context, unsigned char *output,
                            int *outputLength, const unsigned char *input, int inputLength);

/**
 * @brief Observa la granularidad del texto claro y conserva el cifrado real de OpenSSL.
 * @param context Contexto EVP del cifrado.
 * @param output Destino cifrado, o NULL para AAD.
 * @param outputLength Recibe la longitud producida.
 * @param input Bytes de entrada.
 * @param inputLength Longitud de la entrada.
 * @return 0 si se agrupa texto claro, o el resultado de OpenSSL en los demás casos.
 * El nombre está impuesto por --wrap; esta interposición solo se enlaza en esta prueba.
 */
int __wrap_EVP_EncryptUpdate(EVP_CIPHER_CTX *context, unsigned char *output,
                            int *outputLength, const unsigned char *input, int inputLength) {
    // El AAD usa output NULL y puede enviarse como cabecera; el contenido debe ir de uno en uno.
    if (output != NULL) {
        if (inputLength != 1) {
            groupedInputDetected = 1;
            return 0;
        }
        characterCallCount++;
    }
    return __real_EVP_EncryptUpdate(context, output, outputLength, input, inputLength);
}

/**
 * @brief Comprueba una llamada real de cifrado por cada carácter de entrada de C.
 * @param characterCount Cantidad de caracteres, hasta TRANSFER_BLOCK_SIZE + 1.
 * @return 0 si cada llamada procesa un carácter, -1 ante errores o agrupación de entrada.
 */
static int checkCharacterEncryption(size_t characterCount) {
    unsigned char sample[TRANSFER_BLOCK_SIZE + 1];
    unsigned char key[ENCRYPTION_KEY_SIZE] = {0};
    FILE *input = NULL;
    FILE *output = NULL;
    int result = -1;

    if (characterCount > sizeof(sample)) {
        return -1;
    }
    // Incluye ceros y valores 0xFF para detectar confusiones entre un carácter leído y EOF.
    for (size_t index = 0; index < characterCount; index++) {
        sample[index] = (unsigned char)index;
    }
    input = tmpfile();
    output = tmpfile();
    if (input == NULL || output == NULL || RAND_bytes(key, sizeof(key)) != 1 ||
        fwrite(sample, 1, characterCount, input) != characterCount ||
        fseeko(input, 0, SEEK_SET) == -1) {
        goto cleanup;
    }
    // La prueba observa la frontera EVP, sin sustituir las operaciones criptográficas.
    characterCallCount = 0;
    groupedInputDetected = 0;
    if (encryptFile(input, output, characterCount, key) == -1 ||
        groupedInputDetected || characterCallCount != characterCount) {
        fprintf(stderr, "ERROR: expected one plaintext character per EVP update (%zu bytes)\n",
                characterCount);
        goto cleanup;
    }
    result = 0;

cleanup:
    OPENSSL_cleanse(key, sizeof(key));
    if (input != NULL && fclose(input) == EOF) {
        result = -1;
    }
    if (output != NULL && fclose(output) == EOF) {
        result = -1;
    }
    return result;
}

/**
 * @brief Verifica el cifrado carácter por carácter para varias longitudes de entrada.
 * @return EXIT_SUCCESS si todos los casos cumplen el contrato, EXIT_FAILURE si alguno falla.
 */
int main(void) {
    const size_t sampleSizes[] = {0, 1, 6, TRANSFER_BLOCK_SIZE + 1};

    for (size_t index = 0; index < sizeof(sampleSizes) / sizeof(sampleSizes[0]); index++) {
        if (checkCharacterEncryption(sampleSizes[index]) == -1) {
            return EXIT_FAILURE;
        }
    }
    puts("PASS: one plaintext character per EVP update, including empty input and 0xFF");
    return EXIT_SUCCESS;
}
