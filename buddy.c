#include "buddy.h"
#include <stddef.h>

#define PAGE_SIZE 4096
#define MAX_RANK 16
#define MIN_RANK 1
#define MAX_PAGES (128 * 1024)

/* Node in the free list for each rank */
typedef struct block {
    struct block *next;
    struct block *prev;
} block_t;

/* Free list for each rank (1-16) */
static block_t *free_lists[MAX_RANK + 1];

/* Memory region info */
static void *memory_start = NULL;
static void *memory_end = NULL;
static int total_pages = 0;

/* Tracking arrays */
static unsigned char block_ranks[MAX_PAGES];
static unsigned char block_allocated[MAX_PAGES];

/* Array to quickly find block boundaries - stores the size (in pages) of the free block starting at each page */
static unsigned short block_sizes[MAX_PAGES];  /* Size in pages, 0 if not a block start */

/* Helper functions */
static inline int addr_to_page_idx(void *p) {
    return (int)(((size_t)p - (size_t)memory_start) / PAGE_SIZE);
}

static inline int is_valid_rank(int rank) {
    return (rank >= MIN_RANK && rank <= MAX_RANK);
}

static inline int is_valid_addr(void *p) {
    if (p == NULL) return 0;
    if (p < memory_start || p >= memory_end) return 0;
    size_t offset = (size_t)p - (size_t)memory_start;
    return (offset % PAGE_SIZE) == 0;
}

/* Get buddy address using OFFSET from base */
static inline void *get_buddy(void *addr, int rank) {
    size_t block_size = (size_t)PAGE_SIZE << (rank - 1);
    size_t offset = (size_t)addr - (size_t)memory_start;
    size_t buddy_offset = offset ^ block_size;
    return (void *)((size_t)memory_start + buddy_offset);
}

/* Check if block is aligned */
static inline int is_aligned(void *addr, int rank) {
    size_t block_size = (size_t)PAGE_SIZE << (rank - 1);
    size_t offset = (size_t)addr - (size_t)memory_start;
    return (offset % block_size) == 0;
}

/* List operations */
static void init_block_list(block_t *blk) {
    blk->next = blk;
    blk->prev = blk;
}

static void remove_from_list(block_t *blk) {
    blk->prev->next = blk->next;
    blk->next->prev = blk->prev;
}

static void add_to_list(block_t *blk, int rank) {
    if (free_lists[rank] == NULL) {
        init_block_list(blk);
        free_lists[rank] = blk;
    } else {
        blk->next = free_lists[rank];
        blk->prev = free_lists[rank]->prev;
        free_lists[rank]->prev->next = blk;
        free_lists[rank]->prev = blk;
    }
}

static void remove_from_list_safe(block_t *blk, int rank) {
    if (blk->next == blk) {
        free_lists[rank] = NULL;
    } else {
        remove_from_list(blk);
        if (free_lists[rank] == blk) {
            free_lists[rank] = blk->next;
        }
    }
}

/* Get the rank of a free block containing page_idx using block_sizes array */
static int get_free_block_rank(int page_idx) {
    /* Find the start of the block containing this page */
    int scan = page_idx;
    while (scan > 0 && block_sizes[scan] == 0) {
        scan--;
    }
    if (block_sizes[scan] == 0) return MIN_RANK;
    /* Calculate rank from block size in pages */
    int pages = block_sizes[scan];
    int rank = 0;
    while (pages > 0) {
        pages >>= 1;
        rank++;
    }
    return rank;
}

/* Coalesce buddy blocks */
static void coalesce(void *addr, int rank) {
    while (rank < MAX_RANK) {
        void *buddy = get_buddy(addr, rank);
        if (buddy < memory_start || buddy >= memory_end) break;
        
        int addr_idx = addr_to_page_idx(addr);
        int buddy_idx = addr_to_page_idx(buddy);
        
        /* Check if buddy is free */
        if (block_allocated[buddy_idx] != 0) break;
        
        /* Get buddy's rank using block_sizes */
        int scan = buddy_idx;
        while (scan > 0 && block_sizes[scan] == 0) scan--;
        if (block_sizes[scan] == 0) break;
        int buddy_pages = block_sizes[scan];
        int buddy_rank = 0;
        int tmp = buddy_pages;
        while (tmp > 0) { tmp >>= 1; buddy_rank++; }
        
        if (buddy_rank != rank) break;
        
        /* Determine parent address */
        void *parent = (addr < buddy) ? addr : buddy;
        if (!is_aligned(parent, rank + 1)) break;
        
        int parent_idx = addr_to_page_idx(parent);
        int parent_pages = 1 << rank;
        
        /* Remove both from free list */
        remove_from_list_safe((block_t *)buddy, rank);
        remove_from_list_safe((block_t *)addr, rank);
        
        /* Add parent to higher rank */
        add_to_list((block_t *)parent, rank + 1);
        
        /* Update tracking */
        block_ranks[parent_idx] = rank + 1;
        block_allocated[parent_idx] = 0;
        if (addr == parent) {
            block_sizes[parent_idx] = parent_pages * 2;
            block_sizes[addr_idx] = 0;
        } else {
            block_sizes[parent_idx] = parent_pages * 2;
            block_sizes[buddy_idx] = 0;
        }
        /* Clear old block sizes */
        for (int i = 1; i < parent_pages * 2 && (parent_idx + i) < MAX_PAGES; i++) {
            if ((parent_idx + i) != addr_idx && (parent_idx + i) != buddy_idx) {
                block_ranks[parent_idx + i] = 0;
            }
        }
        
        addr = parent;
        rank++;
    }
}

