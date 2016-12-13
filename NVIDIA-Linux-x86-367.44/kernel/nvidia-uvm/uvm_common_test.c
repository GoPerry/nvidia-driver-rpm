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

#include "uvm_common_test.h"

static
NV_STATUS _uvmtest_get_pushbuffer(UvmChannelManager   *channelManager,
                                  UvmPushbuffer       **pushBuffer,
                                  UvmTracker          *trackerIn)
{
    NV_STATUS status =  NV_OK;

    status = uvm_get_pushbuffer(channelManager, pushBuffer);
    if (status != NV_OK)
        return status;

    if (trackerIn)
        uvm_reserve_acquire_space(channelManager, *pushBuffer, trackerIn);

    return NV_OK;
}

static
NV_STATUS _uvmtest_submit_pushbuffer(UvmPushbuffer       *pushBuffer,
                                     UvmTracker          *trackerIn,
                                     UvmTracker          *trackerOut)
{
    NV_STATUS           status =  NV_OK;
    UvmTrackerItem      trackerItem;

    uvm_submit_pushbuffer(pushBuffer->channel->pool->manager,
                          pushBuffer,
                          trackerIn,
                          &trackerItem);

    if (trackerOut)
    {
        // reset the tracker
        uvm_reset_tracker(trackerOut);
        status = uvm_grow_tracker(trackerOut, 1);
        if (status != NV_OK)
        {
            UVM_ERR_PRINT_NV_STATUS("Could not grow tracker.", status);
            return status;
        }
        status = uvm_merge_tracker_item(trackerOut, &trackerItem);
        if (status != NV_OK)
        {
            UVM_ERR_PRINT_NV_STATUS("Could not merge tracker item.", status);
            return status;
        }

        return NV_OK;
    }
    else
    {
        UvmTracker tempTracker;
        uvm_init_tracker(&tempTracker);
        status = uvm_grow_tracker(&tempTracker, 1);
        if (status != NV_OK)
        {
            UVM_ERR_PRINT_NV_STATUS("Could not grow tracker", status);
            goto cleanup;
        }
        status = uvm_merge_tracker_item(&tempTracker, &trackerItem);
        if (status != NV_OK)
        {
            UVM_ERR_PRINT_NV_STATUS("Could not merge tracker item.", status);
            goto cleanup;
        }
        status = uvm_wait_for_tracker(&tempTracker);
        if (status != NV_OK)
        {
            UVM_ERR_PRINT_NV_STATUS("Could not wait for tracker", status);
            goto cleanup;
        }

        if (uvm_shrink_tracker(&tempTracker))
            UVM_ERR_PRINT("tracker state not as expected.");

        return NV_OK;

cleanup:
        uvm_reset_tracker(&tempTracker);
        uvm_shrink_tracker(&tempTracker);
    }
    return status;
}

NV_STATUS uvmtest_memcpy_virt(UvmChannelManager   *channelManager,
                              UvmGpuPointer       dst,
                              UvmGpuPointer       src,
                              NvU64               size,
                              UvmTracker          *trackerIn,
                              UvmTracker          *trackerOut)
{
    NV_STATUS           status =  NV_OK;
    NvU32               ret;
    UvmPushbuffer       *pushBuffer;

    status = _uvmtest_get_pushbuffer(channelManager, &pushBuffer, trackerIn);
    if (NV_OK != status)
        return status;

    UVM_PUSH_METHOD(ret, pushBuffer,
                    pushBuffer->channel->ceOps.launchDma,
                    src, 0,
                    dst, 0,
                    size,
                    NV_UVM_COPY_SRC_TYPE_VIRTUAL |
                    NV_UVM_COPY_DST_TYPE_VIRTUAL);
    if (!ret)
    {
        UVM_ERR_PRINT_NV_STATUS("Could not push copy method to pb.", status);
        return NV_ERR_NO_MEMORY;
    }

    return _uvmtest_submit_pushbuffer(pushBuffer, trackerIn, trackerOut);
}

