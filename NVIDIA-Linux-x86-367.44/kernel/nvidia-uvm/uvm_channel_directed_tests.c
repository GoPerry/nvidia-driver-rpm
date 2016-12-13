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

#include "uvm_gpu_ops_tests.h"
#include "uvm_channel_mgmt.h"
#include "uvm_linux.h"
#include "uvm_common.h"
#include "nv_uvm_interface.h"
#include "uvm_common_test.h"

#define INIT_GPU_BUFFER1_VALUE (0xBA)
#define INIT_GPU_BUFFER2_VALUE (0xAD)
#define INIT_CPU_BUFFER1_VALUE (0xCA)
#define INIT_CPU_BUFFER2_VALUE (0xFE)
#define INIT_GARBAGE_VALUE     (0x43)
#define IDENTITY_MAPPING_VA_BASE 0x800000000000
#define PASCAL_MAX_FB 0x800000000
// Subtests

// Test if the dependency between two op in the same channel pool is correct.
// To do so we are doing two copies into the same region and one should wait
// the other before starting.
static
NV_STATUS channel_directed_singlepool(UvmChannelManager *channelManager,
                                      UvmtestMemblock *srcBuffer1,
                                      UvmtestMemblock *srcBuffer2,
                                      UvmtestMemblock *dstBuffer1,
                                      UvmTracker *tracker)
{
    UvmGpuPointer      dstBuffer2;
    NvU8               expectedValue = 0;
    NV_STATUS          status = NV_OK;

    // Right now source buffer 2 have to be located on CPU for the final check
    if (srcBuffer2->location != MEMBLOCK_CPU)
        return NV_ERR_INVALID_ARGUMENT;
    if (srcBuffer1->size > dstBuffer1->size)
        return NV_ERR_INVALID_ARGUMENT;

    // Initialize buffers
    // (for this test we only need the first and the last byte to be init)
    // TODO Implement a GPU memset support
    *(NvU8*)srcBuffer1->cpu = INIT_GPU_BUFFER1_VALUE;
    status = uvmtest_memcpy_virt(channelManager,
                                 dstBuffer1->gpu,
                                 srcBuffer1->gpu,
                                 1, NULL, NULL);
    if (NV_OK != status)
        return status;
    status = uvmtest_memcpy_virt(channelManager,
                                 dstBuffer1->gpu + dstBuffer1->size - 1,
                                 srcBuffer1->gpu,
                                 1, NULL, NULL);
    if (NV_OK != status)
        return status;
    *(NvU8*)srcBuffer1->cpu = INIT_CPU_BUFFER1_VALUE;
    *(NvU8*)srcBuffer2->cpu = INIT_CPU_BUFFER2_VALUE;
    *((NvU8*)srcBuffer1->cpu + srcBuffer1->size - 1) = INIT_CPU_BUFFER1_VALUE;
    *((NvU8*)srcBuffer2->cpu + srcBuffer2->size - 1) = INIT_CPU_BUFFER2_VALUE;

    expectedValue = *(NvU8*)srcBuffer2->cpu;

    // Create the second destination buffer in the first destination buffer
    // So the two buffers are overlapping
    dstBuffer2 = dstBuffer1->gpu + srcBuffer1->size - 1;

    status = uvmtest_memcpy_virt(channelManager,
                                 dstBuffer1->gpu, srcBuffer1->gpu,
                                 srcBuffer1->size, NULL, tracker);
    if (NV_OK != status)
        return status;
    status = uvmtest_memcpy_virt(channelManager, dstBuffer2, srcBuffer2->gpu,
                                 1, tracker, tracker);
    if (NV_OK != status)
        return status;

    // Wait until the operations are finished
    status = uvm_wait_for_tracker(tracker);
    if (NV_OK != status)
        return status;

    // Now check the value
    *(NvU8*)srcBuffer2->cpu = INIT_GARBAGE_VALUE;
    status = uvmtest_memcpy_virt(channelManager, srcBuffer2->gpu, dstBuffer2,
                                 1, NULL, NULL);
    if (NV_OK != status)
        return status;

    if (*(NvU8*)srcBuffer2->cpu != expectedValue)
    {
        UVM_ERR_PRINT("Invalid data Expected:%d Got:%d\n",
                      (int)expectedValue,
                      (int)*(NvU8*)srcBuffer2->cpu);
        return NV_ERR_INVALID_DATA;
    }
    return NV_OK;
}

// This function does copy according to the following pattern:
// srcBuffer -> tmpBuffer1 -> tmpBuffer2 -> dstBuffer
static
NV_STATUS channel_circular_copy(UvmChannelManager *channelManager,
                                UvmtestMemblock *srcBuffer,
                                UvmtestMemblock *dstBuffer,
                                UvmtestMemblock *tmpBuffer1,
                                UvmtestMemblock *tmpBuffer2)

