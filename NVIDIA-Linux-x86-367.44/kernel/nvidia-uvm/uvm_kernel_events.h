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

#ifndef UVM_COMMON_EVENTS_H_
#define UVM_COMMON_EVENTS_H_

#include "uvm_common.h"
#include "uvmtypes.h"

typedef struct UvmEventQueueInfoUserROData_tag
{
    // Write index into the Event Queue Buffer in units of UvmEventEntry's.
    NV_ATOMIC64 writeIndex;

    // Maximum number of events the Event Queue Buffer can hold.
    unsigned long long maxEventCapacity;
} UvmEventQueueInfoUserROData;

typedef struct UvmEventQueueInfoUserRWData_tag
{
    // Read index into the Event Queue Buffer in units of UvmEventEntry's.
    NV_ATOMIC64 readIndex;

    // Write index into the Event Queue Buffer in units of UvmEventEntry's.
    NV_ATOMIC64 writeIndex;
} UvmEventQueueInfoUserRWData;

typedef struct UvmEventContainer_tag
{
    //
    // A count of enabled events provides a fast way of detecting if events
    // are enabled. enabledEventsCount != 0 is a necessary but not sufficeint
    // indicator of events being enabled, since events could be enabled after
    // a read of this variable.
    //
    atomic_t enabledEventsCount;

    // This lock is used to protect eventListenerLists
    struct rw_semaphore eventListenerListLock;
    //
    // Each element of this array points to a list of UvmEventQueueInfo's that
    // have the particular event type enabled. An event queue may be on more
    // than one listener list if it has more than one event type enabled.
    //
    struct list_head eventListenerLists[UvmEventNumTypes];

    // The wait queue that client threads waiting on notifications are added to.
    wait_queue_head_t wait_queue;

    // Ref counter to know how many users have refenced this container
    atomic_t refcountUsers;
} UvmEventContainer;

typedef struct UvmEventQueueInfo_tag
{
    unsigned index;

    // Page pointing to data that's mapped RO into the client's process.
    struct page *pUserRODataPage;
    // Kernel virtual address of the page above.
    UvmEventQueueInfoUserROData *pUserROData;

    // Page pointing to data that's mapped RW into the client's process.
    struct page *pUserRWDataPage;
    // Kernel virtual address of the page above.
    UvmEventQueueInfoUserRWData *pUserRWData;

    // Number of pages in pBuffer
    NvLength numQueuePages;
    // Page descriptor table for the Event Queue Buffer.
    struct page **ppBufferPageList;
    // Kernel virtual address of the Event Queue Buffer.
    void *pBuffer;

    // Lock to maintain mutual exclusion among event writers.
    struct rw_semaphore eventQueueBufferLock;

    // Bitmask of enabled events.
    unsigned int enabledEventsBitmask;

    //
    // The minimum number of entries that should be in the Event Queue Buffer
    // before a notification is sent to the client.
    //
    unsigned int notificationCount;

    // Event Queue Info List node
    struct list_head eventQueueInfoListNode;

    //
    // Nodes that hook this UvmEventQueueInfo struct into the debuggee's
    // listener list for each event type.
    //
    struct list_head eventListenerListNode[UvmEventNumTypes];
} UvmEventQueueInfo;

struct UvmSessionInfo_tag;

// API Initialization functions
// Function used to initialize the event system. It must be called before
// calling any counter or event functions
NV_STATUS uvm_initialize_events_api(void);
void uvm_deinitialize_events_api(void);

// Struture initialization
void uvm_init_event_listener_list(UvmEventContainer *pEventContainer);

// Allocation and ref counting functions for event container
NV_STATUS uvm_alloc_event_container(UvmEventContainer **ppEventContainer);
void uvm_ref_event_container(UvmEventContainer *pEventContainer);
void uvm_unref_event_container(UvmEventContainer *pEventContainer);

// Allocation and removal functions for event queue 
NV_STATUS uvm_create_event_queue(struct UvmSessionInfo_tag *pSessionInfo,
                                 unsigned *pEventQueueIndex,
                                 NvLength queueSize,
                                 unsigned int notificationCount,
                                 UvmEventTimeStampType timeStampType);

NV_STATUS uvm_get_event_queue(struct UvmSessionInfo_tag *pSessionInfo,
                              UvmEventQueueInfo **ppEventQueueInfo,
                              NvUPtr eventQueueHandle);

void uvm_remove_event_queue(struct UvmSessionInfo_tag *pSessionInfo,
                            UvmEventQueueInfo *pEventQueueInfo);

NV_STATUS uvm_map_event_queue(UvmEventQueueInfo *pEventQueueInfo,
                              NvP64 userRODataAddr,
                              NvP64 userRWDataAddr,
                              NvP64 *pReadIndexAddr,
                              NvP64 *pWriteIndexAddr,
                              NvP64 *pQueueBufferAddr,
                              struct vm_area_struct *roVma,
                              struct vm_area_struct *rwVma,
                              struct file *filp);

// Enable/Disable events
NV_STATUS uvm_enable_event(UvmEventQueueInfo *pEventQueueInfo,
                           UvmEventType eventType,
                           UvmEventContainer *pEventContainer);

NV_STATUS uvm_disable_event(UvmEventQueueInfo *pEventQueueInfo,
                            UvmEventType eventType,
                            UvmEventContainer *pEventContainer);

NvBool uvm_is_event_enabled(UvmEventContainer *pEventContainer,
                            UvmEventType eventType);

// Events recording
NV_STATUS uvm_record_migration_event(UvmEventContainer *pEventContainer,
                                     NvU8 direction,
                                     NvU8 srcIndex,
                                     NvU8 dstIndex,
                                     NvU64 address,
                                     NvU64 migratedBytes,
                                     NvU64 beginTimeStamp,
                                     NvU64 endTimeStamp,
                                     NvU64 streamId);

NV_STATUS uvm_record_memory_violation_event(UvmEventContainer *pEventContainer,
                                            NvU8 accessType,
                                            NvU64 address,
                                            NvU64 timeStamp,
                                            NvU32 pid,
                                            NvU32 threadId);

NV_STATUS uvm_record_gpu_fault(UvmEventContainer *pEventContainer,
                               UvmEventFaultType faultType,
                               UvmEventMemoryAccessType accessType,
                               NvU64 address,
                               NvU64 timeStampCpu,
                               NvU64 timeStampGpu);

NV_STATUS uvm_record_gpu_fault_replay(UvmEventContainer *pEventContainer,
                                      NvU64 address,
                                      NvU64 timeStamp);

NV_STATUS uvm_record_fault_buffer_overflow_event(UvmEventContainer *pEventContainer,
                                                 NvU64 timeStamp);

NvBool uvm_any_event_notifications_pending(UvmEventContainer *pEventContainer);
#endif // UVM_COMMON_EVENTS_H_
