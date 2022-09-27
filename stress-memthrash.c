/*
 * Copyright (C) 2013-2021 Canonical, Ltd.
 * Copyright (C)      2022 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"
#include "core-arch.h"
#include "core-cache.h"
#include "core-nt-store.h"
#include "core-pthread.h"
#include "core-target-clones.h"

static const stress_help_t help[] = {
	{ NULL,	"memthrash N",		"start N workers thrashing a 16MB memory buffer" },
	{ NULL,	"memthrash-ops N",	"stop after N memthrash bogo operations" },
	{ NULL,	"memthrash-method M",	"specify memthrash method M, default is all" },
	{ NULL,	NULL,			NULL }
};

#if defined(HAVE_LIB_PTHREAD)

#define MATRIX_SIZE_MAX_SHIFT	(14)	/* No more than 16 */
#define MATRIX_SIZE_MIN_SHIFT	(10)
#define MATRIX_SIZE		(1 << MATRIX_SIZE_MAX_SHIFT)
#define MEM_SIZE		(MATRIX_SIZE * MATRIX_SIZE)
#define MEM_SIZE_PRIMES		(1 + MATRIX_SIZE_MAX_SHIFT - MATRIX_SIZE_MIN_SHIFT)
#define STRESS_CACHE_LINE_SHIFT	(6)	/* Typical 64 byte size */
#define STRESS_CACHE_LINE_SIZE	(1 << STRESS_CACHE_LINE_SHIFT)

typedef void (*stress_memthrash_func_t)(const stress_args_t *args, size_t mem_size);

typedef struct {
	const char		*name;	/* human readable form of stressor */
	stress_memthrash_func_t	func;	/* the method function */
} stress_memthrash_method_info_t;

typedef struct {
	uint32_t total_cpus;
	uint32_t max_threads;
	const stress_memthrash_method_info_t *memthrash_method;
} stress_memthrash_context_t;

/* Per-pthread information */
typedef struct {
	pthread_t pthread;	/* pthread handle */
	int ret;		/* pthread create return value */
} stress_pthread_info_t;

typedef struct {
	size_t	mem_size;	/* memory size */
	size_t  prime_stride;	/* prime cache sized stride */
} stress_memthrash_primes_t;

static const stress_memthrash_method_info_t memthrash_methods[];
static void *mem;
static volatile bool thread_terminate;
static sigset_t set;

static stress_memthrash_primes_t stress_memthrash_primes[MEM_SIZE_PRIMES];

#if (((defined(__GNUC__) || defined(__clang__)) && 	\
       defined(STRESS_ARCH_X86)) ||			\
     (defined(__GNUC__) && 				\
      defined(HAVE_ATOMIC_ADD_FETCH) &&			\
      defined(__ATOMIC_SEQ_CST) &&			\
      NEED_GNUC(4,7,0) && 				\
      defined(STRESS_ARCH_ARM)))

#if defined(HAVE_ATOMIC_ADD_FETCH)
#define MEM_LOCK(ptr, inc) 				\
do {							\
	__atomic_add_fetch(ptr, inc, __ATOMIC_SEQ_CST);	\
} while (0)
#else
#define MEM_LOCK(ptr, inc)				\
do {							\
	asm volatile("lock addl %1,%0" : "+m" (*ptr) : "ir" (inc));	\
} while (0);
#endif
#endif

static inline HOT OPTIMIZE3 void stress_memthrash_random_chunk(
	const size_t chunk_size,
	const size_t mem_size)
{
	uint32_t i;
	const uint32_t max = stress_mwc16();
	size_t chunks = mem_size / chunk_size;

	if (chunks < 1)
		chunks = 1;

	for (i = 0; !thread_terminate && (i < max); i++) {
		const size_t chunk = stress_mwc32() % chunks;
		const size_t offset = chunk * chunk_size;
		void *ptr = (void *)(((uint8_t *)mem) + offset);

#if defined(__GNUC__)
		(void)__builtin_memset(ptr, stress_mwc8(), chunk_size);
#else
		(void)memset(ptr, stress_mwc8(), chunk_size);
#endif
	}
}

