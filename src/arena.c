#include "pxml_internal.h"

#include <stddef.h>

enum {
    PXML_ARENA_BLOCK_SIZE = 64 * 1024,
    PXML_INTERN_INITIAL_CAPACITY = 64
};

typedef struct PxmlArenaBlock {
    struct PxmlArenaBlock *next;
    size_t used;
    size_t capacity;
    unsigned char data[];
} PxmlArenaBlock;

typedef struct PxmlArena {
    PxmlArenaBlock *first;
    PxmlArenaBlock *current;
} PxmlArena;

typedef struct PxmlInternSlot {
    uint64_t hash;
    uint32_t length;
    char *value;
} PxmlInternSlot;

typedef struct PxmlDocumentStorage {
    PxmlArena arena;
    PxmlInternSlot *intern_slots;
    size_t intern_count;
    size_t intern_capacity;
} PxmlDocumentStorage;

static bool is_power_of_two(size_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static void *arena_alloc(PxmlArena *arena, size_t size, size_t alignment)
{
    PxmlArenaBlock *block = arena->current;
    size_t aligned;
    if (size == 0U) size = 1U;
    if (!is_power_of_two(alignment)) return NULL;
    if (block != NULL) {
        aligned = (block->used + alignment - 1U) & ~(alignment - 1U);
        if (aligned <= block->capacity && size <= block->capacity - aligned) {
            void *result = block->data + aligned;
            block->used = aligned + size;
            memset(result, 0, size);
            return result;
        }
    }
    {
        size_t minimum;
        size_t capacity;
        if (size > SIZE_MAX - alignment + 1U) return NULL;
        minimum = size + alignment - 1U;
        capacity = minimum > PXML_ARENA_BLOCK_SIZE ? minimum : PXML_ARENA_BLOCK_SIZE;
        if (capacity > SIZE_MAX - sizeof(PxmlArenaBlock)) return NULL;
        block = (PxmlArenaBlock *)malloc(sizeof(PxmlArenaBlock) + capacity);
        if (block == NULL) return NULL;
        block->next = NULL;
        block->used = 0U;
        block->capacity = capacity;
        if (arena->current == NULL) {
            arena->first = block;
        } else {
            arena->current->next = block;
        }
        arena->current = block;
    }
    aligned = (block->used + alignment - 1U) & ~(alignment - 1U);
    block->used = aligned + size;
    memset(block->data + aligned, 0, size);
    return block->data + aligned;
}

static void arena_destroy(PxmlArena *arena)
{
    PxmlArenaBlock *block = arena->first;
    while (block != NULL) {
        PxmlArenaBlock *next = block->next;
        free(block);
        block = next;
    }
    memset(arena, 0, sizeof(*arena));
}

static size_t probe_distance(size_t slot, uint64_t hash, size_t capacity)
{
    size_t ideal = (size_t)hash & (capacity - 1U);
    return (slot + capacity - ideal) & (capacity - 1U);
}

static bool intern_insert(PxmlInternSlot *slots, size_t capacity, PxmlInternSlot entry)
{
    size_t slot = (size_t)entry.hash & (capacity - 1U);
    size_t distance = 0U;
    for (;;) {
        PxmlInternSlot *current = &slots[slot];
        if (current->value == NULL) {
            *current = entry;
            return true;
        }
        {
            size_t current_distance = probe_distance(slot, current->hash, capacity);
            if (current_distance < distance) {
                PxmlInternSlot temporary = *current;
                *current = entry;
                entry = temporary;
                distance = current_distance;
            }
        }
        slot = (slot + 1U) & (capacity - 1U);
        distance++;
        if (distance >= capacity) return false;
    }
}

static bool intern_grow(PxmlDocumentStorage *storage)
{
    size_t next_capacity = storage->intern_capacity == 0U
        ? PXML_INTERN_INITIAL_CAPACITY
        : storage->intern_capacity * 2U;
    PxmlInternSlot *next;
    size_t index;
    if (next_capacity < storage->intern_capacity ||
        next_capacity > SIZE_MAX / sizeof(PxmlInternSlot)) return false;
    next = (PxmlInternSlot *)arena_alloc(
        &storage->arena,
        next_capacity * sizeof(PxmlInternSlot),
        _Alignof(PxmlInternSlot));
    if (next == NULL) return false;
    for (index = 0U; index < storage->intern_capacity; ++index) {
        if (storage->intern_slots[index].value != NULL &&
            !intern_insert(next, next_capacity, storage->intern_slots[index])) {
            return false;
        }
    }
    storage->intern_slots = next;
    storage->intern_capacity = next_capacity;
    return true;
}

PxmlDocument *pxml_document_create(
    const char *path,
    const char *source,
    size_t source_length)
{
    PxmlDocument *document = (PxmlDocument *)calloc(1U, sizeof(PxmlDocument));
    PxmlDocumentStorage *storage;
    if (document == NULL) return NULL;
    storage = (PxmlDocumentStorage *)calloc(1U, sizeof(PxmlDocumentStorage));
    if (storage == NULL) {
        free(document);
        return NULL;
    }
    document->_storage = storage;
    document->path = pxml_document_intern(
        document, path == NULL ? "<memory>" : path);
    document->source = (char *)arena_alloc(
        &storage->arena, source_length + 1U, _Alignof(char));
    document->source_length = source_length;
    document->language_version = pxml_document_intern(document, "1.0");
    if (document->path == NULL || document->source == NULL ||
        document->language_version == NULL) {
        pxml_document_destroy(document);
        return NULL;
    }
    if (source_length != 0U && source != NULL) {
        memcpy(document->source, source, source_length);
    }
    document->source[source_length] = '\0';
    return document;
}

void *pxml_document_alloc(PxmlDocument *document, size_t size, size_t alignment)
{
    PxmlDocumentStorage *storage;
    if (document == NULL || document->_storage == NULL) return NULL;
    storage = (PxmlDocumentStorage *)document->_storage;
    return arena_alloc(&storage->arena, size, alignment);
}

char *pxml_document_intern_n(PxmlDocument *document, const char *value, size_t length)
{
    PxmlDocumentStorage *storage;
    uint64_t hash;
    size_t slot;
    size_t distance = 0U;
    char *copy;
    PxmlInternSlot entry;
    if (document == NULL || document->_storage == NULL || value == NULL ||
        length > UINT32_MAX) return NULL;
    storage = (PxmlDocumentStorage *)document->_storage;
    if (storage->intern_capacity == 0U ||
        (storage->intern_count + 1U) * 10U >= storage->intern_capacity * 7U) {
        if (!intern_grow(storage)) return NULL;
    }
    hash = pxml_hash64(value, length, UINT64_C(0x50584D4C53545231));
    if (hash == 0U) hash = 1U;
    slot = (size_t)hash & (storage->intern_capacity - 1U);
    for (;;) {
        PxmlInternSlot *current = &storage->intern_slots[slot];
        if (current->value == NULL) break;
        if (current->hash == hash && current->length == (uint32_t)length &&
            memcmp(current->value, value, length) == 0) {
            return current->value;
        }
        if (probe_distance(slot, current->hash, storage->intern_capacity) < distance) break;
        slot = (slot + 1U) & (storage->intern_capacity - 1U);
        distance++;
    }
    copy = (char *)arena_alloc(&storage->arena, length + 1U, _Alignof(char));
    if (copy == NULL) return NULL;
    memcpy(copy, value, length);
    copy[length] = '\0';
    entry.hash = hash;
    entry.length = (uint32_t)length;
    entry.value = copy;
    if (!intern_insert(storage->intern_slots, storage->intern_capacity, entry)) return NULL;
    storage->intern_count++;
    return copy;
}

char *pxml_document_intern(PxmlDocument *document, const char *value)
{
    return value == NULL ? NULL : pxml_document_intern_n(document, value, strlen(value));
}

bool pxml_document_reserve_array(
    PxmlDocument *document,
    void **items,
    size_t *capacity,
    size_t required,
    size_t item_size,
    size_t item_alignment)
{
    size_t next_capacity;
    void *next;
    if (required <= *capacity) return true;
    next_capacity = *capacity == 0U ? 4U : *capacity;
    while (next_capacity < required) {
        if (next_capacity > SIZE_MAX / 2U) return false;
        next_capacity *= 2U;
    }
    if (item_size != 0U && next_capacity > SIZE_MAX / item_size) return false;
    next = pxml_document_alloc(document, next_capacity * item_size, item_alignment);
    if (next == NULL) return false;
    if (*items != NULL && *capacity != 0U) {
        memcpy(next, *items, *capacity * item_size);
    }
    *items = next;
    *capacity = next_capacity;
    return true;
}

void pxml_document_storage_destroy(PxmlDocument *document)
{
    PxmlDocumentStorage *storage;
    if (document == NULL || document->_storage == NULL) return;
    storage = (PxmlDocumentStorage *)document->_storage;
    arena_destroy(&storage->arena);
    free(storage);
    document->_storage = NULL;
}
