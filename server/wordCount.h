#ifndef WORD_COUNT_H
#define WORD_COUNT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define WORD_BUCKET_COUNT 4096
#define MAX_WORD_BYTES 4096
#define MAX_UNIQUE_WORDS 100000

typedef struct WordEntry {
    uint64_t count;
    struct WordEntry *next;
    // La cadena pertenece a la misma reserva que la entrada.
    char word[];
} WordEntry;

/** Tabla propia del llamador; inicializar con {0} y liberar con clearWordCounts. */
typedef struct {
    WordEntry *buckets[WORD_BUCKET_COUNT];
    size_t uniqueCount;
    uint64_t totalCount;
} WordCounts;

/**
 * @brief Identifica bytes que pertenecen a palabras: ASCII alfanumérico y bytes >= 128.
 * @param character Byte entre 0 y 255, o EOF.
 * @return 1 si forma parte de una palabra, 0 si es separador.
 */
int isWordByte(int character);

/**
 * @brief Cuenta desde la posición actual; convierte solamente mayúsculas ASCII a minúsculas.
 * @param input Texto autenticado, no NULL; conserva su propiedad el llamador.
 * @param counts Tabla inicializada; puede contener conteos previos.
 * @return 0 si completa, -1 con errno; ante error la tabla puede quedar parcialmente actualizada.
 */
int countWords(FILE *input, WordCounts *counts);

/**
 * @brief Serializa una tabla a texto DTC1, con conteos y palabras separados por tabulación.
 * @param output Destino privado, no NULL; conserva su propiedad el llamador.
 * @param counts Tabla válida, no NULL.
 * @return 0 si escribe y vacía la salida, -1 con errno si falla.
 */
int writeWordCounts(FILE *output, const WordCounts *counts);

/**
 * @brief Valida un resultado DTC1 y suma todos sus conteos, incluidos los de otros fragmentos.
 * @param input Resultado autenticado desde su cabecera DTC1, no NULL.
 * @param counts Tabla inicializada, no NULL; puede cambiar parcialmente ante error.
 * @return 0 si completa, -1 con errno si el resultado es inválido o no hay recursos.
 */
int mergeWordCounts(FILE *input, WordCounts *counts);

/**
 * @brief Selecciona el mayor conteo y desempata por orden lexicográfico de bytes UTF-8.
 * @param counts Tabla válida, no NULL.
 * @return Entrada prestada hasta clearWordCounts, o NULL si no hay palabras.
 */
const WordEntry *mostFrequentWord(const WordCounts *counts);

/**
 * @brief Libera palabras y entradas, y restablece una tabla inicializada.
 * @param counts Tabla válida, no NULL; no se libera la propia estructura.
 */
void clearWordCounts(WordCounts *counts);

#endif