static void HOT OPTIMIZE3 stress_memthrash_random_chunkpage(
	const stress_args_t *args,
	const size_t mem_size)
{
	stress_memthrash_random_chunk(args->page_size, mem_size);
}

static void HOT OPTIMIZE3 stress_memthrash_random_chunk256(
	const stress_args_t *args,
	const size_t mem_size)
{
	(void)args;

	stress_memthrash_random_chunk(256, mem_size);
}

static void HOT OPTIMIZE3 stress_memthrash_random_chunk64(
	const stress_args_t *args,
	const size_t mem_size)
{
	(void)args;

	stress_memthrash_random_chunk(64, mem_size);
}

static void HOT OPTIMIZE3 stress_memthrash_random_chunk8(
	const stress_args_t *args,
	const size_t mem_size)
{
	(void)args;

	stress_memthrash_random_chunk(8, mem_size);
}

static void HOT OPTIMIZE3 stress_memthrash_random_chunk1(
	const stress_args_t *args,
	const size_t mem_size)
{
	(void)args;

	stress_memthrash_random_chunk(1, mem_size);
}

static void stress_memthrash_memset(
	const stress_args_t *args,
	const size_t mem_size)
{
	(void)args;

#if defined(__GNUC__)
	(void)__builtin_memset((void *)mem, stress_mwc8(), mem_size);
#else
	(void)memset((void *)mem, stress_mwc8(), mem_size);
#endif
}

static void stress_memthrash_memmove(
	const stress_args_t *args,
	const size_t mem_size)
{
	char *dst = ((char *)mem) + 1;

	(void)args;
#if defined(__GNUC__)
	(void)shim_builtin_memmove((void *)dst, mem, mem_size - 1);
#else
	(void)memmove((void *)dst, mem, mem_size - 1);
#endif
}

static void HOT OPTIMIZE3 stress_memthrash_memset64(
	const stress_args_t *args,
	const size_t mem_size)
{
	register uint64_t *ptr = (uint64_t *)mem;
	register const uint64_t *end = (uint64_t *)(((uintptr_t)mem) + mem_size);
	register uint64_t val = stress_mwc64();

	(void)args;

#if defined(HAVE_NT_STORE64)
	if (stress_cpu_x86_has_sse2()) {
		while (LIKELY(ptr < end)) {
			stress_nt_store64(ptr + 0, val);
			stress_nt_store64(ptr + 1, val);
			stress_nt_store64(ptr + 2, val);
			stress_nt_store64(ptr + 3, val);
			stress_nt_store64(ptr + 4, val);
			stress_nt_store64(ptr + 5, val);
			stress_nt_store64(ptr + 6, val);
			stress_nt_store64(ptr + 7, val);
			ptr += 8;
		}
		return;
	}
#endif
	/* normal temporal stores, non-SSE fallback */

	while (LIKELY(ptr < end)) {
		*ptr++ = val;
		*ptr++ = val;
		*ptr++ = val;
		*ptr++ = val;
		*ptr++ = val;
		*ptr++ = val;
		*ptr++ = val;
		*ptr++ = val;
	}
}

static void OPTIMIZE3 TARGET_CLONES stress_memthrash_swap64(
	const stress_args_t *args,
	const size_t mem_size)
{
	uint64_t *ptr = (uint64_t *)mem;
	register const uint64_t *end = (uint64_t *)(((uintptr_t)mem) + mem_size);

	(void)args;

	while (LIKELY(ptr < end)) {
		register uint64_t r0, r1, r2, r3, r4, r5, r6, r7;

		r0 = ptr[0];
		r1 = ptr[1];
		r2 = ptr[2];
		r3 = ptr[3];
		r4 = ptr[4];
		r5 = ptr[5];
		r6 = ptr[6];
		r7 = ptr[7];
		shim_mb();

		ptr[0] = r4;
		ptr[1] = r5;
		ptr[2] = r6;
		ptr[3] = r7;
		ptr[4] = r0;
		ptr[5] = r1;
		ptr[6] = r2;
		ptr[7] = r3;
		shim_mb();
		ptr += 8;

		r0 = ptr[0];
		r1 = ptr[1];
		r2 = ptr[2];
		r3 = ptr[3];
		r4 = ptr[4];
		r5 = ptr[5];
		r6 = ptr[6];
		r7 = ptr[7];
		shim_mb();

		ptr[0] = r4;
		ptr[1] = r5;
		ptr[2] = r6;
		ptr[3] = r7;
		ptr[4] = r0;
		ptr[5] = r1;
		ptr[6] = r2;
		ptr[7] = r3;
		shim_mb();
		ptr += 8;
	}
}

