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

#include "uvm_common.h"
#include "uvm_kernel_events.h"
#include "uvm_debug_session.h"

#include "uvmtypes.h"
#include "uvm_events.h"

static struct kmem_cache *g_uvmEventContainerCache    __read_mostly = NULL;

// API Initialization
NV_STATUS uvm_initialize_events_api(void)
{
    NV_STATUS status = NV_OK;

    g_uvmEventContainerCache = NULL;

    UVM_DBG_PRINT_RL("Init event API\n");

    g_uvmEventContainerCache = NV_KMEM_CACHE_CREATE("uvm_event_container_t",
                                                    struct UvmEventContainer_tag);
    if (!g_uvmEventContainerCache)
    {
        status = NV_ERR_NO_MEMORY;
        goto fail;
    }

    return NV_OK;

fail:
    kmem_cache_destroy_safe(&g_uvmEventContainerCache);
    return status;
}

void uvm_deinitialize_events_api(void)
{
    kmem_cache_destroy(g_uvmEventContainerCache);
}

// The following functions deal with events from the UVM-lite driver
// to usermode clients.
//
void uvm_init_event_listener_list(UvmEventContainer *pEventContainer)
{
    unsigned i;

    init_rwsem(&pEventContainer->eventListenerListLock);

    for (i = 0; i < UvmEventNumTypes; i++)
        INIT_LIST_HEAD(&pEventContainer->eventListenerLists[i]);

    init_waitqueue_head(&pEventContainer->wait_queue);
}

static void _uvm_free_event_container(UvmEventContainer *pEventContainer)
{
    if (!pEventContainer)
        return;

    kmem_cache_free(g_uvmEventContainerCache, pEventContainer);
}

NV_STATUS uvm_alloc_event_container(UvmEventContainer **ppEventContainer)
{
    if (!ppEventContainer)
        return NV_ERR_INVALID_ARGUMENT;

    *ppEventContainer = (UvmEventContainer *)
            kmem_cache_zalloc(g_uvmEventContainerCache, NV_UVM_GFP_FLAGS);

    if (!*ppEventContainer)
        return NV_ERR_NO_MEMORY;

    NV_ATOMIC_SET((*ppEventContainer)->refcountUsers, 1);

    uvm_init_event_listener_list(*ppEventContainer);

    return NV_OK;
}

void uvm_ref_event_container(UvmEventContainer *pEventContainer)
{
    if (!pEventContainer)
        return;
    NV_ATOMIC_INC(pEventContainer->refcountUsers);
}

void uvm_unref_event_container(UvmEventContainer *pEventContainer)
{
    if (!pEventContainer)
        return;

    if (NV_ATOMIC_DEC_AND_TEST(pEventContainer->refcountUsers))
        _uvm_free_event_container(pEventContainer);
}

