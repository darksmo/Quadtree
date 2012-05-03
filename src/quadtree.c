/*
 * Copyright (C) 2011-2012 Lokku ltd. and contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* See open(2), O_LARGEFILE */
#define _FILE_OFFSET_BITS 64

#define _GNU_SOURCE

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifndef NDEBUG
#include <malloc.h>
#endif

#include "quadtree.h"
#include "quadtree_private.h"
#include "quadtree_portable.h"

/* For debugging */
unsigned long int withins  = 0;
unsigned long int nwithins = 0;


/* Expect this to be faster than a memcpy */
inline void _nullify_quadrants(TransNode **quadrants)
{
    quadrants[NW] = (TransNode *)NULL;
    quadrants[NE] = (TransNode *)NULL;
    quadrants[SW] = (TransNode *)NULL;
    quadrants[SE] = (TransNode *)NULL;
}


/*
 * AMD phenom has 64-byte cache line width.
 * sizeof(inner) = sizeof(void *)*4 = 32 bytes
 * We want to make sure that the inner structs are 64-byte
 * aligned.
 * By storing inner in depth-first traversal order, we can
 * expect cache-hits 1/4 of the time.
 */

/* Page aligned memory */
inline void *_malloc(size_t size)
{
    void *ptr;
    int err;
    if ((err = posix_memalign(&ptr, getpagesize(), size))) {
        fprintf(stderr, "malloc: couldn't allocate %lu bytes",
            (unsigned long)size);
        perror("posix_memalign");
        exit(1);
    }
    return ptr;
}

inline void *_malloc_fast(size_t size)
{
    void *ptr = malloc(size);
    if (ptr == NULL) {
        fprintf(stderr, "malloc: couldn't allocate %lu bytes",
            (unsigned long)size);
        perror("malloc");
        exit(1);
    }
    return ptr;
}

inline void *_realloc(void *ptr, size_t size)
{
    ptr = realloc(ptr, size);
    if (ptr == NULL) {
        fprintf(stderr, "realloc: couldn't allocate %lu bytes",
            (unsigned long)size);
        perror("realloc");
        exit(1);
    }
    return ptr;
}


/* Check if item is in a quadrand */
inline bool in_quadrant(const Item *i, const Quadrant *q)
{
    return ((i->coords[X] >= q->sw[X]) && (i->coords[X] <= q->ne[X]) &&
        (i->coords[Y] >= q->sw[Y]) && (i->coords[Y] <= q->ne[Y]));
}


/* Create an unfinished quadtree that we can add items to. */
UFQuadTree *qt_create_quadtree(Quadrant *region, BUCKETSIZE maxfill)
{
    UFQuadTree *qt = (UFQuadTree *)_malloc_fast(sizeof(UFQuadTree));

    qt->region   = *region;  /* Bound of entire tree */
    qt->size     = 0;        /* Number of items */
    qt->maxdepth = 0;        /* Max seen so far, not the max allowed */
    qt->maxfill  = maxfill;  /* Max items per bucket (leaf node) */

    qt->ninners  = 1;        /* Number of inner nodes (root node) */
    qt->nleafs   = 0;        /* Number of leaf nodes */

    /* Init root node */
    qt->root = _malloc_fast(sizeof(TransNode));
    qt->root->is_inner = 1;
    _nullify_quadrants(qt->root->quadrants);

    return qt;
}

void qt_free(QuadTree *qt)
{
    free(qt);
}

void qtuf_free(UFQuadTree *qt)
{
    free(qt);
}


/* Insert item into unfinished quadtree */
void qt_insert(UFQuadTree *qt, const Item *item)
{
    qt->size++;

    Item *itmcpy = _malloc_fast(sizeof(Item));
    *itmcpy = *item;

    Quadrant quadrant = qt->region;

    _qt_insert(qt, qt->root, itmcpy, &quadrant, 0);
}