#if defined(HAVE_INT128_T)
static void OPTIMIZE3 TARGET_CLONES stress_memthrash_copy128(
	const stress_args_t *args,
	const size_t mem_size)
{
	__uint128_t *ptr = (__uint128_t *)mem;
	register const __uint128_t *end = (__uint128_t *)(((uintptr_t)mem) + mem_size - 128);

	(void)args;

	while (LIKELY(ptr < end)) {
		register __uint128_t r0, r1, r2, r3, r4, r5, r6, r7;

		r0 = ptr[8];
		r1 = ptr[9];
		r2 = ptr[10];
		r3 = ptr[11];
		r4 = ptr[12];
		r5 = ptr[13];
		r6 = ptr[14];
		r7 = ptr[15];
		ptr[0] = r0;
		ptr[1] = r1;
		ptr[2] = r2;
		ptr[3] = r3;
		ptr[4] = r4;
		ptr[5] = r5;
		ptr[6] = r6;
		ptr[7] = r7;
		shim_mb();
		ptr += 8;

		r0 = ptr[8];
		r1 = ptr[9];
		r2 = ptr[10];
		r3 = ptr[11];
		r4 = ptr[12];
		r5 = ptr[13];
		r6 = ptr[14];
		r7 = ptr[15];
		ptr[0] = r0;
		ptr[1] = r1;
		ptr[2] = r2;
		ptr[3] = r3;
		ptr[4] = r4;
		ptr[5] = r5;
		ptr[6] = r6;
		ptr[7] = r7;
		shim_mb();
		ptr += 8;
	}
}
#endif

static void HOT OPTIMIZE3 stress_memthrash_flip_mem(
	const stress_args_t *args,
	const size_t mem_size)
{
	volatile uint64_t *ptr = (volatile uint64_t *)mem;
	const uint64_t *end = (uint64_t *)(((uintptr_t)mem) + mem_size);

	(void)args;

	while (LIKELY(ptr < end)) {
		*ptr = *ptr ^ ~0ULL;
		ptr++;
	}
}

static void HOT OPTIMIZE3 stress_memthrash_swap(
	const stress_args_t *args,
	const size_t mem_size)
{
	size_t i;
	register size_t offset1 = stress_mwc32() % mem_size;
	register size_t offset2 = stress_mwc32() % mem_size;
	uint8_t *mem_u8 = (uint8_t *)mem;

	(void)args;

	for (i = 0; !thread_terminate && (i < 65536); i++) {
		register uint8_t tmp;

		tmp = mem_u8[offset1];
		mem_u8[offset1] = mem_u8[offset2];
		mem_u8[offset2] = tmp;

		offset1 += 129;
		if (offset1 >= mem_size)
			offset1 -= mem_size;
		offset2 += 65;
		if (offset2 >= mem_size)
			offset2 -= mem_size;
	}
}

static void HOT OPTIMIZE3 stress_memthrash_matrix(
	const stress_args_t *args,
	const size_t mem_size)
{
	size_t i, j;
	volatile uint8_t *vmem = mem;

	(void)args;
	(void)mem_size;

	for (i = 0; !thread_terminate && (i < MATRIX_SIZE); i+= ((stress_mwc8() & 0xf) + 1)) {
		for (j = 0; j < MATRIX_SIZE; j+= 16) {
			size_t i1 = (i * MATRIX_SIZE) + j;
			size_t i2 = (j * MATRIX_SIZE) + i;
			uint8_t tmp;

			tmp = vmem[i1];
			vmem[i1] = vmem[i2];
			vmem[i2] = tmp;
		}
	}
}

