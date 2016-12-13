/*******************************************************************************
    Copyright (c) 2014 NVIDIA Corporation

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

        The above copyright notice and this permission notice shall be
        included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

*******************************************************************************/

#include "uvm_lite_region_tracking.h"
#include "uvm_common.h"

struct _tree_node
{
    unsigned long long start;
    unsigned long long end;
    void *data;
    UvmCommitRecord *owner;
    struct rb_node rb;
};

static struct kmem_cache * g_uvmTrackingTreeCache         __read_mostly = NULL;
static struct kmem_cache * g_uvmTrackingTreeNodeCache     __read_mostly = NULL;

NV_STATUS uvm_regiontracker_init(void)
{
    g_uvmTrackingTreeCache = NV_KMEM_CACHE_CREATE("uvm_region_tracker_t",
                                                  struct s_UvmRegionTracker);

    if (!g_uvmTrackingTreeCache)
        return NV_ERR_NO_MEMORY;

    g_uvmTrackingTreeNodeCache = NV_KMEM_CACHE_CREATE("uvm_region_tracker_node_t",
                                                      struct _tree_node);

    if (!g_uvmTrackingTreeNodeCache)
        return NV_ERR_NO_MEMORY;

    return NV_OK;
}

void uvm_regiontracker_exit(void)
{
    kmem_cache_destroy_safe(&g_uvmTrackingTreeNodeCache);
    kmem_cache_destroy_safe(&g_uvmTrackingTreeCache);
}

/* This tree tracks all the memory records used by UVM.
   In order to do so, it uses an rb_tree (Linux kernel's red-black tree).
   The compare function used by the tree is the following function:

   * (A = B) [------] A
             [------] B

   * (A < B) 1. [------------] B
                    [-----]    A

             2.        [-----] B
                [-----]        A

   * (A > B) 1. [------------] A
                    [-----]    B

             2.        [-----] A
                [-----]        B

   * Undefinded:
             1. [---------]    B
                       [-----] A

             2.      [-------] B
                [------]       A
*/

static struct rb_node *_uvm_find_containing_region(struct rb_root *tree,
                                                   unsigned long long start,
                                                   unsigned long long end)
{
    struct rb_node *new = tree->rb_node;
    struct rb_node *parent = NULL;

    while (new)
    {
        struct _tree_node *entry = rb_entry(new, struct _tree_node, rb);
        if (start >= entry->start && end <= entry->end)
        {
            parent = new;
            new = new->rb_left;
        }
        else if (start < entry->start)
            new = new->rb_left;
        else
            new = new->rb_right;
    }
    return parent;
}

static void _uvm_insert_region(struct rb_root *tree,
                               unsigned long long start,
                               unsigned long long end,
                               struct _tree_node *node)
{
    struct rb_node **new = &tree->rb_node;
    struct rb_node *parent = NULL;

    while (*new)
    {
        struct _tree_node *entry = rb_entry(*new, struct _tree_node, rb);
        parent = *new;
        if (start >= entry->start && end <= entry->end)
            new = &parent->rb_left;
        else if (start < entry->start)
            new = &parent->rb_left;
        else
            new = &parent->rb_right;
    }
    rb_link_node(&node->rb, parent, new);
    rb_insert_color(&node->rb, tree);
}

void uvm_track_region(UvmRegionTracker *tree,
                      unsigned long long start,
                      unsigned long long end,
                      void *trackdata, UvmCommitRecord *owner)
{
    struct _tree_node *node = kmem_cache_alloc(g_uvmTrackingTreeNodeCache,
                                               NV_UVM_GFP_FLAGS);

    node->start = start;
    node->end = end;
    node->data = trackdata;
    node->owner = owner;

    down_write(&tree->privLock);
    _uvm_insert_region(&tree->rb_root, start, end, node);
    up_write(&tree->privLock);
}

void *uvm_untrack_region(UvmRegionTracker * tree,
                         unsigned long long start,
                         unsigned long long end)
{
    struct rb_node *node = NULL;

    down_write(&tree->privLock);
    node = _uvm_find_containing_region(&tree->rb_root, start, end);
    if (node)
    {
        struct _tree_node *entry = rb_entry(node, struct _tree_node, rb);
        void *data = entry->data;
        rb_erase(&entry->rb, &tree->rb_root);
        kmem_cache_free(g_uvmTrackingTreeNodeCache, entry);
        up_write(&tree->privLock);
        return data;
    }
    up_write(&tree->privLock);
    return NULL;
}

NV_STATUS uvm_get_info_from_address(UvmRegionTracker *tree,
                                    unsigned long long address,
                                    void **trackdata, UvmCommitRecord **owner)
{
    NV_STATUS status = NV_ERR_OBJECT_NOT_FOUND;
    struct rb_node *parent = NULL;

    if (trackdata)
        *trackdata = NULL;
    if (owner)
        *owner = NULL;

    down_read(&tree->privLock);
    parent = tree->rb_root.rb_node;
    while (parent)
    {
        struct _tree_node *entry = rb_entry(parent, struct _tree_node, rb);
        if (address >= entry->end)
        {
            parent = parent->rb_right;
        }
        else
        {
            if (address >= entry->start)
            {
                if (trackdata)
                    *trackdata = entry->data;
                if (owner)
                    *owner = entry->owner;

                status = NV_OK;
            }
            parent = parent->rb_left;
        }
    }
    up_read(&tree->privLock);
    return status;
}

