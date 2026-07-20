#include "buddy.h"
#include <stddef.h>

#define PAGE_SIZE 4096
#define MAX_RANK 16
#define MIN_RANK 1
/* Increased to handle up to 1 million pages ~ 4GB */
#define MAX_TRACK (1024 * 1024)

static struct block {
    struct block *next, *prev;
} *flist[MAX_RANK + 1];

static void *mstart = NULL;
static int npages = 0;
/* Static arrays for tracking - placed in BSS section */
static unsigned char brank[MAX_TRACK];
static unsigned char balloc[MAX_TRACK];

static inline int pgidx(void *p) {
    return (int)(((size_t)p - (size_t)mstart) / PAGE_SIZE);
}

static inline void *buddy(void *a, int r) {
    size_t sz = (size_t)PAGE_SIZE << (r - 1);
    size_t off = (size_t)a - (size_t)mstart;
    return (void *)((size_t)mstart + (off ^ sz));
}

static inline int algn(void *a, int r) {
    size_t sz = (size_t)PAGE_SIZE << (r - 1);
    return (((size_t)a - (size_t)mstart) & (sz - 1)) == 0;
}

static inline int vr(int r) { return r >= MIN_RANK && r <= MAX_RANK; }

static inline int va(void *p) {
    if (!p) return 0;
    int idx = pgidx(p);
    return idx >= 0 && idx < npages;
}

static void lst_i(struct block *b) { b->next = b->prev = b; }

static void lst_a(struct block *b, int r) {
    if (!flist[r]) { lst_i(b); flist[r] = b; }
    else {
        struct block *h = flist[r];
        b->next = h; b->prev = h->prev;
        h->prev->next = b; h->prev = b;
    }
}

static void lst_r(struct block *b, int r) {
    if (b->next == b) flist[r] = NULL;
    else { 
        b->prev->next = b->next; 
        b->next->prev = b->prev;
        if (flist[r] == b) flist[r] = b->next;
    }
}

static int getr(int i) {
    while (i > 0 && !brank[i]) i--;
    return brank[i];
}

static void coal(void *a, int r) {
    while (r < MAX_RANK) {
        void *b = buddy(a, r);
        if (!va(b)) break;
        int bi = pgidx(b);
        if (balloc[bi]) break;
        if (getr(bi) != r) break;
        void *p = (a < b) ? a : b;
        if (!algn(p, r+1)) break;
        
        lst_r((struct block *)b, r);
        lst_r((struct block *)a, r);
        lst_a((struct block *)p, r+1);
        
        int pi = pgidx(p);
        brank[pi] = r+1; 
        balloc[pi] = 0;
        int n = 1 << (r+1);
        if (pi + n > npages) n = npages - pi;
        for (int i = 1; i < n && pi + i < npages; i++) brank[pi+i] = 0;
        
        a = p; r++;
    }
}

static void spl(int r) {
    if (r <= MIN_RANK || !flist[r]) return;
    struct block *bk = flist[r];
    void *ad = (void *)bk;
    lst_r(bk, r);
    
    size_t ss = (size_t)PAGE_SIZE << (r-2);
    void *a1 = ad, *a2 = (void *)((size_t)ad + ss);
    lst_a((struct block *)a1, r-1);
    lst_a((struct block *)a2, r-1);
    
    int i1 = pgidx(a1), i2 = pgidx(a2);
    brank[i1] = brank[i2] = r-1;
    balloc[i1] = balloc[i2] = 0;
}

int init_page(void *p, int pc) {
    if (!p || pc <= 0) return -EINVAL;
    if (pc > MAX_TRACK) return -EINVAL;
    mstart = p;
    npages = pc;
    
    for (int i = 0; i < pc; i++) { brank[i] = 0; balloc[i] = 0; }
    for (int i = 0; i <= MAX_RANK; i++) flist[i] = NULL;
    
    void *cur = p;
    int off = 0;
    while (off < pc) {
        int rem = pc - off;
        int r = MAX_RANK;
        while (r > MIN_RANK) {
            int need = 1 << (r-1);
            if (need <= rem && algn(cur, r)) break;
            r--;
        }
        int np = 1 << (r-1);
        lst_a((struct block *)cur, r);
        brank[off] = r;
        cur = (void *)((size_t)cur + (size_t)np * PAGE_SIZE);
        off += np;
    }
    return OK;
}

void *alloc_pages(int r) {
    if (!vr(r)) return (void *)(-EINVAL);
    int ar = r;
    while (ar <= MAX_RANK && !flist[ar]) ar++;
    if (ar > MAX_RANK) return (void *)(-ENOSPC);
    while (ar > r) { spl(ar); ar--; }
    
    struct block *b = flist[r];
    void *ad = (void *)b;
    lst_r(b, r);
    
    int idx = pgidx(ad);
    balloc[idx] = 1;
    brank[idx] = r;
    return ad;
}

int return_pages(void *p) {
    if (!va(p)) return -EINVAL;
    int idx = pgidx(p);
    if (!balloc[idx]) return -EINVAL;
    int r = brank[idx];
    if (!vr(r)) return -EINVAL;
    
    balloc[idx] = 0;
    lst_a((struct block *)p, r);
    coal(p, r);
    return OK;
}

int query_ranks(void *p) {
    if (!va(p)) return -EINVAL;
    int idx = pgidx(p);
    return balloc[idx] ? brank[idx] : getr(idx);
}

int query_page_counts(int r) {
    if (!vr(r)) return -EINVAL;
    if (!flist[r]) return 0;
    int cnt = 0;
    struct block *b = flist[r], *s = b;
    do { cnt++; b = b->next; } while (b != s);
    return cnt;
}
