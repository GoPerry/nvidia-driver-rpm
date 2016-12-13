/*******************************************************************************
    Copyright (c) 2013, 2014 NVIDIA Corporation

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

#include "uvm_ioctl.h"
#include "uvm_common.h"
#include "uvm_lite.h"
#include "uvm_kernel_events.h"
#include "uvm_kernel_counters.h"
#include "uvm_debug_session.h"
#include "uvm_lite_region_tracking.h"
#include "uvm_gpu_ops_tests.h"
#include "uvm_unit_test.h"

extern struct rw_semaphore g_uvmDriverPrivateTableLock;

//
// uvm_lite_api.c
//
// This file contains code for UVM API calls that are issued via ioctl()
// call.
//

//
// Locking: you must hold a read / write lock on the mmap_sem.
//
static struct vm_area_struct *
find_common_vma(unsigned long long requestedBase,
                unsigned long long length,
                struct file * filp)
{
    struct vm_area_struct *vma = find_vma(current->mm, requestedBase);
    if (vma == NULL)
        return NULL;
    if (vma->vm_file != filp)
        return NULL;
    if (vma->vm_start > requestedBase)
        return NULL;
    if (requestedBase  + PAGE_ALIGN(length) > vma->vm_end)
        return NULL;

    return vma;
}

//
// Locking: you must hold a read / write lock on the mmap_sem.
//
static struct vm_area_struct *
find_uvmlite_vma(unsigned long long requestedBase,
    unsigned long long length,
    struct file *filp)
{
    unsigned long long counterLowestPage =
        UVM_COUNTERS_OFFSET_BASE >> PAGE_SHIFT;
    unsigned long long pageNr = PAGE_ALIGN(length) >> PAGE_SHIFT;
    struct vm_area_struct *vma = find_common_vma(requestedBase, length, filp);

    if (vma == NULL)
        return NULL;
    if ((vma->vm_pgoff << PAGE_SHIFT) > requestedBase)
        return NULL;
    if (vma->vm_pgoff >= counterLowestPage)
        return NULL;
    if (vma->vm_pgoff + pageNr >= counterLowestPage)
        return NULL;

    return vma;
}

//
// Locking: you must hold a read / write lock on the mmap_sem.
//
struct vm_area_struct *
find_counters_vma(unsigned long long requestedBase,
    unsigned long long length,
    struct file *filp)
{
    unsigned long long counterLowestPage =
        UVM_COUNTERS_OFFSET_BASE >> PAGE_SHIFT;
    unsigned long long pageNr = PAGE_ALIGN(length) >> PAGE_SHIFT;
    struct vm_area_struct *vma = find_common_vma(requestedBase, length, filp);

    if (vma == NULL)
        return NULL;
    if (vma->vm_pgoff < counterLowestPage)
        return NULL;
    if (vma->vm_pgoff + pageNr < counterLowestPage)
        return NULL;

    UVM_PANIC_ON(vma->vm_flags & (VM_WRITE|VM_MAYWRITE));

    return vma;
}

//
// Locking: you must hold a read / write lock on the mmap_sem.
//
struct vm_area_struct *
find_events_vma(unsigned long long requestedBase,
    unsigned long long length,
    struct file *filp)
{
    unsigned long long eventsLowestPage =
        UVM_EVENTS_OFFSET_BASE >> PAGE_SHIFT;
    unsigned long long pageNr = PAGE_ALIGN(length) >> PAGE_SHIFT;
    struct vm_area_struct *vma = find_common_vma(requestedBase, length, filp);

    if (vma == NULL)
        return NULL;
    if (vma->vm_pgoff < eventsLowestPage)
        return NULL;
    if (vma->vm_pgoff + pageNr < eventsLowestPage)
        return NULL;

    return vma;
}

NV_STATUS
uvm_api_reserve_va(UVM_RESERVE_VA_PARAMS *pParams, struct file *filp)
{
    UVM_DBG_PRINT_RL("requestedBase: 0x%llx, length: 0x%llx\n",
                     pParams->requestedBase, pParams->length);
    //
    // There is nothing required here yet. The mmap() call in user space handles
    // everything.
    //
    return NV_OK;
}

NV_STATUS
uvm_api_release_va(UVM_RELEASE_VA_PARAMS *pParams, struct file *filp)
{
    UVM_DBG_PRINT_RL("requestedBase: 0x%llx, length: 0x%llx\n",
                     pParams->requestedBase, pParams->length);
    //
    // There is nothing required here yet. The mmap() call in user space handles
    // everything.
    //
    return NV_OK;
}

//
// Most of the region commit actions are done in the uvmlite_mmap()
// callback. This makes UvmRegionCommit look mostly atomic, from user space.
//
// However, the following items can only be done here:
//     1) Check that the GPU is modern enough to be used for UVM-Lite.
//     2) Assign the streamID that the user requested, to the pRecord.
//     3) Assign the GPU UUID to the pRecord.
//     4) Set up a Copy Engine channel.
//
NV_STATUS
uvm_api_region_commit(UVM_REGION_COMMIT_PARAMS *pParams, struct file *filp)
{
    UvmCommitRecord *pRecord;
    UvmRegionTracker *pRegionTracker;
    DriverPrivate *pPriv = (DriverPrivate*)filp->private_data;
    struct vm_area_struct *vma;
    NV_STATUS rmStatus = NV_OK;

    UVM_DBG_PRINT_RL("requestedBase: 0x%llx, length: 0x%llx, "
                     "streamId: 0x%llx\n",
                     pParams->requestedBase, pParams->length,
                     pParams->streamId);

    // Item (1): Check that the GPU is modern enough to be used for UVM-Lite:
    if (!uvmlite_is_gpu_kepler_and_above(&pParams->gpuUuid))
    {
        UVM_ERR_PRINT("uvmlite_is_gpu_kepler_and_above reported: false\n");
        return NV_ERR_NOT_SUPPORTED;
    }

    if (pParams->streamId == UVM_STREAM_INVALID)
    {
        UVM_ERR_PRINT("invalid stream ID");
        return NV_ERR_INVALID_ARGUMENT;
    }

    down_write(&current->mm->mmap_sem);

    vma = find_uvmlite_vma(pParams->requestedBase, pParams->length, filp);

    if (!vma)
    {
        up_write(&current->mm->mmap_sem);
        UVM_ERR_PRINT("Failed to find the vma (base: 0x%llx, length: %llu\n",
                      pParams->requestedBase, pParams->length);
        return NV_ERR_UVM_ADDRESS_IN_USE;
    }

    // Items 2, 3 and 4 are done by the uvmlite_update_commit_record routine:
    pRegionTracker = (UvmRegionTracker*)vma->vm_private_data;

    if (!pRegionTracker)
    {
        up_write(&current->mm->mmap_sem);
        UVM_ERR_PRINT("attempted to commit region without a preceding mmap() "
                      "call\n");
        return NV_ERR_OBJECT_NOT_FOUND;
    }

    if (NV_OK !=
        uvm_get_owner_from_region(pRegionTracker,
                                  pParams->requestedBase,
                                  pParams->requestedBase + pParams->length,
                                  &pRecord))
    {
      // The vma is associated with a region tracker but there is no commit
      // associated to the specified region
      up_write(&current->mm->mmap_sem);
      UVM_ERR_PRINT("Failed to find the commit associated to the region\n");
      return NV_ERR_UVM_ADDRESS_IN_USE;
    }

    if ((pRecord->baseAddress != pParams->requestedBase) ||
        (PAGE_ALIGN(pRecord->length) != PAGE_ALIGN(pParams->length)))
    {
        up_write(&current->mm->mmap_sem);
        UVM_ERR_PRINT("attempted to commit region with different VA or length"
                      " than used by preceding mmap\n");
        return NV_ERR_UVM_ADDRESS_IN_USE;
    }

    down_write(&pPriv->uvmPrivLock);
    rmStatus = uvmlite_update_commit_record(pRecord, pParams->streamId,
                                            &pParams->gpuUuid, pPriv);
    if (NV_OK != rmStatus)
    {
        // If update failed, then the pRecord has been deleted, so don't have
        // the vma point to pRecord anymore:
        vma->vm_private_data  = NULL;
        UVM_ERR_PRINT("uvmlite_update_commit_record failed: 0x%0x.\n",
                      rmStatus);
    }

    up_write(&pPriv->uvmPrivLock);
    up_write(&current->mm->mmap_sem);

    return rmStatus;
}

NV_STATUS
uvm_api_region_decommit(UVM_REGION_DECOMMIT_PARAMS *pParams, struct file *filp)
{
    //
    // There is nothing required here yet. The vma.close callback handles
    // everything.
    //
    return NV_OK;
}

NV_STATUS
uvm_api_region_set_stream(UVM_REGION_SET_STREAM_PARAMS *pParams,
                          struct file *filp)
{
    DriverPrivate* pPriv = (DriverPrivate*)filp->private_data;
    NV_STATUS rmStatus;
    struct vm_area_struct * vma;
    UvmCommitRecord * pRecord;
    UvmRegionTracker * pRegionTracker;
    unsigned long long start = UVM_PAGE_ALIGN_DOWN(pParams->requestedBase);
    unsigned long long end =
        PAGE_ALIGN(pParams->requestedBase + pParams->length);
    unsigned long long size = end - start;

    UVM_DBG_PRINT_RL("requestedBase: 0x%llx, length: 0x%llx, "
                     "newStreamId: 0x%llx\n",
                     pParams->requestedBase, pParams->length,
                     pParams->newStreamId);

    down_write(&current->mm->mmap_sem);

    vma = find_uvmlite_vma(start, size, filp);
    if (vma == NULL)
    {
        up_write(&current->mm->mmap_sem);
        return NV_ERR_UVM_ADDRESS_IN_USE;
    }

    down_write(&pPriv->uvmPrivLock);
    pRegionTracker = (UvmRegionTracker*)vma->vm_private_data;

    if (!pRegionTracker)
    {
      // Trying to acces to vma with no region tracker associated
      rmStatus = NV_ERR_UVM_ADDRESS_IN_USE;
      UVM_ERR_PRINT("can't find a region tracker for this vma: 0x%0x.\n",
                    rmStatus);
      goto done;
    }

    rmStatus = uvm_get_owner_from_region(pRegionTracker,
                                         start, end,
                                         &pRecord);
    if (rmStatus != NV_OK)
    {
      // The vma is associated with a region tracker but there is no commit
      // associated to the specified region
      rmStatus = NV_ERR_UVM_ADDRESS_IN_USE;
      UVM_ERR_PRINT("can't find a matching commit for this region: 0x%0x.\n",
                    rmStatus);
      goto done;
    }

    if (start == pRecord->baseAddress &&
        (size == pRecord->length || size == 0))
    {
        // Clear all the included commits
        uvm_destroy_included_regions(
            pRegionTracker,
            start, end,
            uvmlite_destroy_commit_record);
        pRecord->hasChildren = NV_FALSE;
        rmStatus = uvmlite_region_set_stream(pRecord, pParams->newStreamId);
    }
    else if (start  >= pRecord->baseAddress &&
             end <= (pRecord->baseAddress + pRecord->length))
    {
        rmStatus =
            uvmlite_attach_record_portion_to_stream(pRecord,
                                                    pParams->newStreamId,
                                                    pRegionTracker,
                                                    start, size);
    }
    else
        rmStatus = NV_ERR_UVM_ADDRESS_IN_USE;

done:
    up_write(&pPriv->uvmPrivLock);
    up_write(&current->mm->mmap_sem);
    return rmStatus;
}

NV_STATUS
uvm_api_region_set_stream_running(UVM_SET_STREAM_RUNNING_PARAMS *pParams,
                                  struct file *filp)
{
    DriverPrivate* pPriv = (DriverPrivate*)filp->private_data;
    NV_STATUS rmStatus;

    UVM_DBG_PRINT_RL("streamID: 0x%llx\n", pParams->streamId);

    down_write(&current->mm->mmap_sem);
    down_write(&pPriv->uvmPrivLock);
    rmStatus = uvmlite_set_stream_running(pPriv, pParams->streamId);
    up_write(&pPriv->uvmPrivLock);
    up_write(&current->mm->mmap_sem);

    return rmStatus;
}

NV_STATUS
uvm_api_region_set_stream_stopped(UVM_SET_STREAM_STOPPED_PARAMS *pParams,
                                  struct file *filp)
{
    DriverPrivate *pPriv = (DriverPrivate*)filp->private_data;
    NV_STATUS rmStatus;

    if (pParams->nStreams > UVM_MAX_STREAMS_PER_IOCTL_CALL)
        return NV_ERR_INVALID_ARGUMENT;

    if (pParams->nStreams > 0)
    {
        UVM_DBG_PRINT_RL("streamIDs 0x%llx - 0x%llx\n",
                         pParams->streamIdArray[0],
                         pParams->streamIdArray[pParams->nStreams - 1]);
    }

    down_write(&current->mm->mmap_sem);
    down_write(&pPriv->uvmPrivLock);
    rmStatus = uvmlite_set_streams_stopped(pPriv, pParams->streamIdArray,
                                           pParams->nStreams);
    up_write(&pPriv->uvmPrivLock);
    up_write(&current->mm->mmap_sem);
    return rmStatus;
}

NV_STATUS
uvm_api_migrate_to_gpu(UVM_MIGRATE_TO_GPU_PARAMS *pParams, struct file *filp)
{
    DriverPrivate *pPriv = (DriverPrivate*)filp->private_data;
    NV_STATUS rmStatus;
    struct vm_area_struct *vma;
    UvmCommitRecord *pRecord;
    UvmRegionTracker *pRegionTracker;

    UVM_DBG_PRINT_RL("requestedBase: 0x%llx, length: 0x%llx, "
                     "flags: 0x%x\n",
                     pParams->requestedBase, pParams->length, pParams->flags);

    down_write(&current->mm->mmap_sem);

    vma = find_uvmlite_vma(pParams->requestedBase, pParams->length, filp);
    if (!vma)
    {
        up_write(&current->mm->mmap_sem);
        return NV_ERR_UVM_ADDRESS_IN_USE;
    }

    down_write(&pPriv->uvmPrivLock);

    pRegionTracker = (UvmRegionTracker*)vma->vm_private_data;

    if (NV_OK !=
        uvm_get_owner_from_region(pRegionTracker,
                                  pParams->requestedBase,
                                  pParams->requestedBase + pParams->length,
                                  &pRecord))
    {
      rmStatus = NV_ERR_UVM_ADDRESS_IN_USE;
      goto done;
    }

    rmStatus = uvmlite_migrate_to_gpu(pParams->requestedBase,
                                      pParams->length,
                                      pParams->flags,
                                      vma,
                                      pRecord);

done:
    up_write(&pPriv->uvmPrivLock);
    up_write(&current->mm->mmap_sem);

    return rmStatus;
}

NV_STATUS
uvm_api_run_test(UVM_RUN_TEST_PARAMS *pParams, struct file *filp)
{
    NV_STATUS status = NV_OK;
    UVM_DBG_PRINT_UUID("Entering", &pParams->gpuUuid);
    
    if (!uvm_enable_builtin_tests)
        return NV_ERR_NOT_SUPPORTED;

    switch (pParams->test)
    {
        case UVM_GPU_OPS_SAMPLE_TEST:
            status = gpuOpsSampleTest(&pParams->gpuUuid);
            break;

        case UVM_REGION_TRACKER_SANITY_TEST:
            status = regionTrackerSanityTest();
            break;

        default:
            UVM_INFO_PRINT("bad test: 0x%x\n", pParams->test);
            status = NV_ERR_INVALID_ARGUMENT;
    }

    return status;
}

NV_STATUS
uvm_api_add_session(UVM_ADD_SESSION_PARAMS *pParams, struct file *filp)
{
    NV_STATUS rmStatus;
    DriverPrivate *pPriv = (DriverPrivate*)filp->private_data;
    UvmCounterContainer *pCounterContainer = NULL;
    UvmEventContainer *pEventContainer = NULL;
    uid_t euid = UVM_ROOT_UID;
    struct vm_area_struct *vma;
    unsigned long countersBaseAddress = 
        (unsigned long) pParams->countersBaseAddress;

    rmStatus = uvmlite_secure_get_process_containers(pParams->pidTarget,
                                                    &pCounterContainer,
                                                    &pEventContainer,
                                                    &euid);
    if (rmStatus != NV_OK)
        return rmStatus;

    down_write(&pPriv->processRecord.sessionInfoLock);
    rmStatus = uvm_add_session_info(euid,
                                    pParams->pidTarget,
                                    &pParams->sessionIndex,
                                    pCounterContainer,
                                    pEventContainer,
                                    countersBaseAddress,
                                    pPriv->processRecord.sessionInfoArray);
    up_write(&pPriv->processRecord.sessionInfoLock);

    if (rmStatus != NV_OK)
    {
        uvm_unref_counter_container(pCounterContainer);
        uvm_unref_event_container(pEventContainer);
        return rmStatus;
    }

    down_write(&current->mm->mmap_sem);
    rmStatus = NV_ERR_INVALID_ARGUMENT; 
    vma = find_counters_vma((unsigned long long) countersBaseAddress, 
                            UVM_MAX_GPUS * UVM_PER_RESOURCE_COUNTERS_SIZE +
                            UVM_PER_PROCESS_PER_GPU_COUNTERS_SHIFT, filp);
    rmStatus = uvm_map_counters_pages(pCounterContainer,
                                      (NvP64)countersBaseAddress,
                                      vma);
    up_write(&current->mm->mmap_sem);

    // 
    // We can not reverse uvm_map_page, so inserted pages will stay
    // until vma teardown. This means that if we  call mmap (success) 
    // and AddSession (fail), we need to unmap previous address and 
    // call mmap again.
    //
    if (rmStatus != NV_OK)
    {
        down_write(&pPriv->processRecord.sessionInfoLock);

        uvm_unref_counter_container(pCounterContainer);
        uvm_unref_event_container(pEventContainer);
        uvm_remove_session_info(pParams->sessionIndex,
                                pPriv->processRecord.sessionInfoArray);

        up_write(&pPriv->processRecord.sessionInfoLock);
    }
    return rmStatus;
}

NV_STATUS
uvm_api_remove_session(UVM_REMOVE_SESSION_PARAMS *pParams, struct file *filp)
{
    NV_STATUS status;
    DriverPrivate *pPriv = (DriverPrivate*)filp->private_data;
    UvmSessionInfo *pSessionInfo;
    down_write(&pPriv->processRecord.sessionInfoLock);

    status = uvm_get_session_info(pParams->sessionIndex,
                                  pPriv->processRecord.sessionInfoArray,
                                  &pSessionInfo);

    if (NV_OK != status)
    {
        up_write(&pPriv->processRecord.sessionInfoLock);
        return status;
    }

    uvm_unref_counter_container(pSessionInfo->pCounterContainer);
    uvm_unref_event_container(pSessionInfo->pEventContainer);

    status = uvm_remove_session_info(pParams->sessionIndex,
                                     pPriv->processRecord.sessionInfoArray);

    up_write(&pPriv->processRecord.sessionInfoLock);
    return status;
}

NV_STATUS
uvm_api_enable_counters(UVM_ENABLE_COUNTERS_PARAMS *pParams, struct file *filp)
{
    NV_STATUS rmStatus;
    DriverPrivate *pPriv = (DriverPrivate*)filp->private_data;
    UvmProcessRecord *pProcessRecord;
    UvmSessionInfo *pSessionInfo;
    pProcessRecord = &pPriv->processRecord;

    down_read(&pProcessRecord->sessionInfoLock);

    rmStatus = uvm_get_session_info(pParams->sessionIndex,
                                    pProcessRecord->sessionInfoArray,
                                    &pSessionInfo);

    if (rmStatus == NV_OK)
    {
        rmStatus = uvm_counter_state_atomic_update(pSessionInfo,
                                                   pParams->config,
                                                   pParams->count);
    }

    up_read(&pProcessRecord->sessionInfoLock);
    return rmStatus;
}

NV_STATUS
uvm_api_map_counter(UVM_MAP_COUNTER_PARAMS *pParams, struct file *filp)
{
    NV_STATUS rmStatus;
    DriverPrivate *pPriv = (DriverPrivate*)filp->private_data;
    UvmProcessRecord *pProcessRecord;
    UvmSessionInfo *pSessionInfo;
    unsigned gpuIndex = 0;

    pProcessRecord = &pPriv->processRecord;

    down_read(&pProcessRecord->sessionInfoLock);

    rmStatus = uvm_get_session_info(pParams->sessionIndex,
                                    pProcessRecord->sessionInfoArray,
                                    &pSessionInfo);
    if (rmStatus == NV_OK)
    {
        if (pParams->scope == UvmCounterScopeProcessSingleGpu &&
            uvmlite_find_gpu_index(&pParams->gpuUuid, &gpuIndex) != NV_OK)
        {
            UVM_ERR_PRINT_UUID("failed to find gpu index ", &pParams->gpuUuid);
            rmStatus = NV_ERR_INVALID_ARGUMENT;
            goto cleanup;
        }
        rmStatus = uvm_map_counter(pSessionInfo,
                                   pParams->scope,
                                   pParams->counterName,
                                   gpuIndex,
                                   (NvUPtr*)&pParams->addr);
    }

cleanup:
    up_read(&pProcessRecord->sessionInfoLock);
    return rmStatus;
}

NV_STATUS
uvm_api_register_mps_server(UVM_REGISTER_MPS_SERVER_PARAMS *pParams, struct file *filp)
{
    NV_STATUS rmStatus;
    DriverPrivate *pPriv = (DriverPrivate*)filp->private_data;

    if (pParams->numGpus > UVM_MAX_GPUS)
        return NV_ERR_INVALID_ARGUMENT;

    down_write(&pPriv->uvmPrivLock);
    rmStatus = uvmlite_register_mps_server(pPriv, pParams->gpuUuidArray,
                                           pParams->numGpus,
                                           &pParams->serverId);
    up_write(&pPriv->uvmPrivLock);

    return rmStatus;
}

NV_STATUS
uvm_api_register_mps_client(UVM_REGISTER_MPS_CLIENT_PARAMS *pParams, struct file *filp)
{
    NV_STATUS rmStatus;
    DriverPrivate *pPriv = (DriverPrivate*)filp->private_data;

    down_write(&pPriv->uvmPrivLock);
    rmStatus = uvmlite_register_mps_client(pPriv, pParams->serverId);
    up_write(&pPriv->uvmPrivLock);

    return rmStatus;
}

NV_STATUS
uvm_api_create_event_queue(UVM_CREATE_EVENT_QUEUE_PARAMS *pParams, struct file *filp)
{
    NV_STATUS rmStatus;
    DriverPrivate *pPriv = (DriverPrivate*)filp->private_data;
    UvmProcessRecord *pProcessRecord;
    UvmSessionInfo *pSessionInfo;
    unsigned eventQueueIndex;

    pProcessRecord = &pPriv->processRecord;

    if (pParams->queueSize <= 0)
    {
        UVM_ERR_PRINT("invalid queue size %lld\n", pParams->queueSize);
        return NV_ERR_INVALID_ARGUMENT;
    }

    down_read(&pProcessRecord->sessionInfoLock);

    rmStatus = uvm_get_session_info(pParams->sessionIndex,
                                    pProcessRecord->sessionInfoArray,
                                    &pSessionInfo);
    if (rmStatus != NV_OK)
    {
        UVM_ERR_PRINT("failed to locate session %d: rmStatus: 0x%x\n",
        pParams->sessionIndex, rmStatus);
        goto done;
    }

    down_write(&pSessionInfo->eventQueueInfoListLock);

    // TODO: Bug 1766109: uvm8: delete UVM-Lite files and remove -lite mode
    //  ...just delete this entire file, instead of the original to-do: which was:
    //
    // RO and RW data sizes should be defined in the ioctl header file
    rmStatus = uvm_create_event_queue(pSessionInfo,
                                      &eventQueueIndex,
                                      pParams->queueSize,
                                      pParams->notificationCount,
                                      pParams->timeStampType);

    up_write(&pSessionInfo->eventQueueInfoListLock);

    if (rmStatus != NV_OK)
    {
        UVM_ERR_PRINT("failed to create event queue: rmStatus: 0x%x\n", rmStatus);
        goto done;
    }

    pParams->eventQueueIndex = eventQueueIndex;

done:
    up_read(&pProcessRecord->sessionInfoLock);
    return rmStatus;
}

NV_STATUS
uvm_api_remove_event_queue(UVM_REMOVE_EVENT_QUEUE_PARAMS *pParams, struct file *filp)
{
    NV_STATUS rmStatus;
    DriverPrivate *pPriv = (DriverPrivate*)filp->private_data;
    UvmProcessRecord *pProcessRecord;
    UvmSessionInfo *pSessionInfo;
    UvmEventQueueInfo *pEventQueueInfo;

    pProcessRecord = &pPriv->processRecord;

    down_read(&pProcessRecord->sessionInfoLock);

    rmStatus = uvm_get_session_info(pParams->sessionIndex,
                                    pProcessRecord->sessionInfoArray,
                                    &pSessionInfo);
    if (rmStatus != NV_OK)
    {
        UVM_ERR_PRINT("failed to locate session %d\n", pParams->sessionIndex);
        up_read(&pProcessRecord->sessionInfoLock);
        return rmStatus;
    }

    down_write(&pSessionInfo->eventQueueInfoListLock);

    rmStatus = uvm_get_event_queue(pSessionInfo,
                                   &pEventQueueInfo,
                                   pParams->eventQueueIndex);

    if (rmStatus != NV_OK)
    {
        UVM_ERR_PRINT("failed to retrieve event queue: rmStatus: 0x%x\n", rmStatus);
        up_write(&pSessionInfo->eventQueueInfoListLock);
        up_read(&pProcessRecord->sessionInfoLock);
        return rmStatus;
    }

    uvm_remove_event_queue(pSessionInfo, pEventQueueInfo);

    up_write(&pSessionInfo->eventQueueInfoListLock);

    up_read(&pProcessRecord->sessionInfoLock);

    return rmStatus;
}

NV_STATUS
uvm_api_map_event_queue(UVM_MAP_EVENT_QUEUE_PARAMS *pParams, struct file *filp)
{
    NV_STATUS rmStatus;
    DriverPrivate *pPriv = (DriverPrivate*)filp->private_data;
    UvmProcessRecord *pProcessRecord;
    UvmSessionInfo *pSessionInfo;
    UvmEventQueueInfo *pEventQueueInfo;
    NvLength numQueuePages;
    struct vm_area_struct *roVma, *rwVma;

    down_write(&current->mm->mmap_sem);

    rwVma = find_events_vma((unsigned long long)pParams->userRWDataAddr,
                            PAGE_SIZE,
                            filp);
    if (!rwVma)
    {
        rmStatus = NV_ERR_INVALID_ARGUMENT;
        goto unlock_mmap_sem;
    }

    pProcessRecord = &pPriv->processRecord;

    down_read(&pProcessRecord->sessionInfoLock);

    rmStatus = uvm_get_session_info(pParams->sessionIndex,
                                    pProcessRecord->sessionInfoArray,
                                    &pSessionInfo);
    if (rmStatus != NV_OK)
    {
        UVM_ERR_PRINT("failed to locate session %d: rmStatus: 0x%x\n",
                pParams->sessionIndex, rmStatus);
        goto unlock_sessionInfo;
    }

    //
    // Write lock here since we're modifying members of eventQueueInfoList
    // An alternative is to have one lock per list member, but this operation
    // isn't in a speed path.
    //
    down_write(&pSessionInfo->eventQueueInfoListLock);

    rmStatus = uvm_get_event_queue(pSessionInfo,
                                   &pEventQueueInfo,
                                   pParams->eventQueueIndex);

    up_write(&pSessionInfo->eventQueueInfoListLock);

    if (rmStatus != NV_OK)
    {
        UVM_ERR_PRINT("failed to retrieve event queue: rmStatus: 0x%x\n",
                rmStatus);
        goto unlock_sessionInfo;
    }

    numQueuePages = pEventQueueInfo->numQueuePages; 

    //
    // Find the vma which will contain one RO page and the full
    // event queue buffer.
    //
    roVma = find_counters_vma((unsigned long long)pParams->userRODataAddr,
                              (1 + numQueuePages) << PAGE_SHIFT,
                              filp);
    if (!roVma)
    {
        rmStatus = NV_ERR_INVALID_ARGUMENT;
        goto unlock_sessionInfo;
    }

    rmStatus = uvm_map_event_queue(pEventQueueInfo,
                                   pParams->userRODataAddr,
                                   pParams->userRWDataAddr,
                                   &pParams->readIndexAddr,
                                   &pParams->writeIndexAddr,
                                   &pParams->queueBufferAddr,
                                   roVma,
                                   rwVma,
                                   filp);
    if (rmStatus != NV_OK)
    {
        UVM_ERR_PRINT("failed to map event queue to userspace: rmStatus: 0x%x\n",
                rmStatus);
        goto unlock_sessionInfo;
    }

unlock_sessionInfo:
    up_read(&pProcessRecord->sessionInfoLock);

unlock_mmap_sem:
    up_write(&current->mm->mmap_sem);

    return rmStatus;
}

NV_STATUS
uvm_api_event_ctrl(UVM_EVENT_CTRL_PARAMS *pParams, struct file *filp)
{
    NV_STATUS rmStatus;
    DriverPrivate *pPriv = (DriverPrivate*)filp->private_data;
    UvmProcessRecord *pProcessRecord;
    UvmSessionInfo *pSessionInfo;
    UvmEventQueueInfo *pEventQueueInfo;

    if (pParams->eventType >= UvmEventNumTypes)
        return NV_ERR_INVALID_ARGUMENT;

    //
    // Acquire g_uvmDriverPrivateTableLock to interlock with uvmlite_close.
    // ensuring that the process records of the debugger and debuggee stay
    // around.
    //
    down_read(&g_uvmDriverPrivateTableLock);

    pProcessRecord = &pPriv->processRecord;

    down_read(&pProcessRecord->sessionInfoLock);

    rmStatus = uvm_get_session_info(pParams->sessionIndex,
                                    pProcessRecord->sessionInfoArray,
                                    &pSessionInfo);
    if (rmStatus != NV_OK)
        goto unlock_session;

    //
    // Write lock here since we're modifying members of eventQueueInfoList.
    // An alternative is to have one lock per list member, but this operation
    // isn't in a speed path.
    //
    down_write(&pSessionInfo->eventQueueInfoListLock);

    rmStatus = uvm_get_event_queue(pSessionInfo,
                                   &pEventQueueInfo,
                                   pParams->eventQueueIndex);
    if (rmStatus != NV_OK)
        goto unlock_event;

    if (pParams->enable)
        rmStatus = uvm_enable_event(pEventQueueInfo,
                                    pParams->eventType,
                                    pSessionInfo->pEventContainer);
    else
        rmStatus = uvm_disable_event(pEventQueueInfo,
                                     pParams->eventType,
                                     pSessionInfo->pEventContainer);

unlock_event:
    up_write(&pSessionInfo->eventQueueInfoListLock);
unlock_session:
    up_read(&pProcessRecord->sessionInfoLock);
    up_read(&g_uvmDriverPrivateTableLock);

    return rmStatus;
}

NV_STATUS
uvm_api_get_gpu_uuid_table(UVM_GET_GPU_UUID_TABLE_PARAMS *pParams,
                           struct file *filp)
{
    return uvmlite_get_gpu_uuid_list(pParams->gpuUuidArray,
                                     &pParams->validCount);
}

NV_STATUS uvm_api_is_8_supported_lite(UVM_IS_8_SUPPORTED_PARAMS *pParams,
                                      struct file *filp)
{
    pParams->is8Supported = 0;
    return NV_OK;
}

NV_STATUS uvm_api_pageable_mem_access_lite(UVM_PAGEABLE_MEM_ACCESS_PARAMS *pParams,
                                           struct file *filp)
{
    // UVM-lite will not support HMM
    pParams->pageableMemAccess = NV_FALSE;
    return NV_OK;
}