static void HOT OPTIMIZE3 stress_memthrash_prefetch(
	const stress_args_t *args,
	const size_t mem_size)
{
	uint32_t i;
	const uint32_t max = stress_mwc16();

	(void)args;

	for (i = 0; !thread_terminate && (i < max); i++) {
		size_t offset = stress_mwc32() % mem_size;
		uint8_t *const ptr = ((uint8_t *)mem) + offset;
		volatile uint8_t *const vptr = ptr;

		shim_builtin_prefetch(ptr, 1, 1);
		//(void)*vptr;
		*vptr = i & 0xff;
	}
}

#if defined(HAVE_ASM_X86_CLFLUSH)
static void HOT OPTIMIZE3 stress_memthrash_flush(
	const stress_args_t *args,
	const size_t mem_size)
{
	uint32_t i;
	const uint32_t max = stress_mwc16();

	(void)args;

	for (i = 0; !thread_terminate && (i < max); i++) {
		size_t offset = stress_mwc32() % mem_size;
		uint8_t *const ptr = ((uint8_t *)mem) + offset;
		volatile uint8_t *const vptr = ptr;

		*vptr = i & 0xff;
		shim_clflush(ptr);
	}
}
#endif

static void HOT OPTIMIZE3 stress_memthrash_mfence(
	const stress_args_t *args,
	const size_t mem_size)
{
	uint32_t i;
	const uint32_t max = stress_mwc16();

	(void)args;

	for (i = 0; !thread_terminate && (i < max); i++) {
		size_t offset = stress_mwc32() % mem_size;
		volatile uint8_t *ptr = ((uint8_t *)mem) + offset;

		*ptr = i & 0xff;
		shim_mfence();
	}
}

#if defined(MEM_LOCK)
static void HOT OPTIMIZE3 stress_memthrash_lock(
	const stress_args_t *args,
	const size_t mem_size)
{
	uint32_t i;

	(void)args;

	for (i = 0; !thread_terminate && (i < 64); i++) {
		size_t offset = stress_mwc32() % mem_size;
		volatile uint8_t *ptr = ((uint8_t *)mem) + offset;

		MEM_LOCK(ptr, 1);
	}
}
#endif

static void HOT OPTIMIZE3 stress_memthrash_spinread(
	const stress_args_t *args,
	const size_t mem_size)
{
	uint32_t i;
	volatile uint32_t *ptr;
	const size_t size = mem_size - (8 * sizeof(*ptr));
	const size_t offset = (stress_mwc32() % size) & ~(size_t)3;

	ptr = (uint32_t *)(((uintptr_t)mem) + offset);
	(void)args;

	for (i = 0; !thread_terminate && (i < 65536); i++) {
		(void)*ptr;
		(void)*ptr;
		(void)*ptr;
		(void)*ptr;

		(void)*ptr;
		(void)*ptr;
		(void)*ptr;
		(void)*ptr;
	}
}

static void HOT OPTIMIZE3 stress_memthrash_spinwrite(
	const stress_args_t *args,
	const size_t mem_size)
{
	uint32_t i;
	volatile uint32_t *ptr;
	const size_t size = mem_size - (8 * sizeof(*ptr));
	const size_t offset = (stress_mwc32() % size) & ~(size_t)3;

	ptr = (uint32_t *)(((uintptr_t)mem) + offset);
	(void)args;

	for (i = 0; !thread_terminate && (i < 65536); i++) {
		*ptr = i;
		*ptr = i;
		*ptr = i;
		*ptr = i;

		*ptr = i;
		*ptr = i;
		*ptr = i;
		*ptr = i;
	}
}

static void HOT OPTIMIZE3 stress_memthrash_tlb(
	const stress_args_t *args,
	const size_t mem_size)
{
	const size_t cache_lines = mem_size >> STRESS_CACHE_LINE_SHIFT;
	const size_t mask = mem_size - 1;		/* assuming mem_size is a power of 2 */
	const size_t offset = (size_t)stress_mwc16() & (STRESS_CACHE_LINE_SIZE - 1);
	size_t prime_stride = 65537 * STRESS_CACHE_LINE_SIZE;	/* prime default */
	register int i;
	volatile uint8_t *ptr;
	register size_t j, k;

	(void)args;

	/* Find size of stride for the given memory size */
	for (i = 0; i < MEM_SIZE_PRIMES; i++) {
		if (mem_size == stress_memthrash_primes[i].mem_size) {
			prime_stride = stress_memthrash_primes[i].prime_stride;
			break;
		}
	}

	/* Stride around memory in prime cache line strides, reads */
	for (j = 0, k = offset; j < cache_lines; j++) {
		ptr = (volatile uint8_t *)mem + k;
		(void)*ptr;
		k = (k + prime_stride) & mask;
	}
	/* Stride around memory in prime cache line strides, writes */
	for (j = 0, k = offset; j < cache_lines; j++) {
		ptr = (volatile uint8_t *)mem + k;
		*ptr = j;
		k = (k + prime_stride) & mask;
	}
}

