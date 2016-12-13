/*******************************************************************************
    Copyright (c) 2015 NVIDIA Corporation

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

#ifndef _UVM_COMMON_TEST_H_
#define _UVM_COMMON_TEST_H_

#include "uvm_linux.h"
#include "uvm_common.h"
#include "nv_uvm_interface.h"
#include "uvm_channel_mgmt.h"

enum UvmtestMemblockLocation
{
    MEMBLOCK_GPU,
    MEMBLOCK_CPU,
};

enum UvmtestMemblockFlag
{
    MAP_CPU        = 0x1,
    CONTIGUOUS     = 0x2,
    PHYSICAL       = 0x4,

    PAGE_SIZE_MASK = 0xF0,  // Reserve 4Bit for the page size
    PAGE_4K        = 0x10,  // If nothing is specified 4K is usually selected
    PAGE_64K       = 0x20,
    PAGE_128K      = 0x40,
    PAGE_2M        = 0x80
};

typedef struct UvmtestPage_s
{
    UvmGpuPointer                    gpu;
    NvU64                            size;
    enum UvmtestMemblockFlag         flags;
} UvmtestPage;

typedef struct UvmtestMemblock_s
{
    UvmGpuPointer                    gpu;
    void                             *cpu;
    UvmtestPage                      *pages;
    enum UvmtestMemblockLocation     location;
    NvU64                            size;
    NvU64                            numPage;
    NvU64                            pageSize;
    NvBool                           globalPointerPresent;
    uvmGpuAddressSpaceHandle         hVaSpace;
    enum UvmtestMemblockFlag         flags;
} UvmtestMemblock;

// Do a memory copy for two regions in virtual memory.
// behavior:
// * trackerIn == NULL: Launch the copy without waiting for something
// * trackerIn != NULL: The copy will wait for trackerIn before being launched
// * trackerOut == NULL: The call is blocking and return only when the copy
//                       is finished
// * trackerOut != NULL: The call is not blocking and the trackerItem is merged
//                       into trackerOut. Note: This function will allocate the
//                       space in trackerOut to merge the item.
// NOTE: trackerIn and trackerOut can be the same
NV_STATUS uvmtest_memcpy_virt(UvmChannelManager   *channelManager,
                              UvmGpuPointer       dst,
                              UvmGpuPointer       src,
                              NvU64               size,
                              UvmTracker          *trackerIn,
                              UvmTracker          *trackerOut);

// Copy all the pages in src into the pages pointed by dst
// behavior:
// * trackerIn == NULL: Launch the copy without waiting for something
// * trackerIn != NULL: The copy will wait for trackerIn before being launched
// * trackerOut == NULL: The call is blocking and return only when the copy
//                       is finished
// * trackerOut != NULL: The call is not blocking and the trackerItem is merged
//                       into trackerOut. Note: This function will allocate the
//                       space in trackerOut to merge the item.
// NOTE: trackerIn and trackerOut can be the same
NV_STATUS uvmtest_memcpy_pages(UvmChannelManager   *channelManager,
                               UvmtestPage         *dst,
                               UvmtestPage         *src,
                               NvU64               size,
                               UvmTracker          *trackerIn,
                               UvmTracker          *trackerOut);

// Do an inline memory copy to a region in virtual memory. src is copied in the
// pushbuffer during the call so src can be overwritten after the call without
// impacting the copy
// behavior:
// * trackerIn == NULL: Launch the copy without waiting for something
// * trackerIn != NULL: The copy will wait for trackerIn before being launched
// * trackerOut == NULL: The call is blocking and return only when the copy
//                       is finished
// * trackerOut != NULL: The call is not blocking and the trackerItem is merged
//                       into trackerOut. Note: This function will allocate the
//                       space in trackerOut to merge the item.
// NOTE: trackerIn and trackerOut can be the same
NV_STATUS uvmtest_inline_memcpy_virt(UvmChannelManager   *channelManager,
                                     UvmGpuPointer       dst,
                                     const void*         src,
                                     NvU64               size,
                                     UvmTracker          *trackerIn,
                                     UvmTracker          *trackerOut);

// Allocate a block of memory in the cpu memory and map it in the cpu va space
// if requested.
NV_STATUS uvmtest_alloc_virt_cpu(uvmGpuAddressSpaceHandle     hVaSpace,
                                 UvmtestMemblock              *memblock,
                                 NvU64                        size,
                                 enum UvmtestMemblockFlag     flag);

// Allocate a block of memory in the gpu memory and map it in the cpu va space
// if requested.
NV_STATUS uvmtest_alloc_virt_gpu(uvmGpuAddressSpaceHandle     hVaSpace,
                                 UvmtestMemblock              *memblock,
                                 NvU64                        size,
                                 enum UvmtestMemblockFlag     flag);

// Free a previously allocated memblock. If memblock is null or if the gpu
// pointer in the memblock is null then this function does nothing.
void uvmtest_free_virt(UvmtestMemblock *memblock);

// Allocate a block of physical gpu memory.
NV_STATUS uvmtest_alloc_phys_gpu(uvmGpuAddressSpaceHandle     hVaSpace,
                                 UvmtestMemblock              *memblock,
                                 NvU64                        size,
                                 enum UvmtestMemblockFlag     flag);

// Free a previously allocated memblock. If memblock is null or if the gpu
// pointer in the memblock is null then this function does nothing.
void uvmtest_free_phys(UvmtestMemblock *memblock);

// Check if we do have video memory for the gpu associated to the provided
// vaspace handle. The result is stored in is0fb.
NV_STATUS uvmtest_is_0fb(uvmGpuAddressSpaceHandle hVaSpace, NvBool *is0fb);

#endif // _UVM_COMMON_TEST_H_