/*
 * Do the actual insertion of an item into an unfinished quadtree, recursing
 * down to the right quadrant and creating new leafs if necessary.
 *
 * Note: *quadrant _is_ modified, but after _qt_insert returns, it is no longer
 * needed (i.e., it's safe to use the address of an auto variable.)
 */
void _qt_insert(UFQuadTree *qt, TransNode *node, Item *item, Quadrant *q,
    unsigned int depth)
{
    if (++depth > qt->maxdepth)
        qt->maxdepth = depth;

RESTART:

    assert(in_quadrant(item, q));
    ASSERT_REGION_SANE(q);

    if (node->is_inner) {
        quadindex quad = 0;

        /* Midpoints of quadrant */
        FLOAT div_x, div_y;
        CALCDIVS(div_x, div_y, q);

        /*
         * Calculate the child quadrant. If item is exactly
         * on a boundary, choose north/east over south/west.
         */
        if (item->coords[X] >= div_x) {
            EAST(quad);
            q->sw[X] = div_x;
        }
        else {
            WEST(quad); /* No effect, here for clarity */
            q->ne[X] = div_x;
        }

        if (item->coords[Y] >= div_y) {
            NORTH(quad); /* No effect, here for clarity */
            q->sw[Y] = div_y;
        }
        else {
            SOUTH(quad);
            q->ne[Y] = div_y;
        }

        ASSERT_REGION_SANE(q);

        /* Make sure child quadrant exists */
        if (node->quadrants[quad] == NULL) {
            node->quadrants[quad] =
                (TransNode *)_malloc_fast(sizeof(TransNode));
            _init_leaf_node(qt, node->quadrants[quad]);
        }

        /* Insert into child quadrant */
        _qt_insert(qt, node->quadrants[quad], item, q, depth);

    }
    else { /* Node is a leaf */
        _ensure_bucket_size(qt, node, q, depth);

        /* Bucket was full, leaf became inner, go again */
        if (node->is_inner)
            goto RESTART;

        /* Do actual insert */
        node->leaf.items[node->leaf.n++] = item;
    }
}


int _itemcmp_direct(Item *a, Item *b)
{
    return _itemcmp(&a, &b);
}

/* Compare two items, returing one of '-1,0,1' like strcmp(3) */
int _itemcmp(Item **aptr, Item **bptr)
{
    Item *a = *aptr, *b = *bptr;

    int wrtx = _FLOATcmp(&a->coords[X], &b->coords[X]);
    int wrty = _FLOATcmp(&a->coords[Y], &b->coords[Y]);

    /*
     * The C99 standard defines '||' so that it returns 0 or 1, so we
     * can't use the more natural expression:
     *    'return wrtx || wrty'
     * Also, don't be introducing any 'if's to this function.
     */
    int v = wrtx + (wrty * !wrtx);

    assert(v == -1 || v == 0 || v == 1);
    assert((wrtx >  0 ) ? (v == 1   ) : 1);
    assert((wrtx == 0 ) ? (v == wrty) : 1);
    assert((wrtx <  0 ) ? (v == -1  ) : 1);

    return v;
}

inline int _FLOATcmp(FLOAT *a, FLOAT *b)
{
    return (*a > *b) - (*a < *b);
}


/* Returns true iff any two items are distinct */
inline bool _distinct_items_exist(TransNode *node)
{
    BUCKETSIZE i;
    for (i=1; i<node->leaf.n; i++) {
        if (_itemcmp(&node->leaf.items[i-1], &node->leaf.items[i]))
            return true;
    }

    return false;
}

inline void _init_leaf_node(UFQuadTree *qt, TransNode *node)
{
    node->is_inner = 0;
    node->leaf.size = qt->maxfill;
    node->leaf.items = _malloc_fast(sizeof(Item *) * node->leaf.size);
    node->leaf.n = 0;
    qt->nleafs++;
}

/*
 * Ensures the node is suitably split so that it can accept _another_ item
 * (i.e., the item hasn't been inserted yet -- node->leaf.n has not been
 * incremented).
 *
 * Note that if the node is a leaf node when passed into the function,
 * it may be an inner node when the function terminates (i.e., the node's
 * type may change as a side effect of this function).
 */