NV_STATUS uvmtest_memcpy_pages(UvmChannelManager   *channelManager,
                               UvmtestPage         *dst,
                               UvmtestPage         *src,
                               NvU64               size,
                               UvmTracker          *trackerIn,
                               UvmTracker          *trackerOut)
{
    NV_STATUS           status =  NV_OK;
    NvU32               ret;
    NvU64               srcPage = 0;
    NvU64               dstPage = 0;
    NvU64               srcOffset = 0;
    NvU64               dstOffset = 0;
    NvU64               chunkSize = 0;
    NvU64               totalSize = 0;
    NvU64               flags = 0;
    UvmGpuPointer       srcChunkStart;
    UvmGpuPointer       dstChunkStart;
    UvmPushbuffer       *pushBuffer = NULL;

    if (size == 0)
        return NV_ERR_INVALID_ARGUMENT;

    status = _uvmtest_get_pushbuffer(channelManager, &pushBuffer, trackerIn);
    if (NV_OK != status)
        return status;

    srcChunkStart = src[0].gpu;
    dstChunkStart = dst[0].gpu;
    chunkSize = 0;

    flags |= (src[0].flags & PHYSICAL ?
              NV_UVM_COPY_SRC_TYPE_PHYSICAL : NV_UVM_COPY_SRC_TYPE_VIRTUAL);
    flags |= (dst[0].flags & PHYSICAL ?
              NV_UVM_COPY_DST_TYPE_PHYSICAL : NV_UVM_COPY_DST_TYPE_VIRTUAL);

    // Each time pages are not contiguous push a copy op for the previous
    // contiguous chunk.
    while (totalSize < size)
    {
        NvBool needPush = NV_FALSE;

        // The size of the remaining block to copy in the source page and the
        // size of the block receiving the data in the destination page are the
        // same. We can simply stage this copy and inspect the next page.
        if (src[srcPage].size - srcOffset == dst[dstPage].size - dstOffset)
        {
            chunkSize += src[srcPage].size - srcOffset;
            totalSize += src[srcPage].size - srcOffset;
            if (totalSize < size)
            {
                dstOffset = 0;
                srcOffset = 0;
                srcPage++;
                dstPage++;
                needPush = ((src[srcPage - 1].gpu + src[srcPage - 1].size) !=
                            src[srcPage].gpu) ||
                           ((dst[dstPage - 1].gpu + dst[dstPage - 1].size) !=
                            dst[dstPage].gpu);
            }
        }
        // Stage a copy for all the remaining data in the destination page and
        // switch to the next destination page (we can't change the source page
        // since we still have data to copy)
        else if (src[srcPage].size - srcOffset > dst[dstPage].size - dstOffset)
        {
            chunkSize += dst[dstPage].size - dstOffset;
            totalSize += dst[dstPage].size - dstOffset;
            if (totalSize < size)
            {
                dstOffset = 0;
                srcOffset += dst[dstPage].size - dstOffset;
                dstPage++;
                needPush = ((dst[dstPage - 1].gpu + dst[dstPage - 1].size) !=
                            dst[dstPage].gpu);
            }
        }
        // Stage a copy for all the remaining data in the source page and
        // switch to the next source page (we can't change the destination page
        // since we still have room to fill)
        else
        {
            chunkSize += src[srcPage].size - srcOffset;
            totalSize += src[srcPage].size - srcOffset;
            if (totalSize < size)
            {
                srcOffset = 0;
                dstOffset += src[srcPage].size - srcOffset;
                srcPage++;
                needPush = ((src[srcPage - 1].gpu + src[srcPage - 1].size) !=
                            src[srcPage].gpu);
            }
        }

        if (needPush || totalSize >= size)
        {
            UVM_PUSH_METHOD(ret, pushBuffer,
                            pushBuffer->channel->ceOps.launchDma,
                            srcChunkStart, 0,
                            dstChunkStart, 0,
                            chunkSize,
                            flags);
            if (!ret)
            {
                UVM_ERR_PRINT_NV_STATUS("Could not push copy method to pb.",
                                        status);
                return NV_ERR_NO_MEMORY;
            }

            srcChunkStart = src[srcPage].gpu + srcOffset;
            dstChunkStart = dst[dstPage].gpu + dstOffset;
            chunkSize = 0;
        }
    }

    return _uvmtest_submit_pushbuffer(pushBuffer, trackerIn, trackerOut);
}

