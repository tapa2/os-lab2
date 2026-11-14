// SPDX-License-Identifier: BSD-3-Clause

#include <stdint.h>
#include <string.h>
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

/* Swallow the next block: its header and payload become part of this block. */
static void merge_with_next(struct block_meta *block)
{
	struct block_meta *next = block->next;

	block->size += META_SIZE + next->size;
	block->next = next->next;
	if (next->next)
		next->next->prev = block;
}

/* Merge every run of neighbouring free blocks into a single free block. */
static void coalesce_all(void)
{
	struct block_meta *block = heap_head;

	while (block && block->next) {
		if (block->status == STATUS_FREE && block->next->status == STATUS_FREE)
			merge_with_next(block);
		else
			block = block->next;
	}
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

/*
 * Cut the unused tail of a block into a new free block. Done only if the tail
 * can hold a header plus at least one aligned byte of payload.
 */
static void split_block(struct block_meta *block, size_t size)
{
	struct block_meta *rest;

	if (block->size < size + META_SIZE + ALIGN(1))
		return;

	rest = (struct block_meta *)((char *)block + META_SIZE + size);
	rest->size = block->size - size - META_SIZE;
	rest->status = STATUS_FREE;
	rest->prev = block;
	rest->next = block->next;

	if (block->next)
		block->next->prev = rest;
	block->next = rest;
	block->size = size;
}

/* Grow the LAST heap block to `size` by moving the program break. */
static void expand_last_block(struct block_meta *block, size_t size)
{
	grow_heap(size - block->size);
	block->size = size;
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

	coalesce_all();

	block = find_best(size);
	if (block) {
		split_block(block, size);
		block->status = STATUS_ALLOC;
		return block;
	}

	/* No fit: if the last block is free, grow it by only the missing bytes. */
	block = get_last_block();
	if (block->status == STATUS_FREE) {
		expand_last_block(block, size);
		block->status = STATUS_ALLOC;
		return block;
	}

	return append_block(size);
}

/*
 * Common part of malloc and calloc: blocks of at least `threshold` bytes
 * (header included) are mapped, smaller ones go to the heap.
 */
static void *alloc_memory(size_t size, size_t threshold)
{
	if (size == 0)
		return NULL;

	size = ALIGN(size);

	if (META_SIZE + size >= threshold)
		return get_payload(alloc_mapped(size));

	return get_payload(alloc_on_heap(size));
}

void *os_malloc(size_t size)
{
	return alloc_memory(size, MMAP_THRESHOLD);
}

static void free_block(struct block_meta *block)
{
	int ret;

	if (block->status == STATUS_MAPPED) {
		ret = munmap(block, META_SIZE + block->size);
		DIE(ret == -1, "munmap");
	} else {
		/* Heap memory is never given back to the OS, only marked for reuse. */
		block->status = STATUS_FREE;
	}
}

void os_free(void *ptr)
{
	if (!ptr)
		return;

	free_block(get_block(ptr));
}

void *os_calloc(size_t nmemb, size_t size)
{
	size_t total;
	void *ptr;

	/* nmemb * size must not overflow size_t. */
	if (size != 0 && nmemb > SIZE_MAX / size)
		return NULL;

	total = nmemb * size;

	/* calloc switches to mmap() already at one page. */
	ptr = alloc_memory(total, getpagesize());
	if (ptr)
		memset(ptr, 0, total);

	return ptr;
}

/* Realloc fallback: allocate a new block, copy the data, release the old one. */
static void *move_block(struct block_meta *block, size_t size)
{
	void *new_ptr = alloc_memory(size, MMAP_THRESHOLD);
	size_t copy = block->size < size ? block->size : size;

	memcpy(new_ptr, get_payload(block), copy);
	free_block(block);

	return new_ptr;
}

/*
 * Grow a heap block in place by swallowing the free blocks after it, one at
 * a time. Returns 1 on success. Merged blocks stay merged even on failure.
 */
static int grow_in_place(struct block_meta *block, size_t size)
{
	while (block->next && block->next->status == STATUS_FREE) {
		merge_with_next(block);
		if (block->size >= size) {
			split_block(block, size);
			return 1;
		}
	}
	return 0;
}

void *os_realloc(void *ptr, size_t size)
{
	struct block_meta *block;

	if (!ptr)
		return alloc_memory(size, MMAP_THRESHOLD);

	block = get_block(ptr);

	if (size == 0) {
		free_block(block);
		return NULL;
	}

	/* Reallocating an already freed block is a user error. */
	if (block->status == STATUS_FREE)
		return NULL;

	size = ALIGN(size);

	/* Mapped blocks, or blocks that become big, always move. */
	if (block->status == STATUS_MAPPED || META_SIZE + size >= MMAP_THRESHOLD)
		return move_block(block, size);

	/* Shrink (or same size): stay in place, give the tail back. */
	if (size <= block->size) {
		split_block(block, size);
		return ptr;
	}

	/* The last block can simply grow at the end of the heap. */
	if (!block->next) {
		expand_last_block(block, size);
		return ptr;
	}

	if (grow_in_place(block, size))
		return ptr;

	return move_block(block, size);
}