inline void _ensure_bucket_size(UFQuadTree *qt, TransNode *node,
    const Quadrant *quadrant, unsigned int depth)
{
    assert(!node->is_inner);

    /* Split if needed */
    if (node->leaf.n+1 > node->leaf.size) {
        _split_node(qt, node, quadrant, depth);
    }

#ifndef NDEBUG
    /* Could be changed by _split_node() */
    if (!node->is_inner) {
        assert(node->leaf.items != NULL);
        assert(malloc_usable_size(node->leaf.items)
            >= sizeof(*node->leaf.items)*node->leaf.n+1);
        assert(malloc_usable_size(node->leaf.items)
            >= sizeof(*node->leaf.items)*node->leaf.size);
    }
#endif
}


/* Too many items, split the quadrant */
void _split_node(UFQuadTree *qt, TransNode *node, const Quadrant *quadrant,
    unsigned int depth)
{
    if (!_distinct_items_exist(node)) {
        /* All items are the same, we cannot further split the node */
        node->leaf.size *= 2;
        node->leaf.items =
            _realloc(node->leaf.items, node->leaf.size*sizeof(Item *));
    }
    else {
        TransNode cpy = *node;

        /* Turn leaf node into inner */
        node->is_inner = 1;
        qt->ninners++;
        qt->nleafs--;
        _nullify_quadrants(node->quadrants);

        /* Insert items, _qt_insert will create leafs */
        Quadrant quadrant_;
        unsigned int i;
        for (i=0; i<cpy.leaf.n; i++) {
            quadrant_ = *quadrant;

            _qt_insert(qt, node, cpy.leaf.items[i], &quadrant_, depth-1);
        }

        free(cpy.leaf.items);
    }
}


/*
 * Things are stored contigously, so we calculate the full size, so it can be
 * copied or written to file all in one go.
 */
u_int64_t _mem_size(const UFQuadTree *qt)
{
    return
        sizeof(QuadTree) +
        sizeof(Inner)*qt->ninners +
        sizeof(Leaf)*qt->nleafs +
        sizeof(Item)*qt->size;
}

/* Copy parts of an unfinalised quadtree to a soon-to-be finalised one */
void _init_quadtree(QuadTree *new, const UFQuadTree *from)
{
    QuadTree tmp = {
        .region   = from->region,
        .size     = from->size,
        .maxdepth = from->maxdepth,
        .ninners  = from->ninners,
        .nleafs   = from->nleafs,
        .padding  = 0
    };

    memcpy(new, &tmp, sizeof(QuadTree));
}

/* Create a finalised quadtree, optionally writing to a file */
const QuadTree *qt_finalise(const UFQuadTree *qt_, const char *file)
{
    const QuadTree *qt;

    u_int64_t bytes = _mem_size(qt_);
    void *mem = _malloc(bytes);

    /*
     * Initialise the QuadTree before assigning the pointer.
     * This is just because I'm cautious. The elements of QuadTree
     * are all const-s, and so once the quadtree pointer is assigned,
     * the compiler could, in theory, choose to cache the memory
     * without ever re-reading it. Whatever, that would never happen,
     * but call _init_quadtree() before assigning to qt anyway.
     */
    _init_quadtree((QuadTree *)mem, qt_);

    qt  = (QuadTree *)mem;

    FinaliseState st;
    st.quadtree = qt;
    st.ninners  = 0;
    st.cur_trans        = qt_->root;
    st.cur_node.as_void = MEM_INNERS(qt);
    st.next_leaf        = MEM_LEAFS(qt);

    /* Finalise nodes */
    _qt_finalise(&st);

    /* Optionally dump to a file */
    if (file != NULL) {
        int fd = open(file, O_CREAT|O_NOATIME|O_TRUNC|O_RDWR, S_IRUSR|S_IWUSR);

        if (fd == -1) {
            perror("open");
            exit(1);
        }

        if ((ssize_t)bytes != write(fd, mem, bytes)) {
            perror("write (qt_finalise)");
            exit(1);
        }
    }

    return qt;
}

