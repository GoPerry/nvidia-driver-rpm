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

#ifndef UVM_COMMON_DEBUG_SESSION_H_
#define UVM_COMMON_DEBUG_SESSION_H_

#include "uvm_common.h"
#include "uvm_kernel_events.h"
#include "uvm_kernel_counters.h"

#define UVM_MAX_SESSIONS_PER_PROCESS (64)
#define UVM_PID_INIT_VALUE (0)

//
// This structure saves per debug session information.
//
typedef struct UvmSessionInfo_tag
{
    // Store the owner of the debugging session. Usually this should be the pid
    // of the debugger
    unsigned pidSessionOwner;
    // effective user id of referenced processRecord
    // stored for checking privilages
    uid_t euidTarget;
    // the pid of the process being debugged, used to search vaspace of debugee
    unsigned pidTarget;
    //
    // user base address for mapping counter pages.
    // First page length address contains per process all gpu counters,
    // others (per process per gpu counters page) are shifted by 1.
    //
    unsigned long mappedUserBaseAddress;

    struct UvmEventContainer_tag *pEventContainer;
    struct UvmCounterContainer_tag *pCounterContainer;

    // Index to be assigned to the next UvmEventQueueInfo created
    unsigned nextEventQueueInfoIndex;

    // List of UvmEventQueueInfo structures, one for each Event Queue.
    struct list_head eventQueueInfoList;

    // This lock is used to protect eventQueueInfoList.
    struct rw_semaphore eventQueueInfoListLock;
} UvmSessionInfo;

// Structure initialization
void uvm_init_session_info(UvmSessionInfo *pSessionInfo);

// Add/Remove debug session in a session array
NV_STATUS uvm_add_session_info(uid_t euidTarget,
                               unsigned pidTarget,
                               int *pSessionIndex,
                               UvmCounterContainer *pTargetCounterContainer,
                               UvmEventContainer *pTargetEventContainer,
                               unsigned long mappedUserBaseAddress,
                               UvmSessionInfo *pSessionInfoArray);

NV_STATUS uvm_remove_session_info(int sessionIndex,
                                  UvmSessionInfo *pSessionInfoArray);

// Get a debug session stored in a session array
NV_STATUS uvm_get_session_info(int sessionIndex,
                               UvmSessionInfo *pSessionInfoArray,
                               UvmSessionInfo **ppSessionInfo);

// Picks user VA for a given counter and returns offset of that counter
NV_STATUS uvm_map_counter(UvmSessionInfo *pSessionInfo,
                          UvmCounterScope scope,
                          UvmCounterName counterName,
                          unsigned gpuIndex,
                          NvUPtr *pAddr);

// Enables counters specified by the session and config structure
NV_STATUS uvm_counter_state_atomic_update(UvmSessionInfo *pSessionInfo,
                                          const UvmCounterConfig *config,
                                          unsigned count);

#endif // UVM_COMMON_DEBUG_SESSION_H_
