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

#include "uvm_common.h"
#include "uvm_page_migration.h"
#include "uvm_utils.h"
#include "uvm_lite.h"
#include "uvm_channel_mgmt.h"
#include "uvm_mmu_mgmt.h"

static struct kmem_cache *g_uvmChannelManagerCache  __read_mostly = NULL;
static struct kmem_cache *g_uvmRingbufferCache      __read_mostly = NULL;
static struct kmem_cache *g_uvmChannelCache         __read_mostly = NULL;
static struct kmem_cache *g_uvmPushbufferCache      __read_mostly = NULL;
static struct kmem_cache *g_uvmTrackerCache         __read_mostly = NULL;
static struct kmem_cache *g_uvmTrackerItemCache     __read_mostly = NULL;
static UvmChannelMgmtMemory g_uvmCMMObject;

static NV_STATUS _create_channel_pool(NvProcessorUuid *pGpuUuidStruct,
                                      UvmChannelPool *channelPool,
                                      UvmChannelManager *channelManager);
static void _destroy_channel_pool(UvmChannelPool *channelPool);

static NV_STATUS _create_channel_list(UvmChannelPool *channelPool,
                                      struct list_head *head,
                                      unsigned numChannels);
static void _destroy_channel_list(UvmChannelPool *channelPool,
                                  struct list_head *head);
static NV_STATUS _create_channel_resources(UvmChannelPool *channelPool,
                                           UvmChannel *channel);
static void _destroy_channel_resources(UvmChannelPool *channelPool,
                                       UvmChannel *channel);

static NV_STATUS _create_ringbuffer_pool(NvProcessorUuid *pGpuUuidStruct,
                                         UvmChannelPool *channelPool,
                                         UvmRingbufferPool *ringbufferPool);

static void _destroy_ringbuffer_pool(UvmChannelPool *channelPool,
                                     UvmRingbufferPool *ringbufferPool);

static NV_STATUS _create_ringbuffer_list(struct list_head *head,
                                         unsigned numRingbuffers,
                                         unsigned ringbufferSize,
                                         NvU64 cpuOffset,
                                         UvmGpuPointer gpuOffset);
static void _destroy_ringbuffer_list(struct list_head *head);

static void _destroy_pushbuffer_list(struct list_head *head);

static NV_STATUS _find_memory_in_ringbuffer(
        UvmRingbuffer *ringbuffer, NvU64 *offset);

static void _reclaim_pushbuffers(UvmChannelManager *channelManager,
                                 UvmChannel *channel);

static void _free_submitted_pushbuffer(
        UvmChannelManager *channelManager, UvmPushbuffer *pushbuffer);

static NV_STATUS _map_semaphores_for_channel_pool(UvmChannelPool * channelPool);

static void _free_semaphores_for_channel_pool(UvmChannelPool * channelPool);

static inline NvBool uvm_query_channel_seq_done(UvmChannel *channel, NvU64 seq);

//
// Thread Safety:
//   Called during the startup of UVM.
//   This must be called and completed before any other operations in the API
//   can be used.
//
NV_STATUS uvm_initialize_channel_mgmt_api(void)
{
    UVM_DBG_PRINT("Entering\n");

    g_uvmChannelManagerCache = NV_KMEM_CACHE_CREATE("uvm_channel_manager_t", UvmChannelManager);
    if (!g_uvmChannelManagerCache)
        goto fail;

    g_uvmRingbufferCache = NV_KMEM_CACHE_CREATE("uvm_ringbuffer_t", UvmRingbuffer);
    if (!g_uvmRingbufferCache)
        goto fail;

    g_uvmChannelCache = NV_KMEM_CACHE_CREATE("uvm_channel_t", UvmChannel);
    if (!g_uvmChannelCache)
        goto fail;

    g_uvmPushbufferCache = NV_KMEM_CACHE_CREATE("uvm_pushbuffer_t", UvmPushbuffer);
    if (!g_uvmPushbufferCache)
        goto fail;

    g_uvmTrackerCache = NV_KMEM_CACHE_CREATE("uvm_tracker_t", UvmTracker);
    if (!g_uvmTrackerCache)
        goto fail;

    g_uvmTrackerItemCache = NV_KMEM_CACHE_CREATE("uvm_trackeritem_t", UvmTrackerItem);
    if (!g_uvmTrackerItemCache)
        goto fail;

    UVM_PANIC_ON(g_uvmCMMObject.bSemaPoolAllocated);
    memset(&g_uvmCMMObject, 0x0, sizeof(g_uvmCMMObject));
    init_rwsem(&g_uvmCMMObject.lock);
    INIT_LIST_HEAD(&g_uvmCMMObject.activePoolsHead);

    return NV_OK;

fail:
    UVM_ERR_PRINT_NV_STATUS("Could not allocate channel mgmt API resources.",
                            NV_ERR_NO_MEMORY);

    kmem_cache_destroy_safe(&g_uvmPushbufferCache);
    kmem_cache_destroy_safe(&g_uvmChannelCache);
    kmem_cache_destroy_safe(&g_uvmRingbufferCache);
    kmem_cache_destroy_safe(&g_uvmChannelManagerCache);
    kmem_cache_destroy_safe(&g_uvmTrackerCache);
    kmem_cache_destroy_safe(&g_uvmTrackerItemCache);

    return NV_ERR_NO_MEMORY;
}

//
// Thread Safety:
//   Called during the shutdown of UVM.
//   This must be called and completed after all other operations in the API
//   are finished.
//
void uvm_deinitialize_channel_mgmt_api(void)
{
    UVM_DBG_PRINT("Entering\n");

    kmem_cache_destroy_safe(&g_uvmPushbufferCache);
    kmem_cache_destroy_safe(&g_uvmChannelCache);
    kmem_cache_destroy_safe(&g_uvmRingbufferCache);
    kmem_cache_destroy_safe(&g_uvmChannelManagerCache);
    kmem_cache_destroy_safe(&g_uvmTrackerCache);
    kmem_cache_destroy_safe(&g_uvmTrackerItemCache);
    UVM_PANIC_ON(g_uvmCMMObject.bSemaPoolAllocated);
}

NV_STATUS uvm_create_channel_manager(NvProcessorUuid *gpuUuid,
                                     UvmChannelManager **channelManager)
{
    NV_STATUS status;
    UvmChannelManager *newChannelManager;

    UVM_DBG_PRINT_UUID("Entering\n", gpuUuid);

    newChannelManager = kmem_cache_zalloc(g_uvmChannelManagerCache,
                                          NV_UVM_GFP_FLAGS);
    if (!newChannelManager)
    {
        status = NV_ERR_NO_MEMORY;
        goto cleanup;
    }

    // Create channel pool
    status = _create_channel_pool(gpuUuid,
                                  &newChannelManager->channelPool,
                                  newChannelManager);
    if (status != NV_OK)
        goto cleanup;

    // Create ringbuffer pool
    status = _create_ringbuffer_pool(gpuUuid,
                                    &newChannelManager->channelPool,
                                    &newChannelManager->ringbufferPool);
    if (status != NV_OK)
        goto cleanup;

    init_rwsem(&newChannelManager->channelManagerLock);

    *channelManager = newChannelManager;

    return NV_OK;

cleanup:

    if (newChannelManager && (newChannelManager->ringbufferPool.numRingbuffers))
        _destroy_ringbuffer_pool(&newChannelManager->channelPool,
                                 &newChannelManager->ringbufferPool);

    if (newChannelManager && (newChannelManager->channelPool.numChannels))
        _destroy_channel_pool(&newChannelManager->channelPool);

    return status;
}

void uvm_destroy_channel_manager(UvmChannelManager *channelManager)
{
    UVM_DBG_PRINT("Entering\n");

    _destroy_ringbuffer_pool(&channelManager->channelPool,
                             &channelManager->ringbufferPool);

    _destroy_channel_pool(&channelManager->channelPool);

    kmem_cache_free(g_uvmChannelManagerCache, channelManager);
}

void uvm_init_tracker(UvmTracker *tracker)
{
    INIT_LIST_HEAD(&tracker->itemHead);
    tracker->usedTail = &tracker->itemHead;
    tracker->nTotalItems = 0;
    tracker->nUsedItems = 0;
}

