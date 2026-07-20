#include "buddy.h"
#include <stddef.h>

#define PAGE_SIZE 4096
#define MAX_RANK 16
#define MIN_RANK 1
#define MAX_TRACK (1 << 20)  /* ~1 million pages */

typedef struct block {
    struct block *next;
    struct block *prev;
} block_t;

static block_t *free_list[MAX_RANK + 1];
static void *mem_start = NULL;
static int num_pages = 0;

static unsigned char ranks[MAX_TRACK];
static unsigned char allocated[MAX_TRACK];

static inline int page_index(void *p) {
    if (!mem_start || !p) return -1;
    size_t offset = (size_t)p - (size_t)mem_start;
    int idx = (int)(offset / PAGE_SIZE);
    if (idx < 0 || idx >= num_pages) return -1;
    return idx;
}

static inline void *get_buddy(void *addr, int rank) {
    size_t block_size = (size_t)PAGE_SIZE << (rank - 1);
    size_t offset = (size_t)addr - (size_t)mem_start;
    return (void *)((size_t)mem_start + (offset ^ block_size));
}

static inline int is_aligned(void *addr, int rank) {
    size_t block_size = (size_t)PAGE_SIZE << (rank - 1);
    size_t offset = (size_t)addr - (size_t)mem_start;
    return (offset & (block_size - 1)) == 0;
}

static inline int valid_rank(int r) {
    return r >= MIN_RANK && r <= MAX_RANK;
}

static inline int valid_addr(void *p) {
    return page_index(p) >= 0;
}

static void list_init(block_t *blk) {
    blk->next = blk;
    blk->prev = blk;
}

static void list_add(block_t *blk, int rank) {
    block_t *head = free_list[rank];
    if (!head) {
        list_init(blk);
        free_list[rank] = blk;
    } else {
        blk->next = head;
        blk->prev = head->prev;
        head->prev->next = blk;
        head->prev = blk;
    }
}

static void list_remove(block_t *blk, int rank) {
    if (blk->next == blk) {
        /* Only element */
        free_list[rank] = NULL;
    } else {
        blk->prev->next = blk->next;
        blk->next->prev = blk->prev;
        if (free_list[rank] == blk) {
            free_list[rank] = blk->next;
        }
    }
}

static int get_block_rank(int page_idx) {
    if (page_idx < 0 || page_idx >= num_pages) return MIN_RANK;
    while (page_idx > 0 && ranks[page_idx] == 0) page_idx--;
    return ranks[page_idx] ? ranks[page_idx] : MIN_RANK;
}

static void coalesce(void *addr, int rank) {
    while (rank < MAX_RANK) {
        void *b = get_buddy(addr, rank);
        if (!valid_addr(b)) break;
        
        int buddy_idx = page_index(b);
        int addr_idx = page_index(addr);
        if (buddy_idx < 0 || addr_idx < 0) break;
        
        if (allocated[buddy_idx]) break;
        if (get_block_rank(buddy_idx) != rank) break;
        
        void *parent = (addr < b) ? addr : b;
        if (!is_aligned(parent, rank + 1)) break;
        
        /* Remove both from current list */
        list_remove((block_t *)b, rank);
        list_remove((block_t *)addr, rank);
        
        /* Add parent to higher rank list */
        list_add((block_t *)parent, rank + 1);
        
        /* Update tracking */
        int parent_idx = page_index(parent);
        if (parent_idx >= 0 && parent_idx < MAX_TRACK) {
            ranks[parent_idx] = rank + 1;
            allocated[parent_idx] = 0;
        }
        
        /* Clear ranks of merged pages */
        int n = 1 << (rank + 1);
        int end = parent_idx + n;
        if (end > num_pages) end = num_pages;
        for (int i = parent_idx + 1; i < end && i < MAX_TRACK; i++) {
            ranks[i] = 0;
        }
        
        addr = parent;
        rank++;
    }
}

