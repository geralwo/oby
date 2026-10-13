#include <stdlib.h>
#include "core/arena.h"

#define CHUNK_SIZE 65536

void arena_init(Arena *a) {
    a->head = NULL;
}

void *arena_alloc(Arena *a, size_t size) {
    // Align size to 8 bytes
    size = (size + 7) & ~7;

    if (a->head == NULL || a->head->used + size > a->head->size) {
        size_t alloc_size = (size > CHUNK_SIZE) ? size : CHUNK_SIZE;
        ArenaChunk *new_chunk = (ArenaChunk *)malloc(sizeof(ArenaChunk));
        if (!new_chunk) return NULL;
        new_chunk->data = malloc(alloc_size);
        if (!new_chunk->data) {
            free(new_chunk);
            return NULL;
        }
        new_chunk->size = alloc_size;
        new_chunk->used = 0;
        new_chunk->next = a->head;
        a->head = new_chunk;
    }

    void *ptr = (char *)a->head->data + a->head->used;
    a->head->used += size;
    return ptr;
}

void arena_free(Arena *a) {
    ArenaChunk *curr = a->head;
    while (curr) {
        ArenaChunk *next = curr->next;
        free(curr->data);
        free(curr);
        curr = next;
    }
    a->head = NULL;
}