UvmTracker* uvm_allocate_tracker(void)
{
    UvmTracker *tracker;

    tracker = kmem_cache_alloc(g_uvmTrackerCache, NV_UVM_GFP_FLAGS);
    if (!tracker)
        return NULL;

    uvm_init_tracker(tracker);
    return tracker;
}

void uvm_free_tracker(UvmTracker *tracker)
{
    UvmTrackerItem *item, *itemSafe;

    if (tracker)
    {
        list_for_each_entry_safe(item, itemSafe, &tracker->itemHead, list)
        {
            list_del(&item->list);
            kmem_cache_free(g_uvmTrackerItemCache, item);
        }
        kmem_cache_free(g_uvmTrackerCache, tracker);
    }
}

// Moves all items from used to free list.
void uvm_reset_tracker(UvmTracker *tracker)
{
    tracker->nUsedItems = 0;
    tracker->usedTail = &tracker->itemHead;
}

size_t uvm_shrink_tracker(UvmTracker *tracker)
{
    UvmTrackerItem *trackerItem;

    while (tracker->nTotalItems > tracker->nUsedItems)
    {
        // Get the first entry after the last used.
        trackerItem = list_first_entry(tracker->usedTail,
                                       UvmTrackerItem,
                                       list);
        list_del(&trackerItem->list);
        kmem_cache_free(g_uvmTrackerItemCache, trackerItem);
        tracker->nTotalItems--;
    }
    return tracker->nTotalItems;
}

// This adds items if they are less.
NV_STATUS uvm_grow_tracker(UvmTracker *tracker, size_t nItems)
{
    UvmTrackerItem *trackerItem;

    while (tracker->nTotalItems < nItems)
    {
        trackerItem = kmem_cache_zalloc(g_uvmTrackerItemCache,
                                            NV_UVM_GFP_FLAGS);
        if (!trackerItem)
            return NV_ERR_NO_MEMORY;

        // Insert after the last used entry.
        list_add(&trackerItem->list, tracker->usedTail);
        tracker->nTotalItems++;
    }
    return NV_OK;
}

// Move valid items from src to dst. Use memory from src if no space in dst.
void uvm_move_tracker(UvmTracker *dst, UvmTracker *src)
{
    UvmTracker temp;
    UvmTrackerItem *item;
    unsigned i = 0;
    INIT_LIST_HEAD(&temp.itemHead);

    // Sanity check if Src tracker even has anything to copy
    if (src->nUsedItems == 0)
        return;

    // 1. Move all the items from dst into temp. (empty dst)
    list_splice_init(&dst->itemHead, &temp.itemHead);
    temp.nTotalItems = dst->nTotalItems;

    // 2. Move all used items(head to usedTail) from src to dst
    list_cut_position(&dst->itemHead,
                      &src->itemHead,
                      src->usedTail);
    if (list_empty(&dst->itemHead))
        dst->usedTail = &dst->itemHead;
    else
        dst->usedTail = src->usedTail;

    dst->nTotalItems = dst->nUsedItems = src->nUsedItems;
    src->nTotalItems -= src->nUsedItems;
    src->nUsedItems = 0;
    src->usedTail = &src->itemHead;

    // 3. If temp.total > dst.total; top-up dst using temp.
    while (temp.nTotalItems > dst->nTotalItems)
    {
        item = list_first_entry(&temp.itemHead,
                                UvmTrackerItem,
                                list);
        list_del(&item->list);
        list_add_tail(&item->list, &dst->itemHead);
        dst->nTotalItems++;
        i++;
    }
    temp.nTotalItems -= i;

    // 4. Move left over temp items to src
    list_splice(&temp.itemHead, &src->itemHead);
    src->nTotalItems += temp.nTotalItems;
}

NV_STATUS uvm_merge_tracker_item(UvmTracker *tracker, UvmTrackerItem *item)
{
    UvmTrackerItem *trackerItem;
    struct list_head *pos = NULL;

    if (tracker->nTotalItems == 0)
        return NV_ERR_NO_MEMORY;

    // If no used entries; usedTail will be head. Just add the item.
    if (tracker->nUsedItems == 0)
    {
        UVM_PANIC_ON(tracker->usedTail != &tracker->itemHead);
        pos = &tracker->itemHead;
    }
    else
    {
        // Search for same channel in list. If encounter free list; just add.
        list_for_each_entry(trackerItem, &tracker->itemHead, list)
        {
            if (trackerItem->channel == item->channel)
            {
                // Replace the item if the seqNum being tracked is lower.
                if (trackerItem->seqNum < item->seqNum)
                    trackerItem->seqNum = item->seqNum;
                return NV_OK;
            }
            else if (&trackerItem->list == tracker->usedTail)
            {
                // This happened to be the last used
                pos = &trackerItem->list;
                break;
            }
        }
    }

    // Check for free space.
    if (!pos || !(tracker->nUsedItems < tracker->nTotalItems))
        return NV_ERR_NO_MEMORY;

    // The next item starting from pos is the free one.
    trackerItem = list_first_entry(pos,
                                   UvmTrackerItem,
                                   list);
    trackerItem->channel = item->channel;
    trackerItem->seqNum = item->seqNum;

    // Update tracker state.
    tracker->nUsedItems++;
    tracker->usedTail = &trackerItem->list;
    return NV_OK;
}

// Moves the given tracker item from used list to free list.
// Function assumes that the item is one of the used items.
static void uvm_retire_tracker_item(UvmTracker *tracker, UvmTrackerItem * item)
{
    UvmTrackerItem* i;
    UVM_PANIC_ON(tracker->nUsedItems == 0);

    tracker->nUsedItems--;
    // If last item; just move the usedtail
    if (&item->list == tracker->usedTail)
    {
        i = list_prev_entry(item, list);
        tracker->usedTail = &i->list;
    }
    else
    {
        list_move(&item->list, tracker->usedTail);
    }
}

// Returns UVM_MAX_NUM_CHANNEL_POOLS if exceeds limit.
// This function acquires the global lock internally. Caller should NOT hold the
// global lock while calling.
static size_t _get_next_free_pool_id_safe(void)
{
    size_t id;
    down_write(&g_uvmCMMObject.lock);
    id = (g_uvmCMMObject.poolCount >= UVM_MAX_NUM_CHANNEL_POOLS) ?
         UVM_MAX_NUM_CHANNEL_POOLS :
         g_uvmCMMObject.poolCount++;
    up_write(&g_uvmCMMObject.lock);
    return id;
}

