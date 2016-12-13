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

//
// Test copying from sysmem to fb.
//
NV_STATUS uvmtest_channel_basic_migration(UvmChannelManager *channelManager)
{
    // arbitrarily choose that our region size will be 16KB
    const unsigned                REGION_SIZE = 16 * 1024;

    NV_STATUS                     status = NV_OK;
    uvmGpuAddressSpaceHandle      hVaSpace = 0;
    UvmtestMemblock               gpuRegion;
    UvmtestMemblock               cpuRegion;
    unsigned                      word = 0;

    memset(&gpuRegion, 0, sizeof(gpuRegion));
    memset(&cpuRegion, 0, sizeof(cpuRegion));

    hVaSpace = channelManager->channelPool.hVaSpace;

    status = uvmtest_alloc_virt_cpu(hVaSpace, &cpuRegion, REGION_SIZE, MAP_CPU);
    if (NV_OK != status)
        goto cleanup;

    status = uvmtest_alloc_virt_gpu(hVaSpace, &gpuRegion, REGION_SIZE, MAP_CPU);
    if (NV_OK != status)
        goto cleanup;

    //setup to copy from SYSMEM to FB
    memset(gpuRegion.cpu, 0xFF, REGION_SIZE);
    memset(cpuRegion.cpu, 0x1, REGION_SIZE);

    //
    // test migration
    //
    status = uvmtest_memcpy_virt(channelManager, gpuRegion.gpu, cpuRegion.gpu,
                                 REGION_SIZE, NULL, NULL);
    if (NV_OK != status)
        goto cleanup;

    // check that the pattern copied to FB is the same as the pattern in SYSMEM
    for (word = 0; word < REGION_SIZE / sizeof(unsigned); word++)
    {
        if (((unsigned*)gpuRegion.cpu)[word] !=
            ((unsigned*)cpuRegion.cpu)[word])
        {
            UVM_ERR_PRINT("ERROR: copy failed. GPU = 0x%X, CPU = 0x%X\n",
                          ((unsigned*)gpuRegion.cpu)[word],
                          ((unsigned*)cpuRegion.cpu)[word]);
            status = NV_ERR_INVALID_DATA;
            goto cleanup;
        }
    }

cleanup:
    uvmtest_free_virt(&cpuRegion);
    uvmtest_free_virt(&gpuRegion);
    return status;
}

