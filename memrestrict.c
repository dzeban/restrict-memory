#define _GNU_SOURCE // Needed for RTLD_NEXT

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <dlfcn.h>
#include <stdarg.h>

#include <uthash.h>

static void* (*libc_malloc) (size_t)         = NULL;
static void* (*libc_calloc) (size_t, size_t) = NULL;
static void* (*libc_realloc)(void *, size_t) = NULL;
static void  (*libc_free)   (void *)         = NULL;
static void* (*libc_mmap)(void *, size_t, int, int, int, off_t) = NULL;
static void* (*libc_mmap2)(void *, size_t, int, int, int, off_t) = NULL;
static void* (*libc_mremap)(void *, size_t, size_t, int, ...) = NULL;
static int   (*libc_brk)(void *) = NULL;
static void* (*libc_sbrk)(intptr_t) = NULL;

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
static unsigned long mem_threshold = 0;

#define DEFAULT_MEM_THRESHOLD 2 * 1048576 // Default threshold is 2 MiB;

// Thread-local var to prevent malloc/free recursion
static __thread int no_hook;

static int DEBUG = -1;
#define log(M, ...) do {\
	if (DEBUG) { fprintf(stderr, "MEMRESTRICT:%d " M "\n", __LINE__, ##__VA_ARGS__); }\
} while(0)

// Wrapper to save original libc functions to global var
#define SAVE_LIBC_FUNC(var, f) do { \
	var = dlsym(RTLD_NEXT, f);\
	if (NULL == var) \
		perror("dlsym");\
} while(0)


static void account_alloc(void *ptr, size_t size)
{
	// Do not account itself
	no_hook = 1;

	// Allocating memory
	if (size != 0) {
		struct malloc_item *item, *out;

		item = malloc(sizeof(*item));
		item->p = ptr;
		item->size = size;

		HASH_ADD_PTR(HT, p, item);

		mem_allocated += size;

		log("Alloc: %p -> %zu\n", ptr, size);
	} 
	// Freeing memory
	else { 
		struct malloc_item *found;

		HASH_FIND_PTR(HT, &ptr, found);
		if (found) {
			mem_allocated -= found->size;
			log("Free: %p -> %zu\n", found->p, found->size);
			HASH_DEL(HT, found);
			free(found);
		} else {
			log("Freeing unaccounted allocation %p\n", ptr);
		}
	}

	log(" [[[:::  %d (%u) :::]]] \n", mem_allocated, HASH_COUNT(HT));

	no_hook = 0;
}


static void account_realloc(void *p, void *ptr, size_t size)
{
	// Do not account itself
	no_hook = 1;

	// ptr == NULL is equivalent to malloc(size) 
	if (ptr == NULL) {
		account_alloc(p, size);
	}
	// size == 0 is equivalent to free(ptr), 
	// and p will be NULL
	else if (size == 0) {
		account_alloc(ptr, size);
	}
	// Now the real realloc
	else {
		log("Realloc: %p -> %d\n", ptr, size);

		// if ptr was moved previous area will be freed
		if (p != ptr) {
			log("Realloc: Replacing pointer %p to %p\n", ptr, p);
			account_alloc(ptr, 0);
			account_alloc(p, size);
		} else {
			struct malloc_item *found;
			int alloc_diff;

			HASH_FIND_PTR(HT, &ptr, found);
			if (found) {
				// alloc_diff may be negative when shrinking memory
				alloc_diff = size - found->size;

				mem_allocated += alloc_diff;
				found->size += alloc_diff;
				log("Realloc: diff %p -> %d\n", ptr, alloc_diff);
			} else {
				log("Reallocating unaccounted pointer %p\n", ptr);
			}
		}
	}

	log(" [[[:::  %d (%u) :::]]] \n", mem_allocated, HASH_COUNT(HT));

	no_hook = 0;
}