//
// channelPool should be zeroed out before calling this function.
//
static NV_STATUS _create_channel_pool(NvProcessorUuid *pGpuUuidStruct,
                                      UvmChannelPool *channelPool,
                                      UvmChannelManager *channelManager)
{
    NV_STATUS status;
    unsigned numChannels = UVM_CHANNEL_POOL_DEFAULT_SIZE;
    struct list_head channelListHead;
    size_t id;

    // Create a GPU Session
    status = nvUvmInterfaceSessionCreate(&channelPool->hSession);
    if (status != NV_OK)
    {
        UVM_ERR_PRINT_UUID("Could not create a session. "
                           "NV_STATUS: 0x%x.",
                           pGpuUuidStruct, status);
        goto cleanup;
    }

    // Create a VASpace (shared between RM adn UVM)
    // RM owns PDE3[0] and rest are owned by UVM
    status = nvUvmInterfaceAddressSpaceCreate(
            channelPool->hSession,
            pGpuUuidStruct,
            &channelPool->hVaSpace,
            0,
            0x400000000ULL - 1);
    if (status != NV_OK)
    {
        UVM_ERR_PRINT_UUID("Could not create an address space. "
                           "NV_STATUS: 0x%x.",
                           pGpuUuidStruct, status);
        goto cleanup;
    }

    // Get the gmmu utils template for this gpu vaspace
    status = nvUvmInterfaceGetGmmuFmt(channelPool->hVaSpace,
        (void**)&channelPool->pGmmuFmt);
    if (status != NV_OK)
    {
        UVM_ERR_PRINT_UUID("Could not query the GMMU format. "
            "NV_STATUS: 0x%x.",
            pGpuUuidStruct, status);
        goto cleanup;
    }

    // Get GPU caps like ECC support on GPU, big page size, small page size, etc.
    status = nvUvmInterfaceQueryCaps(channelPool->hVaSpace,
                                     &channelPool->gpuCaps);
    if (status != NV_OK)
    {
        UVM_ERR_PRINT_UUID("Could not lookup GPU capabilities. "
                           "NV_STATUS: 0x%x.",
                           pGpuUuidStruct, status);
        goto cleanup;
    }

    // Map the global semaphore pool on cpu and gpu vaspace.
    status = _map_semaphores_for_channel_pool(channelPool);
    if (status != NV_OK)
    {
        UVM_ERR_PRINT_UUID("Could not map semaphore pool."
                           "NV_STATUS: 0x%x.",
                           pGpuUuidStruct, status);
        goto cleanup;
    }

    id = _get_next_free_pool_id_safe();
    if (id >= UVM_MAX_NUM_CHANNEL_POOLS)
    {
        status = NV_ERR_INSUFFICIENT_RESOURCES;
        goto cleanup;
    }
    channelPool->poolId = id;
    // Semaphore offset is the location of the semaphores for the associated 
    // channels from the start of the semaphore pool based on the Pool Id.
    channelPool->semaOffset = channelPool->poolId *
                                UVM_CHANNEL_POOL_DEFAULT_SIZE *
                                UVM_SEMAPHORE_SIZE_BYTES;
    channelPool->manager = channelManager;

    // build channel list. Keep assigning channel ids for sema tracking.
    INIT_LIST_HEAD(&channelListHead);
    status = _create_channel_list(channelPool, &channelListHead, numChannels);
    if (status != NV_OK)
    {
        UVM_ERR_PRINT_UUID("Could not create channel list. "
                           "NV_STATUS: 0x%x.",
                           pGpuUuidStruct, status);
        goto cleanup;
    }

    INIT_LIST_HEAD(&channelPool->channelListHead);
    list_replace(&channelListHead, &channelPool->channelListHead);
    channelPool->numChannels = numChannels;

    return NV_OK;

cleanup:

    _free_semaphores_for_channel_pool(channelPool);

    if (channelPool->hVaSpace)
    {
        nvUvmInterfaceAddressSpaceDestroy(channelPool->hVaSpace);
        channelPool->hVaSpace = NULL;
    }

    if (channelPool->hSession)
    {
        nvUvmInterfaceSessionDestroy(channelPool->hSession);
        channelPool->hSession = NULL;
    }

    return status;
}

static void _destroy_channel_pool(UvmChannelPool *channelPool)
{
    _destroy_channel_list(channelPool, &channelPool->channelListHead);

    _free_semaphores_for_channel_pool(channelPool);

    // De-associate the pool from the api.
    down_write(&g_uvmCMMObject.lock);
    list_del(&channelPool->poolList);
    g_uvmCMMObject.mapCount--;
    // If no more channel pools; clear the api object
    if (list_empty(&g_uvmCMMObject.activePoolsHead))
    {
        UVM_PANIC_ON(g_uvmCMMObject.mapCount);
        g_uvmCMMObject.bSemaPoolAllocated = NV_FALSE;
        g_uvmCMMObject.poolCount = 0;
    }
    up_write(&g_uvmCMMObject.lock);

    nvUvmInterfaceAddressSpaceDestroy(channelPool->hVaSpace);
    nvUvmInterfaceSessionDestroy(channelPool->hSession);

    INIT_LIST_HEAD(&channelPool->channelListHead);
    channelPool->numChannels = 0;
    channelPool->hVaSpace    = NULL;
    channelPool->hSession    = NULL;
}

//
// ringbufferPool should be zeroed out before calling this function.
//
static NV_STATUS _create_ringbuffer_pool(NvProcessorUuid *pGpuUuidStruct,
                                         UvmChannelPool *channelPool,
                                         UvmRingbufferPool *ringbufferPool)
{
    NV_STATUS status;
    unsigned numRingbuffers = UVM_RINGBUFFER_POOL_DEFAULT_SIZE;
    unsigned ringbufferSize = UVM_RINGBUFFER_DEFAULT_SIZE;
    UvmRingbuffer *ringbuffer = NULL;
    UvmGpuPointer gpuPtr = 0;
    NvUPtr        cpuPtr = 0;
    struct list_head ringbufferListHead;

    // Allocate memory on sysmem and map to CPU.
    status = nvUvmInterfaceMemoryAllocSys(
            channelPool->hVaSpace,
            numRingbuffers * ringbufferSize,
            &gpuPtr, NULL);
    if (status != NV_OK)
    {
        UVM_ERR_PRINT("ERROR: could not allocate GPU memory"
                      " for ringbuffer pool. NV_STATUS: 0x%x.\n",
                      status);
        goto cleanup;
    }

    status = nvUvmInterfaceMemoryCpuMap(
            channelPool->hVaSpace,
            gpuPtr,
            numRingbuffers * ringbufferSize,
            (void**)&cpuPtr, UVM_PAGE_SIZE_DEFAULT);
    if (status != NV_OK)
    {
        UVM_ERR_PRINT("ERROR: could not map GPU VA to CPU VA. "
                      "NV_STATUS: 0x%x.\n", status);
        goto cleanup;
    }

    // build ring buffer list
    INIT_LIST_HEAD(&ringbufferListHead);
    status = _create_ringbuffer_list(&ringbufferListHead,
                                     numRingbuffers,
                                     ringbufferSize,
                                     cpuPtr,
                                     gpuPtr);
    if (status != NV_OK)
        goto cleanup;

    INIT_LIST_HEAD(&ringbufferPool->ringbufferListHead);
    list_replace(&ringbufferListHead,
                 &ringbufferPool->ringbufferListHead);

    INIT_LIST_HEAD(&ringbufferPool->ringbufferFreeListHead);
    list_for_each_entry(ringbuffer,
                        &ringbufferPool->ringbufferListHead,
                        ringbufferList)
    {
        list_add(&ringbuffer->ringbufferFreeList,
                 &ringbufferPool->ringbufferFreeListHead);
    }

    INIT_LIST_HEAD(&ringbufferPool->pbFreeListHead);

    ringbufferPool->numRingbuffers = numRingbuffers;
    ringbufferPool->gpuPtr         = gpuPtr;
    ringbufferPool->cpuPtr         = cpuPtr;

    return NV_OK;

cleanup:

    if (cpuPtr)
        nvUvmInterfaceMemoryCpuUnMap(channelPool->hVaSpace, (void*)cpuPtr);

    if (gpuPtr)
        nvUvmInterfaceMemoryFree(channelPool->hVaSpace, gpuPtr);

    return status;
}

static void _destroy_ringbuffer_pool(UvmChannelPool *channelPool,
                                     UvmRingbufferPool *ringbufferPool)
{
    // Free ringbuffer list
    _destroy_pushbuffer_list(&ringbufferPool->pbFreeListHead);
    _destroy_ringbuffer_list(&ringbufferPool->ringbufferListHead);

    // Unmap CPU mapping and free GPU memory allocation
    nvUvmInterfaceMemoryCpuUnMap(channelPool->hVaSpace,
                                 (void*)(ringbufferPool->cpuPtr));
    nvUvmInterfaceMemoryFree(channelPool->hVaSpace,
                             ringbufferPool->gpuPtr);


    INIT_LIST_HEAD(&ringbufferPool->pbFreeListHead);
    INIT_LIST_HEAD(&ringbufferPool->ringbufferListHead);
    INIT_LIST_HEAD(&ringbufferPool->ringbufferFreeListHead);
    ringbufferPool->numRingbuffers = 0;
    ringbufferPool->gpuPtr         = 0;
    ringbufferPool->cpuPtr         = 0;
}