static void split_block(int rank) {
    if (rank <= MIN_RANK) return;
    block_t *blk = free_list[rank];
    if (!blk) return;
    
    void *addr = (void *)blk;
    list_remove(blk, rank);
    
    size_t half_size = (size_t)PAGE_SIZE << (rank - 2);
    void *left = addr;
    void *right = (void *)((size_t)addr + half_size);
    
    list_add((block_t *)left, rank - 1);
    list_add((block_t *)right, rank - 1);
    
    int left_idx = page_index(left);
    int right_idx = page_index(right);
    if (left_idx >= 0 && left_idx < MAX_TRACK) {
        ranks[left_idx] = rank - 1;
        allocated[left_idx] = 0;
    }
    if (right_idx >= 0 && right_idx < MAX_TRACK) {
        ranks[right_idx] = rank - 1;
        allocated[right_idx] = 0;
    }
}

int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0 || pgcount > MAX_TRACK) return -EINVAL;
    
    mem_start = p;
    num_pages = pgcount;
    
    /* Clear arrays */
    int limit = pgcount < MAX_TRACK ? pgcount : MAX_TRACK;
    for (int i = 0; i < limit; i++) {
        ranks[i] = 0;
        allocated[i] = 0;
    }
    for (int i = 0; i <= MAX_RANK; i++) {
        free_list[i] = NULL;
    }
    
    /* Create initial free blocks */
    void *current = p;
    int offset = 0;
    while (offset < pgcount) {
        int remaining = pgcount - offset;
        int rank = MAX_RANK;
        
        /* Find largest fitting rank */
        while (rank > MIN_RANK) {
            int pages_needed = 1 << (rank - 1);
            if (pages_needed <= remaining && is_aligned(current, rank)) {
                break;
            }
            rank--;
        }
        
        int pages = 1 << (rank - 1);
        list_add((block_t *)current, rank);
        if (offset >= 0 && offset < MAX_TRACK) {
            ranks[offset] = rank;
        }
        
        current = (void *)((size_t)current + (size_t)pages * PAGE_SIZE);
        offset += pages;
    }
    
    return OK;
}

void *alloc_pages(int rank) {
    if (!valid_rank(rank)) return (void *)(-EINVAL);
    
    /* Find smallest available block >= requested rank */
    int alloc_rank = rank;
    while (alloc_rank <= MAX_RANK && !free_list[alloc_rank]) {
        alloc_rank++;
    }
    
    if (alloc_rank > MAX_RANK) return (void *)(-ENOSPC);
    
    /* Split down to desired rank */
    while (alloc_rank > rank) {
        split_block(alloc_rank);
        alloc_rank--;
    }
    
    /* Allocate from free list */
    block_t *blk = free_list[rank];
    if (!blk) return (void *)(-ENOSPC);
    
    void *addr = (void *)blk;
    list_remove(blk, rank);
    
    int idx = page_index(addr);
    if (idx >= 0 && idx < MAX_TRACK) {
        allocated[idx] = 1;
        ranks[idx] = rank;
    }
    
    return addr;
}

int return_pages(void *p) {
    if (!valid_addr(p)) return -EINVAL;
    
    int idx = page_index(p);
    if (idx < 0 || idx >= num_pages) return -EINVAL;
    if (!allocated[idx]) return -EINVAL;
    
    int rank = ranks[idx];
    if (!valid_rank(rank)) return -EINVAL;
    
    allocated[idx] = 0;
    list_add((block_t *)p, rank);
    coalesce(p, rank);
    
    return OK;
}

int query_ranks(void *p) {
    if (!valid_addr(p)) return -EINVAL;
    int idx = page_index(p);
    if (idx < 0 || idx >= num_pages) return -EINVAL;
    return allocated[idx] ? ranks[idx] : get_block_rank(idx);
}

int query_page_counts(int rank) {
    if (!valid_rank(rank)) return -EINVAL;
    if (!free_list[rank]) return 0;
    
    int count = 0;
    block_t *blk = free_list[rank];
    block_t *start = blk;
    do {
        count++;
        blk = blk->next;
    } while (blk != start && blk != NULL);
    
    return count;
}