//
// This function must be called with a write lock on
// pSessionInfo->eventQueueInfoListLock
//
NV_STATUS
uvm_create_event_queue(UvmSessionInfo *pSessionInfo,
                       unsigned *pEventQueueIndex,
                       NvLength queueSize,
                       unsigned int notificationCount,
                       UvmEventTimeStampType timeStampType)
{
    NV_STATUS rmStatus = NV_OK;
    UvmEventQueueInfo *pEventQueueInfo;
    const unsigned entrySize = sizeof(UvmEventEntry);
    NvLength i, numQueuePages = PAGE_ALIGN(queueSize * entrySize) >> PAGE_SHIFT;

    pEventQueueInfo = vmalloc(sizeof(UvmEventQueueInfo));
    if (pEventQueueInfo == NULL)
    {
        UVM_ERR_PRINT("failed to allocate memory for UvmEventQueueInfo\n");
        rmStatus = NV_ERR_NO_MEMORY;
        goto done;
    }

    memset(pEventQueueInfo, 0, sizeof (*pEventQueueInfo));

    INIT_LIST_HEAD(&pEventQueueInfo->eventQueueInfoListNode);
    for (i = 0; i < UvmEventNumTypes; ++i)
        INIT_LIST_HEAD(&pEventQueueInfo->eventListenerListNode[i]);

    pEventQueueInfo->index = pSessionInfo->nextEventQueueInfoIndex++;
    pEventQueueInfo->enabledEventsBitmask = 0;
    pEventQueueInfo->notificationCount = notificationCount;
    pEventQueueInfo->numQueuePages = numQueuePages;

    // Alocate memory for the page that will be mapped RO into the client
    pEventQueueInfo->pUserRODataPage = alloc_page(NV_UVM_GFP_FLAGS |
                                                  GFP_HIGHUSER);

    if (pEventQueueInfo->pUserRODataPage == NULL)
    {
        UVM_ERR_PRINT("failed to allocate page for pUserRODataPage\n");
        rmStatus = NV_ERR_NO_MEMORY;
        goto done;
    }

    pEventQueueInfo->pUserROData = kmap(pEventQueueInfo->pUserRODataPage);
    if (pEventQueueInfo->pUserROData == NULL)
    {
        UVM_ERR_PRINT("failed to map page for pUserROData\n");
        rmStatus = NV_ERR_INSUFFICIENT_RESOURCES;
        goto done;
    }

    memset(pEventQueueInfo->pUserROData, 0, PAGE_SIZE);

    NV_ATOMIC64_SET(pEventQueueInfo->pUserROData->writeIndex, 0);
    pEventQueueInfo->pUserROData->maxEventCapacity = queueSize;

    // Alocate memory for the page that will be mapped RW into the client
    pEventQueueInfo->pUserRWDataPage = alloc_page(NV_UVM_GFP_FLAGS |
                                                  GFP_HIGHUSER);

    if (pEventQueueInfo->pUserRODataPage == NULL)
    {
        UVM_ERR_PRINT("failed to allocate page for pUserRWDataPage\n");
        rmStatus = NV_ERR_NO_MEMORY;
        goto done;
    }

    pEventQueueInfo->pUserRWData = kmap(pEventQueueInfo->pUserRWDataPage);
    if (pEventQueueInfo->pUserRWData == NULL)
    {
        UVM_ERR_PRINT("failed to map page for pUserRWData\n");
        rmStatus = NV_ERR_INSUFFICIENT_RESOURCES;
        goto done;
    }

    memset(pEventQueueInfo->pUserRWData, 0, PAGE_SIZE);

    NV_ATOMIC64_SET(pEventQueueInfo->pUserRWData->readIndex, 0);
    NV_ATOMIC64_SET(pEventQueueInfo->pUserRWData->writeIndex, 0);

    // Alocate memory for the list of event queue buffer pages
    pEventQueueInfo->ppBufferPageList = vmalloc(numQueuePages *
                                                         sizeof(struct page *));
    if (pEventQueueInfo->ppBufferPageList == NULL)
    {
        UVM_ERR_PRINT("failed to allocate page for ppBufferPageList\n");
        rmStatus = NV_ERR_NO_MEMORY;
        goto done;
    }

    // Allocate the page descriptor table for the event queue buffer
    for (i = 0; i < numQueuePages; i++)
    {
        pEventQueueInfo->ppBufferPageList[i] =
            alloc_page(NV_UVM_GFP_FLAGS | GFP_HIGHUSER);
        if (pEventQueueInfo->ppBufferPageList[i] == NULL)
        {
            UVM_ERR_PRINT("failed to allocate page for ppBufferPageList\n");
            rmStatus = NV_ERR_NO_MEMORY;
            goto done;
        }
    }

    //
    // Map this buffer into kernel VA space
    //
    // TODO: Bug 1766104: uvm8: delete uvmfull/ subdirectory before release
    // TODO: Bug 1766109: uvm8: delete UVM-Lite files and remove -lite mode
    //  ...just delete this entire file, instead of the original to-do: which was:
    //
    // Double-check these flags
    //
    pEventQueueInfo->pBuffer = vmap(pEventQueueInfo->ppBufferPageList,
                                    numQueuePages,
                                    VM_READ | VM_WRITE,
                                    PAGE_KERNEL);
    if (pEventQueueInfo->pBuffer == NULL)
    {
        UVM_ERR_PRINT("failed to map pBuffer\n");
        rmStatus = NV_ERR_NO_MEMORY;
        goto done;
    }

    // Initialize the lock that will protect the event queue buffer
    init_rwsem(&pEventQueueInfo->eventQueueBufferLock);

    // Add the new UvmEventQueueInfo to pSessionInfo's list.
    list_add_tail(&pEventQueueInfo->eventQueueInfoListNode,
                  &pSessionInfo->eventQueueInfoList);

    //
    // Initialize hooks with which this UvmEventQueueInfo connects to
    // the Event Listener Lists of the debuggee.
    //
    for (i = 0; i < UvmEventNumTypes; i++)
    {
        INIT_LIST_HEAD(&pEventQueueInfo->eventListenerListNode[i]);
    }

    *pEventQueueIndex = pEventQueueInfo->index;

done:
    if (rmStatus != NV_OK)
    {
        uvm_remove_event_queue(pSessionInfo, pEventQueueInfo);
    }

    return rmStatus;
}

