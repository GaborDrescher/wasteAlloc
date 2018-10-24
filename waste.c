#include <stddef.h>
#include <inttypes.h>
#include <string.h> // memset
#include <sys/mman.h>

#define MEASURE_TIME

#ifdef MEASURE_TIME
#	include <stdio.h>
#	include <time.h>
#endif

#define OS_BLOCK_BITS (12)
#define OS_BLOCK_SIZE (((uintptr_t)1) << OS_BLOCK_BITS)
#define OS_PAGE_SIZE (4096)

#define NUM_BUCKETS (OS_BLOCK_BITS - 1)

struct Block;
struct Smallblock
{
	uintptr_t size;
	void *start;
};
_Static_assert(sizeof(struct Smallblock) == (2 * sizeof(void*)), "");

struct Block
{
	uintptr_t free_count;
	uintptr_t avail_count;
	struct Smallblock small_block;
};
_Static_assert(sizeof(struct Block) == (4 * sizeof(void*)), "");

struct Bin
{
	struct Block *blocks[NUM_BUCKETS];
};


static __thread struct Bin bin;


#ifdef MEASURE_TIME
static uint64_t get_cycles_start()
{
	uint64_t cycles;
	asm volatile(
			"cpuid\n\t"
			"rdtsc\n\t"
			"shl $32, %%rdx\n\t"
			"or %%rdx, %%rax"
			: "=a"(cycles)
			:
			: "%rbx", "%rcx", "%rdx", "memory"
			);
	return cycles;
}

static uint64_t get_cycles_stop()
{
	uint32_t cycles_high;
	uint32_t cycles_low;
	asm volatile(
			"rdtscp\n\t"
			"mov %%edx, %0\n\t"
			"mov %%eax, %1\n\t"
			"cpuid\n\t"
			: "=r"(cycles_high), "=r"(cycles_low)
			:
			: "%rax", "%rbx", "%rcx", "%rdx", "memory"
			);
	const uint64_t rdtscVal = (((uint64_t)cycles_high) << 32)
		| ((uint64_t)cycles_low);
	return rdtscVal;
}
#endif

static uintptr_t align_up(uintptr_t numToRound, uintptr_t multiple) 
{
	uintptr_t mask = multiple - 1;
    return (numToRound + mask) & ~mask;
}

static uintptr_t bin_bits(uintptr_t x)
{
	//if(x < 2) {
	//	return 0;
	//}
	return (8 * sizeof(uintptr_t)) - __builtin_clzl(x - 1);
}

static uintptr_t bin_internal_size(uintptr_t binIdx)
{
	return sizeof(struct Smallblock) + (((uintptr_t)1) << (binIdx));
}

static void* os_mmap(uintptr_t size)
{
	void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if(mem == MAP_FAILED) {
		return NULL;
	}

	return mem;
}

static void os_unmap(void *ptr, uintptr_t size)
{
	munmap(ptr, size);
}

static void* waste_alloc(uintptr_t size)
{
	if(size == 0) {
		return NULL;
	}

	if(size < (2 * sizeof(void*))) {
		size = 2 * sizeof(void*);
	}

	uintptr_t binIdx = bin_bits(size);

	if(binIdx < NUM_BUCKETS) {
		// small allocation, fast path
		const uintptr_t small_intern_size = bin_internal_size(binIdx);
		struct Block *block = bin.blocks[binIdx];
		if(block == NULL) {
			block = (struct Block*)os_mmap(OS_BLOCK_SIZE);
			if(block == NULL) {
				return NULL;
			}
			bin.blocks[binIdx] = block;
			uintptr_t available_small_blocks = (OS_BLOCK_SIZE - sizeof(struct Block)) / small_intern_size;
			block->free_count = available_small_blocks;
			block->avail_count = available_small_blocks;
			block->small_block.size = OS_BLOCK_SIZE;
			block->small_block.start = block + 1;
		}

		struct Smallblock *small_block = (struct Smallblock*)block->small_block.start;
		block->small_block.start = (void*)(((uintptr_t)small_block) + small_intern_size);
		small_block->size = small_intern_size;
		small_block->start = block;

		uintptr_t avail = block->avail_count;
		avail -= 1;
		block->avail_count = avail;
		if(avail == 0) {
			bin.blocks[binIdx] = NULL;
		}

		return small_block + 1;
	}

	// else large allocation
	uintptr_t mmapSize = align_up(size + sizeof(struct Block), OS_PAGE_SIZE);
	struct Block *block = (struct Block*)os_mmap(mmapSize);
	if(block == NULL) {
		return NULL;
	}
	block->free_count = 1;
	block->avail_count = 0;
	block->small_block.size = mmapSize;
	block->small_block.start = block;
	return block + 1;

	return NULL;
}

