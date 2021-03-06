#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <pthread.h>

#include "third_party/libdivide.h"

#include "config.h"
#include "malloc.h"
#include "mutex.h"
#include "memory.h"
#include "pages.h"
#include "random.h"
#include "util.h"

static_assert(sizeof(void *) == 8, "64-bit only");

static_assert(!WRITE_AFTER_FREE_CHECK || ZERO_ON_FREE, "WRITE_AFTER_FREE_CHECK depends on ZERO_ON_FREE");

// either sizeof(uint64_t) or 0
static const size_t canary_size = SLAB_CANARY ? sizeof(uint64_t) : 0;

#define CACHELINE_SIZE 64

static union {
    struct {
        void *slab_region_start;
        void *slab_region_end;
        struct region_info *regions[2];
        atomic_bool initialized;
    };
    char padding[PAGE_SIZE];
} ro __attribute__((aligned(PAGE_SIZE))) = {
    .initialized = ATOMIC_VAR_INIT(false)
};

struct slab_metadata {
    uint64_t bitmap;
    struct slab_metadata *next;
    struct slab_metadata *prev;
    uint64_t canary_value;
};

static const size_t min_align = 16;
static const size_t max_slab_size_class = 16384;

static const uint16_t size_classes[] = {
    /* 0 */ 0,
    /* 16 */ 16, 32, 48, 64, 80, 96, 112, 128,
    /* 32 */ 160, 192, 224, 256,
    /* 64 */ 320, 384, 448, 512,
    /* 128 */ 640, 768, 896, 1024,
    /* 256 */ 1280, 1536, 1792, 2048,
    /* 512 */ 2560, 3072, 3584, 4096,
    /* 1024 */ 5120, 6144, 7168, 8192,
    /* 2048 */ 10240, 12288, 14336, 16384
};

static const uint16_t size_class_slots[] = {
    /* 0 */ 256,
    /* 16 */ 256, 128, 85, 64, 51, 42, 36, 64,
    /* 32 */ 51, 64, 54, 64,
    /* 64 */ 64, 64, 64, 64,
    /* 128 */ 64, 64, 64, 64,
    /* 256 */ 16, 16, 16, 16,
    /* 512 */ 8, 8, 8, 8,
    /* 1024 */ 8, 8, 8, 8,
    /* 2048 */ 6, 5, 4, 4
};

#define N_SIZE_CLASSES (sizeof(size_classes) / sizeof(size_classes[0]))

struct size_info {
    size_t size;
    size_t class;
};

static inline struct size_info get_size_info(size_t size) {
    if (size == 0) {
        return (struct size_info){0, 0};
    }
    if (size <= 128) {
        return (struct size_info){(size + 15) & ~15, ((size - 1) >> 4) + 1};
    }
    for (unsigned class = 9; class < N_SIZE_CLASSES; class++) {
        size_t real_size = size_classes[class];
        if (size <= real_size) {
            return (struct size_info){real_size, class};
        }
    }
    fatal_error("invalid size for slabs");
}

// alignment must be a power of 2 <= PAGE_SIZE since slabs are only page aligned
static inline struct size_info get_size_info_align(size_t size, size_t alignment) {
    for (unsigned class = 1; class < N_SIZE_CLASSES; class++) {
        size_t real_size = size_classes[class];
        if (size <= real_size && !(real_size & (alignment - 1))) {
            return (struct size_info){real_size, class};
        }
    }
    fatal_error("invalid size for slabs");
}

static size_t get_slab_size(size_t slots, size_t size) {
    return PAGE_CEILING(slots * size);
}

// limit on the number of cached empty slabs before attempting purging instead
static const size_t max_empty_slabs_total = 64 * 1024;

static struct size_class {
    struct mutex lock;
    void *class_region_start;
    struct slab_metadata *slab_info;

    // slabs with at least one allocated slot and at least one free slot
    //
    // LIFO doubly-linked list
    struct slab_metadata *partial_slabs;

    // slabs without allocated slots that are cached for near-term usage
    //
    // LIFO singly-linked list
    struct slab_metadata *empty_slabs;
    size_t empty_slabs_total; // length * slab_size

    // slabs without allocated slots that are purged and memory protected
    //
    // FIFO singly-linked list
    struct slab_metadata *free_slabs_head;
    struct slab_metadata *free_slabs_tail;