// Caller should hold the global memory object lock and 
// expect the lock be held on return.
static NV_STATUS _allocate_sema_pool_for_channel_pool(
                                                   UvmChannelPool * channelPool)
{
    NV_STATUS status;
    UvmGpuPointer gpuPointer = 0;
    void * cpuPointer = NULL;

    status = nvUvmInterfaceMemoryAllocSys(channelPool->hVaSpace,
                                          UVM_SEMAPHORE_POOL_SIZE_BYTES,
                                          &gpuPointer,
                                          NULL);
    if (NV_OK != status)
        goto cleanup;

    status = nvUvmInterfaceMemoryCpuMap(channelPool->hVaSpace,
                                        gpuPointer,
                                        UVM_SEMAPHORE_POOL_SIZE_BYTES,
                                        &cpuPointer, UVM_PAGE_SIZE_DEFAULT);
    if (NV_OK != status)
        goto cleanup;

    memset(cpuPointer, 0x0, UVM_SEMAPHORE_POOL_SIZE_BYTES);
    channelPool->semaPool.semaGpuPointerBase = gpuPointer;
    channelPool->semaPool.semaCpuPointerBase = (NvUPtr)cpuPointer;

    // Associate with api.
    list_add(&channelPool->poolList, &g_uvmCMMObject.activePoolsHead);
    g_uvmCMMObject.bSemaPoolAllocated = NV_TRUE;
    g_uvmCMMObject.mapCount++;
    return NV_OK;
cleanup:
    if (cpuPointer)
        nvUvmInterfaceMemoryCpuUnMap(channelPool->hVaSpace, cpuPointer);

    if (gpuPointer)
        nvUvmInterfaceMemoryFree(channelPool->hVaSpace, gpuPointer);
    return NV_ERR_INSUFFICIENT_RESOURCES;
}

// Caller should hold the api lock and expect the lock be held on return.
static NV_STATUS _duplicate_sema_pool_for_channel_pool(
                                                   UvmChannelPool * channelPool)
{
    UvmChannelPool *pool;
    NV_STATUS status;

    UVM_PANIC_ON(!g_uvmCMMObject.bSemaPoolAllocated);
    UVM_PANIC_ON(list_empty(&g_uvmCMMObject.activePoolsHead));

    // We need to duplicate using any one channel pool entry from the list.
    pool = list_first_entry(&g_uvmCMMObject.activePoolsHead,
                            UvmChannelPool,
                            poolList);
    status = nvUvmInterfaceDupAllocation(0,
                                      pool->hVaSpace,
                                      pool->semaPool.semaGpuPointerBase,
                                      channelPool->hVaSpace,
                                      &channelPool->semaPool.semaGpuPointerBase,
                                      NV_FALSE/*handle not valid*/);
    if (NV_OK != status)
        return status;

    status = nvUvmInterfaceMemoryCpuMap(channelPool->hVaSpace,
                        channelPool->semaPool.semaGpuPointerBase,
                        UVM_SEMAPHORE_POOL_SIZE_BYTES,
                        (void**)(&channelPool->semaPool.semaCpuPointerBase),
                        UVM_PAGE_SIZE_DEFAULT);
    if (NV_OK != status)
    {
        nvUvmInterfaceMemoryFree(channelPool->hVaSpace,
                                 channelPool->semaPool.semaGpuPointerBase);
        return status;
    }

    // Associate with global list
    list_add(&channelPool->poolList, &g_uvmCMMObject.activePoolsHead);
    g_uvmCMMObject.mapCount++;
    return status;
}

// Allocates or duplicates the semaphore pool in the channelPool's VASpace.
static NV_STATUS _map_semaphores_for_channel_pool(UvmChannelPool * channelPool)
{
    NV_STATUS status;

    // Allocating or duplicating inside the api lock may be ok for UVM-RM
    // interaction because the api object may only be touched in callbacks which
    // dont hold the RM api/gpu locks. Ex: startDevice, stopDevice.
    down_write(&g_uvmCMMObject.lock);

    if (g_uvmCMMObject.bSemaPoolAllocated)
        status = _duplicate_sema_pool_for_channel_pool(channelPool);
    else
        status = _allocate_sema_pool_for_channel_pool(channelPool);

    up_write(&g_uvmCMMObject.lock);
    return status;
}

static void _free_semaphores_for_channel_pool(UvmChannelPool * channelPool)
{
    down_write(&g_uvmCMMObject.lock);
    if (channelPool->semaPool.semaCpuPointerBase)
        nvUvmInterfaceMemoryCpuUnMap(channelPool->hVaSpace,
                            (void*)(channelPool->semaPool.semaCpuPointerBase));

    if (channelPool->semaPool.semaGpuPointerBase)
        nvUvmInterfaceMemoryFree(channelPool->hVaSpace,
                                 channelPool->semaPool.semaGpuPointerBase);
    up_write(&g_uvmCMMObject.lock);
}

static NV_STATUS _create_channel_list(UvmChannelPool *channelPool,
                                      struct list_head *head,
                                      unsigned numChannels)
{
    NV_STATUS status;
    unsigned ch;
    UvmChannel *channel;

    if (channelPool->numChannels + numChannels > UVM_CHANNEL_POOL_DEFAULT_SIZE)
    {
        UVM_ERR_PRINT("ERROR: Num of channels %d in pool exceed limit.\n", 
                        channelPool->numChannels + numChannels);
        return NV_ERR_INVALID_REQUEST;
    }

    for (ch = 0; ch < numChannels; ++ch)
    {
        channel = kmem_cache_alloc(g_uvmChannelCache, NV_UVM_GFP_FLAGS);
        if (!channel)
        {
            status = NV_ERR_NO_MEMORY;
            UVM_ERR_PRINT("ERROR: could not allocate memory for channel "
                          "at index %u. NV_STATUS: 0x%x.\n", ch, status);
            goto cleanup_channel_list;
        }

        // Increment channel id on top of previous possible call.
        channel->id   = ch + channelPool->numChannels;
        // Populate back pointer to pool
        channel->pool = channelPool;

        status = _create_channel_resources(channelPool, channel);
        if (status != NV_OK)
        {
            UVM_ERR_PRINT("ERROR: could not create channel resources "
                          "at index %d. NV_STATUS: 0x%x.\n",
                          ch, status);
            kmem_cache_free(g_uvmChannelCache, channel);
            goto cleanup_channel_list;
        }

        list_add(&channel->channelList, head);

        INIT_LIST_HEAD(&channel->pbSubmittedListHead);
    }
    channelPool->numChannels += ch;
    return NV_OK;

cleanup_channel_list:
    _destroy_channel_list(channelPool, head);

    return status;
}

static void _destroy_channel_list(UvmChannelPool *channelPool,
                                  struct list_head *head)
{
    if (!list_empty(head))
    {
        struct list_head *pos;
        struct list_head *safepos;
        UvmChannel       *channel;

        list_for_each_safe(pos, safepos, head)
        {
            channel = list_entry(pos, UvmChannel, channelList);
            list_del(pos);
            _destroy_channel_resources(channelPool, channel);
            kmem_cache_free(g_uvmChannelCache, channel);
        }
    }
}

