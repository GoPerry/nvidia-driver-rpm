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
#include "uvm_debug_session.h"

#include "uvm_ioctl.h"
#include "uvmtypes.h"

// Session index is initialized to an invalid value.
static const int UVM_SESSION_INDEX_INIT_VALUE   = -1;

void uvm_init_session_info(UvmSessionInfo *pSessionInfo)
{
    if (!pSessionInfo)
        return;

    pSessionInfo->pCounterContainer = NULL;
    pSessionInfo->pEventContainer = NULL;
    pSessionInfo->pidSessionOwner = UVM_PID_INIT_VALUE;
    pSessionInfo->pidTarget = UVM_PID_INIT_VALUE;
    pSessionInfo->euidTarget = UVM_ROOT_UID;
}

// Locking: you must hold SessionInfoLock for write before calling it.
NV_STATUS uvm_add_session_info(uid_t euidTarget,
                               NvU32 pidTarget,
                               int *pSessionIndex,
                               UvmCounterContainer *pCounterContainer,
                               UvmEventContainer *pEventContainer,
                               unsigned long mappedUserBaseAddress,
                               UvmSessionInfo *pSessionInfoArray)
{
    int sessionArrayIndex;
    UvmSessionInfo *pSessionInfo = NULL;

    if (!pSessionIndex || !pSessionInfoArray)
        return NV_ERR_INVALID_ARGUMENT;

    for(sessionArrayIndex = 0; sessionArrayIndex < UVM_MAX_SESSIONS_PER_PROCESS;
        sessionArrayIndex++)
    {
        if (pSessionInfoArray[sessionArrayIndex].
            pidSessionOwner ==  UVM_PID_INIT_VALUE)
        {
            pSessionInfo = &pSessionInfoArray[sessionArrayIndex];

            // save a pointer to the counter and event information of the target process
            pSessionInfo->pEventContainer = pEventContainer;
            pSessionInfo->pCounterContainer = pCounterContainer;
            // save the owner pid (used later for validating the session owner)
            pSessionInfo->pidSessionOwner = uvm_get_stale_process_id();
            pSessionInfo->euidTarget = euidTarget;
            pSessionInfo->pidTarget = pidTarget;
            pSessionInfo->mappedUserBaseAddress = mappedUserBaseAddress;

            // initialize the UvmEventQueueInfo list
            INIT_LIST_HEAD(&pSessionInfo->eventQueueInfoList);
            init_rwsem(&pSessionInfo->eventQueueInfoListLock);
            pSessionInfo->nextEventQueueInfoIndex = 0;

            *pSessionIndex = sessionArrayIndex;
            break;
        }
    }

    if (!pSessionInfo)
        return NV_ERR_INSUFFICIENT_RESOURCES;

    return NV_OK;
}

// Locking: you must hold SessionInfoLock for write before calling it.
NV_STATUS uvm_remove_session_info
(
    int sessionIndex,
    UvmSessionInfo *pSessionInfoArray
)
{
    UvmSessionInfo *pSessionInfo;
    NV_STATUS status;

    status = uvm_get_session_info(sessionIndex,
                                  pSessionInfoArray,
                                  &pSessionInfo);
    if (status == NV_OK)
        uvm_init_session_info(pSessionInfo);

    return status;
}

// Locking: you must hold SessionInfoLock for read before calling this routine.
NV_STATUS uvm_get_session_info(int sessionIndex,
                               UvmSessionInfo *pSessionInfoArray,
                               UvmSessionInfo **ppSessionInfo)
{
    if (!ppSessionInfo || !pSessionInfoArray)
        return NV_ERR_INVALID_ARGUMENT;

    // check if session index is valid
    if((sessionIndex > UVM_SESSION_INDEX_INIT_VALUE) &&
      (sessionIndex < UVM_MAX_SESSIONS_PER_PROCESS))
    {
        unsigned pidCurrent = uvm_get_stale_process_id();

        // check if the current process id is the session owner
        if (pSessionInfoArray[sessionIndex].pidSessionOwner != pidCurrent)
        {
            return NV_ERR_INSUFFICIENT_PERMISSIONS;
        }

        *ppSessionInfo = &pSessionInfoArray[sessionIndex];

        return NV_OK;
    }

    return NV_ERR_INVALID_ARGUMENT;
}