{
    const NvU32    LOOPS = 3;
    NvU32          index = 0;
    NvU32          i = 0;
    NV_STATUS      status = NV_OK;
    UvmTracker     tracker;

    if (!channelManager ||
        !srcBuffer || !dstBuffer || !tmpBuffer1 || !tmpBuffer2)
        return NV_ERR_INVALID_ARGUMENT;

    uvm_init_tracker(&tracker);

    for (index = 0; index < LOOPS; ++index)
    {
        for (i = 0; i < (srcBuffer->size / sizeof(unsigned)); i++)
            ((unsigned*)srcBuffer->cpu)[i] = index;
        // Set a different pattern for the destination region
        for (i = 0; i < (dstBuffer->size / sizeof(unsigned)); i++)
            ((unsigned*)dstBuffer->cpu)[i] = index + 1;

        status = uvmtest_memcpy_pages(channelManager,
                                      tmpBuffer1->pages,
                                      srcBuffer->pages,
                                      srcBuffer->size,
                                      NULL,
                                      &tracker);
        if (NV_OK != status)
            goto cleanup;

        status = uvmtest_memcpy_pages(channelManager,
                                      tmpBuffer2->pages,
                                      tmpBuffer1->pages,
                                      tmpBuffer1->size,
                                      &tracker,
                                      &tracker);
        if (NV_OK != status)
            goto cleanup;

        status = uvmtest_memcpy_pages(channelManager,
                                      dstBuffer->pages,
                                      tmpBuffer2->pages,
                                      tmpBuffer2->size,
                                      &tracker,
                                      NULL);
        if (NV_OK != status)
            goto cleanup;

        for (i = 0; i < (dstBuffer->size / sizeof(unsigned)); i++)
        {
            if (((unsigned*)dstBuffer->cpu)[i] != index)
            {
                UVM_ERR_PRINT("ERROR: Copy failed. Expected=0x%X, Got=0x%X\n",
                              index,
                              ((unsigned*)dstBuffer->cpu)[i]);
                status = NV_ERR_INVALID_DATA;
                goto cleanup;
            }
        }
    }

cleanup:
    uvm_reset_tracker(&tracker);
    uvm_shrink_tracker(&tracker);
    return status;
}

// Tests
NV_STATUS uvmtest_channel_directed(UvmChannelManager *channelManager)
{
    const NvU64 blockSize = 16 * 1024 * 1024;

    NV_STATUS status = NV_OK;
    uvmGpuAddressSpaceHandle hVaSpace;

    UvmtestMemblock cpuBuffer1;
    UvmtestMemblock cpuBuffer2;
    UvmtestMemblock gpuBuffer1;
    UvmtestMemblock gpuBuffer2;

    UvmTracker tracker;

    // Setup

    memset(&cpuBuffer1, 0, sizeof(cpuBuffer1));
    memset(&cpuBuffer2, 0, sizeof(cpuBuffer2));
    memset(&gpuBuffer1, 0, sizeof(gpuBuffer1));
    memset(&gpuBuffer2, 0, sizeof(gpuBuffer2));

    uvm_init_tracker(&tracker);

    hVaSpace = channelManager->channelPool.hVaSpace;

    status = uvmtest_alloc_virt_cpu(hVaSpace, &cpuBuffer1, blockSize, MAP_CPU);
    if (NV_OK != status)
        goto cleanup;
    status = uvmtest_alloc_virt_cpu(hVaSpace, &cpuBuffer2, blockSize, MAP_CPU);
    if (NV_OK != status)
        goto cleanup;
    status = uvmtest_alloc_virt_gpu(hVaSpace, &gpuBuffer1, blockSize, 0);
    if (NV_OK != status)
        goto cleanup;
    status = uvmtest_alloc_virt_gpu(hVaSpace, &gpuBuffer2, blockSize, 0);
    if (NV_OK != status)
        goto cleanup;

    // Test
    UVM_RUN_SUBTEST(status,
                    channel_directed_singlepool,
                    channelManager,
                    &cpuBuffer1, &cpuBuffer2, &gpuBuffer1,
                    &tracker);
    if (NV_OK != status)
        goto cleanup;

    // Teardown
cleanup:
    uvmtest_free_virt(&cpuBuffer1);
    uvmtest_free_virt(&cpuBuffer2);
    uvmtest_free_virt(&gpuBuffer1);
    uvmtest_free_virt(&gpuBuffer2);

    if (uvm_shrink_tracker(&tracker))
        UVM_ERR_PRINT("tracker state not as expected.");

    return status;
}

// Since we allocate 4MB if this test is used with the CONTIGUOUS flag we
// will ultimatly test physical copies larger than the page size.
NV_STATUS uvmtest_channel_pagesize_directed(UvmChannelManager *channelManager,
                                            enum UvmtestMemblockFlag *pagesize)
{
    int sizeA = 0;
    int sizeB = 0;
    // Allocate 4MB to be able to test copies larger than 2MB
    const NvU64 REGION_SIZE = 4 * 1024 * 1024;
    NV_STATUS status = NV_OK;

    uvmGpuAddressSpaceHandle hVaSpace;

    UvmtestMemblock virtBuffer1;
    UvmtestMemblock virtBuffer2;
    UvmtestMemblock physBuffer1;
    UvmtestMemblock physBuffer2;

    // Setup
    hVaSpace = channelManager->channelPool.hVaSpace;