/* Finalise a node (inner or leaf) */
inline void _qt_finalise(FinaliseState *st)
{
    assert(st->cur_trans != NULL);

    if (st->cur_trans->is_inner) {
        _qt_finalise_inner(st);
    }
    else {
        _qt_finalise_leaf(st);
    }
}


/*
 * pre-condition:
 *   * st->cur_node points to the memory address for the translated
 *     st->cur_trans node (no need for *st->cur_node to be zeroed)
 *   * st->next_leaf points to the memory address for the next
 *     leaf (i.e., just after the previous leaf). Again, no need for
 *     this to be zeroed.
 *
 * Side-effects:
 *   * increments st->ninners
 *   * updates st->cur_node to child node
 *   * updates (st->quadtree->mem.as_inner+X)->quadrants[Y]
 */
void _qt_finalise_inner(FinaliseState *st)
{
    int i;
    TransNode *const cur = st->cur_trans;
    Inner *const inner = st->cur_node.as_inner;

    assert(cur   != NULL);
    assert(inner != NULL);

    st->ninners++;

    /* For each child quadrant */
    for (i=0; i<4; i++) {
        /* Set st->cur_trans to the child, ready for recursion */
        st->cur_trans = cur->quadrants[i];

        if (st->cur_trans == NULL) {
            inner->quadrants[i] = ROOT;
        }
        else {
            /*
             * Note: ((Inner *)MEM_INNERS(qt))[st->ninners] is the _next_
             * node to be finalised (i.e., _not_ this one.)
             */
            st->cur_node.as_void = st->cur_trans->is_inner ?
                (void *) &((Inner *)MEM_INNERS(st->quadtree))[st->ninners] :
                (void *) st->next_leaf;

            inner->quadrants[i] =
                st->cur_node.as_void - MEM_INNERS(st->quadtree);

            _qt_finalise(st);
        }
    }

    free(cur);

#ifndef NDEBUG
    st->cur_trans = NULL;
#endif
}

void _qt_finalise_leaf(FinaliseState *st)
{
    assert(IS_LEAF(st->quadtree, st->cur_node.as_void));

    st->next_leaf += sizeof(Leaf) + st->cur_trans->leaf.n*sizeof(Item);

    TransNode *cur = st->cur_trans;

    Leaf *leaf = st->cur_node.as_leaf;
    leaf->n = cur->leaf.n;

    unsigned int i;
    for (i=0; i<leaf->n; i++) {
        leaf->items[i] = *cur->leaf.items[i];

        free(cur->leaf.items[i]);
    }

    free(cur->leaf.items);
    free(cur);

#ifndef NDEBUG
    st->cur_trans = NULL;
#endif
}


/* Create an iterator for items in a quadtree region */
inline Qt_Iterator *qt_query_itr(const QuadTree *qt, const Quadrant *region) {
    /* Allocate memory for both Qt_Iterator and the stack */
    Qt_Iterator *itr = (Qt_Iterator *)_malloc(
        sizeof(Qt_Iterator) +
        sizeof(*itr->stack)*qt->maxdepth);

    itr->quadtree = qt;
    itr->region = *region;

    itr->stack = ((void *)itr)+sizeof(Qt_Iterator);
    itr->so = 0; /* Stack offset */

    itr->stack[0].node.as_node  = MEM_ROOT(qt);
    itr->stack[0].quadrant      = 0;
    itr->stack[0].within_parent = 0;

    /* Figure out all the quadrants */
    _gen_quadrants(&qt->region, itr->stack[0].quadrants);

    _itr_next_recursive(itr);

    return itr;
}


/* Grab the next item from the iterator. When there are no more items, it will
 * return NULL and free the iterator, after which it shouldn't be used. */