static void OPTIMIZE3 TARGET_CLONES stress_memthrash_swapfwdrev(
	const stress_args_t *args,
	const size_t mem_size)
{
	uint64_t *fwd, *rev, *end = (uint64_t *)((uintptr_t)mem + mem_size);

	(void)args;
	for (fwd = (uint64_t *)mem, rev = end - 1; fwd < end; rev--, fwd++) {
		register uint64_t tmp;

		tmp = *fwd;
		*fwd = *rev;
		*rev = tmp;
	}
	for (fwd = (uint64_t *)mem, rev = end - 1; fwd < end; rev--, fwd++) {
		register uint64_t tmp;

		tmp = *rev;
		*rev = *fwd;
		*fwd = tmp;
	}
}


static void stress_memthrash_all(const stress_args_t *args, size_t mem_size);
static void stress_memthrash_random(const stress_args_t *args, size_t mem_size);

static const stress_memthrash_method_info_t memthrash_methods[] = {
	{ "all",	stress_memthrash_all },		/* MUST always be first! */

	{ "chunk1",	stress_memthrash_random_chunk1 },
	{ "chunk8",	stress_memthrash_random_chunk8 },
	{ "chunk64",	stress_memthrash_random_chunk64 },
	{ "chunk256",	stress_memthrash_random_chunk256 },
	{ "chunkpage",	stress_memthrash_random_chunkpage },
#if defined(HAVE_INT128_T)
	{ "copy128",	stress_memthrash_copy128 },
#endif
	{ "flip",	stress_memthrash_flip_mem },
#if defined(HAVE_ASM_X86_CLFLUSH)
	{ "flush",	stress_memthrash_flush },
#endif
#if defined(MEM_LOCK)
	{ "lock",	stress_memthrash_lock },
#endif
	{ "matrix",	stress_memthrash_matrix },
	{ "memmove",	stress_memthrash_memmove },
	{ "memset",	stress_memthrash_memset },
	{ "memset64",	stress_memthrash_memset64 },
	{ "mfence",	stress_memthrash_mfence },
	{ "prefetch",	stress_memthrash_prefetch },
	{ "random",	stress_memthrash_random },
	{ "spinread",	stress_memthrash_spinread },
	{ "spinwrite",	stress_memthrash_spinwrite },
	{ "swap",	stress_memthrash_swap },
	{ "swap64",	stress_memthrash_swap64 },
	{ "swapfwdrev",	stress_memthrash_swapfwdrev },
	{ "tlb",	stress_memthrash_tlb },
};

static void stress_memthrash_all(const stress_args_t *args, size_t mem_size)
{
	static size_t i = 1;
	const double t = stress_time_now();

	do {
		memthrash_methods[i].func(args, mem_size);
	} while (!thread_terminate && (stress_time_now() - t < 0.01));

	i++;
	if (UNLIKELY(i >= SIZEOF_ARRAY(memthrash_methods)))
		i = 1;
}

static void stress_memthrash_random(const stress_args_t *args, size_t mem_size)
{
	/* loop until we find a good candidate */
	for (;;) {
		size_t i = stress_mwc8() % SIZEOF_ARRAY(memthrash_methods);
		const stress_memthrash_func_t func = (stress_memthrash_func_t)memthrash_methods[i].func;

		/* Don't run stress_memthrash_random/all to avoid recursion */
		if ((func != stress_memthrash_random) &&
		    (func != stress_memthrash_all)) {
			func(args, mem_size);
			return;
		}
	}
}

/*
 *  stress_set_memthrash_method()
 *	set the default memthresh method
 */