    struct libdivide_u32_t size_divisor;
    struct libdivide_u64_t slab_size_divisor;
    struct random_state rng;
    size_t metadata_allocated;
    size_t metadata_count;
} __attribute__((aligned(CACHELINE_SIZE))) size_class_metadata[N_SIZE_CLASSES];

static const size_t class_region_size = 128ULL * 1024 * 1024 * 1024;
static const size_t real_class_region_size = class_region_size * 2;
static const size_t slab_region_size = real_class_region_size * N_SIZE_CLASSES;
static_assert(PAGE_SIZE == 4096, "bitmap handling will need adjustment for other page sizes");

static void *get_slab(struct size_class *c, size_t slab_size, struct slab_metadata *metadata) {
    size_t index = metadata - c->slab_info;
    return (char *)c->class_region_start + (index * slab_size);
}

static size_t get_metadata_max(size_t slab_size) {
    return class_region_size / slab_size;
}

static struct slab_metadata *alloc_metadata(struct size_class *c, size_t slab_size, bool non_zero_size) {
    if (unlikely(c->metadata_count >= c->metadata_allocated)) {
        size_t metadata_max = get_metadata_max(slab_size);
        if (c->metadata_count >= metadata_max) {
            errno = ENOMEM;
            return NULL;
        }
        size_t allocate = c->metadata_allocated * 2;
        if (allocate > metadata_max) {
            allocate = metadata_max;
        }
        if (memory_protect_rw(c->slab_info, allocate * sizeof(struct slab_metadata))) {
            return NULL;
        }
        c->metadata_allocated = allocate;
    }

    struct slab_metadata *metadata = c->slab_info + c->metadata_count;
    void *slab = get_slab(c, slab_size, metadata);
    if (non_zero_size && memory_protect_rw(slab, slab_size)) {
        return NULL;
    }
    c->metadata_count++;
    if (GUARD_SLABS) {
        c->metadata_count++;
    }
    return metadata;
}

static void check_index(size_t index) {
    if (index >= 64) {
        fatal_error("invalid index");
    }
}

static void set_slot(struct slab_metadata *metadata, size_t index) {
    check_index(index);
    metadata->bitmap |= 1UL << index;
}

static void clear_slot(struct slab_metadata *metadata, size_t index) {
    check_index(index);
    metadata->bitmap &= ~(1UL << index);
}

static bool get_slot(struct slab_metadata *metadata, size_t index) {
    check_index(index);
    return (metadata->bitmap >> index) & 1UL;
}

static uint64_t get_mask(size_t slots) {
    return slots < 64 ? ~0UL << slots : 0;
}

static size_t get_free_slot(struct random_state *rng, size_t slots, struct slab_metadata *metadata) {
    if (slots > 64) {
        slots = 64;
    }

    uint64_t masked = metadata->bitmap | get_mask(slots);
    if (masked == ~0UL) {
        fatal_error("no zero bits");
    }

    if (SLOT_RANDOMIZE) {
        // randomize start location for linear search (uniform random choice is too slow)
        uint64_t random_split = ~(~0UL << get_random_u16_uniform(rng, slots));

        size_t slot = ffzl(masked | random_split);
        if (slot) {
            return slot - 1;
        }
    }

    return ffzl(masked) - 1;
}

static bool has_free_slots(size_t slots, struct slab_metadata *metadata) {
    if (slots > 64) {
        slots = 64;
    }

    uint64_t masked = metadata->bitmap | get_mask(slots);
    return masked != ~0UL;
}

static bool is_free_slab(struct slab_metadata *metadata) {
    return !metadata->bitmap;
}

static struct slab_metadata *get_metadata(struct size_class *c, void *p) {
    size_t offset = (char *)p - (char *)c->class_region_start;
    size_t index = libdivide_u64_do(offset, &c->slab_size_divisor);
    // still caught without this check either as a read access violation or "double free"
    if (index >= c->metadata_allocated) {
        fatal_error("invalid free within a slab yet to be used");
    }
    return c->slab_info + index;
}

static void *slot_pointer(size_t size, void *slab, size_t slot) {
    return (char *)slab + slot * size;
}

static void write_after_free_check(char *p, size_t size) {
    if (!WRITE_AFTER_FREE_CHECK) {
        return;
    }

    for (size_t i = 0; i < size; i += sizeof(uint64_t)) {
        if (*(uint64_t *)(p + i)) {
            fatal_error("detected write after free");
        }
    }
}

