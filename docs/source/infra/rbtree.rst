Red-Black Tree
==============

The JNU kernel uses an intrusive red-black tree (``rbtree.h``) modeled
after the Linux kernel's implementation. "Intrusive" means that the tree
node (``struct rb_node``) is embedded directly in the containing structure
rather than allocated separately, which eliminates per-node heap allocations
and avoids a pointer indirection on every access.

The rbtree is used as the backing data structure for:

- The VMA tree in each ``struct addr_space`` (keyed by ``vma->start``).
- Any future data structure that requires ``O(log n)`` ordered lookup.

Data Structures
---------------

.. code-block:: c

   enum rb_color { RB_RED = 0, RB_BLACK = 1 };

   struct rb_node {
       struct rb_node *parent;
       struct rb_node *left;
       struct rb_node *right;
       enum rb_color   color;
   };

   struct rb_root {
       struct rb_node *root;  /* NULL for an empty tree */
   };

   #define RB_ROOT ((struct rb_root){ .root = NULL })

Embedding Pattern
-----------------

To use the rbtree, embed ``struct rb_node`` as a member of the payload
type and recover the payload pointer using the ``container_of`` idiom.
Example (from ``mm/vma.c``):

.. code-block:: c

   struct vma {
       struct rb_node  rb;    /* must be first, or use container_of */
       vaddr_t         start;
       vaddr_t         end;
       uint32_t        flags;
   };

   /* Recover the vma from a node pointer: */
   struct vma *v = (struct vma *)node;   /* rb is the first member */

API
---

.. code-block:: c

   void rb_init(struct rb_root *root);

Initializes ``root->root`` to ``NULL``. Equivalent to assigning
``RB_ROOT``.

**Insertion** uses a two-step idiom. The caller locates the correct parent
slot by traversing the tree with its own comparator, then calls:

.. code-block:: c

   void rb_link_node(struct rb_node *node, struct rb_node *parent,
                     struct rb_node **slot);

Links ``node`` as the child of ``parent`` via ``slot`` (which is either
``&parent->left`` or ``&parent->right``). Does not maintain the
red-black invariants; must be followed immediately by:

.. code-block:: c

   void rb_insert_color(struct rb_root *root, struct rb_node *node);

Restores the red-black tree invariants after a plain link by performing
rotations and recolorings. Runs in ``O(log n)`` amortized time.

.. code-block:: c

   void rb_erase(struct rb_root *root, struct rb_node *node);

Removes ``node`` from ``root`` and restores the invariants. Runs in
``O(log n)``. The caller is responsible for freeing the memory of the
removed node and its enclosing structure.

.. code-block:: c

   struct rb_node *rb_first(const struct rb_root *root);

Returns the leftmost (smallest-keyed) node in the tree, or ``NULL`` if
the tree is empty. Runs in ``O(log n)``.

.. code-block:: c

   struct rb_node *rb_next(const struct rb_node *node);

Returns the in-order successor of ``node``, or ``NULL`` if ``node`` is
the rightmost element. Used with ``rb_first()`` to iterate the tree in
sorted order. Runs in ``O(log n)`` amortized (``O(1)`` in the common case
of a right child being absent).

Selftests
---------

``rbtree_selftest()`` inserts a set of nodes in random order, verifies
the in-order traversal is sorted, erases half of them, and checks that
the remaining nodes are still consistently ordered and colored.