static void* waste_memalign(uintptr_t alignment, uintptr_t size)
{
	// if not power of two
	if(!((alignment != 0) && !(alignment & (alignment - 1)))) {
		return NULL;
	}

	if(size == 0) {
		return NULL;
	}

	if(alignment < (2 * sizeof(void*))) {
		return waste_alloc(size);
	}

	void *mem = waste_alloc(size + (alignment - 1) + sizeof(struct Smallblock));
	if(mem == NULL) {
		return NULL;
	}

	struct Smallblock *small_block = ((struct Smallblock*)mem) - 1;

	uintptr_t mem_val = (uintptr_t)mem;
	uintptr_t out_mem_val = align_up(mem_val + sizeof(struct Smallblock), alignment);
	struct Smallblock *my_sb = (struct Smallblock*)(out_mem_val - sizeof(struct Smallblock));
	my_sb->size = small_block->size;
	my_sb->start = small_block->start;
	return my_sb + 1;
}

static void waste_free(void *ptr)
{
	if(ptr == NULL) {
		return;
	}

	struct Smallblock *small_block = ((struct Smallblock*)ptr) - 1;
	struct Block *block = (struct Block*)(small_block->start);

	uintptr_t new_count = __atomic_sub_fetch(&block->free_count, 1, __ATOMIC_SEQ_CST);
	if(new_count == 0) {
		os_unmap(block, block->small_block.size);
	}
}

static void* waste_realloc(void *ptr, uintptr_t size)
{
	if(ptr == NULL) {
		return waste_alloc(size);
	}

	if(size == 0) {
		waste_free(ptr);
		return NULL;
	}

	struct Smallblock *small_block = ((struct Smallblock*)ptr) - 1;
	uintptr_t old_intern_size = small_block->size;

	uintptr_t old_user_size = old_intern_size;
	if(old_intern_size > bin_internal_size(NUM_BUCKETS - 1)) {
		// large block
		old_user_size -= sizeof(struct Block);
	}
	else {
		// small block
		old_user_size -= sizeof(struct Smallblock);
	}

	if(old_user_size >= size) {
		return ptr;
	}

	void *out = waste_alloc(size);
	if(out == NULL) {
		return NULL;
	}

	memcpy(out, ptr, old_user_size);

	waste_free(ptr);
	return out;
}


//////
//////
//////
#ifdef __cplusplus
extern "C" {
#endif

void* malloc(size_t size)
{
	#ifdef MEASURE_TIME
	uint64_t time = get_cycles_start();
	#endif

	void *out = waste_alloc(size);

	#ifdef MEASURE_TIME
	time = get_cycles_stop() - time;
	fprintf(stderr, "malloc %" PRIu64 "\n", time);
	#endif

	return out;
}

void* memalign(size_t alignment, size_t size)
{
	#ifdef MEASURE_TIME
	uint64_t time = get_cycles_start();
	#endif

	void *out = waste_memalign(alignment, size);

	#ifdef MEASURE_TIME
	time = get_cycles_stop() - time;
	fprintf(stderr, "memalign %" PRIu64 "\n", time);
	#endif

	return out;
}

void free(void *ptr)
{
	#ifdef MEASURE_TIME
	uint64_t time = get_cycles_start();
	#endif

	waste_free(ptr);

	#ifdef MEASURE_TIME
	time = get_cycles_stop() - time;
	fprintf(stderr, "free %" PRIu64 "\n", time);
	#endif
}

void* realloc(void *ptr, size_t size)
{
	#ifdef MEASURE_TIME
	uint64_t time = get_cycles_start();
	#endif

	void *out = waste_realloc(ptr, size);

	#ifdef MEASURE_TIME
	time = get_cycles_stop() - time;
	fprintf(stderr, "realloc %" PRIu64 "\n", time);
	#endif

	return out;
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	void *out;

	if((alignment % sizeof(void*)) != 0) {
		return 22;
	}

	/* if not power of two */
	if(!((alignment != 0) && !(alignment & (alignment - 1)))) {
		return 22;
	}

	if(size == 0) {
		*memptr = NULL;
		return 0;
	}

	out = memalign(alignment, size);
	if(out == NULL) {
		return 12;
	}

	*memptr = out;
	return 0;
}

void* calloc(size_t nmemb, size_t size)
{
	void *out;
	size_t fullsize = nmemb * size;

	if((size != 0) && ((fullsize / size) != nmemb)) {
		return NULL;
	}
	
	out = malloc(fullsize);
	if(out == NULL) {
		return NULL;
	}

	memset(out, 0, fullsize);
	return out;
}

void* valloc(size_t size)
{
	return memalign(OS_PAGE_SIZE, size);
}

void* pvalloc(size_t size)
{
	size_t rem, allocsize;

	rem = size % OS_PAGE_SIZE;
	allocsize = size;
	if(rem != 0) {
		allocsize = OS_PAGE_SIZE + (size - rem);
	}

	return memalign(OS_PAGE_SIZE, allocsize);
}

void* aligned_alloc(size_t alignment, size_t size)
{
	if(alignment > size) {
		return NULL;
	}

	if((size % alignment) != 0) {
		return NULL;
	}

	return memalign(alignment, size);
}

#ifdef __cplusplus
}
#endif