static const uint64_t canary_mask = __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ ?
    0xffffffffffffff00UL :
    0x00ffffffffffffffUL;

static void set_canary(struct slab_metadata *metadata, void *p, size_t size) {
    memcpy((char *)p + size - canary_size, &metadata->canary_value, canary_size);
}

static inline void *allocate_small(size_t requested_size) {
    struct size_info info = get_size_info(requested_size);
    size_t size = info.size ? info.size : 16;
    struct size_class *c = &size_class_metadata[info.class];
    size_t slots = size_class_slots[info.class];
    size_t slab_size = get_slab_size(slots, size);

    mutex_lock(&c->lock);

    if (c->partial_slabs == NULL) {
        if (c->empty_slabs != NULL) {
            struct slab_metadata *metadata = c->empty_slabs;
            c->empty_slabs = c->empty_slabs->next;
            c->empty_slabs_total -= slab_size;

            metadata->next = NULL;
            metadata->prev = NULL;

            c->partial_slabs = metadata;

            void *slab = get_slab(c, slab_size, metadata);
            size_t slot = get_free_slot(&c->rng, slots, metadata);
            set_slot(metadata, slot);
            void *p = slot_pointer(size, slab, slot);
            if (requested_size) {
                write_after_free_check(p, size - canary_size);
                set_canary(metadata, p, size);
            }

            mutex_unlock(&c->lock);
            return p;
        } else if (c->free_slabs_head != NULL) {
            struct slab_metadata *metadata = c->free_slabs_head;
            metadata->canary_value = get_random_u64(&c->rng);

            void *slab = get_slab(c, slab_size, metadata);
            if (requested_size && memory_protect_rw(slab, slab_size)) {
                mutex_unlock(&c->lock);
                return NULL;
            }

            c->free_slabs_head = c->free_slabs_head->next;
            if (c->free_slabs_head == NULL) {
                c->free_slabs_tail = NULL;
            }

            metadata->next = NULL;
            metadata->prev = NULL;

            c->partial_slabs = metadata;

            size_t slot = get_free_slot(&c->rng, slots, metadata);
            set_slot(metadata, slot);
            void *p = slot_pointer(size, slab, slot);
            if (requested_size) {
                set_canary(metadata, p, size);
            }

            mutex_unlock(&c->lock);
            return p;
        }

        struct slab_metadata *metadata = alloc_metadata(c, slab_size, requested_size);
        if (unlikely(metadata == NULL)) {
            mutex_unlock(&c->lock);
            return NULL;
        }
        metadata->canary_value = get_random_u64(&c->rng) & canary_mask;

        c->partial_slabs = metadata;
        void *slab = get_slab(c, slab_size, metadata);
        size_t slot = get_free_slot(&c->rng, slots, metadata);
        set_slot(metadata, slot);
        void *p = slot_pointer(size, slab, slot);
        if (requested_size) {
            set_canary(metadata, p, size);
        }

        mutex_unlock(&c->lock);
        return p;
    }

    struct slab_metadata *metadata = c->partial_slabs;
    size_t slot = get_free_slot(&c->rng, slots, metadata);
    set_slot(metadata, slot);

    if (!has_free_slots(slots, metadata)) {
        c->partial_slabs = c->partial_slabs->next;
        if (c->partial_slabs) {
            c->partial_slabs->prev = NULL;
        }
    }

    void *slab = get_slab(c, slab_size, metadata);
    void *p = slot_pointer(size, slab, slot);
    if (requested_size) {
        write_after_free_check(p, size - canary_size);
        set_canary(metadata, p, size);
    }

    mutex_unlock(&c->lock);
    return p;
}

static size_t slab_size_class(void *p) {
    size_t offset = (char *)p - (char *)ro.slab_region_start;
    return offset / real_class_region_size;
}

static size_t slab_usable_size(void *p) {
    return size_classes[slab_size_class(p)];
}

static void enqueue_free_slab(struct size_class *c, struct slab_metadata *metadata) {
    metadata->next = NULL;

    if (c->free_slabs_tail != NULL) {
        c->free_slabs_tail->next = metadata;
    } else {
        c->free_slabs_head = metadata;
    }
    c->free_slabs_tail = metadata;
}

