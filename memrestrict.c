#define _GNU_SOURCE // Needed for RTLD_NEXT

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <dlfcn.h>

#include <uthash.h>

static void* (*libc_malloc)(size_t) = NULL;
static void* (*libc_calloc)(size_t, size_t) = NULL;
static void* (*libc_realloc)(void *, size_t) = NULL;
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

static void save_libc_malloc()
{
	libc_malloc = dlsym(RTLD_NEXT, "malloc");
	if (NULL == libc_malloc) 
		perror("dlsym");
}

static void save_libc_calloc()
{
	libc_calloc = dlsym(RTLD_NEXT, "calloc");
	if (NULL == libc_calloc) 
		perror("dlsym");
}

static void save_libc_realloc()
{
	libc_realloc = dlsym(RTLD_NEXT, "realloc");
	if (NULL == libc_realloc)
		perror("dlsym");
}

static void save_libc_free()
{
	libc_free = dlsym(RTLD_NEXT, "free");
	if (NULL == libc_free) 
		perror("dlsym");
}

static void account_alloc(void *ptr, size_t size)
{
	// Do not account itself
	no_hook = 1;

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

	no_hook = 0;
}

static void account_realloc(void *p, void *ptr, size_t size)
{
	// Do not account itself
	no_hook = 1;

	// ptr == NULL is equivalent to malloc(size) 
	if (ptr == NULL)
	{
		account_alloc(p, size);
	}
	// size == 0 is equivalent to free(ptr), 
	// and p will be NULL
	else if (size == 0)
	{
		account_alloc(ptr, size);
	}
	// Now the real realloc
	else
	{
		fprintf(stderr, "Realloc: %p -> %d\n", ptr, size);

		// if ptr was moved previous area will be freed
		if (p != ptr)
		{
			fprintf(stderr, "Realloc: Replacing pointer %p to %p\n", ptr, p);
			account_alloc(ptr, 0);
			account_alloc(p, size);
		}
		else
		{
			struct malloc_item *found;
			int alloc_diff;

			HASH_FIND_PTR(HT, &ptr, found);
			if (found)
			{
				// alloc_diff may be negative when shrinking memory
				alloc_diff = size - found->size;

				mem_allocated += alloc_diff;
				found->size += alloc_diff;
				fprintf(stderr, "Realloc: diff %p -> %d\n", ptr, alloc_diff);
			}
			else
			{
				fprintf(stderr, "Reallocating unaccounted pointer %p\n", ptr);
			}
		}
	}

	fprintf(stderr, " [[[:::  %d (%u) :::]]] \n", mem_allocated, HASH_COUNT(HT));

	no_hook = 0;
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
		account_alloc(p, size);
		no_hook = 0;
	}

	return p;
}

void *calloc(size_t nmemb, size_t size)
{
	void *p = NULL;

	if (libc_calloc == NULL)
		save_libc_calloc();

	if (mem_allocated <= MEM_THRESHOLD)
	{
		p = libc_calloc(nmemb, size);
	}
	else
	{
		errno = ENOMEM;
		return NULL;
	}

	if (!no_hook)
	{
		no_hook = 1;
		account_alloc(p, nmemb * size);
		no_hook = 0;
	}

	return p;
}

void *realloc(void *ptr, size_t size)
{
	void *p = NULL;

	if (libc_realloc == NULL)
		save_libc_realloc();

	if (mem_allocated <= MEM_THRESHOLD)
	{
		p = libc_realloc(ptr, size);
	}
	else
	{
		errno = ENOMEM;
		return NULL;
	}

	if (!no_hook)
		account_realloc(p, ptr, size);

	return p;
}

void free(void *ptr)
{
	if (libc_free == NULL)
		save_libc_free();

	libc_free(ptr);

	if (!no_hook)
		account_alloc(ptr, 0);
}
