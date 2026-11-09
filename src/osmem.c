// SPDX-License-Identifier: BSD-3-Clause

#include <sys/mman.h>
#include "osmem.h"
#include "block_meta.h"

/* Every address we hand out (and every header) is a multiple of 8. */
#define ALIGNMENT		8
#define ALIGN(size)		(((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

/* Header size, rounded up so the payload after it stays aligned. */
#define META_SIZE		ALIGN(sizeof(struct block_meta))

/* Blocks of at least this many bytes (header included) go to mmap(). */
#define MMAP_THRESHOLD		(128 * 1024)

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

void *os_malloc(size_t size)
{
	if (size == 0)
		return NULL;

	size = ALIGN(size);

	if (META_SIZE + size >= MMAP_THRESHOLD)
		return get_payload(alloc_mapped(size));

	/* TODO (next stage): small blocks on the heap with sbrk() */
	return NULL;
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