static inline void deallocate_small(void *p, size_t *expected_size) {
    size_t class = slab_size_class(p);

    struct size_class *c = &size_class_metadata[class];
    size_t size = size_classes[class];
    if (expected_size && size != *expected_size) {
        fatal_error("sized deallocation mismatch");
    }
    bool is_zero_size = size == 0;
    if (is_zero_size) {
        size = 16;
    }
    size_t slots = size_class_slots[class];
    size_t slab_size = get_slab_size(slots, size);

    mutex_lock(&c->lock);

    struct slab_metadata *metadata = get_metadata(c, p);
    void *slab = get_slab(c, slab_size, metadata);
    size_t slot = libdivide_u32_do((char *)p - (char *)slab, &c->size_divisor);

    if (slot_pointer(size, slab, slot) != p) {
        fatal_error("invalid unaligned free");
    }

    if (!get_slot(metadata, slot)) {
        fatal_error("double free");
    }

    if (!is_zero_size) {
        if (ZERO_ON_FREE) {
            memset(p, 0, size - canary_size);
        }

        if (canary_size) {
            uint64_t canary_value;
            memcpy(&canary_value, (char *)p + size - canary_size, canary_size);
            if (unlikely(canary_value != metadata->canary_value)) {
                fatal_error("canary corrupted");
            }
        }
    }

    if (!has_free_slots(slots, metadata)) {
        metadata->next = c->partial_slabs;
        metadata->prev = NULL;

        if (c->partial_slabs) {
            c->partial_slabs->prev = metadata;
        }
        c->partial_slabs = metadata;
    }

    clear_slot(metadata, slot);

    if (is_free_slab(metadata)) {
        if (metadata->prev) {
            metadata->prev->next = metadata->next;
        } else {
            c->partial_slabs = metadata->next;
        }
        if (metadata->next) {
            metadata->next->prev = metadata->prev;
        }

        metadata->prev = NULL;

        if (c->empty_slabs_total + slab_size > max_empty_slabs_total) {
            if (!memory_map_fixed(slab, slab_size)) {
                enqueue_free_slab(c, metadata);
                mutex_unlock(&c->lock);
                return;
            }
            // handle out-of-memory by just putting it into the empty slabs list
        }

        metadata->next = c->empty_slabs;
        c->empty_slabs = metadata;
        c->empty_slabs_total += slab_size;
    }

    mutex_unlock(&c->lock);
}

struct region_info {
    void *p;
    size_t size;
    size_t guard_size;
};

static const size_t initial_region_table_size = 256;
static const size_t max_region_table_size = class_region_size / PAGE_SIZE;

static struct random_state regions_rng;
static struct region_info *regions;
static size_t regions_total = initial_region_table_size;
static size_t regions_free = initial_region_table_size;
static struct mutex regions_lock = MUTEX_INITIALIZER;

static size_t hash_page(void *p) {
    uintptr_t u = (uintptr_t)p >> PAGE_SHIFT;
    size_t sum = u;
    sum = (sum << 7) - sum + (u >> 16);
    sum = (sum << 7) - sum + (u >> 32);
    sum = (sum << 7) - sum + (u >> 48);
    return sum;
}

static int regions_grow(void) {
    if (regions_total > SIZE_MAX / sizeof(struct region_info) / 2) {
        return 1;
    }

    size_t newtotal = regions_total * 2;
    size_t newsize = newtotal * sizeof(struct region_info);
    size_t mask = newtotal - 1;

    if (newtotal > max_region_table_size) {
        return 1;
    }

    struct region_info *p = regions == ro.regions[0] ?
        ro.regions[1] : ro.regions[0];

    if (memory_protect_rw(p, newsize)) {
        return 1;
    }

    for (size_t i = 0; i < regions_total; i++) {
        void *q = regions[i].p;
        if (q != NULL) {
            size_t index = hash_page(q) & mask;
            while (p[index].p != NULL) {
                index = (index - 1) & mask;
            }
            p[index] = regions[i];
        }
    }

    memory_map_fixed(regions, regions_total * sizeof(struct region_info));
    regions_free = regions_free + regions_total;
    regions_total = newtotal;
    regions = p;
    return 0;
}

static int regions_insert(void *p, size_t size, size_t guard_size) {
    if (regions_free * 4 < regions_total) {
        if (regions_grow()) {
            return 1;
        }
    }

    size_t mask = regions_total - 1;
    size_t index = hash_page(p) & mask;
    void *q = regions[index].p;
    while (q != NULL) {
        index = (index - 1) & mask;
        q = regions[index].p;
    }
    regions[index].p = p;
    regions[index].size = size;
    regions[index].guard_size = guard_size;
    regions_free--;
    return 0;
}