//
// Test copying from sysmem to physical vidmem.
//
NV_STATUS uvmtest_channel_physical_migration(UvmChannelManager *channelManager)
{
    // arbitrarily choose that our region size will be 256KB
    const unsigned                REGION_SIZE = 256 * 1024;
    const unsigned                LOOPS = 10;
    NV_STATUS                     status = NV_OK;
    uvmGpuAddressSpaceHandle      hVaSpace = 0;
    UvmtestMemblock               gpuPhysRegion1;
    UvmtestMemblock               gpuPhysRegion2;
    UvmtestMemblock               gpuVirtRegion1;
    UvmtestMemblock               gpuVirtRegion2;
    UvmTracker                    tracker;
    unsigned                      i = 0;
    unsigned                      index = 0;

    memset(&gpuPhysRegion1, 0, sizeof(gpuPhysRegion1));
    memset(&gpuPhysRegion2, 0, sizeof(gpuPhysRegion2));
    memset(&gpuVirtRegion1, 0, sizeof(gpuVirtRegion1));
    memset(&gpuVirtRegion2, 0, sizeof(gpuVirtRegion2));

    uvm_init_tracker(&tracker);

    hVaSpace = channelManager->channelPool.hVaSpace;

    status = uvmtest_alloc_phys_gpu(hVaSpace,
                                    &gpuPhysRegion1,
                                    REGION_SIZE,
                                    PAGE_64K | CONTIGUOUS);
    if (NV_OK != status)
        goto cleanup;

    status = uvmtest_alloc_phys_gpu(hVaSpace,
                                    &gpuPhysRegion2,
                                    REGION_SIZE,
                                    PAGE_64K);
    if (NV_OK != status)
        goto cleanup;

    status = uvmtest_alloc_virt_gpu(hVaSpace,
                                    &gpuVirtRegion1,
                                    REGION_SIZE,
                                    MAP_CPU);

    if (NV_OK != status)
        goto cleanup;

    status = uvmtest_alloc_virt_gpu(hVaSpace,
                                    &gpuVirtRegion2,
                                    REGION_SIZE,
                                    MAP_CPU);

    if (NV_OK != status)
        goto cleanup;

    //
    // Test migration
    for (index = 0; index < LOOPS; ++index)
    {
        for (i = 0; i < (REGION_SIZE / sizeof(unsigned)); i++)
            ((unsigned*)gpuVirtRegion1.cpu)[i] = index;
        // Set a different pattern for the region 2
        for (i = 0; i < (REGION_SIZE / sizeof(unsigned)); i++)
            ((unsigned*)gpuVirtRegion2.cpu)[i] = index + 1;

        status = uvmtest_memcpy_pages(channelManager,
                                      gpuPhysRegion1.pages,
                                      gpuVirtRegion1.pages,
                                      REGION_SIZE,
                                      NULL,
                                      &tracker);
        if (NV_OK != status)
            goto cleanup;

        status = uvmtest_memcpy_pages(channelManager,
                                      gpuPhysRegion2.pages,
                                      gpuPhysRegion1.pages,
                                      REGION_SIZE,
                                      &tracker,
                                      &tracker);
        if (NV_OK != status)
            goto cleanup;

        status = uvmtest_memcpy_pages(channelManager,
                                      gpuVirtRegion2.pages,
                                      gpuPhysRegion2.pages,
                                      REGION_SIZE,
                                      &tracker,
                                      NULL);
        if (NV_OK != status)
            goto cleanup;

        for (i = 0; i < (REGION_SIZE / sizeof(unsigned)); i++)
        {
            if (((unsigned*)gpuVirtRegion2.cpu)[i] != index)
            {
                UVM_ERR_PRINT("ERROR: Copy failed. Expected=0x%X, Got=0x%X\n",
                              index,
                              ((unsigned*)gpuVirtRegion2.cpu)[i]);
                status = NV_ERR_INVALID_DATA;
                goto cleanup;
            }
        }
    }

cleanup:
    uvmtest_free_phys(&gpuPhysRegion1);
    uvmtest_free_phys(&gpuPhysRegion2);
    uvmtest_free_virt(&gpuVirtRegion1);
    uvmtest_free_virt(&gpuVirtRegion2);

    uvm_reset_tracker(&tracker);
    uvm_shrink_tracker(&tracker);

    return status;
}

#define CHANNEL_MGMT_API_TEST_SURFACES 3
//
// Simple Pushbuffer Sanity Test
//
//   Summary: Allocate Surface 0, 1 and 2 in sysmem, FB and sysmem respectively.
//            Copy data from 0 to 1 and then from 1 to 2.
//            Wait on tracker for the last operation to complete.
//            Verify data.
//
NV_STATUS uvmtest_channel_pushbuffer_sanity(UvmChannelManager *channelManager)
{
    // arbitrarily choose that our region size will be 128KB
    const unsigned REGION_SIZE = 128 * 1024;
    const unsigned loops = 2;
    const unsigned surfaces = CHANNEL_MGMT_API_TEST_SURFACES;

    NV_STATUS                   status = NV_OK;
    UvmTracker                  tracker;
    uvmGpuAddressSpaceHandle    hVaSpace = 0;
    UvmtestMemblock             surf[CHANNEL_MGMT_API_TEST_SURFACES] = {{0}};
    unsigned                    i = 0;
    unsigned                    index = 0;

    uvm_init_tracker(&tracker);

    hVaSpace = channelManager->channelPool.hVaSpace;

    for (index = 0; index < surfaces; index++)
    {
        // odd surface on FB
        if (index % 2)
            status = uvmtest_alloc_virt_gpu(hVaSpace, &surf[index],
                                            REGION_SIZE, MAP_CPU);
        else
            status = uvmtest_alloc_virt_cpu(hVaSpace, &surf[index],
                                            REGION_SIZE, MAP_CPU);
        memset(surf[index].cpu, 0xFF, REGION_SIZE);

        if (NV_OK != status)
            goto cleanup;
    }

    //
    // Test migration
    for (index = 0; index < loops; ++index)
    {
        for (i = 0; i < (REGION_SIZE / sizeof(unsigned)); i++)
            ((unsigned*)surf[0].cpu)[i] = index;

        status = uvmtest_memcpy_virt(channelManager, surf[1].gpu, surf[0].gpu,
                                     REGION_SIZE, NULL, &tracker);

        if (NV_OK != status)
            goto cleanup;

        status = uvmtest_memcpy_virt(channelManager, surf[2].gpu, surf[1].gpu,
                                     REGION_SIZE, &tracker, NULL);

        if (NV_OK != status)
            goto cleanup;

        // Check the pattern is copied to last surface
        for (i = 0; i < REGION_SIZE / sizeof(unsigned); i++)
        {
            if (((unsigned*)surf[0].cpu)[i] != ((unsigned*)surf[2].cpu)[i])
            {
                UVM_ERR_PRINT("ERROR: Copy failed. Surf0=0x%X, Surf2=0x%X\n",
                              ((unsigned*)surf[0].cpu)[i],
                              ((unsigned*)surf[2].cpu)[i]);
                status = NV_ERR_INVALID_DATA;
                goto cleanup;
            }
        }
    }

cleanup:
    for (index = 0; index < surfaces; index++)
        uvmtest_free_virt(&surf[index]);

    uvm_reset_tracker(&tracker);
    uvm_shrink_tracker(&tracker);
    return status;
}

