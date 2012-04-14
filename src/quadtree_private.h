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

#ifndef QUADTREE_PRIVATE_h
#define QUADTREE_PRIVATE_h

#include <stdio.h>

#include "quadtree.h"



typedef void Node;


struct _Inner;
struct _Leaf;
struct _TransNode;
struct _FinaliseState;
struct _Qt_Itr_Frame;

typedef struct _TransNode       TransNode;
typedef struct _Inner           Inner;
typedef struct _Leaf            Leaf;
typedef struct _FinaliseState   FinaliseState;
typedef struct _Qt_Itr_Frame    Qt_Itr_Frame;





/*
 * Note that both Inner and Leaf are __packed__ for portability.
 * (see: http://gcc.gnu.org/onlinedocs/gcc/Type-Attributes.html)
 * This is because the structs are written to files, and the
 * binary which reads them may not be compiled on the same
 * architecture as that which wrote them. To be truly portable
 * we ought to standardise on a endianness too
 * (see the functions in endian(3)). This is on my TODO list.
 *
 * This also applies to struct Item, defined in quadtree.h
 */

struct _Inner {
  /* quadrants:
   *   offsets in bytes to other _Inner or _Leaf structs.
   *   an offset of 0 means that there is no child.
   */
  u_int64_t quadrants[4];
} __attribute__ ((__packed__));

struct _Leaf {
  u_int64_t n;
  Item items[];
} __attribute__ ((__packed__));

/* A transient node: soon it will be either an inner or leaf node */
struct _TransNode {
  _Bool is_inner;
  union {
    TransNode *quadrants[4];
    struct {
      Item **items;
      unsigned int n;
      unsigned int size;

      /* Notes:
       * size:
           if non-0, indicates that this bucket is too big, and that
           the reason for this is that it contains identically-coordinated
           nodes that cannot be split into other buckets. The actual
           value indicates the size (in number of items, not bytes) of
           the memory malloc()ed for the bucket.
      */
    } leaf;
  };  /* "unnamed struct/union": http://gcc.gnu.org/onlinedocs/gcc/Unnamed-Fields.html */
};




struct UFQuadTree {

  TransNode *root;

  Quadrant region;

  u_int64_t size;

  u_int32_t maxdepth;
  u_int32_t maxfill;

  u_int64_t ninners;
  u_int64_t nleafs;

};


struct _Qt_Itr_Frame {
  union {
    Leaf  *as_leaf;
    Inner *as_inner;
    Node  *as_node;
  } node;

  Quadrant  quadrants[4];
  quadindex quadrant;

  /* Should _all_ items under (or in) this node be included? */
  _Bool within_parent;
};

  /* I am scared of floating point errors if I don't explicitly store
   * the quadrant values. That is, if we have:
   *
   * x1 = x0/2
   * x2 = x1/2
   * ...
   * xn = x(n-1)/2
   *
   * Can we be sure of recovering the original value of x0 if we follow
   * the reverse method?
   *
   * x(n-1) = xn*2
   * ...
   * x0     = x1*2
   *
   * If at any point the floating point division overflowed, causing
   * a loss of precision, then the reverse process will not necessarily
   * result in an exact recovery of x0. This would be bad.
   *
   * Therefore, I just explicitly record the xi-th value, even though
   * I could probably record just x0, and find xi = x0 * 2^-i
   */


struct Qt_Iterator {

  Qt_Itr_Frame *stack;

  Leaf *lp;  /* leaf pointer: Not an optimisation for a good compiler,
              * but improves code clarity (in my opinion)
              */

  int so;    /* stack offset (both node and quad) */

  u_int64_t cur_item;

  Quadrant region;

  const QuadTree *quadtree;

};



struct _FinaliseState {

  const QuadTree *quadtree;

  u_int64_t ninners;

  union {
    void  *as_void;
    Leaf  *as_leaf;
    Inner *as_inner;
  } cur_node;

  void *next_leaf;

  TransNode *cur_trans;

};