static struct region_info *regions_find(void *p) {
    size_t mask = regions_total - 1;
    size_t index = hash_page(p) & mask;
    void *r = regions[index].p;
    while (r != p && r != NULL) {
        index = (index - 1) & mask;
        r = regions[index].p;
    }
    return (r == p && r != NULL) ? &regions[index] : NULL;
}

static void regions_delete(struct region_info *region) {
    size_t mask = regions_total - 1;

    regions_free++;

    size_t i = region - regions;
    for (;;) {
        regions[i].p = NULL;
        regions[i].size = 0;
        size_t j = i;
        for (;;) {
            i = (i - 1) & mask;
            if (regions[i].p == NULL) {
                return;
            }
            size_t r = hash_page(regions[i].p) & mask;
            if ((i <= r && r < j) || (r < j && j < i) || (j < i && i <= r)) {
                continue;
            }
            regions[j] = regions[i];
            break;
        }
    }
}

static void full_lock(void) {
    mutex_lock(&regions_lock);
    for (unsigned class = 0; class < N_SIZE_CLASSES; class++) {
        mutex_lock(&size_class_metadata[class].lock);
    }
}

static void full_unlock(void) {
    mutex_unlock(&regions_lock);
    for (unsigned class = 0; class < N_SIZE_CLASSES; class++) {
        mutex_unlock(&size_class_metadata[class].lock);
    }
}

static void post_fork_child(void) {
    mutex_init(&regions_lock);
    random_state_init(&regions_rng);
    for (unsigned class = 0; class < N_SIZE_CLASSES; class++) {
        struct size_class *c = &size_class_metadata[class];
        mutex_init(&c->lock);
        random_state_init(&c->rng);
    }
}

static inline bool is_init(void) {
    return atomic_load_explicit(&ro.initialized, memory_order_acquire);
}

static inline void enforce_init(void) {
    if (!is_init()) {
        fatal_error("invalid uninitialized allocator usage");
    }
}

COLD static void init_slow_path(void) {
    static struct mutex lock = MUTEX_INITIALIZER;

    mutex_lock(&lock);

    if (is_init()) {
        mutex_unlock(&lock);
        return;
    }

    if (sysconf(_SC_PAGESIZE) != PAGE_SIZE) {
        fatal_error("page size mismatch");
    }

    random_state_init(&regions_rng);
    for (unsigned i = 0; i < 2; i++) {
        ro.regions[i] = allocate_pages(max_region_table_size, PAGE_SIZE, false);
        if (ro.regions[i] == NULL) {
            fatal_error("failed to reserve memory for regions table");
        }
    }
    regions = ro.regions[0];
    if (memory_protect_rw(regions, regions_total * sizeof(struct region_info))) {
        fatal_error("failed to unprotect memory for regions table");
    }

    ro.slab_region_start = memory_map(slab_region_size);
    if (ro.slab_region_start == NULL) {
        fatal_error("failed to allocate slab region");
    }
    ro.slab_region_end = (char *)ro.slab_region_start + slab_region_size;

    for (unsigned class = 0; class < N_SIZE_CLASSES; class++) {
        struct size_class *c = &size_class_metadata[class];

        mutex_init(&c->lock);
        random_state_init(&c->rng);

        size_t bound = (real_class_region_size - class_region_size) / PAGE_SIZE - 1;
        size_t gap = (get_random_u64_uniform(&regions_rng, bound) + 1) * PAGE_SIZE;
        c->class_region_start = (char *)ro.slab_region_start + real_class_region_size * class + gap;

        size_t size = size_classes[class];
        if (size == 0) {
            size = 16;
        }
        c->size_divisor = libdivide_u32_gen(size);
        size_t slab_size = get_slab_size(size_class_slots[class], size);
        c->slab_size_divisor = libdivide_u64_gen(slab_size);
        size_t metadata_max = get_metadata_max(slab_size);
        c->slab_info = allocate_pages(metadata_max * sizeof(struct slab_metadata), PAGE_SIZE, false);
        if (c->slab_info == NULL) {
            fatal_error("failed to allocate slab metadata");
        }
        c->metadata_allocated = PAGE_SIZE / sizeof(struct slab_metadata);
        if (memory_protect_rw(c->slab_info, c->metadata_allocated * sizeof(struct slab_metadata))) {
            fatal_error("failed to allocate initial slab info");
        }
    }

    atomic_store_explicit(&ro.initialized, true, memory_order_release);

    if (memory_protect_ro(&ro, sizeof(ro))) {
        fatal_error("failed to protect allocator data");
    }

    mutex_unlock(&lock);

    // may allocate, so wait until the allocator is initialized to avoid deadlocking
    if (pthread_atfork(full_lock, full_unlock, post_fork_child)) {
        fatal_error("pthread_atfork failed");
    }
}