/* Split a block */
static void split_block(int rank) {
    if (rank <= MIN_RANK || free_lists[rank] == NULL) return;
    
    block_t *blk = free_lists[rank];
    void *addr = (void *)blk;
    int idx = addr_to_page_idx(addr);
    
    remove_from_list_safe(blk, rank);
    
    size_t smaller_size = (size_t)PAGE_SIZE << (rank - 2);
    void *addr1 = addr;
    void *addr2 = (void *)((size_t)addr + smaller_size);
    
    add_to_list((block_t *)addr1, rank - 1);
    add_to_list((block_t *)addr2, rank - 1);
    
    int idx1 = addr_to_page_idx(addr1);
    int idx2 = addr_to_page_idx(addr2);
    int pages = 1 << (rank - 2);
    
    block_ranks[idx1] = rank - 1;
    block_ranks[idx2] = rank - 1;
    block_allocated[idx1] = 0;
    block_allocated[idx2] = 0;
    block_sizes[idx1] = pages;
    block_sizes[idx2] = pages;
}

int init_page(void *p, int pgcount) {
    if (p == NULL || pgcount <= 0 || pgcount > MAX_PAGES) {
        return -EINVAL;
    }
    
    memory_start = p;
    total_pages = pgcount;
    memory_end = (void *)((size_t)p + (size_t)pgcount * PAGE_SIZE);
    
    /* Clear tracking arrays */
    for (int i = 0; i < pgcount; i++) {
        block_ranks[i] = 0;
        block_allocated[i] = 0;
        block_sizes[i] = 0;
    }
    for (int i = pgcount; i < MAX_PAGES; i++) {
        block_ranks[i] = 0;
        block_allocated[i] = 0;
        block_sizes[i] = 0;
    }
    
    for (int i = 0; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
    }
    
    void *current = p;
    int page_offset = 0;
    
    while (page_offset < pgcount) {
        int remaining = pgcount - page_offset;
        
        int rank = MAX_RANK;
        while (rank > MIN_RANK) {
            int pages_needed = 1 << (rank - 1);
            if (pages_needed <= remaining && is_aligned(current, rank)) {
                break;
            }
            rank--;
        }
        
        int pages_in_block = 1 << (rank - 1);
        
        add_to_list((block_t *)current, rank);
        
        block_ranks[page_offset] = rank;
        block_sizes[page_offset] = pages_in_block;
        for (int i = 1; i < pages_in_block; i++) {
            block_ranks[page_offset + i] = 0;
            block_sizes[page_offset + i] = 0;
        }
        
        current = (void *)((size_t)current + ((size_t)PAGE_SIZE << (rank - 1)));
        page_offset += pages_in_block;
    }
    
    return OK;
}

void *alloc_pages(int rank) {
    if (!is_valid_rank(rank)) {
        return (void *)(-EINVAL);
    }
    
    int alloc_rank = rank;
    while (alloc_rank <= MAX_RANK && free_lists[alloc_rank] == NULL) {
        alloc_rank++;
    }
    
    if (alloc_rank > MAX_RANK) {
        return (void *)(-ENOSPC);
    }
    
    while (alloc_rank > rank) {
        split_block(alloc_rank);
        alloc_rank--;
    }
    
    block_t *blk = free_lists[rank];
    void *addr = (void *)blk;
    
    remove_from_list_safe(blk, rank);
    
    int idx = addr_to_page_idx(addr);
    int pages = 1 << (rank - 1);
    
    block_allocated[idx] = 1;
    block_ranks[idx] = rank;
    block_sizes[idx] = 0;  /* Not a free block anymore */
    for (int i = 1; i < pages && (idx + i) < MAX_PAGES; i++) {
        block_ranks[idx + i] = 0;
    }
    
    return addr;
}

int return_pages(void *p) {
    if (!is_valid_addr(p)) {
        return -EINVAL;
    }
    
    int idx = addr_to_page_idx(p);
    
    if (block_allocated[idx] == 0) {
        return -EINVAL;
    }
    
    int rank = block_ranks[idx];
    if (!is_valid_rank(rank)) {
        return -EINVAL;
    }
    
    int pages = 1 << (rank - 1);
    
    block_allocated[idx] = 0;
    block_sizes[idx] = pages;
    
    add_to_list((block_t *)p, rank);
    
    coalesce(p, rank);
    
    return OK;
}

int query_ranks(void *p) {
    if (!is_valid_addr(p)) {
        return -EINVAL;
    }
    
    int idx = addr_to_page_idx(p);
    
    if (block_allocated[idx]) {
        return block_ranks[idx];
    } else {
        return get_free_block_rank(idx);
    }
}

int query_page_counts(int rank) {
    if (!is_valid_rank(rank)) {
        return -EINVAL;
    }
    
    if (free_lists[rank] == NULL) {
        return 0;
    }
    
    int count = 0;
    block_t *blk = free_lists[rank];
    block_t *start = blk;
    do {
        count++;
        blk = blk->next;
    } while (blk != start);
    
    return count;
}