static NV_STATUS _create_channel_resources(UvmChannelPool *channelPool,
                                           UvmChannel *channel)
{
    NV_STATUS status;
    unsigned ceInstance;
    NvU8 tempMemory[64];
    unsigned *tempPut;

    channel->trackingInfo.semaGpuPointer = 0;
    channel->trackingInfo.semaCpuPointer = 0;

    // Get all the channel pointers
    status = nvUvmInterfaceChannelAllocate(
            channelPool->hVaSpace,
            &channel->hChannel,
            &channel->channelInfo);
    if (status != NV_OK)
        return status;

    // Allocate copy engine object
    for (ceInstance = 1; ceInstance <= MAX_NUM_COPY_ENGINES; ++ceInstance)
    {
        status = nvUvmInterfaceCopyEngineAllocate(channel->hChannel,
                                                  ceInstance,
                                                  &channel->ceClassNumber,
                                                  &channel->hCopyEngine);

        if ((status == NV_ERR_INVALID_INDEX) || (status == NV_OK))
            break;
    }
    if (status != NV_OK)
        goto cleanup_channel;

    // Set up CE hal functions
    status = NvUvmHalInit(
            channel->ceClassNumber,
            channel->channelInfo.channelClassNum,
            &channel->ceOps);
    if (NV_OK != status)
        goto cleanup_channel;

    status = NvUvmMemOpsInit(channel->channelInfo.channelClassNum,
                             &channel->memOps);
    if ((NV_OK != status) && (NV_ERR_NOT_SUPPORTED != status))
      goto cleanup_channel;

    tempPut = (unsigned*)tempMemory;
    // Calculate beforehand the acquire method size.
    // TODO: Bug 1766104: uvm8: delete uvmfull/ subdirectory before release
    // ...just delete this entire file, instead of the original to-do: which was:
    //
    // This should be queried from the HAL directly.
    channel->acquireBytes = channel->ceOps.semaphoreAcquire_GEQ(
                            &tempPut,
                            (unsigned*)(tempMemory + sizeof(tempMemory)),
                            0,
                            0);
    UVM_PANIC_ON(channel->acquireBytes == 0);

    // Initialize channel tracking info
    channel->trackingInfo.curGpFifoOffset          = 0;
    channel->trackingInfo.numReservedGpFifoEntries = 0;
    // We have to keep a free GPFIFO to avoid GPPUT and GPGET to overlap
    channel->trackingInfo.numFreeGpFifoEntries =
        channel->channelInfo.numGpFifoEntries - 1;

    // Semaphore pointers are memory locations corresponding to that channel
    // in global semaphore pool.
    channel->trackingInfo.semaGpuPointer =
                              (NvU64)channelPool->semaPool.semaGpuPointerBase +
                              channelPool->semaOffset +
                              (channel->id * UVM_SEMAPHORE_SIZE_BYTES);
    channel->trackingInfo.semaCpuPointer = 
                              channelPool->semaPool.semaCpuPointerBase +
                              channelPool->semaOffset +
                              (channel->id * UVM_SEMAPHORE_SIZE_BYTES);
    NV_ATOMIC64_SET(channel->trackingInfo.seqNumDone, 0);
    channel->trackingInfo.seqNumPending = 0;
    spin_lock_init(&channel->trackingInfo.lock);

    return NV_OK;

cleanup_channel:
    _destroy_channel_resources(channelPool, channel);

    return status;
}

static void _destroy_channel_resources(
        UvmChannelPool *channelPool, UvmChannel *channel)
{
    nvUvmInterfaceChannelDestroy(channel->hChannel);
}

static NV_STATUS _create_ringbuffer_list(struct list_head *head,
                                         unsigned numRingbuffers,
                                         unsigned ringbufferSize,
                                         NvU64 cpuOffset,
                                         UvmGpuPointer gpuOffset)
{
    unsigned i;
    UvmRingbuffer *ringbuffer;

    for (i = 0; i < numRingbuffers; ++i)
    {
        ringbuffer = kmem_cache_alloc(g_uvmRingbufferCache, NV_UVM_GFP_FLAGS);
        if (!ringbuffer)
        {
            UVM_ERR_PRINT("ERROR: could not allocate memory for ringbuffer "
                          "at index %d.\n", i);
            goto cleanup_ringbuffer_list;
        }

        //
        // Initalize ringbuffer
        //

        ringbuffer->cpuBegin  = cpuOffset;
        ringbuffer->cpuEnd    = cpuOffset + ringbufferSize;
        ringbuffer->gpuBegin  = gpuOffset;
        ringbuffer->curOffset = 0;

        INIT_LIST_HEAD(&ringbuffer->pbListHead);

        list_add(&ringbuffer->ringbufferList, head);
        INIT_LIST_HEAD(&ringbuffer->ringbufferFreeList);

        cpuOffset += ringbufferSize;
        gpuOffset += ringbufferSize;
    }

    return NV_OK;

cleanup_ringbuffer_list:
    _destroy_ringbuffer_list(head);

    return NV_ERR_NO_MEMORY;
}

static void _destroy_ringbuffer_list(struct list_head *head)
{
    if (!list_empty(head))
    {
        struct list_head *pos;
        struct list_head *safepos;
        UvmRingbuffer    *ringbuffer;

        list_for_each_safe(pos, safepos, head)
        {
            ringbuffer = list_entry(pos, UvmRingbuffer, ringbufferList);
            list_del(pos);
            list_del(&ringbuffer->ringbufferFreeList);

            _destroy_pushbuffer_list(&ringbuffer->pbListHead);

            kmem_cache_free(g_uvmRingbufferCache, ringbuffer);
        }
    }
}

static void _destroy_pushbuffer_list(struct list_head *head)
{
    if (!list_empty(head))
    {
        struct list_head *pos;
        struct list_head *safepos;
        UvmPushbuffer    *pushbuffer;

        list_for_each_safe(pos, safepos, head)
        {
            pushbuffer = list_entry(pos, UvmPushbuffer, pbList);
            list_del(pos);

            kmem_cache_free(g_uvmPushbufferCache, pushbuffer);
        }
    }
}

void uvm_lock_channel_manager(UvmChannelManager *channelManager)
{
    down_write(&channelManager->channelManagerLock);
}

void uvm_unlock_channel_manager(UvmChannelManager *channelManager)
{
    up_write(&channelManager->channelManagerLock);
}

NV_STATUS uvm_channel_manager_alloc_pushbuffer_structure(
        UvmChannelManager *channelManager, UvmPushbuffer **pushbuffer)
{
    UvmPushbuffer *newPushbuffer;
    struct list_head *entry;
    struct list_head *safepos;

    // Find the first free pb structure in the ringbuffer pool.
    list_for_each_safe(
            entry, safepos, &channelManager->ringbufferPool.pbFreeListHead)
    {
        newPushbuffer = list_entry(entry, UvmPushbuffer, pbList);
        list_del(entry);

        goto done;
    }

    // Allocate pushbuffer structure, since we couldn't find any free pb.
    newPushbuffer = kmem_cache_zalloc(g_uvmPushbufferCache, NV_UVM_GFP_FLAGS);
    if (!newPushbuffer)
        return NV_ERR_NO_MEMORY;

done:
    *pushbuffer = newPushbuffer;

    return NV_OK;
}

void uvm_channel_manager_free_pushbuffer_structure(
        UvmChannelManager *channelManager, UvmPushbuffer *pushbuffer)
{
    //
    // TODO: Bug 1766104: uvm8: delete uvmfull/ subdirectory before release
    // ...just delete this entire file, instead of the original to-do: which was:
    // Use some heruistics to determine whether or not we need
    //       to release some memory back to the OS.
    //
    // Right now, we're just putting it back on the free list.
    //

    list_move(
            &pushbuffer->pbList,
            &channelManager->ringbufferPool.pbFreeListHead);
}

NV_STATUS uvm_channel_manager_get_ringbuffer(
        UvmChannelManager *channelManager, UvmRingbuffer **ringbuffer)
{
    struct list_head *entry;
    struct list_head *safepos;
    UvmRingbuffer *freeRingbuffer;
    UvmRingbufferPool *ringbufferPool = &channelManager->ringbufferPool;

    //
    // Find the first available ringbuffer with enough space in the pool.
    // Remove it from the free list and return it to the caller.
    //
    list_for_each_safe(
            entry, safepos, &ringbufferPool->ringbufferFreeListHead)
    {
        NvU64 curAddr;

        freeRingbuffer = list_entry(entry, UvmRingbuffer, ringbufferFreeList);

        if (NV_OK == _find_memory_in_ringbuffer(freeRingbuffer, &curAddr))
        {
            NvU64 cpuBegin = freeRingbuffer->cpuBegin;

            freeRingbuffer->curOffset = curAddr - cpuBegin;
            list_del_init(entry);

            *ringbuffer = freeRingbuffer;
            return NV_OK;
        }
    }

    return NV_ERR_NO_MEMORY;
}

