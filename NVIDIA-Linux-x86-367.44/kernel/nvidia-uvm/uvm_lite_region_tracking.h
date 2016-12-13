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

#ifndef _UVM_LITE_REGION_TRACKING_H
#define _UVM_LITE_REGION_TRACKING_H

#include "uvm_linux.h"
#include "uvm_lite.h"



typedef struct s_UvmRegionTracker
{
    struct rb_root rb_root;
    struct vm_area_struct *vma;
    DriverPrivate* osPrivate;
    struct rw_semaphore privLock;
} UvmRegionTracker;

NV_STATUS uvm_regiontracker_init(void);
void uvm_regiontracker_exit(void);

// This function add a new region in the tracking tree.
// Owner and track data can be NULL.
void uvm_track_region(UvmRegionTracker * tree,
                      unsigned long long start,
                      unsigned long long end,
                      void *trackdata, UvmCommitRecord *owner);
// Delete a region that is tracked by the tree and returns the
// associated tracking data.
// NOTE: This function does not free owner and trackdata.
void *uvm_untrack_region(UvmRegionTracker * tree,
                         unsigned long long start,
                         unsigned long long end);

// Get the information (trackdata and owner) associated to a specific
// address. if trackdata or owner is NULL then the variable is not set.
NV_STATUS uvm_get_info_from_address(UvmRegionTracker * tree,
                                    unsigned long long address,
                                    void **trackdata, UvmCommitRecord **owner);
// Does the same thing as uvm_get_info_from_address but for a region.
// if the attributes of the region are not coherent then the behavior is
// not defined.
NV_STATUS uvm_get_info_from_region(UvmRegionTracker * tree,
                                   unsigned long long start,
                                   unsigned long long end,
                                   void **trackdata, UvmCommitRecord **owner);

// Return only the trackdata associated to an address
NV_STATUS uvm_get_trackdata_from_address(UvmRegionTracker * tree,
                                         unsigned long long address,
                                         void **trackdata);

// Return only the owner associated to an address
NV_STATUS uvm_get_owner_from_address(UvmRegionTracker * tree,
                                     unsigned long long address,
                                     UvmCommitRecord **owner);

// Return only the trackdata associated to a region
NV_STATUS uvm_get_trackdata_from_region(UvmRegionTracker * tree,
                                        unsigned long long start,
                                        unsigned long long end,
                                        void **trackdata);

// Return only the owner associated to a region
NV_STATUS uvm_get_owner_from_region(UvmRegionTracker * tree,
                                    unsigned long long start,
                                    unsigned long long end,
                                    UvmCommitRecord **owner);

typedef void (*UvmTrackingTreeDestroyNode) (UvmCommitRecord *owner);

UvmRegionTracker *uvm_create_region_tracker(struct vm_area_struct *vma);

// Delete all the commit included in the specified region
// NOTE: This is a strict inclusion so if a commit match the region
// boundaries it is not destroyed.
void uvm_destroy_included_regions(UvmRegionTracker *tree,
                                  unsigned long long start,
                                  unsigned long long end,
                                  UvmTrackingTreeDestroyNode destroyFunc);

void uvm_destroy_region_tracker(UvmRegionTracker * regionTracker,
                                UvmTrackingTreeDestroyNode destroyFunc);

#endif