NV_STATUS uvm_get_info_from_region(UvmRegionTracker *tree,
                                   unsigned long long start,
                                   unsigned long long end,
                                   void **trackdata, UvmCommitRecord **owner)
{
    struct rb_node *node = NULL;

    down_read(&tree->privLock);
    node = _uvm_find_containing_region(&tree->rb_root, start, end);
    if (node)
    {
        struct _tree_node *entry = rb_entry(node, struct _tree_node, rb);
        if (trackdata)
            *trackdata = entry->data;
        if (owner)
            *owner = entry->owner;
        up_read(&tree->privLock);
        return NV_OK;
    }
    up_read(&tree->privLock);
    return NV_ERR_OBJECT_NOT_FOUND;
}

NV_STATUS uvm_get_trackdata_from_address(UvmRegionTracker *tree,
                                         unsigned long long address,
                                         void **trackdata)
{
    return uvm_get_info_from_address(tree, address, trackdata, NULL);
}

NV_STATUS uvm_get_owner_from_address(UvmRegionTracker *tree,
                                     unsigned long long address,
                                     UvmCommitRecord **owner)
{
    return uvm_get_info_from_address(tree, address, NULL, owner);
}

NV_STATUS uvm_get_trackdata_from_region(UvmRegionTracker *tree,
                                        unsigned long long start,
                                        unsigned long long end,
                                        void **trackdata)
{
    return uvm_get_info_from_region(tree, start, end, trackdata, NULL);
}

NV_STATUS uvm_get_owner_from_region(UvmRegionTracker *tree,
                                    unsigned long long start,
                                    unsigned long long end,
                                    UvmCommitRecord **owner)
{
    return uvm_get_info_from_region(tree, start, end, NULL, owner);
}


UvmRegionTracker *uvm_create_region_tracker(struct vm_area_struct *vma)
{
    UvmRegionTracker *tree = NULL;
    tree = kmem_cache_alloc(g_uvmTrackingTreeCache, NV_UVM_GFP_FLAGS);

    if (tree == NULL)
        return NULL;

    tree->rb_root = RB_ROOT;
    tree->vma = vma;
    init_rwsem(&tree->privLock);
    return tree;
}

static void _uvm_destroy_node(struct rb_root *tree,
                              struct rb_node *node,
                              UvmTrackingTreeDestroyNode destroyFunc)
{
    if (node)
    {
        struct _tree_node *entry =
            rb_entry(node, struct _tree_node, rb);
        if (entry)
        {
            destroyFunc(entry->owner);
        }
        rb_erase(node, tree);
        kmem_cache_free(g_uvmTrackingTreeNodeCache, entry);
    }
}

static void _uvm_destroy_included_regions_rec(struct rb_root *tree,
                                              struct rb_node *node,
                                              unsigned long long start,
                                              unsigned long long end,
                                              UvmTrackingTreeDestroyNode destroyFunc)
{
    if (node)
    {
        struct _tree_node *entry =
            rb_entry(node, struct _tree_node, rb);
        if (entry)
        {
            if (start == entry->start && end == entry->end)
            {
                _uvm_destroy_included_regions_rec(tree, node->rb_left,
                                                  start, end, destroyFunc);
            }
            else if (start >= entry->start && end <= entry->end)
            {
                _uvm_destroy_included_regions_rec(tree, node->rb_left,
                                                  start, end, destroyFunc);
                _uvm_destroy_included_regions_rec(tree, node->rb_right,
                                                  start, end, destroyFunc);
                _uvm_destroy_node(tree, node, destroyFunc);
            }
        }
    }
}

void uvm_destroy_included_regions(UvmRegionTracker *tree,
                                  unsigned long long start,
                                  unsigned long long end,
                                  UvmTrackingTreeDestroyNode destroyFunc)
{
    struct rb_node *node = NULL;
    if (!tree)
        return;
    node = _uvm_find_containing_region(&tree->rb_root, start, end);
    if (!node)
        return;
    _uvm_destroy_included_regions_rec(&tree->rb_root, node,
                                      start, end, destroyFunc);
}

void uvm_destroy_region_tracker(UvmRegionTracker *tree,
                                UvmTrackingTreeDestroyNode destroyFunc)
{
    if (tree == NULL)
        return;

    while (tree->rb_root.rb_node && tree->rb_root.rb_node->rb_left)
        _uvm_destroy_node(&tree->rb_root, tree->rb_root.rb_node->rb_left,
                          destroyFunc);

    while (tree->rb_root.rb_node && tree->rb_root.rb_node->rb_right)
        _uvm_destroy_node(&tree->rb_root, tree->rb_root.rb_node->rb_right,
                          destroyFunc);

    if (tree->rb_root.rb_node)
        _uvm_destroy_node(&tree->rb_root, tree->rb_root.rb_node, destroyFunc);

    kmem_cache_free(g_uvmTrackingTreeCache, tree);
}