//
// This function must be called with a lock on
// pSessionInfo->eventQueueInfoListLock
//
NV_STATUS
uvm_get_event_queue(UvmSessionInfo *pSessionInfo,
                    UvmEventQueueInfo **ppEventQueueInfo,
                    NvUPtr eventQueueHandle)
{
    struct list_head *ptr;
    UvmEventQueueInfo *entry; 

    list_for_each(ptr, &pSessionInfo->eventQueueInfoList)
    {
        entry = list_entry(ptr, UvmEventQueueInfo, eventQueueInfoListNode);
        if (eventQueueHandle == entry->index)
        {
            *ppEventQueueInfo = entry;
            return NV_OK;
        }
    }

    // Not found in the list
    *ppEventQueueInfo = NULL;
    return NV_ERR_INVALID_ARGUMENT;
}

//
// Free as much of the event queue as was allocated
// This function must be called with a read lock on
// g_uvmDriverPrivateTableLock and a write lock on
// pSessionInfo->eventQueueInfoListLock.
//
void uvm_remove_event_queue(UvmSessionInfo *pSessionInfo,
                            UvmEventQueueInfo *pEventQueueInfo)
{
    NV_STATUS rmStatus;
    NvLength i, numQueuePages;
    UvmEventType eventType = 0;

    if (!pEventQueueInfo || !pEventQueueInfo->pUserROData)
    {
        goto done;
    }

    // Remove all enabled events
    while (pEventQueueInfo->enabledEventsBitmask != 0)
    {
        if (((1 << eventType) & pEventQueueInfo->enabledEventsBitmask) != 0)
        {
            rmStatus = uvm_disable_event(pEventQueueInfo,
                                         eventType,
                                         pSessionInfo->pEventContainer);
            if (rmStatus != NV_OK)
                goto done;
        }

        eventType++;
    }

    numQueuePages = pEventQueueInfo->numQueuePages;

    kunmap(pEventQueueInfo->pUserRODataPage);
    __free_page(pEventQueueInfo->pUserRODataPage);

    if (!pEventQueueInfo->pUserRWData)
    {
        goto done;
    }

    kunmap(pEventQueueInfo->pUserRWDataPage);
    __free_page(pEventQueueInfo->pUserRWDataPage);

    vunmap(pEventQueueInfo->pBuffer);

    // Free the pages in the page descriptor table
    for (i = 0; i < numQueuePages; i++)
    {
        __free_page(pEventQueueInfo->ppBufferPageList[i]);
    }

    vfree(pEventQueueInfo->ppBufferPageList);

    // Remove the UvmEventQueueInfo from pSessionInfo's list.
    list_del_init(&pEventQueueInfo->eventQueueInfoListNode);

    vfree(pEventQueueInfo);

done:

    return;
}

