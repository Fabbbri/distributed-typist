#include "wordCount.h"
#include "protocol.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

int isWordByte(int character) {
    return (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') ||
        (character >= 128 && character <= 255);
}

/**
 * @brief Suma una palabra normalizada y valida límites de conteos y memoria.
 * @param counts Tabla inicializada que adquiere una copia si la palabra es nueva.
 * @param word Cadena válida no vacía, de hasta MAX_WORD_BYTES bytes.
 * @param amount Incremento positivo, acotado por MAX_FILE_SIZE.
 * @return 0 si suma, -1 con errno si excede límites o falla la reserva.
 */
static int addWord(WordCounts *counts, const char *word, uint64_t amount) {
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t wordLength = strlen(word);
    WordEntry *entry = NULL;
    size_t bucket = 0;

    // El total también limita resultados remotos mal formados antes de sumarlos.
    if (wordLength == 0 || wordLength > MAX_WORD_BYTES || amount == 0 ||
        amount > MAX_FILE_SIZE || counts->totalCount > MAX_FILE_SIZE - amount) {
        errno = EOVERFLOW;
        return -1;
    }
    for (size_t index = 0; index < wordLength; index++) {
        hash = (hash ^ (unsigned char)word[index]) * UINT64_C(1099511628211);
    }
    bucket = (size_t)(hash % WORD_BUCKET_COUNT);
    for (entry = counts->buckets[bucket]; entry != NULL; entry = entry->next) {
        if (strcmp(entry->word, word) == 0) {
            entry->count += amount;
            counts->totalCount += amount;
            return 0;
        }
    }
    // Las entradas y sus cadenas pertenecen a la tabla hasta clearWordCounts.
    if (counts->uniqueCount >= MAX_UNIQUE_WORDS) {
        errno = E2BIG;
        return -1;
    }
    entry = malloc(sizeof(*entry) + wordLength + 1);
    if (entry == NULL) {
        return -1;
    }
    memcpy(entry->word, word, wordLength + 1);
    entry->count = amount;
    entry->next = counts->buckets[bucket];
    counts->buckets[bucket] = entry;
    counts->uniqueCount++;
    counts->totalCount += amount;
    return 0;
}

int countWords(FILE *input, WordCounts *counts) {
    char word[MAX_WORD_BYTES + 1];
    size_t wordLength = 0;
    int character = EOF;

    if (input == NULL || counts == NULL) {
        errno = EINVAL;
        return -1;
    }
    // Las fronteras de fragmentos se sitúan en separadores, de modo que no se cortan palabras.
    while ((character = fgetc(input)) != EOF) {
        if (isWordByte(character)) {
            if (wordLength == MAX_WORD_BYTES) {
                errno = E2BIG;
                return -1;
            }
            word[wordLength++] = (char)(character >= 'A' && character <= 'Z' ?
                character + ('a' - 'A') : character);
        } else if (wordLength > 0) {
            word[wordLength] = '\0';
            if (addWord(counts, word, 1) == -1) {
                return -1;
            }
            wordLength = 0;
        }
    }
    if (ferror(input)) {
        return -1;
    }
    // Incluye la última palabra aunque no termine en un separador o salto de línea.
    if (wordLength > 0) {
        word[wordLength] = '\0';
        return addWord(counts, word, 1);
    }
    return 0;
}

int writeWordCounts(FILE *output, const WordCounts *counts) {
    if (output == NULL || counts == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (fputs("DTC1\n", output) == EOF) {
        return -1;
    }
    // Una palabra no contiene tabulaciones ni saltos, por lo que el formato es inequívoco.
    for (size_t bucket = 0; bucket < WORD_BUCKET_COUNT; bucket++) {
        for (const WordEntry *entry = counts->buckets[bucket]; entry != NULL;
             entry = entry->next) {
            if (fprintf(output, "%" PRIu64 "\t%s\n", entry->count, entry->word) < 0) {
                return -1;
            }
        }
    }
    return fflush(output) == EOF ? -1 : 0;
}

int mergeWordCounts(FILE *input, WordCounts *counts) {
    char line[MAX_WORD_BYTES + 32];

    if (input == NULL || counts == NULL) {
        errno = EINVAL;
        return -1;
    }
    // Acota cada registro antes de convertir números o incorporar cadenas a la tabla.
    if (fgets(line, sizeof(line), input) == NULL || strcmp(line, "DTC1\n") != 0) {
        errno = EBADMSG;
        return -1;
    }
    while (fgets(line, sizeof(line), input) != NULL) {
        size_t length = strlen(line);
        char *word = NULL;
        char *numberEnd = NULL;
        uintmax_t amount = 0;

        if (length == 0 || line[length - 1] != '\n' || line[0] < '0' || line[0] > '9') {
            errno = EBADMSG;
            return -1;
        }
        line[length - 1] = '\0';
        errno = 0;
        amount = strtoumax(line, &numberEnd, 10);
        if (errno != 0 || *numberEnd != '\t' || amount == 0 || amount > MAX_FILE_SIZE) {
            errno = EBADMSG;
            return -1;
        }
        word = numberEnd + 1;
        for (const unsigned char *byte = (const unsigned char *)word; *byte != '\0'; byte++) {
            if (!isWordByte(*byte) || (*byte >= 'A' && *byte <= 'Z')) {
                errno = EBADMSG;
                return -1;
            }
        }
        if (addWord(counts, word, (uint64_t)amount) == -1) {
            return -1;
        }
    }
    return ferror(input) ? -1 : 0;
}

const WordEntry *mostFrequentWord(const WordCounts *counts) {
    const WordEntry *winner = NULL;

    // El desempate explícito elimina la dependencia del orden de llegada o del hash.
    for (size_t bucket = 0; bucket < WORD_BUCKET_COUNT; bucket++) {
        for (const WordEntry *entry = counts->buckets[bucket]; entry != NULL;
             entry = entry->next) {
            if (winner == NULL || entry->count > winner->count ||
                (entry->count == winner->count && strcmp(entry->word, winner->word) < 0)) {
                winner = entry;
            }
        }
    }
    return winner;
}

void clearWordCounts(WordCounts *counts) {
    // Guarda el enlace antes de liberar cada entrada para no acceder a memoria ya liberada.
    for (size_t bucket = 0; bucket < WORD_BUCKET_COUNT; bucket++) {
        WordEntry *entry = counts->buckets[bucket];

        while (entry != NULL) {
            WordEntry *next = entry->next;
            free(entry);
            entry = next;
        }
    }
    memset(counts, 0, sizeof(*counts));
}
