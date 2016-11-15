#ifdef __linux__
#define _GNU_SOURCE
#endif

#include <unix_internal.h>

typedef struct region_heap {
    struct heap h;
    u64 base, max, fill;
    void *freelist;
    boolean recycle;
} *region_heap;


static void *region_pages(heap h, bytes s)
{
    void *p;
    region_heap r = (void *)h;
    unsigned int length = pad(s, h->pagesize);
    // check upper bound and fail
    if ((s == h->pagesize) && (r->freelist)) {
        p = r->freelist;
        r->freelist = *(void **)r->freelist;
    } else {
        u64 addr = fetch_and_add(&r->fill, length);
        p = mmap((void *)addr, length,
                 PROT_READ|PROT_WRITE,
                 MAP_PRIVATE|MAP_ANON|MAP_FIXED,
                 -1,0);
        if (p == MAP_FAILED) return 0;
    }
    h->allocated += length;
    return(p);
}

static void region_free(heap h, void *x, bytes size)
{
    region_heap r = (region_heap)h;
    h->allocated -= pad(size, h->pagesize);
    if ((size == h->pagesize) && r->recycle){
        // multipage
        *(void **)x = r->freelist;
        r->freelist = x;
    } else {
        munmap(x, pad(size, h->pagesize));
    }
    h->allocated -= size;
}

boolean in_region(region_heap r, void *p) {
    unsigned long x = (unsigned long)p;
    return ((x >= r->base) && (x <= r->fill));
}


heap init_fixed_page_region(heap meta,
                            u64 base_address,
                            u64 max_address,
                            bytes pagesize,
                            boolean recycle)
{
    region_heap r = allocate(meta, sizeof(struct region_heap));
    r->h.alloc = region_pages;
    r->h.dealloc = region_free;
    r->h.pagesize = pagesize;
    r->base = base_address;
    r->fill = r->base;
    r->max = max_address;
    r->freelist = 0;
    r->recycle = recycle;
    return (heap)r;
}