    memset(&virtBuffer1, 0, sizeof(virtBuffer1));
    memset(&virtBuffer2, 0, sizeof(virtBuffer2));
    memset(&physBuffer1, 0, sizeof(physBuffer1));
    memset(&physBuffer2, 0, sizeof(physBuffer2));

    status = uvmtest_alloc_virt_gpu(hVaSpace,
                                    &virtBuffer1,
                                    REGION_SIZE,
                                    MAP_CPU);
    if (NV_OK != status)
        goto cleanup;

    status = uvmtest_alloc_virt_gpu(hVaSpace,
                                    &virtBuffer2,
                                    REGION_SIZE,
                                    MAP_CPU);
    if (NV_OK != status)
        goto cleanup;

    // Test copies with page size pairs
    for (sizeA = 0; pagesize[sizeA]; sizeA++)
    {
        status = uvmtest_alloc_phys_gpu(hVaSpace,
                                        &physBuffer1,
                                        REGION_SIZE,
                                        pagesize[sizeA]);
        if (NV_OK != status)
            goto cleanup;

        for (sizeB = 0; pagesize[sizeB]; sizeB++)
        {
            status = uvmtest_alloc_phys_gpu(hVaSpace,
                                            &physBuffer2,
                                            REGION_SIZE,
                                            pagesize[sizeB]);
            if (NV_OK != status)
                goto cleanup;

            status = channel_circular_copy(channelManager,
                                           &virtBuffer1,
                                           &virtBuffer2,
                                           &physBuffer1,
                                           &physBuffer2);

            if (NV_OK != status)
                goto cleanup;

            uvmtest_free_phys(&physBuffer2);
        }
        uvmtest_free_phys(&physBuffer1);
    }

    // Teardown
cleanup:
    uvmtest_free_virt(&virtBuffer1);
    uvmtest_free_virt(&virtBuffer2);
    uvmtest_free_phys(&physBuffer1);
    uvmtest_free_phys(&physBuffer2);

    return status;

}

NV_STATUS uvmtest_channel_p2p_migration(UvmChannelManager *channelManager,
                                        UvmChannelManager *peerChannelManager,
                                        NvU32 peerId)
{
    // Push 2MB copies - Allocate a 2MB page
    const NvU64 blockSize = 2 * 1024 * 1024;

    NV_STATUS status = NV_OK;
    uvmGpuAddressSpaceHandle hVaSpace;

    UvmtestMemblock cpuBuffer1;
    UvmtestMemblock cpuBuffer2;
    UvmtestMemblock gpuBuffer1;
    UvmtestMemblock gpuBuffer2;

    if (!channelManager || !peerChannelManager)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    // Setup
    memset(&cpuBuffer1, 0, sizeof(cpuBuffer1));
    memset(&cpuBuffer2, 0, sizeof(cpuBuffer2));
    memset(&gpuBuffer1, 0, sizeof(gpuBuffer1));
    memset(&gpuBuffer2, 0, sizeof(gpuBuffer2));

    // Setup buffers for a src GPU
    hVaSpace = channelManager->channelPool.hVaSpace;
    status = uvmtest_alloc_virt_cpu(hVaSpace, &cpuBuffer1, blockSize, MAP_CPU);
    if (NV_OK != status)
        goto cleanup;
    status = uvmtest_alloc_virt_cpu(hVaSpace, &cpuBuffer2, blockSize, MAP_CPU);
    if (NV_OK != status)
        goto cleanup;
    status = uvmtest_alloc_phys_gpu(hVaSpace, &gpuBuffer1, blockSize, CONTIGUOUS | PAGE_2M);
    if (NV_OK != status)
        goto cleanup;

    // Setup buffers for a dest GPU
    // Note: Here we will create a physical buffer. But, we need to convert it
    // into virtual by overwriting flags and the address (calculated)
    hVaSpace = peerChannelManager->channelPool.hVaSpace;
    status = uvmtest_alloc_phys_gpu(hVaSpace, &gpuBuffer2, blockSize, CONTIGUOUS | PAGE_2M);
    if (NV_OK != status)
        goto cleanup;

    // Translating buffer PA -> VA
    gpuBuffer2.flags &= ~PHYSICAL;
    gpuBuffer2.pages[0].flags &= ~PHYSICAL;
    gpuBuffer2.pages[0].gpu += IDENTITY_MAPPING_VA_BASE + peerId * PASCAL_MAX_FB;

    status = channel_circular_copy(channelManager,
                                   &cpuBuffer1,
                                   &cpuBuffer2,
                                   &gpuBuffer1,
                                   &gpuBuffer2);

    // Translating buffer VA -> PA
    gpuBuffer2.flags |= PHYSICAL;
    gpuBuffer2.pages[0].flags |= PHYSICAL;
    gpuBuffer2.pages[0].gpu -= IDENTITY_MAPPING_VA_BASE + peerId * PASCAL_MAX_FB;

    // Teardown
cleanup:
    uvmtest_free_virt(&cpuBuffer1);
    uvmtest_free_virt(&cpuBuffer2);
    uvmtest_free_phys(&gpuBuffer1);
    uvmtest_free_phys(&gpuBuffer2);

    return status;
}