//
// Find the immediate next slot that fits a pushbuffer.
//
static NV_STATUS _find_memory_in_ringbuffer(
        UvmRingbuffer *ringbuffer, NvU64 *offset)
{
    UvmPushbuffer *pbHead;
    UvmPushbuffer *pbTail;
    NvU64 pbHeadBegin;
    NvU64 pbHeadEnd;
    NvU64 pbTailBegin;
    NvU64 pbTailEnd;
    NvU64 ringbufferBegin;
    NvU64 ringbufferEnd;

    ringbufferBegin = ringbuffer->cpuBegin;
    ringbufferEnd   = ringbuffer->cpuEnd;

    // Empty list, we start from the beginning.
    if (list_empty(&ringbuffer->pbListHead))
    {
        *offset = ringbufferBegin;
        return NV_OK;
    }

    //
    // A list of multiple pushbuffers, we look for space in:
    // 1. top of ringbuffer
    // 2. bottom of ringbuffer
    // 3. between pb head and tail
    //

    pbHead =
        list_entry(
                ringbuffer->pbListHead.next,
                UvmPushbuffer,
                pbList);
    pbHeadBegin = pbHead->cpuBegin;
    pbHeadEnd   = pbHeadBegin + pbHead->curOffset;

    pbTail =
        list_entry(
                ringbuffer->pbListHead.prev,
                UvmPushbuffer,
                pbList);
    pbTailBegin = pbTail->cpuBegin;
    pbTailEnd   = pbTailBegin + pbTail->curOffset;

    if (pbHeadBegin > pbTailBegin)
    {
        if (pbHeadBegin - pbTailEnd >= UVM_PUSHBUFFER_RESERVATION_SIZE)
        {
            *offset = pbTailEnd;
            return NV_OK;
        }
        else
        {
            return NV_ERR_NO_MEMORY;
        }
    }
    else
    {
        if (ringbufferEnd - pbTailEnd >= UVM_PUSHBUFFER_RESERVATION_SIZE)
        {
            *offset = pbTailEnd;
            return NV_OK;
        }
        else if (pbHeadBegin - ringbufferBegin >= UVM_PUSHBUFFER_RESERVATION_SIZE)
        {
            *offset = ringbufferBegin;
            return NV_OK;
        }
        else
        {
            return NV_ERR_NO_MEMORY;
        }
    }

    return NV_ERR_NO_MEMORY;
}

void uvm_channel_manager_put_ringbuffer(
        UvmChannelManager *channelManager, UvmRingbuffer *ringbuffer)
{
    //
    // Put it the tail of the free list.
    // This way, the ringbuffer in the front get more time to free up space.
    //
    list_add_tail(
            &ringbuffer->ringbufferFreeList,
            &channelManager->ringbufferPool.ringbufferFreeListHead);
}

NV_STATUS uvm_channel_manager_get_channel(
        UvmChannelManager *channelManager, UvmChannel **channel)
{
    struct list_head *entry;
    struct list_head *safepos;
    UvmChannel *freeChannel;
    UvmChannelPool *channelPool = &channelManager->channelPool;

    //
    // Find the first available channel with spare GPFIFO entries.
    // Use a round robin policy.
    // Also reserve a GPFIFO entry.
    //
    list_for_each_safe(
            entry, safepos, &channelPool->channelListHead)
    {
        freeChannel = list_entry(entry, UvmChannel, channelList);

        if (freeChannel->trackingInfo.numFreeGpFifoEntries)
        {
            list_move_tail(entry, &channelPool->channelListHead);

            --freeChannel->trackingInfo.numFreeGpFifoEntries;
            ++freeChannel->trackingInfo.numReservedGpFifoEntries;

            *channel = freeChannel;

            return NV_OK;
        }
    }

    return NV_ERR_INSUFFICIENT_RESOURCES;
}

void uvm_channel_manager_put_channel(
        UvmChannelManager *channelManager, UvmChannel *channel)
{
    //
    // Put the channel back the to head of the channel pool.
    // And un-reserve the GPFIFO entry.
    //
    // This makes the channel ready for submitting a pushbuffer right away.
    //

    list_move(
            &channel->channelList,
            &channelManager->channelPool.channelListHead);

    UVM_PANIC_ON(!channel->trackingInfo.numReservedGpFifoEntries);
    --channel->trackingInfo.numReservedGpFifoEntries;
}

//
// The general idea of reclaim is:
//
// 1. We look at the oldest pushbuffer in each ringbuffer
// 2. For each completed oldest pushbuffer, reclaim all completed pushbuffers in
//    the channel corresponding to that oldest pushbuffer.
// NOTE: This policy has the potential to take a very long time, depending on
//       the number of completed pushbuffers. We should investigate ways to
//       limit this, such as reclaiming only as much space is needed or trying
//       to reclaim at least one pushbuffer on every call to uvm_get_pushbuffer
//       regardless of whether we're out of ringbuffer space.
//
NV_STATUS uvm_channel_manager_reclaim(UvmChannelManager *channelManager)
{
    UvmRingbufferPool  *ringbufferPool;
    UvmRingbuffer      *ringbuffer;
    UvmPushbuffer      *pb;

    ringbufferPool = &channelManager->ringbufferPool;

    UVM_DBG_PRINT("Reclaim is triggered.\n");

    //
    // Check the head of each ringbuffer to find out if they've completed.
    //
    list_for_each_entry(ringbuffer,
                        &ringbufferPool->ringbufferListHead,
                        ringbufferList)
    {
        if (!list_empty(&ringbuffer->pbListHead))
        {
            pb = list_entry(ringbuffer->pbListHead.next, UvmPushbuffer, pbList);

            // Here the first pushbuffer of the ringbuffer might not be
            // submitted yet. However, since pb->seqnum will be max NvU64,
            // we will never call _reclaim_pushbuffers on them.
            if (uvm_query_channel_seq_done(pb->channel, pb->seqNum))
                _reclaim_pushbuffers(channelManager, pb->channel);
        }
    }

    return NV_OK;
}

//
// Remove all the completed PBs in the same channel.
//
static void _reclaim_pushbuffers(UvmChannelManager *channelManager,
                                 UvmChannel *channel)
{
    UvmPushbuffer *pbEntry      = NULL;
    UvmPushbuffer *pbSafepos    = NULL;
    struct list_head *pbListHead = &channel->pbSubmittedListHead;
    NvU32 curSeq                = uvm_update_channel_progress(channel);

    list_for_each_entry_safe(pbEntry, pbSafepos, pbListHead, pbSubmittedList)
    {
        // The pushbuffer list is in submission order so we can early exit as
        // soon as we find an uncompleted pushbuffer
        if (pbEntry->seqNum <= curSeq)
            _free_submitted_pushbuffer(channelManager, pbEntry);
        else
            break;
    }
}

//
// Free memory associated of this pushbuffer back into ringbuffer,
// and put the pb data structure back into the free list.
//
static void _free_submitted_pushbuffer(
        UvmChannelManager *channelManager, UvmPushbuffer *pushbuffer)
{
    UVM_DBG_PRINT(
            "Free Pushbuffer: { begin: 0x%llx, length: 0x%llx }\n",
            pushbuffer->cpuBegin,
            pushbuffer->curOffset);

    ++pushbuffer->channel->trackingInfo.numFreeGpFifoEntries;

    list_del(&pushbuffer->pbSubmittedList);

    uvm_channel_manager_free_pushbuffer_structure(
            channelManager, pushbuffer);
}

void uvm_update_all_channel_progress_for_manager(
        UvmChannelManager *channelManager)
{
    UvmChannel *channel;
    list_for_each_entry(channel,
                        &channelManager->channelPool.channelListHead,
                        channelList)
    {
        uvm_update_channel_progress(channel);
    }
}