NV_STATUS uvmtest_inline_memcpy_virt(UvmChannelManager   *channelManager,
                                     UvmGpuPointer       dst,
                                     const void*         src,
                                     NvU64               size,
                                     UvmTracker          *trackerIn,
                                     UvmTracker          *trackerOut)
{
    NV_STATUS           status =  NV_OK;
    NvU32               ret;
    UvmPushbuffer       *pushBuffer;
    UvmPbInlineRegion   copyRegion = {0};

    status = _uvmtest_get_pushbuffer(channelManager, &pushBuffer, trackerIn);
    if (NV_OK != status)
        return status;

    // Create an inline region in the pushbuffer and fill it with data
    uvm_pushbuffer_inline_start(pushBuffer, &copyRegion);
    copyRegion.size = size;
    memcpy((void*)copyRegion.regionStart, src, size);
    uvm_pushbuffer_inline_end(pushBuffer, &copyRegion);

    // Now push a copy using the previously create inline region
    copyRegion.copy.bValid = NV_TRUE;
    copyRegion.copy.copySize = size;
    copyRegion.copy.dstAddr = dst;
    copyRegion.copy.dstAperture = 0;
    copyRegion.copy.dstCopyFlags = 0;
    ret = uvm_pushbuffer_copy_region(pushBuffer, &copyRegion);
    if (!ret)
    {
        UVM_ERR_PRINT_NV_STATUS("Could not push copy method to pb.", status);
        return NV_ERR_NO_MEMORY;
    }

    return _uvmtest_submit_pushbuffer(pushBuffer, trackerIn, trackerOut);
}

NV_STATUS uvmtest_alloc_virt_cpu(uvmGpuAddressSpaceHandle     hVaSpace,
                                 UvmtestMemblock              *memblock,
                                 NvU64                        size,
                                 enum UvmtestMemblockFlag     flags)
{
    NV_STATUS status = NV_OK;
    NvU64 numPage = 0;
    NvU64 page = 0;

    numPage = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    memset(memblock, 0, sizeof(*memblock));

    memblock->hVaSpace = hVaSpace;
    memblock->numPage = numPage;
    memblock->pageSize = PAGE_SIZE;
    memblock->size = numPage * PAGE_SIZE;
    memblock->location = MEMBLOCK_CPU;
    memblock->flags = flags;

    status = nvUvmInterfaceMemoryAllocSys(memblock->hVaSpace,
                                          memblock->size,
                                          &(memblock->gpu),
                                          NULL);
    if (NV_OK != status)
    {
        UVM_ERR_PRINT_NV_STATUS("Could not allocate SYSMEM region", status);
        uvmtest_free_virt(memblock);
        return status;
    }

    if (flags & MAP_CPU)
    {
        status = nvUvmInterfaceMemoryCpuMap(hVaSpace,
                                            memblock->gpu,
                                            size,
                                            &(memblock->cpu),
                                            UVM_PAGE_SIZE_DEFAULT);
        if (NV_OK != status)
        {
            UVM_ERR_PRINT_NV_STATUS("Could not map GPU mem to CPU VA.", status);
            uvmtest_free_virt(memblock);
            return status;
        }
    }

    memblock->pages = kzalloc(sizeof(UvmtestPage) * numPage, NV_UVM_GFP_FLAGS);
    if (!memblock->pages)
    {
        status = NV_ERR_NO_MEMORY;
        uvmtest_free_virt(memblock);
        return status;
    }

    for (page = 0; page < numPage; page++)
    {
        memblock->pages[page].gpu = memblock->gpu + page * PAGE_SIZE;
        memblock->pages[page].size = PAGE_SIZE;
        memblock->pages[page].flags = flags;
    }

    return NV_OK;
}

static NvU64 _uvmtest_get_page_size_from_flag(enum UvmtestMemblockFlag flag)
{
    switch (flag & PAGE_SIZE_MASK)
    {
        // If nothing is specified 4K is selected
        case 0:
        case PAGE_4K: return 1024 * 4;
        case PAGE_64K: return 1024 * 64;
        case PAGE_128K: return 1024 * 128;
        case PAGE_2M: return 1024 * 1024 * 2;
        default: return 0;
    }
}