// This function must be called with a write lock on mmap_sem.
NV_STATUS uvm_map_event_queue(UvmEventQueueInfo *pEventQueueInfo,
                              NvP64 userRODataAddr,
                              NvP64 userRWDataAddr,
                              NvP64 *pReadIndexAddr,
                              NvP64 *pWriteIndexAddr,
                              NvP64 *pQueueBufferAddr,
                              struct vm_area_struct *roVma,
                              struct vm_area_struct *rwVma,
                              struct file *filp)
{
    NV_STATUS rmStatus;
    struct page *pPage;
    NvLength i, numQueuePages = pEventQueueInfo->numQueuePages;
    NvP64 userAddress;

    // Map the RW page into userspace
    pPage = pEventQueueInfo->pUserRWDataPage,
    rmStatus = uvm_map_page(rwVma, pPage, (NvUPtr)userRWDataAddr);
    if (rmStatus != NV_OK)
    {
        goto done;
    }

    // Map the RO page into userspace
    pPage = pEventQueueInfo->pUserRODataPage;
    rmStatus = uvm_map_page(roVma, pPage, (NvUPtr)userRODataAddr);
    if (rmStatus != NV_OK)
    {
        goto done;
    }

    // Map the event queue buffer into userspace
    userAddress = userRODataAddr + PAGE_SIZE;
    for (i = 0; i < numQueuePages; i++)
    {
        pPage = pEventQueueInfo->ppBufferPageList[i];
        rmStatus = uvm_map_page(roVma, pPage, (NvUPtr)userAddress);
        if (rmStatus != NV_OK)
        {
            goto done;
        }

        userAddress += PAGE_SIZE;
    }

    *pReadIndexAddr = userRWDataAddr + offsetof(UvmEventQueueInfoUserRWData,
                                                readIndex);
    *pWriteIndexAddr = userRWDataAddr + offsetof(UvmEventQueueInfoUserRWData,
                                                 writeIndex);
    *pQueueBufferAddr = userRODataAddr + PAGE_SIZE;

done:
    return rmStatus;
}

//
// This function must be called with a read lock on
// g_uvmDriverPrivateTableLock and a write lock on
// pSessionInfo->eventQueueInfoListLock.
//
NV_STATUS uvm_enable_event(UvmEventQueueInfo *pEventQueueInfo,
                           UvmEventType eventType,
                           UvmEventContainer *pEventContainer)
{
    // Nothing to do if the event is already enabled.
    if ((pEventQueueInfo->enabledEventsBitmask & (1 << eventType)) != 0)
        return NV_OK;

    // Add this UvmEventQueueInfo structure to the debuggee's
    // eventListenerList
    down_write(&pEventContainer->eventListenerListLock);

    list_add_tail(&pEventQueueInfo->eventListenerListNode[eventType],
                  &pEventContainer->eventListenerLists[eventType]);

    up_write(&pEventContainer->eventListenerListLock);

    // Increment the enabled events count
    atomic_inc(&pEventContainer->enabledEventsCount);

    // Finally, mark the event as enabled
    pEventQueueInfo->enabledEventsBitmask |= (1 << eventType);

    return NV_OK;
}

//
// This function must be called with a read lock on
// g_uvmDriverPrivateTableLock and a write lock on
// pSessionInfo->eventQueueInfoListLock.
//
NV_STATUS uvm_disable_event(UvmEventQueueInfo *pEventQueueInfo,
                            UvmEventType eventType,
                            UvmEventContainer *pEventContainer)
{
    // Nothing to do if the event is already disabled.
    if ((pEventQueueInfo->enabledEventsBitmask & (1 << eventType)) == 0)
        return NV_OK;

    // Remove this UvmEventQueueInfo structure from the debuggee's
    // eventListenerList
    down_write(&pEventContainer->eventListenerListLock);

    list_del_init(&pEventQueueInfo->eventListenerListNode[eventType]);

    up_write(&pEventContainer->eventListenerListLock);

    // Decrement the enabled events count
    atomic_dec(&pEventContainer->enabledEventsCount);

    // Finally, mark the event as disabled 
    pEventQueueInfo->enabledEventsBitmask &= ~(1 << eventType);

    return NV_OK;
}