inline Item *qt_itr_next(Qt_Iterator *itr) {
ENTER:

    /* If leaf pointer is null, there are no more items left */
    if (itr->lp == NULL) {
        free(itr);
        return NULL;
    }

    /*
     * Cunning use of '*' instead of '&&' to avoid a pipeline stall.
     * Note that no shortcutting is done, so the second operand to '*'
     * is always evaluated, regardless of the outcome of the first,
     * but this doesn't matter: we know that itr->sp points to malloc()ed
     * memory, so won't segfault even if the actual data is not really
     * a Leaf.
     */
    while (itr->stack[(itr->so * !! itr->lp)].node.as_leaf->n - 1
        >= itr->cur_item)
    {
        Item *itm = &itr->lp->items[itr->cur_item++];
        if (in_quadrant(itm, &itr->region))
            return itm;
    }

    itr->so--;
    itr->stack[itr->so].quadrant++;
    _itr_next_recursive(itr);

    goto ENTER;
}


void _itr_next_recursive(Qt_Iterator *itr)
{
RECURSE:
    assert(itr->so >= 0);

    register int so = itr->so;

    if (IS_LEAF(itr->quadtree, FRAME(itr,so).node.as_node)) {
        /*
         * Done. (Success)
         *
         * post-conditions:
         *   * itr->stack[itr->so].quadrant     has _not_ been traversed yet.
         *   * itr->stack[itr->so -1].quadrant  has _not_ been traversed yet.
         *   * itr->stack[itr->so].curitem      has _not_ been returned yet.
         *   * itr->stack[itr->so   ]           is the currently-visited node
         */

        itr->cur_item = 0;
        itr->lp = FRAME(itr,so).node.as_node;
        return;
    }

    /*
     * invariant: itr->stack[itr->so].quadrant has _not_ been traversed yet.
     */
    while (itr->so >= 0) {
        assert(IS_INNER(itr->quadtree, FRAME(itr,so).node.as_node));

        /* Loop through each quadrant */
        while (FRAME(itr,so).quadrant != QUAD) {

            /* Skip empty/uninitialised quadrants */
            if (FRAME(itr,so).node.as_inner->quadrants[
                FRAME(itr,so).quadrant ] == ROOT)
            {
                goto NEXTQUADRANT;
            }


            /* If this quadrant is within the query region, traverse downwards
             * into the child node.
             */
            if (OVERLAP(itr->region,
                FRAME(itr,so).quadrants[FRAME(itr,so).quadrant]))
            {
                assert(itr->so >= 0);
                assert(OVERLAP(FRAME(itr,so).quadrants[FRAME(itr,so).quadrant],
                    itr->region));

                /* Enter the child */
                so++;
                itr->so = so;

                FRAME(itr,so).quadrant = 0;
                _gen_quadrants(
                    &PREVFRAME(itr,so).quadrants[PREVFRAME(itr,so).quadrant],
                    FRAME(itr,so).quadrants
                );

                /* FRAME(itr,so).region already initialised above */

                FRAME(itr,so).within_parent =
                    PREVFRAME(itr,so).within_parent ||
                    CONTAINED(
                        PREVFRAME(itr,so).quadrants[PREVFRAME(itr,so).quadrant],
                        itr->region
                    );

                FRAME(itr,so).node.as_node = (Node *)
                    (MEM_INNERS(itr->quadtree) +
                    PREVFRAME(itr,so).node.as_inner->quadrants[
                        PREVFRAME(itr,so).quadrant
                    ]);

                /* Recurse.
                 *
                 * post-conditions:
                 *   * itr->stack[itr->so].quadrant
                 *      has _not_ been traversed yet.
                 *   * itr->stack[itr->so -1].quadrant
                 *      has _not_ been traversed yet.
                 *   * itr->stack[itr->so   ]
                 *     is the currently-visited node
                 */

                goto RECURSE;
            }

NEXTQUADRANT:
            /* Skip to the next quadrant */
            FRAME(itr,so).quadrant++;

        }

        /* No quadrants on this node remaining --- backtrack one node */
        so--;
        itr->so = so;

        if (itr->so >= 0)
            FRAME(itr,so).quadrant++;

    }

    assert(itr->so == -1);

    itr->lp = NULL;

    /* Failure.
     *
     * post-condition:
     *   itr->lp == NULL, and by virtue of this,
     *   the next call to qt_itr_next() will return NULL.
     */
    return;
}