NV_STATUS uvmtest_alloc_virt_gpu(uvmGpuAddressSpaceHandle     hVaSpace,
                                 UvmtestMemblock              *memblock,
                                 NvU64                        size,
                                 enum UvmtestMemblockFlag     flags)
{
    NV_STATUS status = NV_OK;
    UvmGpuAllocInfo gpuAllocInfo;
    NvU64 pageSize = _uvmtest_get_page_size_from_flag(flags);
    NvU64 page = 0;
    NvU64 numPage = 0;
    NvBool is0fb = NV_FALSE;

    // Ensure that we are not in a 0fb configuration before allocating vidmem
    status = uvmtest_is_0fb(hVaSpace, &is0fb);

    if (NV_OK != status)
    {
        UVM_ERR_PRINT_NV_STATUS("Could not check VIDMEM status", status);
        return status;
    }

    // In a 0fb config we will allocate sysmem instead
    if (is0fb)
        return uvmtest_alloc_virt_cpu(hVaSpace, memblock, size, flags);

    numPage = (size + pageSize - 1) / pageSize;

    memset(memblock, 0, sizeof(*memblock));

    memblock->numPage = numPage;
    memblock->pageSize = pageSize;
    memblock->hVaSpace = hVaSpace;
    memblock->size = numPage * pageSize;
    memblock->location = MEMBLOCK_GPU;
    memblock->flags = flags;

    memset(&gpuAllocInfo, 0, sizeof(gpuAllocInfo));

    gpuAllocInfo.pageSize = pageSize;

    status = nvUvmInterfaceMemoryAllocFB(memblock->hVaSpace,
                                         memblock->size,
                                         &(memblock->gpu),
                                         &gpuAllocInfo);
    if (NV_OK != status)
    {
        UVM_ERR_PRINT_NV_STATUS("Could not allocate VIDMEM region", status);
        uvmtest_free_virt(memblock);
        return status;
    }

    if (flags & MAP_CPU)
    {
        status = nvUvmInterfaceMemoryCpuMap(hVaSpace,
                                            memblock->gpu,
                                            size,
                                            &(memblock->cpu),
                                            UVM_PAGE_SIZE_DEFAULT);
        if (NV_OK != status)
        {
            UVM_ERR_PRINT_NV_STATUS("Could not map VIDMEM to CPU VA.", status);
            uvmtest_free_virt(memblock);
            return status;
        }
    }

    memblock->pages = kzalloc(sizeof(UvmtestPage) * numPage, NV_UVM_GFP_FLAGS);
    if (!memblock->pages)
    {
        status = NV_ERR_NO_MEMORY;
        uvmtest_free_virt(memblock);
        return status;
    }

    for (page = 0; page < numPage; page++)
    {
        memblock->pages[page].gpu = memblock->gpu + page * pageSize;
        memblock->pages[page].size = pageSize;
        memblock->pages[page].flags = flags;
    }
    return NV_OK;
}

void uvmtest_free_virt(UvmtestMemblock *memblock)
{
    if (memblock)
    {
        if (memblock->gpu)
            nvUvmInterfaceMemoryFree(memblock->hVaSpace, memblock->gpu);
        if (memblock->pages)
            kfree(memblock->pages);

        memset(memblock, 0, sizeof(*memblock));
    }
}

NV_STATUS uvmtest_alloc_phys_gpu(uvmGpuAddressSpaceHandle     hVaSpace,
                                 UvmtestMemblock              *memblock,
                                 NvU64                        size,
                                 enum UvmtestMemblockFlag     flags)
{
    NV_STATUS status = NV_OK;
    UvmGpuAllocInfo gpuAllocInfo;
    NvBool contiguous = flags & CONTIGUOUS;
    NvU64 pageSize = _uvmtest_get_page_size_from_flag(flags);
    NvU64 numPage = 0;

    memset(memblock, 0, sizeof(*memblock));

    if (pageSize == 0)
        return NV_ERR_INVALID_ARGUMENT;

