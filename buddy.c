#include "buddy.h"
#include <stddef.h>

#define PAGE_SIZE 4096
#define MAX_RANK 16
#define MIN_RANK 1
#define MAX_PAGES (200000)

static struct block {
    struct block *next;
    struct block *prev;
} *free_lists[MAX_RANK + 1];

static void *mem_start = NULL;
static void *mem_end = NULL;
static int num_pages = 0;

/* Tracking */
static unsigned char ranks_arr[MAX_PAGES];
static unsigned char alloc_arr[MAX_PAGES];

static inline int pgidx(void *p) {
    return (int)(((size_t)p - (size_t)mem_start) / PAGE_SIZE);
}

static inline void *buddyof(void *addr, int rank) {
    size_t sz = (size_t)PAGE_SIZE << (rank - 1);
    size_t off = (size_t)addr - (size_t)mem_start;
    return (void *)((size_t)mem_start + (off ^ sz));
}

static inline int aligned(void *addr, int rank) {
    size_t sz = (size_t)PAGE_SIZE << (rank - 1);
    return (((size_t)addr - (size_t)mem_start) & (sz - 1)) == 0;
}

static inline int valid_rank(int r) {
    return r >= MIN_RANK && r <= MAX_RANK;
}

static inline int valid_addr(void *p) {
    if (!p) return 0;
    if (p < mem_start || p >= mem_end) return 0;
    return (((size_t)p - (size_t)mem_start) & (PAGE_SIZE - 1)) == 0;
}

static void lst_init(struct block *b) {
    b->next = b->prev = b;
}

static void lst_add(struct block *b, int rank) {
    if (!free_lists[rank]) {
        lst_init(b);
        free_lists[rank] = b;
    } else {
        struct block *h = free_lists[rank];
        b->next = h;
        b->prev = h->prev;
        h->prev->next = b;
        h->prev = b;
    }
}

static void lst_rem(struct block *b, int rank) {
    if (b->next == b) {
        free_lists[rank] = 0;
    } else {
        b->prev->next = b->next;
        b->next->prev = b->prev;
        if (free_lists[rank] == b) free_lists[rank] = b->next;
    }
}

static int get_rank(int idx) {
    while (idx > 0 && !ranks_arr[idx]) idx--;
    return ranks_arr[idx];
}

static void coal(void *addr, int rank) {
    while (rank < MAX_RANK) {
        void *bud = buddyof(addr, rank);
        if (!valid_addr(bud)) break;
        
        int aidx = pgidx(addr);
        int bidx = pgidx(bud);
        if (bidx < 0 || bidx >= num_pages) break;
        
        if (alloc_arr[bidx]) break;
        if (get_rank(bidx) != rank) break;
        
        void *par = (addr < bud) ? addr : bud;
        if (!aligned(par, rank + 1)) break;
        
        lst_rem((struct block *)bud, rank);
        lst_rem((struct block *)addr, rank);
        lst_add((struct block *)par, rank + 1);
        
        int pidx = pgidx(par);
        ranks_arr[pidx] = rank + 1;
        alloc_arr[pidx] = 0;
        
        int n = 1 << rank;
        int lim = pidx + n + n;
        if (lim > num_pages) lim = num_pages;
        for (int i = pidx + 1; i < lim; i++) ranks_arr[i] = 0;
        
        addr = par;
        rank++;
    }
}

static void split(int rank) {
    if (rank <= MIN_RANK || !free_lists[rank]) return;
    struct block *b = free_lists[rank];
    void *addr = (void *)b;
    lst_rem(b, rank);
    
    size_t ss = (size_t)PAGE_SIZE << (rank - 2);
    void *a1 = addr;
    void *a2 = (void *)((size_t)addr + ss);
    
    lst_add((struct block *)a1, rank - 1);
    lst_add((struct block *)a2, rank - 1);
    
    int i1 = pgidx(a1), i2 = pgidx(a2);
    ranks_arr[i1] = ranks_arr[i2] = rank - 1;
    alloc_arr[i1] = alloc_arr[i2] = 0;
}

int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0 || pgcount > MAX_PAGES) return -EINVAL;
    
    mem_start = p;
    num_pages = pgcount;
    mem_end = (void *)((size_t)p + (size_t)pgcount * PAGE_SIZE);
    
    for (int i = 0; i < pgcount; i++) {
        ranks_arr[i] = alloc_arr[i] = 0;
    }
    for (int i = 0; i <= MAX_RANK; i++) free_lists[i] = 0;
    
    void *cur = p;
    int off = 0;
    while (off < pgcount) {
        int rem = pgcount - off;
        int rank = MAX_RANK;
        while (rank > MIN_RANK) {
            int need = 1 << (rank - 1);
            if (need <= rem && aligned(cur, rank)) break;
            rank--;
        }
        int npg = 1 << (rank - 1);
        lst_add((struct block *)cur, rank);
        ranks_arr[off] = rank;
        cur = (void *)((size_t)cur + (size_t)npg * PAGE_SIZE);
        off += npg;
    }
    return OK;
}

void *alloc_pages(int rank) {
    if (!valid_rank(rank)) return (void *)(-EINVAL);
    
    int ar = rank;
    while (ar <= MAX_RANK && !free_lists[ar]) ar++;
    if (ar > MAX_RANK) return (void *)(-ENOSPC);
    
    while (ar > rank) {
        split(ar);
        ar--;
    }
    
    struct block *b = free_lists[rank];
    void *addr = (void *)b;
    lst_rem(b, rank);
    
    int idx = pgidx(addr);
    alloc_arr[idx] = 1;
    ranks_arr[idx] = rank;
    
    return addr;
}

int return_pages(void *p) {
    if (!valid_addr(p)) return -EINVAL;
    
    int idx = pgidx(p);
    if (!alloc_arr[idx]) return -EINVAL;
    
    int rank = ranks_arr[idx];
    if (!valid_rank(rank)) return -EINVAL;
    
    alloc_arr[idx] = 0;
    lst_add((struct block *)p, rank);
    coal(p, rank);
    
    return OK;
}

int query_ranks(void *p) {
    if (!valid_addr(p)) return -EINVAL;
    int idx = pgidx(p);
    return alloc_arr[idx] ? ranks_arr[idx] : get_rank(idx);
}

int query_page_counts(int rank) {
    if (!valid_rank(rank)) return -EINVAL;
    if (!free_lists[rank]) return 0;
    
    int cnt = 0;
    struct block *b = free_lists[rank];
    struct block *s = b;
    do {
        cnt++;
        b = b->next;
    } while (b != s);
    return cnt;
}