/* Figure out all the quadrants together, for speed */
inline void _gen_quadrants(const Quadrant *region, Quadrant *mem)
{
    ASSERT_REGION_SANE(region);

    /* Midpoints of region */
    FLOAT div_x, div_y;
    CALCDIVS(div_x, div_y, region);

    mem[NE].ne[X] = region->ne[X];
    mem[NE].ne[Y] = region->ne[Y];
    mem[NE].sw[X] = div_x;
    mem[NE].sw[Y] = div_y;

    mem[SE].ne[X] = region->ne[X];
    mem[SE].ne[Y] = div_y;
    mem[SE].sw[X] = div_x;
    mem[SE].sw[Y] = region->sw[Y];

    mem[SW].ne[X] = div_x;
    mem[SW].ne[Y] = div_y;
    mem[SW].sw[X] = region->sw[X];
    mem[SW].sw[Y] = region->sw[Y];

    mem[NW].ne[X] = div_x;
    mem[NW].ne[Y] = region->ne[Y];
    mem[NW].sw[X] = region->sw[X];
    mem[NW].sw[Y] = div_y;

    ASSERT_REGION_SANE(&mem[NE]);
    ASSERT_REGION_SANE(&mem[SE]);
    ASSERT_REGION_SANE(&mem[SW]);
    ASSERT_REGION_SANE(&mem[NW]);
}

/* Get an array of at most maxn items from the specified region */
Item **qt_query_ary(const QuadTree *quadtree, const Quadrant *region,
    u_int64_t *maxn)
{
    u_int64_t alloced = 32;
    Item **items = _malloc(sizeof(*items) * alloced);

    /* Get an iterator */
    Qt_Iterator *itr = qt_query_itr(quadtree, region);

    /* Iterate through all items */
    u_int64_t i;
    for (i=0; (items[i] = qt_itr_next(itr)) != NULL; i++) {
        /* At most maxn items */
        if (*maxn > 0 && i >= *maxn)
            break;

        /* Exponential allocation */
        if (i+1 >= alloced) {
            alloced *= 2;
            items = _realloc(items, sizeof(*items) * alloced);
        }
    }

    /* Free iterator */
    free(itr);

    /* Return size and items */
    *maxn = i;
    return items;
}


/* Get an array of at most maxn items from the specified region */
Item **qt_query_ary_fast(const QuadTree *quadtree, const Quadrant *region,
    u_int64_t *maxn)
{
    u_int64_t alloced = 4096 / sizeof(Item *);
    Item **items = _malloc_fast(sizeof(*items) * alloced);

    /* Get an iterator */
    Qt_Iterator *itr = qt_query_itr(quadtree, region);

    u_int64_t i=0;
    while (itr->lp != NULL) {
        /* At most maxn items */
        if ((*maxn != 0) && (i >= *maxn))
            break;

        _include_leaf(&items, &i, &alloced, itr->lp, &itr->region,
            itr->stack[itr->so].within_parent);

        itr->so--;
        itr->stack[itr->so].quadrant++;
        _itr_next_recursive(itr);
    }

    /* Free iterator */
    free(itr);

    /* Return size and items */
    *maxn = i;
    return items;
}

/*
 * Note: offset is the position at which we can start storing items
 * (i.e., *items+offset must not already contain an item).
 */