static inline void init(void) {
    if (unlikely(!is_init())) {
        init_slow_path();
    }
}

// trigger early initialization to set up pthread_atfork and protect state as soon as possible
COLD __attribute__((constructor(101))) static void trigger_early_init(void) {
    // avoid calling init directly to skip it if this isn't the malloc implementation
    h_free(h_malloc(16));
}

static size_t get_guard_size(struct random_state *state, size_t size) {
    return (get_random_u64_uniform(state, size / PAGE_SIZE / 8) + 1) * PAGE_SIZE;
}

static void *allocate(size_t size) {
    if (size <= max_slab_size_class) {
        return allocate_small(size);
    }

    mutex_lock(&regions_lock);
    size_t guard_size = get_guard_size(&regions_rng, size);
    mutex_unlock(&regions_lock);

    void *p = allocate_pages(size, guard_size, true);
    if (p == NULL) {
        return NULL;
    }

    mutex_lock(&regions_lock);
    if (regions_insert(p, size, guard_size)) {
        mutex_unlock(&regions_lock);
        deallocate_pages(p, size, guard_size);
        return NULL;
    }
    mutex_unlock(&regions_lock);

    return p;
}

static void deallocate_large(void *p, size_t *expected_size) {
    enforce_init();

    mutex_lock(&regions_lock);
    struct region_info *region = regions_find(p);
    if (region == NULL) {
        fatal_error("invalid free");
    }
    size_t size = region->size;
    if (expected_size && size != *expected_size) {
        fatal_error("sized deallocation mismatch");
    }
    size_t guard_size = region->guard_size;
    regions_delete(region);
    mutex_unlock(&regions_lock);

    deallocate_pages(p, size, guard_size);
}

static size_t adjust_size_for_canaries(size_t size) {
    if (size > 0 && size <= max_slab_size_class) {
        return size + canary_size;
    }
    return size;
}

EXPORT void *h_malloc(size_t size) {
    init();
    size = adjust_size_for_canaries(size);
    return allocate(size);
}

EXPORT void *h_calloc(size_t nmemb, size_t size) {
    size_t total_size;
    if (unlikely(__builtin_mul_overflow(nmemb, size, &total_size))) {
        errno = ENOMEM;
        return NULL;
    }
    init();
    total_size = adjust_size_for_canaries(total_size);
    if (ZERO_ON_FREE) {
        return allocate(total_size);
    }
    void *p = allocate(total_size);
    if (unlikely(p == NULL)) {
        return NULL;
    }
    if (size && size <= max_slab_size_class) {
        memset(p, 0, total_size - canary_size);
    }
    return p;
}

static const size_t mremap_threshold = 4 * 1024 * 1024;