// Initialize library parameters from environment.
// We don't have main(), so we have to call it in every callback.
static inline void init_env()
{
	char *t;

	if (mem_threshold == 0) {
		t = secure_getenv("MR_THRESHOLD");
		if (t)
			mem_threshold = strtol(t, NULL, 0);
		else
			mem_threshold = DEFAULT_MEM_THRESHOLD;
	}

	if (DEBUG == -1) {
		t = secure_getenv("MR_DEBUG");
		if (t)
			DEBUG = strtol(t, NULL, 0);
		else
			DEBUG = 0;
	}
}


void *malloc(size_t size)
{
	void *p = NULL;

	if (libc_malloc == NULL) 
		SAVE_LIBC_FUNC(libc_malloc, "malloc");

	init_env();

	if (mem_allocated + size <= mem_threshold) {
		p = libc_malloc(size);
	} else {
		log("Restricting malloc of %zu bytes\n", size);
		errno = ENOMEM;
		return NULL;
	}

	if (!no_hook) 
		account_alloc(p, size);

	return p;
}


void *calloc(size_t nmemb, size_t size)
{
	void *p = NULL;

	if (libc_calloc == NULL)
		SAVE_LIBC_FUNC(libc_calloc, "calloc");

	init_env();

	if (mem_allocated + nmemb * size <= mem_threshold) {
		p = libc_calloc(nmemb, size);
	} else {
		log("Restricting calloc of %zu elements per %zu size\n", nmemb, size);
		errno = ENOMEM;
		return NULL;
	}

	if (!no_hook)
		account_alloc(p, nmemb * size);

	return p;
}


void *realloc(void *ptr, size_t size)
{
	void *p = NULL;

	if (libc_realloc == NULL)
		SAVE_LIBC_FUNC(libc_realloc, "realloc");

	init_env();

	// TODO: CHECK new heap size *before* allowing realloc
	if (mem_allocated <= mem_threshold) {
		p = libc_realloc(ptr, size);
	} else {
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
		SAVE_LIBC_FUNC(libc_free, "free");

	init_env();

	libc_free(ptr);

	if (!no_hook)
		account_alloc(ptr, 0);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	void *p;

	log("mmap for %p of length %zu prot %d flags %d fd %d offset %lld\n", 
			addr, length, prot, flags, fd, offset);

	if (libc_mmap == NULL)
		SAVE_LIBC_FUNC(libc_mmap, "mmap");

	p = libc_mmap(addr, length, prot, flags, fd, offset);
	return p;
}

void *mmap2(void *addr, size_t length, int prot, int flags, int fd, off_t pgoffset)
{
	void *p;

	log("mmap2 for %p of length %zu prot %d flags %d fd %d pgoffset %lld\n", 
			addr, length, prot, flags, fd, pgoffset);

	if (libc_mmap2 == NULL)
		SAVE_LIBC_FUNC(libc_mmap2, "mmap2");

	p = libc_mmap2(addr, length, prot, flags, fd, pgoffset);
	return p;
}

void *mremap(void *old_address, size_t old_size, size_t new_size, int flags, ...)
{
	va_list ap;
	void *new_address;
	void *p;

	log("mremap for %p of size %zu, new_size %zu, flags %d\n", 
			old_address, old_size, new_size, flags);

	if (libc_mremap == NULL)
		SAVE_LIBC_FUNC(libc_mremap, "mremap");

	va_start(ap, flags);
	new_address = va_arg(ap, void *);
	va_end(ap);

	p = libc_mremap(old_address, old_size, new_size, flags, new_address);
	return p;
}

int brk(void *addr)
{
	int ret;

	log("brk set to %p\n", addr);
	if (libc_brk == NULL)
		SAVE_LIBC_FUNC(libc_brk, "brk");

	ret = libc_brk(addr);
	return ret;
}

void *sbrk(intptr_t increment)
{
	void *p;

	log("sbrk increment %d\n", increment);
	if (libc_sbrk == NULL)
		SAVE_LIBC_FUNC(libc_sbrk, "sbrk");

	p = sbrk(increment);
	return p;
}

