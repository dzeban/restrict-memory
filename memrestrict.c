#define _GNU_SOURCE // Needed for RTLD_NEXT

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <dlfcn.h>

#include <uthash.h>

static void* (*libc_malloc)(size_t) = NULL;
static void* (*libc_free)(void *) = NULL;

struct malloc_item
{
	void *p;
	size_t size;
	UT_hash_handle hh; // makes this structure hashable
};

// Hash table for accounting malloc/free
struct malloc_item *HT = NULL;

// Total memory allocated
static unsigned long mem_allocated = 0;

// TODO: Read this from env
#define MEM_THRESHOLD 2*1048576 // 2 MiB threshold

// Thread-local var to prevent malloc/free recursion
static __thread int no_hook;

static void account(void *ptr, size_t size)
{
	// Allocating memory
	if (size != 0)
	{
		struct malloc_item *item, *out;

		item = malloc(sizeof(*item));
		item->p = ptr;
		item->size = size;

		HASH_ADD_PTR(HT, p, item);

		mem_allocated += size;

		fprintf(stderr, "Alloc: %p -> %zu\n", ptr, size);
	}
	// Freeing memory
	else
	{
		struct malloc_item *found;

		HASH_FIND_PTR(HT, &ptr, found);
		if (found)
		{
			mem_allocated -= found->size;
			fprintf(stderr, "Free: %p -> %zu\n", found->p, found->size);
			HASH_DEL(HT, found);
			free(found);
		}
		else
		{
			fprintf(stderr, "Freeing unaccounted allocation %p\n", ptr);
		}
	}

	fprintf(stderr, " [[[:::  %d (%u) :::]]] \n", mem_allocated, HASH_COUNT(HT));
}

static void save_libc_malloc()
{
	libc_malloc = dlsym(RTLD_NEXT, "malloc");
	if (NULL == libc_malloc) 
		perror("dlsym");
}

void *malloc(size_t size)
{
	void *p = NULL;

	if (libc_malloc == NULL) 
		save_libc_malloc();

	if (mem_allocated <= MEM_THRESHOLD)
	{
		p = libc_malloc(size);
	}
	else
	{
		errno = ENOMEM;
		return NULL;
	}

	if (!no_hook) 
	{
		no_hook = 1;
		account(p, size);
		no_hook = 0;
	}

	return p;
}

static void save_libc_free()
{
	libc_free = dlsym(RTLD_NEXT, "free");
	if (NULL == libc_free) 
		perror("dlsym");
}

void free(void *ptr)
{
	if (libc_free == NULL)
		save_libc_free();

	libc_free(ptr);

	if (!no_hook)
	{
		no_hook = 1;
		account(ptr, 0);
		no_hook = 0;
	}
}
