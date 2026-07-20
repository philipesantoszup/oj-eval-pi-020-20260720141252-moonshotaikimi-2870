#include "buddy.h"
#include <stddef.h>

#define PAGE_SIZE 4096
#define MAX_RANK 16
#define MIN_RANK 1
#define MAX_PAGES (256 * 1024)  /* Increased max pages */

/* Node in the free list for each rank */
typedef struct block {
    struct block *next;
    struct block *prev;
} block_t;

/* Free list for each rank (1-16) */
static block_t *free_lists[MAX_RANK + 1];

/* Memory region info */
static void *memory_start = NULL;
static int total_pages = 0;

/* Tracking arrays - use static initialization */
static unsigned char block_ranks[MAX_PAGES];
static unsigned char block_allocated[MAX_PAGES];

/* Helper: Get page index from address */
static inline int addr_to_page_idx(void *p) {
    size_t offset = (size_t)p - (size_t)memory_start;
    return (int)(offset / PAGE_SIZE);
}

static inline int is_valid_rank(int rank) {
    return (rank >= MIN_RANK && rank <= MAX_RANK);
}

static inline int is_valid_addr(void *p) {
    if (p == NULL) return 0;
    int idx = addr_to_page_idx(p);
    return (idx >= 0 && idx < total_pages);
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

/* Get the rank of a free block - backward scan */
static int get_free_block_rank(int page_idx) {
    int scan = page_idx;
    while (scan > 0 && block_ranks[scan] == 0) {
        scan--;
    }
    return block_ranks[scan];
}

/* Coalesce buddy blocks - iterative instead of recursive */
static void coalesce(void *addr, int rank) {
    while (rank < MAX_RANK) {
        void *buddy = get_buddy(addr, rank);
        
        /* Check if buddy is valid */
        if (!is_valid_addr(buddy)) break;
        
        int addr_idx = addr_to_page_idx(addr);
        int buddy_idx = addr_to_page_idx(buddy);
        
        /* Bounds check */
        if (buddy_idx < 0 || buddy_idx >= total_pages) break;
        
        /* Check if buddy is free */
        if (block_allocated[buddy_idx] != 0) break;
        
        /* Get buddy's rank */
        int buddy_rank = get_free_block_rank(buddy_idx);
        if (buddy_rank != rank) break;
        
        /* Determine parent address */
        void *parent = (addr < buddy) ? addr : buddy;
        if (!is_aligned(parent, rank + 1)) break;
        
        /* Remove both from free list */
        remove_from_list_safe((block_t *)buddy, rank);
        remove_from_list_safe((block_t *)addr, rank);
        
        /* Add parent to higher rank */
        add_to_list((block_t *)parent, rank + 1);
        
        /* Update tracking */
        int parent_idx = addr_to_page_idx(parent);
        block_ranks[parent_idx] = rank + 1;
        block_allocated[parent_idx] = 0;
        
        /* Clear block_ranks for all pages in parent except first */
        int parent_pages = 1 << rank;
        int start_clear = parent_idx + 1;
        int end_clear = parent_idx + parent_pages + parent_pages;
        if (end_clear > total_pages) end_clear = total_pages;
        for (int i = start_clear; i < end_clear; i++) {
            block_ranks[i] = 0;
        }
        
        addr = parent;
        rank++;
    }
}

/* Split a block of rank into two blocks of rank-1 */
static void split_block(int rank) {
    if (rank <= MIN_RANK) return;
    if (free_lists[rank] == NULL) return;
    
    block_t *blk = free_lists[rank];
    void *addr = (void *)blk;
    
    remove_from_list_safe(blk, rank);
    
    size_t smaller_size = (size_t)PAGE_SIZE << (rank - 2);
    void *addr1 = addr;
    void *addr2 = (void *)((size_t)addr + smaller_size);
    
    add_to_list((block_t *)addr1, rank - 1);
    add_to_list((block_t *)addr2, rank - 1);
    
    int idx1 = addr_to_page_idx(addr1);
    int idx2 = addr_to_page_idx(addr2);
    
    block_ranks[idx1] = rank - 1;
    block_ranks[idx2] = rank - 1;
    block_allocated[idx1] = 0;
    block_allocated[idx2] = 0;
}

int init_page(void *p, int pgcount) {
    if (p == NULL || pgcount <= 0) {
        return -EINVAL;
    }
    if (pgcount > MAX_PAGES) {
        return -EINVAL;
    }
    
    memory_start = p;
    total_pages = pgcount;
    
    /* Clear tracking arrays - only up to pgcount */
    for (int i = 0; i < pgcount; i++) {
        block_ranks[i] = 0;
        block_allocated[i] = 0;
    }
    
    for (int i = 0; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
    }
    
    void *current = p;
    int page_offset = 0;
    
    while (page_offset < pgcount) {
        int remaining = pgcount - page_offset;
        
        /* Find the largest rank block that fits and is properly aligned */
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
        
        current = (void *)((size_t)current + ((size_t)PAGE_SIZE << (rank - 1)));
        page_offset += pages_in_block;
    }
    
    return OK;
}

void *alloc_pages(int rank) {
    if (!is_valid_rank(rank)) {
        return (void *)(-EINVAL);
    }
    
    /* Find the smallest available block >= requested rank */
    int alloc_rank = rank;
    while (alloc_rank <= MAX_RANK && free_lists[alloc_rank] == NULL) {
        alloc_rank++;
    }
    
    if (alloc_rank > MAX_RANK) {
        return (void *)(-ENOSPC);
    }
    
    /* Split down to desired rank */
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
    
    block_allocated[idx] = 0;
    
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