//
// Pushbuffer inline region sanity test.
//
//   Summary: Allocate the following surfaces:
//            Surf0 : sysmem. Will be the target buffer in sysmem.
//            Surf1 : fb. Will be the target buffer in fb.
//
//            For each verif loop i from 0 to n
//                acquire tracker
//                get pb region, populate i in region (4k length)
//                queue inline copy from pb to surf0
//                get pb region, populate i in region (4k length)
//                queue inline copy from pb to surf1
//                verify value on surf0, surf1
//
NV_STATUS uvmtest_channel_pushbuffer_inline(UvmChannelManager *channelManager)
{
    // arbitrarily choose that our region size will be 4KB
    const unsigned REGION_SIZE = 4 * 1024;
    const unsigned loops = 2;

    NV_STATUS                   status = NV_OK;
    unsigned                    index = 0;
    unsigned                    i = 0;
    uvmGpuAddressSpaceHandle    hVaSpace = 0;
    UvmtestMemblock             pattern;
    UvmtestMemblock             gpuRegion;
    UvmtestMemblock             cpuRegion;

    memset(&pattern, 0, sizeof(pattern));
    memset(&gpuRegion, 0, sizeof(gpuRegion));
    memset(&cpuRegion, 0, sizeof(cpuRegion));

    hVaSpace = channelManager->channelPool.hVaSpace;

    status = uvmtest_alloc_virt_cpu(hVaSpace, &pattern, REGION_SIZE, MAP_CPU);
    if (NV_OK != status)
        goto cleanup;

    status = uvmtest_alloc_virt_gpu(hVaSpace, &gpuRegion, REGION_SIZE, MAP_CPU);
    if (NV_OK != status)
        goto cleanup;
    memset(gpuRegion.cpu, 0xFF, REGION_SIZE);

    status = uvmtest_alloc_virt_cpu(hVaSpace, &cpuRegion, REGION_SIZE, MAP_CPU);
    if (NV_OK != status)
        goto cleanup;
    memset(cpuRegion.cpu, 0xFE, REGION_SIZE);

    for (index = 0; index < loops; ++index)
    {
        for (i = 0; i < (REGION_SIZE / sizeof(unsigned)); i++)
            ((unsigned*)pattern.cpu)[i] = index;

        status = uvmtest_inline_memcpy_virt(channelManager, gpuRegion.gpu, pattern.cpu,
                                            REGION_SIZE, NULL, NULL);
        if (status != NV_OK)
            goto cleanup;

        status = uvmtest_inline_memcpy_virt(channelManager, cpuRegion.gpu, pattern.cpu,
                                            REGION_SIZE, NULL, NULL);
        if (status != NV_OK)
            goto cleanup;

        // Check the pattern is copied
        for (i = 0; i < REGION_SIZE / sizeof(unsigned); i++)
        {
            if ( ((unsigned*)gpuRegion.cpu)[i] != ((unsigned*)pattern.cpu)[i] ||
                 ((unsigned*)cpuRegion.cpu)[i] != ((unsigned*)pattern.cpu)[i])
            {
                UVM_ERR_PRINT("ERROR: Copy failed. %dth loop", index);
                status = NV_ERR_INVALID_OPERATION;
                goto cleanup;
            }
        }
    }

cleanup:
    uvmtest_free_virt(&pattern);
    uvmtest_free_virt(&cpuRegion);
    uvmtest_free_virt(&gpuRegion);

    return status;
}