static int stress_set_memthrash_method(const char *name)
{
	size_t i;

	for (i = 0; i < SIZEOF_ARRAY(memthrash_methods); i++) {
		const stress_memthrash_method_info_t *info = &memthrash_methods[i];
		if (!strcmp(memthrash_methods[i].name, name)) {
			stress_set_setting("memthrash-method", TYPE_ID_UINTPTR_T, &info);
			return 0;
		}
	}

	(void)fprintf(stderr, "memthrash-method must be one of:");
	for (i = 0; i < SIZEOF_ARRAY(memthrash_methods); i++) {
		(void)fprintf(stderr, " %s", memthrash_methods[i].name);
	}
	(void)fprintf(stderr, "\n");

	return -1;
}

static void stress_memthrash_find_primes(void)
{
	size_t i;

	for (i = 0; i < MEM_SIZE_PRIMES; i++) {
		const size_t mem_size = 1 << (2 * (i + MATRIX_SIZE_MIN_SHIFT));
		const size_t cache_lines = (mem_size / STRESS_CACHE_LINE_SIZE) + 137;

		stress_memthrash_primes[i].mem_size = mem_size;
		stress_memthrash_primes[i].prime_stride =
			(size_t)stress_get_prime64((uint64_t)cache_lines) * STRESS_CACHE_LINE_SIZE;
	}
}

/*
 *  stress_memthrash_func()
 *	pthread that exits immediately
 */
static void *stress_memthrash_func(void *arg)
{
	static void *nowt = NULL;
	const stress_pthread_args_t *parg = (stress_pthread_args_t *)arg;
	const stress_args_t *args = parg->args;
	const stress_memthrash_func_t func = (stress_memthrash_func_t)parg->data;

	/*
	 *  Block all signals, let controlling thread
	 *  handle these
	 */
	(void)sigprocmask(SIG_BLOCK, &set, NULL);

	while (!thread_terminate && keep_stressing(args)) {
		size_t j;

		for (j = MATRIX_SIZE_MIN_SHIFT; j <= MATRIX_SIZE_MAX_SHIFT &&
		     !thread_terminate && keep_stressing(args); j++) {
			size_t mem_size = 1 << (2 * j);

			size_t i;
			for (i = 0; i < SIZEOF_ARRAY(memthrash_methods); i++)
				if (func == memthrash_methods[i].func)
					break;
			func(args, mem_size);
			inc_counter(args);
			shim_sched_yield();
		}
	}

	/* Wake parent up, all done! */
	(void)kill(args->pid, SIGALRM);
	return &nowt;
}

static inline uint32_t stress_memthrash_max(
	const uint32_t instances,
	const uint32_t total_cpus)
{
	if ((instances >= total_cpus) || (instances == 0)) {
		return 1;
	} else {
		uint32_t max = total_cpus / instances;

		return ((total_cpus % instances) == 0) ? max : max + 1;
	}
}

static inline uint32_t stress_memthash_optimal(
	const uint32_t instances,
	const uint32_t total_cpus)
{
	uint32_t n = instances;

	while (n > 1) {
		if (total_cpus % n == 0)
			return n;
		n--;
	}
	return 1;
}

static inline char *plural(uint32_t n)
{
	return n > 1 ? "s" : "";
}

static void stress_memthrash_sigalrm_handler(int signum)
{
	(void)signum;

	thread_terminate = true;
}

static int stress_memthrash_child(const stress_args_t *args, void *ctxt)
{
	stress_memthrash_context_t *context = (stress_memthrash_context_t *)ctxt;
	const uint32_t max_threads = context->max_threads;
	uint32_t i;
	int ret;
	stress_pthread_args_t pargs;
	stress_pthread_info_t *pthread_info;

	int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_POPULATE)
	flags |= MAP_POPULATE;
#endif

	pthread_info = calloc(max_threads, sizeof(*pthread_info));
	if (!pthread_info) {
		pr_inf("%s: failed to allocate pthread information array\n",
			args->name);
		return EXIT_NO_RESOURCE;
	}

	ret = stress_sighandler(args->name, SIGALRM, stress_memthrash_sigalrm_handler, NULL);
	(void)ret;

	pargs.args = args;
	pargs.data = (void *)context->memthrash_method->func;