NV_STATUS uvm_get_pushbuffer(
        UvmChannelManager *channelManager, UvmPushbuffer **pushbuffer)
{
    NV_STATUS status;
    UvmRingbuffer *ringbuffer    = NULL;
    UvmChannel    *channel       = NULL;
    UvmPushbuffer *newPushbuffer = NULL;

    uvm_lock_channel_manager(channelManager);

    // Look for avalable ringbuffer.
    while (NV_OK !=
            uvm_channel_manager_get_ringbuffer(channelManager, &ringbuffer))
    {
        uvm_update_all_channel_progress_for_manager(channelManager);
        uvm_channel_manager_reclaim(channelManager);

        uvm_unlock_channel_manager(channelManager);
        uvm_lock_channel_manager(channelManager);
    }

    // Get a channel with spare GPFIFO entries.
    while (NV_OK !=
            uvm_channel_manager_get_channel(channelManager, &channel))
    {
        uvm_update_all_channel_progress_for_manager(channelManager);
        uvm_channel_manager_reclaim(channelManager);

        uvm_unlock_channel_manager(channelManager);
        uvm_lock_channel_manager(channelManager);
    }

    // Allocate a pushbuffer structure.
    status =
        uvm_channel_manager_alloc_pushbuffer_structure(
                channelManager, &newPushbuffer);
    if (status != NV_OK)
        goto cleanup;

    // Associate the pushbuffer with the ringbuffer.
    list_add_tail(&newPushbuffer->pbList, &ringbuffer->pbListHead);

    // Initialize pushbuffer structure.
    newPushbuffer->cpuBegin   = ringbuffer->cpuBegin + ringbuffer->curOffset;
    newPushbuffer->gpuBegin   = ringbuffer->gpuBegin + ringbuffer->curOffset;
    newPushbuffer->pbOffset   = newPushbuffer->cpuBegin;
    newPushbuffer->curOffset  = 0;
    // If we are running out of GPFIFO and if one of the ringbuffer was
    // empty just before the call to uvm_get_pushbuffer then we will call
    // uvm_channel_manager_reclaim while a pushbuffer is still not
    // submitted. The reclaim function must handle this correctly.
    // Set seqNum to Max NvU64 to ensure it is not considered as completed
    // before it gets submitted
    newPushbuffer->seqNum     = (NvU64)(~0ULL) ;
    newPushbuffer->gpFifoOffset = 0;
    newPushbuffer->ringbuffer = ringbuffer;
    newPushbuffer->channel    = channel;
    newPushbuffer->acquireSpaceRsvd = NV_FALSE;
    newPushbuffer->nRsvdAcquires = 0;

    *pushbuffer = newPushbuffer;

    uvm_unlock_channel_manager(channelManager);

    return NV_OK;

cleanup:

    if (newPushbuffer)
        uvm_channel_manager_free_pushbuffer_structure(
                channelManager, newPushbuffer);

    if (channel)
        uvm_channel_manager_put_channel(channelManager, channel);

    if (ringbuffer)
        uvm_channel_manager_put_ringbuffer(channelManager, ringbuffer);

    uvm_unlock_channel_manager(channelManager);

    return status;
}

void uvm_cancel_pushbuffer(
        UvmChannelManager *channelManager, UvmPushbuffer *pushbuffer)
{
    UvmChannel *channel;
    UvmRingbuffer *ringbuffer;

    uvm_lock_channel_manager(channelManager);

    channel    = pushbuffer->channel;
    ringbuffer = pushbuffer->ringbuffer;

    uvm_channel_manager_free_pushbuffer_structure(
            channelManager, pushbuffer);

    uvm_channel_manager_put_channel(channelManager, channel);

    uvm_channel_manager_put_ringbuffer(channelManager, ringbuffer);

    ++channel->trackingInfo.numFreeGpFifoEntries;

    uvm_unlock_channel_manager(channelManager);
}

// It is the client's responsibility to reserve space for acquires at the start
// of the pushBuffer.
void uvm_reserve_acquire_space(
                UvmChannelManager *channelManager, UvmPushbuffer *pushBuffer,
                UvmTracker *tracker)
{
    NvU64 numBytes;
    UVM_PANIC_ON(!tracker);
    // Check for double and "late" reserve.
    UVM_PANIC_ON(pushBuffer->acquireSpaceRsvd);
    UVM_PANIC_ON(pushBuffer->pbOffset != pushBuffer->cpuBegin);

    numBytes = pushBuffer->channel->acquireBytes * tracker->nUsedItems;
    UVM_PANIC_ON(numBytes > UVM_PUSHBUFFER_RESERVATION_SIZE);
    pushBuffer->pbOffset += numBytes;
    pushBuffer->curOffset += numBytes;
    pushBuffer->nRsvdAcquires = tracker->nUsedItems;
    pushBuffer->acquireSpaceRsvd = NV_TRUE;
}

NV_STATUS uvm_submit_pushbuffer(
                UvmChannelManager *channelManager, UvmPushbuffer *pushbuffer,
                UvmTracker *trackerToAcquire, UvmTrackerItem *newItem)
{
    UvmChannel *channel;
    UvmPushbuffer *oldestPushbuffer = NULL;
    UvmRingbuffer *ringbuffer;
    NvU64 gpfifoAdjust = 0;
    NvU64 nextGpFifoOffset = 0;
    NvU64 numBytes = 0;

    uvm_lock_channel_manager(channelManager);

    channel    = pushbuffer->channel;
    ringbuffer = pushbuffer->ringbuffer;

    nextGpFifoOffset = (channel->trackingInfo.curGpFifoOffset + 1) %
                       channel->channelInfo.numGpFifoEntries;

    if (!list_empty(&channel->pbSubmittedListHead))
    {
        oldestPushbuffer = list_entry(channel->pbSubmittedListHead.next,
                                      UvmPushbuffer,
                                      pbSubmittedList);
        // Ensure that GPGET will not be equal to GPPUT by comparing the offset
        // of the oldest pushbuffer with the next one
        UVM_PANIC_ON(oldestPushbuffer->gpFifoOffset == nextGpFifoOffset);
    }

    // Push acquires at the very beginning of the pushbuffer.
    // The below can happen if:
    //  1. The tracker got changed between reserve and submit.
    //  2. The client did not reserve space for acquires in the pushbuffer.
    UVM_PANIC_ON(trackerToAcquire &&
                 (trackerToAcquire->nUsedItems > pushbuffer->nRsvdAcquires));
    if (pushbuffer->nRsvdAcquires)
    {
        // For number of tracker items in tracker; push acquires in the
        // reserved area. If num of items is less than rsvd acquires;
        // place the GPFIFO ptr to skip extra space.
        NvU64 offset;
        UvmTrackerItem *trackerItem;
        size_t usedItems = trackerToAcquire ? trackerToAcquire->nUsedItems :
                                              0;
        gpfifoAdjust = (pushbuffer->nRsvdAcquires - usedItems) *
                        channel->acquireBytes;
        offset = pushbuffer->cpuBegin + gpfifoAdjust;

        if (usedItems)
        {
            // Now fill the space with valid acquires.
            list_for_each_entry(trackerItem, &trackerToAcquire->itemHead, list)
            {
                NvU64 targetSemaBase =
                        trackerItem->channel->pool->semaPool.semaGpuPointerBase;
                NvU64 targetSema =
                        trackerItem->channel->trackingInfo.semaGpuPointer;
                NvU64 semaOffset = targetSema - targetSemaBase;
                UVM_PANIC_ON(targetSema < targetSemaBase);
                // TODO: Bug 1766104: uvm8: delete uvmfull/ subdirectory before release
                // ...just delete this entire file, instead of the original to-do: which was:
                //
                // skip acquiring semaphores that have already finished
                // the values being tracked.
                numBytes = channel->ceOps.semaphoreAcquire_GEQ(
                        (unsigned **)&offset,
                        (unsigned*)(pushbuffer->cpuBegin +
                        (pushbuffer->nRsvdAcquires * channel->acquireBytes)),
                        channel->pool->semaPool.semaGpuPointerBase + semaOffset,
                        (unsigned)trackerItem->seqNum);
                UVM_PANIC_ON(!numBytes);
                // Check if this is the last used item.
                if (&trackerItem->list == trackerToAcquire->usedTail)
                    break;
            }
        }
    }

    // Push release value at the end of the user area.
    numBytes = channel->ceOps.semaphoreRelease(
                                (unsigned **)&pushbuffer->pbOffset,
                                (unsigned*)(pushbuffer->cpuBegin +
                                            UVM_PUSHBUFFER_RESERVATION_SIZE),
                                channel->trackingInfo.semaGpuPointer,
                                channel->trackingInfo.seqNumPending + 1);
    UVM_PANIC_ON(!numBytes);
    pushbuffer->curOffset += numBytes;

    // Write the GP entry to the adjusted gpfifo (gpfifoAdjust)
    channel->ceOps.writeGpEntry(channel->channelInfo.gpFifoEntries,
                                channel->trackingInfo.curGpFifoOffset,
                                pushbuffer->gpuBegin + gpfifoAdjust,
                                pushbuffer->curOffset - gpfifoAdjust);

    ++channel->trackingInfo.seqNumPending;
    pushbuffer->gpFifoOffset = channel->trackingInfo.curGpFifoOffset;
    pushbuffer->seqNum       = channel->trackingInfo.seqNumPending;

    // Update current GPFIFO offset and launch pushbuffer
    channel->trackingInfo.curGpFifoOffset = nextGpFifoOffset;
    channel->ceOps.queueWork(channel->channelInfo.GPPut, nextGpFifoOffset,
                             channel->channelInfo.workSubmissionOffset,
                             channel->channelInfo.workSubmissionToken);

    // Update the trackerItem with the released value.
    if (newItem)
    {
        newItem->seqNum = channel->trackingInfo.seqNumPending;
        newItem->channel = channel;
    }

    // Add the pushbuffer to the list of submitted pushbuffers for this channel
    list_add_tail(&pushbuffer->pbSubmittedList, &channel->pbSubmittedListHead);

    // Put ringbuffer back to free list.
    uvm_channel_manager_put_ringbuffer(channelManager, ringbuffer);

    // Put channel back to free list
    uvm_channel_manager_put_channel(channelManager, channel);

    uvm_unlock_channel_manager(channelManager);

    return NV_OK;
}