NvBool uvm_is_event_enabled(UvmEventContainer *pEventContainer,
                            UvmEventType eventType)
{
    NvBool bEnabled = NV_TRUE;

    down_read(&pEventContainer->eventListenerListLock);

    bEnabled = !(list_empty(&pEventContainer->eventListenerLists[eventType]));

    up_read(&pEventContainer->eventListenerListLock);

    return bEnabled;
}

// Events recording

static NV_STATUS _uvm_record_event(UvmEventContainer *pEventContainer,
                                   NvU8 eventType,
                                   void* pSrcEvent,
                                   NvU64 eventStructSize)
{
    NV_STATUS status = NV_OK;
    struct list_head *ptr;
    UvmEventQueueInfo *entry;
    struct list_head *eventsList;
    void *pDstEvent;
    NvU64 queueWriteIndex, queueSize;

    down_read(&pEventContainer->eventListenerListLock);
    eventsList = &pEventContainer->eventListenerLists[eventType];

    if (list_empty(eventsList))
    {
        up_read(&pEventContainer->eventListenerListLock);
        return status;
    }

    list_for_each(ptr, eventsList)
    {
        entry = list_entry(ptr,
                           UvmEventQueueInfo,
                           eventListenerListNode[eventType]);

        down_write(&entry->eventQueueBufferLock);

        queueSize = entry->pUserROData->maxEventCapacity;
        (void)NV_DIV64(NV_ATOMIC64_READ(entry->pUserROData->writeIndex),
                       queueSize,
                       &queueWriteIndex);
        pDstEvent = (void*)((UvmEventEntry*)entry->pBuffer + queueWriteIndex);
        memcpy(pDstEvent, pSrcEvent, eventStructSize);

        // Make prior writes visible to all CPUs
        smp_wmb();

        //
        // Atomically increment both the kernel and the user's copy of
        // the write counter
        //
        NV_ATOMIC64_INC(entry->pUserROData->writeIndex);
        NV_ATOMIC64_INC(entry->pUserRWData->writeIndex);

        // Signal the wait queue if enough events are available
        if (NV_ATOMIC64_READ(entry->pUserROData->writeIndex) -
            NV_ATOMIC64_READ(entry->pUserRWData->readIndex) >
            entry->notificationCount)
            wake_up_interruptible_all(&pEventContainer->wait_queue);

        up_write(&entry->eventQueueBufferLock);
    }

    up_read(&pEventContainer->eventListenerListLock);

    return status;
}

// Locking: The caller must hold a read lock on the struct holding the container
NV_STATUS uvm_record_memory_violation_event(UvmEventContainer *pEventContainer,
                                            NvU8 accessType,
                                            NvU64 address,
                                            NvU64 timeStamp,
                                            NvU32 pid,
                                            NvU32 threadId)
{
    UvmEventCpuFaultInfo violationInfo;

    UVM_DBG_PRINT_RL("Event: Memory Violation\n");

    memset(&violationInfo, 0, sizeof(violationInfo));
    violationInfo.eventType = UvmEventTypeCpuFault;
    violationInfo.accessType = accessType;
    violationInfo.address = address;
    violationInfo.timeStamp = timeStamp;
    violationInfo.pid = pid;
    violationInfo.threadId = threadId;
    violationInfo.threadId = threadId;

    return _uvm_record_event(pEventContainer,
                             UvmEventTypeCpuFault,
                             &violationInfo,
                             sizeof(violationInfo));

}