    numPage = (size + pageSize - 1) / pageSize;

    memblock->hVaSpace = hVaSpace;
    memblock->flags = flags | PHYSICAL;
    memblock->size = pageSize * numPage;
    memblock->location = MEMBLOCK_GPU;
    memblock->numPage = numPage;
    memblock->pages = kzalloc(sizeof(UvmtestPage) * numPage, NV_UVM_GFP_FLAGS);

    if (!memblock->pages)
    {
        memset(memblock, 0, sizeof(*memblock));
        return NV_ERR_NO_MEMORY;
    }
    if (contiguous)
    {
        // If the allocation is contiguous we can deduce the position of each
        // page
        NvU64 offset = 0;
        NvU64 page = 0;

        // We may get the physical page 0x0 so we can't do a NULL pointer check
        // during the free.
        memblock->globalPointerPresent = NV_TRUE;

        memset(&gpuAllocInfo, 0, sizeof(gpuAllocInfo));
        gpuAllocInfo.bContiguousPhysAlloc = NV_TRUE;
        gpuAllocInfo.pageSize = pageSize;
        status = nvUvmInterfaceMemoryAllocGpuPa(hVaSpace,
                                                numPage * pageSize,
                                                &(memblock->gpu),
                                                &gpuAllocInfo);

        if (NV_OK != status)
        {
            UVM_ERR_PRINT_NV_STATUS("Could not alloc physical VIDMEM.", status);
            uvmtest_free_phys(memblock);
            return status;
        }

        offset = gpuAllocInfo.gpuPhysOffset;
        for (page = 0; page < numPage; page++)
        {
            memblock->pages[page].gpu = offset;
            memblock->pages[page].size = pageSize;
            memblock->pages[page].flags = flags | PHYSICAL;
            offset += pageSize;
        }
    }
    else
    {
        // If the mapping is not contiguous we will have to find the location of
        // each page
        NvU64 page = 0;
        for (page = 0; page < numPage; page++)
        {
            memset(&gpuAllocInfo, 0, sizeof(gpuAllocInfo));
            gpuAllocInfo.bContiguousPhysAlloc = NV_TRUE;
            gpuAllocInfo.pageSize = pageSize;
            status = nvUvmInterfaceMemoryAllocGpuPa(hVaSpace,
                                                    pageSize,
                                                    &memblock->pages[page].gpu,
                                                    &gpuAllocInfo);

            if (NV_OK != status)
            {
                UVM_ERR_PRINT_NV_STATUS("Could not alloc physical VIDMEM.",
                                        status);
                uvmtest_free_phys(memblock);
                return status;
            }
            memblock->pages[page].size = pageSize;
            memblock->pages[page].flags = flags | PHYSICAL;
        }
    }

    return NV_OK;
}

void uvmtest_free_phys(UvmtestMemblock *memblock)
{
    if (memblock)
    {
        if (memblock->globalPointerPresent)
            nvUvmInterfaceMemoryFreePa(memblock->hVaSpace, memblock->gpu);
        else
        {
            // If there is no global pointer we should free it page by page
            if (memblock->pages)
            {
                NvU64 page = 0;
                for (page = 0; page < memblock->numPage; page++)
                {
                    if (memblock->pages[page].gpu)
                        nvUvmInterfaceMemoryFreePa(memblock->hVaSpace,
                                                   memblock->pages[page].gpu);
                }
            }
        }

        if (memblock->pages)
            kfree(memblock->pages);
        memset(memblock, 0, sizeof(*memblock));
    }
}

NV_STATUS uvmtest_is_0fb(uvmGpuAddressSpaceHandle hVaSpace, NvBool* is0fb)
{
    NV_STATUS status = NV_OK;
    UvmGpuFbInfo gpuFbInfo;

    if (!is0fb)
        return NV_ERR_INVALID_ARGUMENT;

    memset(&gpuFbInfo, 0x0, sizeof(gpuFbInfo));
    status = nvUvmInterfaceGetFbInfo(hVaSpace, &gpuFbInfo);
    if (status != NV_OK)
        return status;

    *is0fb = gpuFbInfo.bZeroFb;

    return NV_OK;
}
