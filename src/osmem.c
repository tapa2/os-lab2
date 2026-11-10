// SPDX-License-Identifier: BSD-3-Clause

#include <sys/mman.h>
#include <unistd.h>
#include "osmem.h"
#include "block_meta.h"

/* Every address we hand out (and every header) is a multiple of 8. */
#define ALIGNMENT		8
#define ALIGN(size)		(((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

/* Header size, rounded up so the payload after it stays aligned. */
#define META_SIZE		ALIGN(sizeof(struct block_meta))

/* Blocks of at least this many bytes (header included) go to mmap(). */
#define MMAP_THRESHOLD		(128 * 1024)

/* First block of the heap list (NULL until the heap is used for the first time). */
static struct block_meta *heap_head;

/* Header address <-> payload address. */
static void *get_payload(struct block_meta *block)
{
	return (char *)block + META_SIZE;
}

static struct block_meta *get_block(void *ptr)
{
	return (struct block_meta *)((char *)ptr - META_SIZE);
}

/* Map a separate memory zone for one block; size is the aligned payload size. */
static struct block_meta *alloc_mapped(size_t size)
{
	struct block_meta *block;

	block = mmap(NULL, META_SIZE + size, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	DIE(block == MAP_FAILED, "mmap");

	block->size = size;
	block->status = STATUS_MAPPED;
	block->prev = NULL;
	block->next = NULL;

	return block;
}

/* Move the program break up by size bytes; returns the start of the new zone. */
static void *grow_heap(size_t size)
{
	void *old_brk = sbrk(size);

	DIE(old_brk == (void *)-1, "sbrk");
	return old_brk;
}

/* On the first heap use, take 128 KB at once and keep it as one free block. */
static void preallocate_heap(void)
{
	heap_head = grow_heap(MMAP_THRESHOLD);
	heap_head->size = MMAP_THRESHOLD - META_SIZE;
	heap_head->status = STATUS_FREE;
	heap_head->prev = NULL;
	heap_head->next = NULL;
}

static struct block_meta *get_last_block(void)
{
	struct block_meta *block = heap_head;

	while (block->next)
		block = block->next;
	return block;
}

/* Best fit: the smallest free block that is still big enough (NULL if none). */
static struct block_meta *find_best(size_t size)
{
	struct block_meta *best = NULL;
	struct block_meta *block;

	for (block = heap_head; block; block = block->next) {
		if (block->status != STATUS_FREE || block->size < size)
			continue;
		if (!best || block->size < best->size)
			best = block;
	}
	return best;
}

/* Create a new allocated block at the end of the heap. */
static struct block_meta *append_block(size_t size)
{
	struct block_meta *last = get_last_block();
	struct block_meta *block = grow_heap(META_SIZE + size);

	block->size = size;
	block->status = STATUS_ALLOC;
	block->prev = last;
	block->next = NULL;
	last->next = block;

	return block;
}

/* Small allocation: reuse a free heap block or grow the heap. */
static struct block_meta *alloc_on_heap(size_t size)
{
	struct block_meta *block;

	if (!heap_head)
		preallocate_heap();

	block = find_best(size);
	if (block) {
		block->status = STATUS_ALLOC;
		return block;
	}

	return append_block(size);
}

void *os_malloc(size_t size)
{
	if (size == 0)
		return NULL;

	size = ALIGN(size);

	if (META_SIZE + size >= MMAP_THRESHOLD)
		return get_payload(alloc_mapped(size));

	return get_payload(alloc_on_heap(size));
}

void os_free(void *ptr)
{
	struct block_meta *block;
	int ret;

	if (!ptr)
		return;

	block = get_block(ptr);

	if (block->status == STATUS_MAPPED) {
		ret = munmap(block, META_SIZE + block->size);
		DIE(ret == -1, "munmap");
	} else {
		/* Heap memory is never given back to the OS, only marked for reuse. */
		block->status = STATUS_FREE;
	}
}

void *os_calloc(size_t nmemb, size_t size)
{
	/* TODO: Implement os_calloc */
	(void)nmemb;
	(void)size;
	return NULL;
}

void *os_realloc(void *ptr, size_t size)
{
	/* TODO: Implement os_realloc */
	(void)ptr;
	(void)size;
	return NULL;
}
