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

//
// This file contains supporting functions to get proper process/ gpu 
// information required for incrementing/ enabling/ mapping counters.
//

#include "uvm_common.h"
#include "uvm_kernel_counters.h"

#include "uvm_ioctl.h"
#include "uvmtypes.h"

static struct kmem_cache *g_UvmCounterContainerCache    __read_mostly = NULL;

// API Initialization functions
NV_STATUS uvm_initialize_counters_api(void)
{
    NV_STATUS status = NV_OK;

    g_UvmCounterContainerCache = NULL;

    UVM_DBG_PRINT_RL("Init counters API\n");

    g_UvmCounterContainerCache = NV_KMEM_CACHE_CREATE("uvm_counter_container_t",
                                                      struct UvmCounterContainer_tag);
    if (!g_UvmCounterContainerCache)
    {
        status = NV_ERR_NO_MEMORY;
        goto fail;
    }

    return NV_OK;

fail:
    kmem_cache_destroy_safe(&g_UvmCounterContainerCache);
    return status;
}

void uvm_deinitialize_counters_api(void)
{
    kmem_cache_destroy(g_UvmCounterContainerCache);
}

// These functions map and unmap counter pages
static void _uvm_unmap_counter_info(UvmCounterInfo *pCounter)
{
    if (!pCounter)
        return;

    if (pCounter->pCounterPage != NULL)
    {
        kunmap(pCounter->pCounterPage);
        __free_page(pCounter->pCounterPage);
    }
}

static NV_STATUS _uvm_map_counter_info(UvmCounterInfo *pCounter)
{
    NV_STATUS status = NV_OK;

    if (!pCounter)
        return NV_ERR_INVALID_ARGUMENT;

    memset(pCounter, 0, sizeof(*pCounter));
    pCounter->pCounterPage = alloc_page(NV_UVM_GFP_FLAGS | GFP_HIGHUSER);
    if (!pCounter->pCounterPage)
        return NV_ERR_NO_MEMORY;

    pCounter->sysAddr = kmap(pCounter->pCounterPage);
    if (pCounter->sysAddr == NULL)
    {
        status = NV_ERR_INSUFFICIENT_RESOURCES;
        goto fail;
    }
    memset(pCounter->sysAddr, 0, PAGE_SIZE);

    return NV_OK;

fail:
    _uvm_unmap_counter_info(pCounter);
    return status;
}


// Allocation and ref counting functions
static void _uvm_free_counter_container(UvmCounterContainer *pCounterContainer)
{
    unsigned gpu;

    if (!pCounterContainer)
        return;

    for (gpu = 0; gpu < UVM_MAX_GPUS; gpu++)
        _uvm_unmap_counter_info(&pCounterContainer->perGpuCounterArray[gpu]);
    _uvm_unmap_counter_info(&pCounterContainer->allGpuCounter);
    kmem_cache_free(g_UvmCounterContainerCache, pCounterContainer);
}

NV_STATUS uvm_alloc_counter_container(UvmCounterContainer **ppCounterContainer)
{
    NV_STATUS status = NV_OK;
    unsigned gpu;

    if (!ppCounterContainer)
        return NV_ERR_INVALID_ARGUMENT;

    *ppCounterContainer = (UvmCounterContainer *)
            kmem_cache_zalloc(g_UvmCounterContainerCache, NV_UVM_GFP_FLAGS);

    if (!*ppCounterContainer)
        return NV_ERR_NO_MEMORY;

    for (gpu = 0; gpu < UVM_MAX_GPUS; gpu++)
    {
        status = _uvm_map_counter_info(
            &(*ppCounterContainer)->perGpuCounterArray[gpu]);
        if (NV_OK != status)
            goto fail;
    }

    status = _uvm_map_counter_info(&(*ppCounterContainer)->allGpuCounter);
    if (NV_OK != status)
        goto fail;

    NV_ATOMIC_SET((*ppCounterContainer)->refcountUsers, 1);

    return NV_OK;

fail:
    _uvm_free_counter_container(*ppCounterContainer);
    return status;
}

void uvm_ref_counter_container(UvmCounterContainer *pCounterContainer)
{
    if (!pCounterContainer)
        return;
    NV_ATOMIC_INC(pCounterContainer->refcountUsers);
}

void uvm_unref_counter_container(UvmCounterContainer *pCounterContainer)
{
    if (!pCounterContainer)
        return;

    if (NV_ATOMIC_DEC_AND_TEST(pCounterContainer->refcountUsers))
        _uvm_free_counter_container(pCounterContainer);
}

NV_STATUS uvm_map_counters_pages(UvmCounterContainer *pCounterContainer,
                                 NvP64 userCountersBaseAddr,
                                 struct vm_area_struct *pVma)
{
    NV_STATUS status = NV_OK;
    struct page *pPage = NULL;
    NvUPtr currentUserBaseAddress = (NvUPtr)userCountersBaseAddr;

    if (!pVma || !pCounterContainer)
        return NV_ERR_INVALID_ARGUMENT;

    pPage = pCounterContainer->allGpuCounter.pCounterPage;
    status = uvm_map_page(pVma, pPage, currentUserBaseAddress);
    currentUserBaseAddress += UVM_PER_PROCESS_PER_GPU_COUNTERS_SHIFT;
    if (NV_OK == status)
    {
        unsigned i;
        for (i = 0; i < UVM_MAX_GPUS; ++i)
        {
            pPage = pCounterContainer->perGpuCounterArray[i].pCounterPage;
            status = uvm_map_page(pVma, pPage, currentUserBaseAddress);
            currentUserBaseAddress += UVM_PER_RESOURCE_COUNTERS_SIZE;
            if (status != NV_OK)
                return status;
        }
    }
    return NV_OK;
}

// this function checks if the countername is valid and returns its index
NV_STATUS uvm_get_counter_index(UvmCounterName counterName,
                                unsigned *counterIndex)
{
    if(counterName >= 0 && counterName < UVM_TOTAL_COUNTERS) 
    {
        *counterIndex = counterName;
        return NV_OK;
    }
    return NV_ERR_INVALID_ARGUMENT;
}

// Locking: Need to acquire process lock before incrementing process counters
void uvm_increment_process_counters(unsigned gpuIndex,
                                    UvmCounterContainer *pCounterContainer,
                                    UvmCounterName counterName,
                                    unsigned incrementVal)
{
    unsigned long long *counterArray;
    // The value of the counter name is used as its index in the counter array
    unsigned counterIndex = counterName;

    if (!pCounterContainer)
        return;

    // Increment process all gpu counters if any session has enabled them.
    if (NV_ATOMIC_READ(pCounterContainer->
                       allGpuCounter.sessionCount[counterIndex]) != 0)
    {
        counterArray = pCounterContainer->allGpuCounter.sysAddr;
        counterArray[counterIndex] += incrementVal;
    }

    // Process Single Gpu counters are enabled by default:
    counterArray = pCounterContainer->perGpuCounterArray[gpuIndex].sysAddr;
    counterArray[counterIndex] += incrementVal;
}