inline void _ensure_child_quad(UFQuadTree *qt, TransNode *node, quadindex quad, Item *item);
inline void _ensure_bucket_size(UFQuadTree *qt, TransNode *node, const Quadrant *quadrant, unsigned int depth);
inline int _FLOATcmp(FLOAT *a, FLOAT *b);
inline int _count_distinct_items(TransNode *node);
inline void _finalise(FinaliseState *st);
inline _Bool in_quadrant(const Item *i, const Quadrant *q);
inline void _target_quadrant(quadindex q, Quadrant *region);
inline void _qt_finalise(FinaliseState *st);
inline void _include_leaf(Item ***items, u_int64_t *offset, u_int64_t *size, Leaf *leaf, Quadrant *region,_Bool within);
inline void _gen_quadrants(const Quadrant *region, Quadrant *mem);


void  _qt_insert(UFQuadTree *qt, TransNode *node, Item *item, Quadrant *quadrant, unsigned int depth);
int   _itemcmp(Item **a, Item **b);
int   _itemcmp_direct(Item *a, Item *b);
void  _split_node(UFQuadTree *qt, TransNode *node, const Quadrant *quadrant, unsigned int depth);
void  _init_root(UFQuadTree *qt);
void  _itr_next_recursive(Qt_Iterator *itr);
void  _free_itr(Qt_Iterator *itr);
void  _mk_quadtree(QuadTree *new, const UFQuadTree *from);
void _read_mem(void *mem, int fd, u_int64_t bytes);
u_int64_t _mem_size(const UFQuadTree *qt);




/*
 * Coordinates:
 *
 *
 * N: 0X
 * S: 1X
 * E: X1
 * W: X0
 *
 * +---------+
 * | 00 | 01 |
 * +----+----+
 * | 10 | 11 |
 * +----+----+
 *
 *
 ***************/




#define NORTH(x) ((x) = (x) & NE)
#define SOUTH(x) ((x) = (x) | SW)
#define  EAST(x) ((x) = (x) | NE)
#define  WEST(x) ((x) = (x) & SW)

#define ISSOUTH(x) ((x) & SW)
#define ISNORTH(x) (!ISSOUTH(x))

#define ISEAST(x)  ((x) & NE)
#define ISWEST(x)  (!ISEAST(x))





#define OVERLAP(qA, qB)                                         \
  (                                                             \
   ((qA).sw[X] <= (qB).ne[X]) * ((qA).sw[Y] <= (qB).ne[Y])      \
   *                                                            \
   ((qA).ne[X] >= (qB).sw[X]) * ((qA).ne[Y] >= (qB).sw[Y])      \
  )

#define CONTAINED(inner, outer)                                 \
  (                                                             \
   ((inner).sw[X] >= (outer).sw[X]) *                           \
   ((inner).sw[Y] >= (outer).sw[Y]) *                           \
                                                                \
   ((inner).ne[X] <= (outer).ne[X]) *                           \
   ((inner).ne[Y] <= (outer).ne[Y])                             \
  )



#define ASSERT_REGION_SANE(region)            \
  assert((region)->ne[X] > (region)->sw[X]);  \
  assert((region)->ne[Y] > (region)->sw[Y]);





#define IS_LEAF(quadtree, node)  \
  ((node) >= MEM_LEAFS(quadtree))

#define IS_INNER(quadtree, node) \
  !IS_LEAF(quadtree, node)



#define MEM_INNERS(quadtree) ((void *)(((void *)(quadtree))+sizeof(QuadTree)))
#define MEM_LEAFS(quadtree)  ((void *)(MEM_INNERS(quadtree) + (quadtree)->ninners*sizeof(Inner)))


#define CALCDIVS(div_x, div_y, region)                                 \
  div_x = (region)->sw[X] + ((region)->ne[X] - (region)->sw[X]) / 2;   \
  div_y = (region)->sw[Y] + ((region)->ne[Y] - (region)->sw[Y]) / 2;




#define MEM_ROOT(quadtree) MEM_INNERS(quadtree)

#define ROOT ((u_int64_t)0)




#define FRAME(itr,so)     ((itr)->stack[so])
#define NEXTFRAME(itr,so) ((itr)->stack[so+1])
#define PREVFRAME(itr,so) ((itr)->stack[so-1])


#endif