// Returns NV_TRUE if item has acquired the value.
// in any case; updates the channel state.
static inline NvBool uvm_query_channel_seq_done(UvmChannel *channel, NvU64 seq)
{
    if ((NvU64)NV_ATOMIC64_READ(channel->trackingInfo.seqNumDone) >= seq)
        return NV_TRUE;
    return (uvm_update_channel_progress(channel) >= seq);
}

// This functions checks whether all the valid tracker items in the given
// tracker have had their channels attain the acquire seq num.
// Returns
//      - NV_OK if all tracker items are done
//      - NV_WARN_MORE_PROCESSING_REQUIRED if items are pending
//      - NV_ERR_ECC_ERROR if an ECC error happened
//      - NV_ERR_RC_ERROR if a RC happened
NV_STATUS uvm_query_tracker(UvmTracker *tracker)
{
    UvmTrackerItem *item, *itemSafe;
    NvBool bEndOfUsed;

    if (tracker->nUsedItems == 0)
        return NV_OK;

    list_for_each_entry_safe(item, itemSafe, &tracker->itemHead, list)
    {
        bEndOfUsed = (&item->list == tracker->usedTail);
        // Query tracker item. Move it to free list if its done.
        if (uvm_query_channel_seq_done(item->channel, item->seqNum))
            uvm_retire_tracker_item(tracker, item);

        if (bEndOfUsed)
            break;
    }

    // TODO: Bug 1766104: uvm8: delete uvmfull/ subdirectory before release
    // ...just delete this entire file, instead of the original to-do: which was:
    //
    // 1. Check ECC error for all GPUs involved
    // 2. Check RC error

    if (tracker->nUsedItems)
        return NV_WARN_MORE_PROCESSING_REQUIRED;
    else
    {
        // TODO: Bug 1766104: uvm8: delete uvmfull/ subdirectory before release
        // ...just delete this entire file, instead of the original to-do: which was:
        //
        // Insert acquire memory barrier here.
    }

    return NV_OK;
}

// This call assumes that the tracker state is not being changed by anybody else
NV_STATUS uvm_wait_for_tracker(UvmTracker *tracker)
{
    NV_STATUS status;
    while (1)
    {
        status = uvm_query_tracker(tracker);
        if (status == NV_WARN_MORE_PROCESSING_REQUIRED)
        {
            cpu_relax();
        }
        else if (status == NV_OK)
        {
            return status;
        }
        else
        {
            // TODO: Bug 1766104: uvm8: delete uvmfull/ subdirectory before release
            // ...just delete this entire file, instead of the original to-do: which was:
            //
            // ECC/RC error happened. Call cleanup.
            return status;
        }
    }

    return NV_OK;
}

NvU64 uvm_update_channel_progress(UvmChannel *channel)
{
    NvU64 swSeqNum;
    NvU32 hwSeqNum;
    UvmChannelTracking *trackingInfo;

    UVM_PANIC_ON(!channel);
    trackingInfo = &channel->trackingInfo;

    spin_lock(&trackingInfo->lock);
    swSeqNum = NV_ATOMIC64_READ(trackingInfo->seqNumDone);
    hwSeqNum = UVM_READ_SEMA(trackingInfo->semaCpuPointer);

    if (hwSeqNum == (NvU32)swSeqNum)
        goto unlock;

    // check for wrap around case. Increment the upper.
    if (hwSeqNum < (NvU32)swSeqNum)
        swSeqNum += 0x100000000ull;

    // update lower
    swSeqNum &= 0xFFFFFFFF00000000ull;
    swSeqNum |= (NvU64)hwSeqNum;
    NV_ATOMIC64_SET(trackingInfo->seqNumDone, swSeqNum);

unlock:
    spin_unlock(&trackingInfo->lock);
    return swSeqNum;
}

void uvm_pushbuffer_inline_start(UvmPushbuffer *pb,
                                 UvmPbInlineRegion *region)
{
    // A pending region already out there.
    UVM_PANIC_ON(pb->bRegionPending);
    pb->bRegionPending = NV_TRUE;

    region->nopLocation = pb->pbOffset;
    pb->pbOffset       += 4;             // sizeof NOP
    region->regionStart = (void*)(pb->pbOffset);
}

void uvm_pushbuffer_inline_end(UvmPushbuffer *pb,
                               UvmPbInlineRegion *region)
{
    size_t      numBytes;
    size_t      nopPayloadSize;
    NvUPtr      pbNopOffset = region->nopLocation;

    UVM_PANIC_ON(pb->bRegionPending == NV_FALSE);

    nopPayloadSize = NV_ALIGN_UP(region->size, 4);

    numBytes = pb->channel->ceOps.insertNop((unsigned**)&pbNopOffset,
                    (unsigned*)(pb->cpuBegin + UVM_PUSHBUFFER_RESERVATION_SIZE),
                     nopPayloadSize/4);
    UVM_PANIC_ON(numBytes == 0);

    // Update the pushbuffer offsets with the final NOP size
    pb->pbOffset = pbNopOffset;
    UVM_PANIC_ON(pb->pbOffset < pb->cpuBegin);
    pb->curOffset = pb->pbOffset - pb->cpuBegin;

    // Close the region
    pb->bRegionPending = NV_FALSE;
}

size_t uvm_pushbuffer_copy_region(UvmPushbuffer *pb,
                                      UvmPbInlineRegion *region)
{
    size_t numBytes = 0;

    if (!pb->bRegionPending &&
        region->copy.bValid &&
        region->copy.copySize)
    {
        NvU64 srcGpuVirt;
        NvU32 flags = NV_UVM_COPY_SRC_TYPE_VIRTUAL;
        UVM_PANIC_ON((NvUPtr)region->regionStart < pb->cpuBegin);
        srcGpuVirt = ((NvUPtr)region->regionStart - pb->cpuBegin) +
                        pb->gpuBegin;

        numBytes = pb->channel->ceOps.launchDma(
                                       (unsigned **)&pb->pbOffset,
                                       (unsigned*)(pb->cpuBegin +
                                          UVM_PUSHBUFFER_RESERVATION_SIZE),
                                        srcGpuVirt,
                                        NV_UVM_COPY_SRC_LOCATION_SYSMEM,
                                        region->copy.dstAddr,
                                        region->copy.dstAperture,
                                        region->copy.copySize,
                                        region->copy.dstCopyFlags | flags);
        UVM_PANIC_ON(!numBytes);
        pb->curOffset += numBytes;
        return region->copy.copySize;
    }
    return 0;
}

void uvm_pushbuffer_inline_copy_region_start(UvmPushbuffer *pb,
                                             UvmPbInlineRegion *region)
{
    uvm_pushbuffer_inline_start(pb, region);
}

size_t uvm_pushbuffer_inline_copy_region_end(UvmPushbuffer *pb,
                                           UvmPbInlineRegion *region)
{
    uvm_pushbuffer_inline_end(pb, region);
    return uvm_pushbuffer_copy_region(pb, region);
}