mmap_retry:
	mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, flags, -1, 0);
	if (mem == MAP_FAILED) {
#if defined(MAP_POPULATE)
		flags &= ~MAP_POPULATE;	/* Less aggressive, more OOMable */
#endif
		if (!keep_stressing_flag()) {
			pr_dbg("%s: mmap failed: %d %s\n",
				args->name, errno, strerror(errno));
			free(pthread_info);
			return EXIT_NO_RESOURCE;
		}
		(void)shim_usleep(100000);
		if (!keep_stressing_flag())
			goto reap_mem;
		goto mmap_retry;
	}

	for (i = 0; i < max_threads; i++) {
		pthread_info[i].ret = pthread_create(&pthread_info[i].pthread,
						NULL, stress_memthrash_func,
						(void *)&pargs);
		if (pthread_info[i].ret) {
			ret = pthread_info[i].ret;

			/* Just give up and go to next thread */
			if (ret == EAGAIN)
				continue;
			/* Something really unexpected */
			pr_fail("%s: pthread create failed, errno=%d (%s)\n",
				args->name, ret, strerror(ret));
			goto reap;
		}
		if (!keep_stressing_flag())
			goto reap;
	}
	/* Wait for SIGALRM or SIGINT/SIGHUP etc */
	(void)pause();

reap:
	thread_terminate = true;
	for (i = 0; i < max_threads; i++) {
		if (!pthread_info[i].ret) {
			pthread_info[i].ret = pthread_join(pthread_info[i].pthread, NULL);
			if (pthread_info[i].ret && (pthread_info[i].ret != ESRCH)) {
				pr_fail("%s: pthread join failed, errno=%d (%s)\n",
					args->name, pthread_info[i].ret, strerror(pthread_info[i].ret));
			}
		}
	}
reap_mem:
	(void)munmap(mem, MEM_SIZE);
	free(pthread_info);

	return EXIT_SUCCESS;
}


/*
 *  stress_memthrash()
 *	stress by creating pthreads
 */
static int stress_memthrash(const stress_args_t *args)
{
	stress_memthrash_context_t context;
	int rc;

	stress_memthrash_find_primes();

	context.total_cpus = (uint32_t)stress_get_processors_online();
	context.max_threads = stress_memthrash_max(args->num_instances, context.total_cpus);
	context.memthrash_method = &memthrash_methods[0];

	(void)stress_get_setting("memthrash-method", &context.memthrash_method);

	pr_dbg("%s: using method '%s'\n", args->name, context.memthrash_method->name);
	if (args->instance == 0) {
		pr_inf("%s: starting %" PRIu32 " thread%s on each of the %"
			PRIu32 " stressors on a %" PRIu32 " CPU system\n",
			args->name, context.max_threads, plural(context.max_threads),
			args->num_instances, context.total_cpus);
		if (context.max_threads * args->num_instances > context.total_cpus) {
			pr_inf("%s: this is not an optimal choice of stressors, "
				"try %" PRIu32 " instead\n",
			args->name,
			stress_memthash_optimal(args->num_instances, context.total_cpus));
		}
	}

	(void)sigfillset(&set);

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	rc = stress_oomable_child(args, &context, stress_memthrash_child, STRESS_OOMABLE_NORMAL);

	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	return rc;
}

static const stress_opt_set_func_t opt_set_funcs[] = {
	{ OPT_memthrash_method,	stress_set_memthrash_method },
	{ 0,			NULL }
};

stressor_info_t stress_memthrash_info = {
	.stressor = stress_memthrash,
	.class = CLASS_MEMORY,
	.opt_set_funcs = opt_set_funcs,
	.help = help
};
#else

static int stress_set_memthrash_method(const char *name)
{
	(void)name;

	(void)pr_inf("warning: --memthrash-method not available on this system\n");
	return 0;
}

static const stress_opt_set_func_t opt_set_funcs[] = {
	{ OPT_memthrash_method,	stress_set_memthrash_method },
	{ 0,			NULL }
};

stressor_info_t stress_memthrash_info = {
	.stressor = stress_not_implemented,
	.class = CLASS_MEMORY,
	.opt_set_funcs = opt_set_funcs,
	.help = help
};
#endif