EXPORT void *h_realloc(void *old, size_t size) {
    if (old == NULL) {
        init();
        size = adjust_size_for_canaries(size);
        return allocate(size);
    }

    size = adjust_size_for_canaries(size);

    size_t old_size;
    if (old >= ro.slab_region_start && old < ro.slab_region_end) {
        old_size = slab_usable_size(old);
        if (size <= max_slab_size_class && get_size_info(size).size == old_size) {
            return old;
        }
    } else {
        enforce_init();

        mutex_lock(&regions_lock);
        struct region_info *region = regions_find(old);
        if (region == NULL) {
            fatal_error("invalid realloc");
        }
        old_size = region->size;
        size_t old_guard_size = region->guard_size;
        if (PAGE_CEILING(old_size) == PAGE_CEILING(size)) {
            region->size = size;
            mutex_unlock(&regions_lock);
            return old;
        }
        mutex_unlock(&regions_lock);

        // in-place shrink
        if (size < old_size && size > max_slab_size_class) {
            size_t rounded_size = PAGE_CEILING(size);
            size_t old_rounded_size = PAGE_CEILING(old_size);

            void *new_end = (char *)old + rounded_size;
            if (memory_map_fixed(new_end, old_guard_size)) {
                return NULL;
            }
            void *new_guard_end = (char *)new_end + old_guard_size;
            memory_unmap(new_guard_end, old_rounded_size - rounded_size);

            mutex_lock(&regions_lock);
            struct region_info *region = regions_find(old);
            if (region == NULL) {
                fatal_error("invalid realloc");
            }
            region->size = size;
            mutex_unlock(&regions_lock);

            return old;
        }

        size_t copy_size = size < old_size ? size : old_size;
        if (copy_size >= mremap_threshold) {
            void *new = allocate(size);
            if (new == NULL) {
                return NULL;
            }

            mutex_lock(&regions_lock);
            struct region_info *region = regions_find(old);
            if (region == NULL) {
                fatal_error("invalid realloc");
            }
            regions_delete(region);
            mutex_unlock(&regions_lock);

            if (memory_remap_fixed(old, old_size, new, size)) {
                memcpy(new, old, copy_size);
                deallocate_pages(old, old_size, old_guard_size);
            } else {
                memory_unmap((char *)old - old_guard_size, old_guard_size);
                memory_unmap((char *)old + PAGE_CEILING(old_size), old_guard_size);
            }
            return new;
        }
    }

    void *new = allocate(size);
    if (new == NULL) {
        return NULL;
    }
    size_t copy_size = size < old_size ? size : old_size;
    if (size > 0 && size <= max_slab_size_class) {
        copy_size -= canary_size;
    }
    memcpy(new, old, copy_size);
    if (old_size <= max_slab_size_class) {
        deallocate_small(old, NULL);
    } else {
        deallocate_large(old, NULL);
    }
    return new;
}

static int alloc_aligned(void **memptr, size_t alignment, size_t size, size_t min_alignment) {
    if ((alignment - 1) & alignment || alignment < min_alignment) {
        return EINVAL;
    }

    if (alignment <= PAGE_SIZE) {
        if (size <= max_slab_size_class && alignment > min_align) {
            size = get_size_info_align(size, alignment).size;
        }

        void *p = allocate(size);
        if (p == NULL) {
            return ENOMEM;
        }
        *memptr = p;
        return 0;
    }

    mutex_lock(&regions_lock);
    size_t guard_size = get_guard_size(&regions_rng, size);
    mutex_unlock(&regions_lock);

    void *p = allocate_pages_aligned(size, alignment, guard_size);
    if (p == NULL) {
        return ENOMEM;
    }

    mutex_lock(&regions_lock);
    if (regions_insert(p, size, guard_size)) {
        mutex_unlock(&regions_lock);
        deallocate_pages(p, size, guard_size);
        return ENOMEM;
    }
    mutex_unlock(&regions_lock);

    *memptr = p;
    return 0;
}

static void *alloc_aligned_simple(size_t alignment, size_t size) {
    void *ptr;
    int ret = alloc_aligned(&ptr, alignment, size, 1);
    if (ret) {
        errno = ret;
        return NULL;
    }
    return ptr;
}

EXPORT int h_posix_memalign(void **memptr, size_t alignment, size_t size) {
    init();
    size = adjust_size_for_canaries(size);
    return alloc_aligned(memptr, alignment, size, sizeof(void *));
}

EXPORT void *h_aligned_alloc(size_t alignment, size_t size) {
    init();
    size = adjust_size_for_canaries(size);
    return alloc_aligned_simple(alignment, size);
}

EXPORT void *h_memalign(size_t alignment, size_t size) ALIAS(h_aligned_alloc);

EXPORT void *h_valloc(size_t size) {
    init();
    size = adjust_size_for_canaries(size);
    return alloc_aligned_simple(PAGE_SIZE, size);
}

EXPORT void *h_pvalloc(size_t size) {
    size_t rounded = PAGE_CEILING(size);
    if (!rounded) {
        errno = ENOMEM;
        return NULL;
    }
    init();
    size = adjust_size_for_canaries(size);
    return alloc_aligned_simple(PAGE_SIZE, rounded);
}

EXPORT void h_free(void *p) {
    if (p == NULL) {
        return;
    }

    if (p >= ro.slab_region_start && p < ro.slab_region_end) {
        deallocate_small(p, NULL);
        return;
    }

    deallocate_large(p, NULL);
}

EXPORT void h_cfree(void *ptr) ALIAS(h_free);