// This function returns the offset at which the counter value is present
static NV_STATUS _uvm_get_counter_offset(unsigned long pUserMappedCounterPage,
                                         UvmCounterName counterName,
                                         NvUPtr* pAddr)
{
    NV_STATUS status;
    unsigned counterIndex;

    // Check if the counter name is valid and get its index
    status = uvm_get_counter_index(counterName, &counterIndex);

    if(status == NV_OK)
        *pAddr = pUserMappedCounterPage + counterIndex * UVM_COUNTER_SIZE;
    return status;
}

//
// Picks user VA for a given counter and returns offset of that counter
// Locking: you must hold SessionInfoLock for read before calling this routine.
//
NV_STATUS uvm_map_counter(UvmSessionInfo *pSessionInfo,
                          UvmCounterScope scope,
                          UvmCounterName counterName,
                          unsigned gpuIndex,
                          NvUPtr* pAddr)
{
    unsigned long pUserMappedCounterPage;

    switch (scope)
    {
        case UvmCounterScopeProcessSingleGpu:
            pUserMappedCounterPage = pSessionInfo->mappedUserBaseAddress + 
                                     gpuIndex * UVM_PER_RESOURCE_COUNTERS_SIZE +
                                     UVM_PER_PROCESS_PER_GPU_COUNTERS_SHIFT;
            break;

        case UvmCounterScopeProcessAllGpu:
            pUserMappedCounterPage = pSessionInfo->mappedUserBaseAddress;
            break;

        case UvmCounterScopeGlobalSingleGpu:
            return NV_ERR_NOT_SUPPORTED;
        default:
            return NV_ERR_INVALID_ARGUMENT;
    }

    return _uvm_get_counter_offset(pUserMappedCounterPage, counterName, pAddr);
}

static NV_STATUS _uvm_increment_session_count(UvmCounterInfo *pCtrInfo,
                                              UvmCounterName counterName)
{
    unsigned counterIndex;
    NV_STATUS status;

    status = uvm_get_counter_index(counterName, &counterIndex);
    if(status == NV_OK)
        NV_ATOMIC_INC(pCtrInfo->sessionCount[counterIndex]);

    return status;
}

static NV_STATUS _uvm_decrement_session_count(UvmCounterInfo *pCtrInfo,
                                              UvmCounterName counterName)
{
    unsigned counterIndex = 0;
    NV_STATUS status = NV_OK;

    status = uvm_get_counter_index(counterName, &counterIndex);
    if(status == NV_OK)
        NV_ATOMIC_DEC(pCtrInfo->sessionCount[counterIndex]);

    return status;
}

// Enables counters specified by the session and config structure.
// Locking: you must hold SessionInfoLock for read before calling this routine.
NV_STATUS uvm_counter_state_atomic_update(UvmSessionInfo *pSessionInfo,
                                          const UvmCounterConfig *config,
                                          unsigned count)
{
    const UvmCounterConfig *currConfig;
    unsigned counterNum;
    NV_STATUS status = NV_OK;
    UvmCounterContainer *pCounterContainer;

    if (count > UVM_MAX_COUNTERS_PER_IOCTL_CALL)
        return NV_ERR_INVALID_ARGUMENT;

    pCounterContainer = pSessionInfo->pCounterContainer;

    for (counterNum = 0; counterNum < count; counterNum++)
    {
        currConfig = &config[counterNum];

        switch (currConfig->scope)
        {
            case UvmCounterScopeProcessSingleGpu:
                // These are enabled by default and cannot be disabled
                break;

            case UvmCounterScopeProcessAllGpu:
                if(currConfig->state == 
                UVM_COUNTER_CONFIG_STATE_ENABLE_REQUESTED)
                    status = _uvm_increment_session_count(
                        &pCounterContainer->allGpuCounter,
                        currConfig->name);
                else
                    status = _uvm_decrement_session_count(
                        &pCounterContainer->allGpuCounter,
                        currConfig->name);
                break;

            case UvmCounterScopeGlobalSingleGpu:
                return NV_ERR_NOT_SUPPORTED;
            default:
                return NV_ERR_INVALID_ARGUMENT;
        }
    }

    return status;
}
