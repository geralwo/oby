#ifndef OBY_ARENA_H
#define OBY_ARENA_H

#include <stddef.h>

typedef struct ArenaChunk {
    void *data;
    size_t size;
    size_t used;
    struct ArenaChunk *next;
} ArenaChunk;

typedef struct {
    ArenaChunk *head;
} Arena;

void arena_init(Arena *a);
void *arena_alloc(Arena *a, size_t size);
void arena_free(Arena *a);

#endif // OBY_ARENA_H