EXPORT void h_free_sized(void *p, size_t expected_size) {
    if (p == NULL) {
        return;
    }

    if (p >= ro.slab_region_start && p < ro.slab_region_end) {
        expected_size = get_size_info(adjust_size_for_canaries(expected_size)).size;
        deallocate_small(p, &expected_size);
        return;
    }

    deallocate_large(p, &expected_size);
}

EXPORT size_t h_malloc_usable_size(void *p) {
    if (p == NULL) {
        return 0;
    }

    if (p >= ro.slab_region_start && p < ro.slab_region_end) {
        size_t size = slab_usable_size(p);
        return size ? size - canary_size : 0;
    }

    enforce_init();

    mutex_lock(&regions_lock);
    struct region_info *region = regions_find(p);
    if (p == NULL) {
        fatal_error("invalid malloc_usable_size");
    }
    size_t size = region->size;
    mutex_unlock(&regions_lock);

    return size;
}

EXPORT size_t h_malloc_object_size(void *p) {
    if (p == NULL) {
        return 0;
    }

    if (p >= ro.slab_region_start && p < ro.slab_region_end) {
        size_t size = slab_usable_size(p);
        return size ? size - canary_size : 0;
    }

    if (unlikely(!is_init())) {
        return 0;
    }

    mutex_lock(&regions_lock);
    struct region_info *region = regions_find(p);
    size_t size = p == NULL ? SIZE_MAX : region->size;
    mutex_unlock(&regions_lock);

    return size;
}

EXPORT size_t h_malloc_object_size_fast(void *p) {
    if (p == NULL) {
        return 0;
    }

    if (p >= ro.slab_region_start && p < ro.slab_region_end) {
        size_t size = slab_usable_size(p);
        return size ? size - canary_size : 0;
    }

    if (unlikely(!is_init())) {
        return 0;
    }

    return SIZE_MAX;
}

EXPORT int h_mallopt(UNUSED int param, UNUSED int value) {
    return 0;
}

EXPORT int h_malloc_trim(UNUSED size_t pad) {
    if (unlikely(!is_init())) {
        return 0;
    }

    bool is_trimmed = false;

    // skip zero byte size class since there's nothing to change
    for (unsigned class = 1; class < N_SIZE_CLASSES; class++) {
        struct size_class *c = &size_class_metadata[class];
        size_t slab_size = get_slab_size(size_class_slots[class], size_classes[class]);

        mutex_lock(&c->lock);
        struct slab_metadata *iterator = c->empty_slabs;
        while (iterator) {
            void *slab = get_slab(c, slab_size, iterator);
            if (memory_map_fixed(slab, slab_size)) {
                break;
            }

            struct slab_metadata *trimmed = iterator;
            iterator = iterator->next;
            c->empty_slabs_total -= slab_size;

            enqueue_free_slab(c, trimmed);

            is_trimmed = true;
        }
        c->empty_slabs = iterator;
        mutex_unlock(&c->lock);
    }

    return is_trimmed;
}

EXPORT void h_malloc_stats(void) {}

#if defined(__GLIBC__) || defined(__ANDROID__)
EXPORT struct mallinfo h_mallinfo(void) {
    return (struct mallinfo){0};
}
#endif

EXPORT int h_malloc_info(UNUSED int options, UNUSED FILE *fp) {
    errno = ENOSYS;
    return -1;
}

COLD EXPORT void *h_malloc_get_state(void) {
    return NULL;
}

COLD EXPORT int h_malloc_set_state(UNUSED void *state) {
    return -2;
}

#ifdef __ANDROID__
EXPORT size_t __mallinfo_narenas(void) {
    return 0;
}

EXPORT size_t __mallinfo_nbins(void) {
    return 0;
}

EXPORT struct mallinfo __mallinfo_arena_info(UNUSED size_t arena) {
    return (struct mallinfo){0};
}

EXPORT struct mallinfo __mallinfo_bin_info(UNUSED size_t arena, UNUSED size_t bin) {
    return (struct mallinfo){0};
}

COLD EXPORT int h_iterate(UNUSED uintptr_t base, UNUSED size_t size,
                          UNUSED void (*callback)(uintptr_t ptr, size_t size, void *arg),
                          UNUSED void *arg) {
    fatal_error("not implemented");
}

COLD EXPORT void h_malloc_disable(void) {
    full_lock();
}

COLD EXPORT void h_malloc_enable(void) {
    full_unlock();
}
#endif
