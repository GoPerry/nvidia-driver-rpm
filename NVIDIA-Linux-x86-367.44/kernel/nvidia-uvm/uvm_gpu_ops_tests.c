/*******************************************************************************
    Copyright (c) 2014-2015 NVIDIA Corporation

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

#include "nv_uvm_interface.h"
#include "uvm_gpu_ops_tests.h"
#include "uvm_page_migration.h"
#include "uvm_common.h"
#include "uvm_linux.h"
#include "uvm_lite_region_tracking.h"
#include "uvm_lite.h"
#include "uvm_channel_mgmt.h"

// Helper structure to define a test surface.
typedef struct UvmTestSurface_t
{
    UvmGpuPointer       gpuPointer;
    void                *cpuPointer;
    NvU64               size;
    unsigned            aperture;
} UvmTestSurface;

// Helper structure to define a copy operation.
typedef struct UvmTestCopyOp_t
{
    UvmGpuPointer       dstGpuPointer;
    UvmGpuPointer       srcGpuPointer;
    unsigned            dstAperture;
    unsigned            srcAperture;
    NvU64               surfSize;
    unsigned            copyFlags;    // SRC|DST PHYSICAL|VIRUAL
} UvmTestCopyOp;

NV_STATUS gpuOpsSampleTest(NvProcessorUuid  * pUuidStruct)
{
    UVM_DBG_PRINT_UUID("Entering", pUuidStruct);
    return NV_OK;
}

void _destroy_dummy_region(UvmCommitRecord * commit)
{
    const unsigned long long REGION_NUMBER = 0x1000;
    // The commit is supposed to be a fake commit
    // just check that the pointer contain the magick number
    if (commit > (UvmCommitRecord *) REGION_NUMBER)
        UVM_ERR_PRINT_NV_STATUS("Ask to destroy an invalid commit",
                                NV_ERR_INVALID_ARGUMENT);
}

//
// Simple Region Tracking Sanity Test
//
//   1. Create a region tracking tree
//
//   2. Add a regions in the tree
//
//   3. Check the regions
//
//   4. Destroy the region tracking tree
//
NV_STATUS regionTrackerSanityTest(void)
{
    const unsigned long long REGION_NUMBER = 0x1000;
    const unsigned long long REGION_SIZE = 0x1000;

    NV_STATUS status = NV_OK;
    struct vm_area_struct vma;
    UvmRegionTracker *region_tracker = NULL;
    unsigned long long region = 0;
    unsigned long long offset = 0;

    // Create a fake vma
    vma.vm_start = 0x0;
    vma.vm_end = (unsigned long)(REGION_NUMBER * REGION_SIZE);

    region_tracker = uvm_create_region_tracker(&vma);

    if (!region_tracker)
    {
        status = NV_ERR_NO_MEMORY;
        UVM_ERR_PRINT_NV_STATUS("Could not create a region tracking tree.",
                                status);
        goto cleanup;
    }

    for (region = 0; region < REGION_NUMBER; region++)
    {
      unsigned long long fakeRegionStart = region * REGION_SIZE;
      unsigned long long fakeRegionEnd = (region + 1) * REGION_SIZE;

      // Create a fake commit record pointer that is only an incremental
      // counter
      UvmCommitRecord * fakeCommitRecord = (UvmCommitRecord *) (region);

      uvm_track_region(region_tracker,
                       fakeRegionStart,
                       fakeRegionEnd, NULL,
                       fakeCommitRecord);
    }

    // Check that everything is correct
    for (region = 0; region < REGION_NUMBER; region++)
    {
        unsigned long long fakeRegionStart = region * REGION_SIZE;
        UvmCommitRecord * fakeCommitRecordExpected = (UvmCommitRecord *) (region);
        UvmCommitRecord *commitToCheck;
        for (offset = 0; offset < REGION_SIZE; offset++)
        {
            status = uvm_get_owner_from_address(region_tracker,
                                                fakeRegionStart + offset,
                                                &commitToCheck);
            if (NV_OK != status)
            {
                UVM_ERR_PRINT_NV_STATUS("Could not get information for the address.",
                                        status);
                goto cleanup;
            }
            // The fake commit record is supposed to be an incremental counter
            // Just check if the expected value of the counter is the one store
            // in the region tracking tree
            if (commitToCheck != fakeCommitRecordExpected)
            {
                status = NV_ERR_INVALID_ARGUMENT;
                UVM_ERR_PRINT_NV_STATUS("Invalid owner in the region tracking tree.",
                                        status);
            }
        }
    }

cleanup:
    uvm_destroy_region_tracker(region_tracker, _destroy_dummy_region);

    return status;
}