// Locking: The caller must hold a read lock on the struct holding the container
NV_STATUS uvm_record_migration_event(UvmEventContainer *pEventContainer,
                                     NvU8 direction,
                                     NvU8 srcIndex,
                                     NvU8 dstIndex,
                                     NvU64 address,
                                     NvU64 migratedBytes,
                                     NvU64 beginTimeStamp,
                                     NvU64 endTimeStamp,
                                     NvU64 streamId)
{
    UvmEventMigrationInfo migInfo;

    UVM_DBG_PRINT_RL("Event: Migration\n");

    memset(&migInfo, 0, sizeof(migInfo));
    migInfo.eventType = UvmEventTypeMigration;
    migInfo.direction = direction;
    migInfo.srcIndex = srcIndex;
    migInfo.dstIndex = dstIndex;
    migInfo.address = address;
    migInfo.migratedBytes = migratedBytes;
    migInfo.beginTimeStamp = beginTimeStamp;
    migInfo.endTimeStamp = endTimeStamp;
    migInfo.rangeGroupId = streamId;

    return _uvm_record_event(pEventContainer,
                             UvmEventTypeMigration,
                             &migInfo,
                             sizeof(UvmEventMigrationInfo));
}

// Locking: The caller must hold a read lock on the struct holding the container
NV_STATUS uvm_record_gpu_fault(UvmEventContainer *pEventContainer,
                               UvmEventFaultType faultType,
                               UvmEventMemoryAccessType accessType,
                               NvU64 address,
                               NvU64 timestampCpu,
                               NvU64 timestampGpu)
{
    UvmEventGpuFaultInfo faultInfo;

    UVM_DBG_PRINT_RL("Event: Gpu Fault\n");

    memset(&faultInfo, 0, sizeof(faultInfo));
    faultInfo.eventType = UvmEventTypeGpuFault;
    faultInfo.faultType = (NvU8)faultType;
    faultInfo.accessType = (NvU8)accessType;
    faultInfo.address = address;
    faultInfo.timeStamp    = timestampCpu;
    faultInfo.timeStampGpu = timestampGpu;


    return _uvm_record_event(pEventContainer,
                             UvmEventTypeGpuFault,
                             &faultInfo,
                             sizeof(UvmEventGpuFaultInfo));
}

// Locking: The caller must hold a read lock on the struct holding the container
NV_STATUS uvm_record_gpu_fault_replay(UvmEventContainer *pEventContainer,
                                      NvU64 address,
                                      NvU64 timestamp)
{
    UvmEventGpuFaultReplayInfo faultInfo;

    UVM_DBG_PRINT_RL("Event: Gpu Fault\n");

    memset(&faultInfo, 0, sizeof(faultInfo));
    faultInfo.eventType = UvmEventTypeGpuFaultReplay;
    faultInfo.timeStamp = timestamp;

    return _uvm_record_event(pEventContainer,
                             UvmEventTypeGpuFaultReplay,
                             &faultInfo,
                             sizeof(UvmEventGpuFaultReplayInfo));
}

// Locking: The caller must hold a read lock on the struct holding the container
NvBool uvm_any_event_notifications_pending(UvmEventContainer *pEventContainer)
{
    unsigned i;
    struct list_head *eventList, *ptr;
    UvmEventQueueInfo *entry; 

    UVM_DBG_PRINT_RL("begin\n");
    down_read(&pEventContainer->eventListenerListLock);

    for (i = 0; i < UvmEventNumTypes; i++)
    {
        eventList = &pEventContainer->eventListenerLists[i];

        list_for_each(ptr, eventList)
        {
            entry = list_entry(ptr, UvmEventQueueInfo, eventListenerListNode[i]);

            if (NV_ATOMIC64_READ(entry->pUserROData->writeIndex) -
                NV_ATOMIC64_READ(entry->pUserRWData->readIndex) >
                entry->notificationCount)
            {
                up_read(&pEventContainer->eventListenerListLock);
                UVM_DBG_PRINT_RL("notification pending\n");
                return NV_TRUE;
            }
        }
    }

    up_read(&pEventContainer->eventListenerListLock);
    UVM_DBG_PRINT_RL("no notification pending\n");

    return NV_FALSE;
}