inline void _include_leaf(Item ***items, u_int64_t *offset, u_int64_t *size,
    Leaf *leaf, Quadrant *quadrant, bool within)
{
    assert(leaf != NULL);

    u_int32_t i;

    /* Do _not_ put this inside the for() loop to try and save memory! */
    u_int64_t required = *offset + leaf->n;  /* _not_ +1 */

    /* Exponential allocation */
    if (required > *size) {
        *size = required * 2;
        *items = (Item **)realloc(*items, sizeof(Item *) * *size);
        assert(*items != NULL);
    }

    Item  *const addr   = leaf->items;
    Item **const itemsv = *items;
    u_int64_t offsetv = *offset;

    if (within) {
#ifndef NDEBUG
        withins++;
#endif

        /*
         * It would be so cool to bypass the L2 cache right now, writing direct
         * to memory
         */
        for (i=0; i<leaf->n; i++)
            itemsv[offsetv + i] = addr+i;

    }
    else {
#ifndef NDEBUG
        nwithins++;
#endif

        u_int64_t j;
        for (i=0, j=0; j<leaf->n; j++) {
            if (in_quadrant(addr+j, quadrant))
                itemsv[offsetv + i++] = addr+j;
        }
    }

    *offset+= i;
}


/* Read a file into memory  */
void _read_mem(void *mem, int fd, u_int64_t bytes)
{
    /*
     * This is the page size of the virtual memory pages, not the
     * filesystem or IO device pages. It also happens that I don't
     * care --- I just needed a reasonable block size to read.
     */
    const int pagesize = getpagesize();

    u_int64_t npages = bytes/pagesize;
    u_int64_t rest   = bytes%pagesize;
    u_int64_t i;
    ssize_t got;

    /* XXX (Vincent): When would this ever overflow? Perhaps just assert */
    if ((npages*pagesize + rest) != bytes) {
        fprintf(stderr,
            "_read_mem: %" PRIu64 "*%d + %" PRIu64 " != %" PRIu64 "\n",
            npages, pagesize, rest, bytes);
        exit(1);
    }

    /* Read in each page */
    for (i=0; i<npages; i++) {
        got = read(fd, mem+i*pagesize, pagesize);

        /* Make sure we read in a page */
        if (got == -1) {
            perror("read (_read_mem)");
            exit(1);
        }
        else if (got != pagesize) {
            fprintf(stderr, "unexpected retval from read: %ld:%d (_read_mem)",
                (signed long)got, pagesize);
            exit(1);
        }

        /* Hint that the file shouldn't be cached */
        posix_fadvise(fd, i*pagesize, pagesize, POSIX_FADV_DONTNEED);
    }


    /* Read the smaller-than-a-page leftovers at end of file */
    got = read(fd, mem+npages*pagesize, rest);

    /* Make sure we read the right ammount */
    if (got == -1) {
        perror("read (_read_mem)");
        exit(1);
    }
    else if (got != (ssize_t)rest) {
        fprintf(stderr,
            "unexpected return value from read: %lu:%" PRIu64 " (_read_mem)",
            (unsigned long)got, rest);
        exit(1);
    }

    /* Hint that the file shouldn't be cached */
    posix_fadvise(fd, npages*pagesize, pagesize, POSIX_FADV_DONTNEED);
}


/* Load quadtree from a file (written by qt_finalise()) */
extern const QuadTree *qt_load(const char *file)
{
    QuadTree *qt;
    void *mem;

    /* Must pass a file */
    if (file == NULL) {
        fprintf(stderr, "error: no file given to qt_load\n");
        exit(1);
    }

    /* Open file */
    int fd = open(file, O_RDONLY|O_NOATIME);
    if (fd == -1) {
        perror("open (qt_load)");
        exit(1);
    }

    /* Stat for filesize */
    struct stat finfo;
    if (fstat(fd, &finfo)) {
        perror("stat (qt_load)");
        exit(1);
    }

    /* Malloc size to fit whole file */
    mem = _malloc(finfo.st_size);

    /* Read file into memory */
    _read_mem(mem, fd, finfo.st_size);

    /* Close file */
    close(fd);

    /* The quadtree is packed, so it is read directly from file */
    qt = (QuadTree *) mem;
    return qt;
}

