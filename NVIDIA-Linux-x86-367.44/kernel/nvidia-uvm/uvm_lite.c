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
#include "uvm_linux_ioctl.h"
#include "uvm_common.h"
#include "uvm_kernel_events.h"
#include "uvm_kernel_counters.h"
#include "uvm_debug_session.h"
#include "uvm_page_migration.h"
#include "uvm_utils.h"
#include "uvm_lite.h"
#include "uvm_lite_prefetch.h"
#include "uvm_channel_mgmt.h"
#include "uvm_lite_region_tracking.h"
#include "ctrl2080mc.h"

//
// uvm_lite.c
// This file contains code that is specific to the UVM-Lite mode of operation.
//

#ifndef NVIDIA_UVM_ENABLED
#error "Building uvm code without NVIDIA_UVM_ENABLED!"
#endif

#define UVM_SEM_INIT 0x0
#define UVM_SEM_DONE 0xFACEFEED

// Macros used to manipulate "outdated" bit in pages
// We use the Checked page flag provided by the Linux kernel. This flag is
// reserved for private use by the filesystem, so we can safely use it here
#define UVM_PAGE_SET_OUTDATED(p) SetPageChecked(p)
#define UVM_PAGE_CLEAR_OUTDATED(p) ClearPageChecked(p)
#define UVM_PAGE_OUTDATED(p) PageChecked(p)

// Module parameter to enable/disable prefetching in UVM-Lite. Use signed
// instead of unsigned int variables for modules to avoid compilation warnings
// in old versions of the kernel.
int uvm_prefetch = 0;
module_param(uvm_prefetch, int, S_IRUGO);

static struct cdev g_uvmlite_cdev;

// table of attached GUIDS
static UvmGpuState g_attached_uuid_list[UVM_MAX_GPUS];
static unsigned g_attached_uuid_num;
static struct rw_semaphore g_attached_uuid_lock;

//
// Locking acquisition order (only take the locks you need, but always follow
// this order):
//
//      1. mm->mmap_sem
//
//      2. g_uvmDriverPrivateTableLock
//
//      3. DriverPrivate.uvmPrivLock
//
//      4. UvmMpsServer.mpsLock
//
//      5. MPS server's DriverPrivate.uvmPrivLock
//
//      6. g_uvmMpsServersListLock
//
//      7. sessionInfoLock
//
//      8. g_attached_uuid_lock
//
//      9. eventQueueInfoListLock
//
//      10. eventListenerListLock
//
static struct kmem_cache * g_uvmPrivateCache              __read_mostly = NULL;
static struct kmem_cache * g_uvmCommitRecordCache         __read_mostly = NULL;
static struct kmem_cache * g_uvmMigTrackerCache           __read_mostly = NULL;
static struct kmem_cache * g_uvmStreamRecordCache         __read_mostly = NULL;
static struct kmem_cache * g_uvmMappingCache              __read_mostly = NULL;
static struct kmem_cache * g_uvmMpsServerCache            __read_mostly = NULL;

struct rw_semaphore g_uvmDriverPrivateTableLock;
static struct rw_semaphore g_uvmMpsServersListLock;

//
// Root of global driver private list
// This list contains DriverPrivate pointers,
// which are valid as long, as you are holding
// r/w g_uvmDriverPrivateTableLock
//
static LIST_HEAD(g_uvmDriverPrivateTable);

//
// This list contains UvmMpsServer pointers,
// which are valid as long, as tou are holding
// r/w g_uvmMpsServersListLock
//
static LIST_HEAD(g_uvmMpsServersList);

// uvm kernel privileged region
static NvU64 g_uvmKernelPrivRegionStart;
static NvU64 g_uvmKernelPrivRegionLength;

static void _mmap_close(struct vm_area_struct *vma);
static void _mmap_open(struct vm_area_struct *vma);
static void _destroy_migration_resources(UvmGpuMigrationTracking *pMigTracking);
static NV_STATUS _create_migration_resources(NvProcessorUuid * pGpuUuidStruct,
                                             UvmGpuMigrationTracking *pMigTracking);
static void _set_record_accessible(UvmCommitRecord *pRecord);
static void _set_record_inaccessible(UvmCommitRecord *pRecord);
static int _is_record_included_in_vma(UvmCommitRecord *pRecord);
static void _record_detach_from_stream(UvmCommitRecord *pRecord);
static NV_STATUS _record_attach_to_stream(UvmCommitRecord *pRecord,
                                          UvmStreamRecord *pNewStream);

static UvmStreamRecord * _stream_alloc(UvmProcessRecord *processRecord,
                                       UvmStream streamId);
static UvmStreamRecord * _stream_find_in_cache(UvmProcessRecord *processRecord,
                                               UvmStream streamId);
static UvmStreamRecord * _stream_find_in_list(UvmProcessRecord *processRecord,
                                              UvmStream streamId);
static UvmStreamRecord * _stream_find(UvmProcessRecord *processRecord,
                                      UvmStream streamId);
static UvmStreamRecord * _stream_find_or_alloc(UvmProcessRecord *processRecord,
                                               UvmStream streamId);
static NV_STATUS _find_or_add_gpu_index(NvProcessorUuid *gpuUuidStruct,
                                        unsigned *pIndex);
static unsigned _find_gpu_index(NvProcessorUuid *gpuUuidStruct);
static NvBool _is_gpu_kepler_and_above(unsigned index);
static void _stream_destroy(UvmStreamRecord *pStream);
static void _stream_destroy_if_empty(UvmStreamRecord *pStream);
static void _stream_remove_from_cache(UvmStreamRecord *pStream);
static void _stream_save_in_cache(UvmStreamRecord *pStream);
static NV_STATUS _wait_for_migration_completion(UvmGpuMigrationTracking *pMigTracker,
                                                UvmCommitRecord *pRecord,
                                                UvmGpuPointer pageVirtualAddr,
                                                UvmGpuPointer cpuPhysAddr,
                                                char ** cpuPbPointer,
                                                char * cpuPbEnd,
                                                NvLength * numMethods);
static NV_STATUS _clear_cache(UvmCommitRecord *pRecord);
static void _update_gpu_migration_counters(UvmCommitRecord *pRecord,
                                           unsigned long long migratedPages);
static
NV_STATUS _preexisting_error_on_channel(UvmGpuMigrationTracking *pMigTracker,
                                        UvmCommitRecord *pRecord);
static NvBool _lock_mps_server(UvmProcessRecord *mpsClientProcess);
static void _unlock_mps_server(UvmProcessRecord *mpsClientProcess);
static NvBool _is_mps_server(UvmProcessRecord *processRecord);
static NvBool _is_mps_client(UvmProcessRecord *processClient);
static void _delete_mps_server(struct kref *kref);

//
// uvmlite_gpu_event_(start|stop)_device() deal with events from the RM to the
// UVM-lite driver.
//
NV_STATUS uvmlite_gpu_event_start_device(NvProcessorUuid *gpuUuidStruct)
{
    UVM_DBG_PRINT_UUID("Start", gpuUuidStruct);

    if (uvmlite_enable_gpu_uuid(gpuUuidStruct) != NV_OK)
        return NV_ERR_GENERIC;
    return NV_OK;
}

NV_STATUS uvmlite_gpu_event_stop_device(NvProcessorUuid *gpuUuidStruct)
{

    //
    // TODO: Bug 1766109: uvm8: delete UVM-Lite files and remove -lite mode
    //  ...just delete this entire file, instead of the original to-do: which was:
    //
    // implement
    //
    // Grab the global lock
    // Grab the per process lock
    // Clean up all the migration resources for this UUID
    // and other relevant data structures
    // Also:
    // Other related fixes that need to be added
    // need to take this global lock when UVM is being rmmodded
    // The copy/mig loop needs to now follow this
    // 1) Take per process lock
    // 2) Check if migration resources exist(channel is valid)
    // 3) migrate pages()
    // 4) Unlock per process lock
    // 5) is_schedule()
    //

    UVM_DBG_PRINT_UUID("Stop", gpuUuidStruct);
    if (uvmlite_disable_gpu_uuid(gpuUuidStruct) != NV_OK)
        return NV_ERR_GENERIC;

    umvlite_destroy_per_process_gpu_resources(gpuUuidStruct);
    return NV_OK;
}

static void _set_timeout_in_usec(struct timeval *src,
                                 struct timeval *result,
                                 unsigned long timeoutInUsec)
{
    if (!src || !result)
        return;

    result->tv_usec = src->tv_usec + timeoutInUsec;

    // Add the overflow from tv_usec into seconds & clamp uSec if overflowed
    result->tv_sec = src->tv_sec + result->tv_usec/1000000;
    result->tv_usec %= 1000000;
}

//
// Currently the driver refuses to work with vmas that have been modified
// since the original mmap() call. Mark them as inaccessible.
//
// Locking: this is called with mmap_sem for write
//
static void _mmap_open(struct vm_area_struct *vma)
{
    UvmRegionTracker* pTracking_tree = (UvmRegionTracker*)vma->vm_private_data;
    UVM_DBG_PRINT_RL("vma 0x%p [0x%p, 0x%p)\n",
                     vma, (void*)vma->vm_start, (void*)vma->vm_end);
    //
    // The vma that was originally created is being modified.
    // Mark the cloned vma as inaccessible and reset its private data to make
    // sure the same commit record is not referenced by multiple vmas.
    //
    vma->vm_private_data = NULL;

    if (pTracking_tree)
    {
        DriverPrivate *pDriverPriv = pTracking_tree->osPrivate;
        // mmap_open should never be called for the original vma
        UVM_PANIC_ON(pTracking_tree->vma == vma);

        // Destroy the entire tree of commit records that were associated with
        // this vma
        down_write(&pDriverPriv->uvmPrivLock);
        // clear the original vma's private field
        pTracking_tree->vma->vm_private_data = NULL;
        uvm_destroy_region_tracker(pTracking_tree, uvmlite_destroy_commit_record);
        up_write(&pDriverPriv->uvmPrivLock);
    }
}

static void _stream_destroy(UvmStreamRecord *pStream)
{
    if (pStream == NULL)
        return;

    UVM_DBG_PRINT_RL("stream %lld\n", pStream->streamId);

    // Stream should be stopped
    UVM_PANIC_ON(pStream->isRunning);

    // Stream should be empty
    UVM_PANIC_ON(!list_empty(&pStream->commitRecordsList));

    list_del(&pStream->allStreamListNode);
    _stream_remove_from_cache(pStream);

    kmem_cache_free(g_uvmStreamRecordCache, pStream);
}

void _stop_and_destroy_leftover_streams(UvmProcessRecord *processRecord)
{
    struct list_head *pos;
    struct list_head *safepos;
    UvmStreamRecord *pStream = NULL;

    list_for_each_safe(pos, safepos, &processRecord->allStreamList)
    {
        pStream = list_entry(pos, UvmStreamRecord, allStreamListNode);
        pStream->isRunning = NV_FALSE;
        _stream_destroy(pStream);
    }
}

//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock
// and mmap_sem, before calling this routine.
//
void uvmlite_destroy_commit_record(UvmCommitRecord *pRecord)
{
    NvLength pageIdx;
    NvLength nPages;

    if (!pRecord)
        return;

    // If the record is the child of a parent record then
    // the CommitRecordPages willl be owned by the parent
    if (!pRecord->commitRecordPages || pRecord->isChild)
        goto free_record;

    nPages = (PAGE_ALIGN(pRecord->length)) >> PAGE_SHIFT;
    UVM_DBG_PRINT_RL("nPages: %llu\n", nPages);

    for (pageIdx = 0; pageIdx < nPages; ++pageIdx)
    {
        uvm_page_cache_free_page(pRecord->commitRecordPages[pageIdx],
                                 __FUNCTION__);
    }

    vfree(pRecord->commitRecordPages);

free_record:
    pRecord->commitRecordPages = NULL;
    pRecord->isAccessible = NV_FALSE;
    // If the record has children then the record no longer
    // belong to a stream
    if (!pRecord->hasChildren)
        _record_detach_from_stream(pRecord);

    uvmlite_destroy_prefetch_info(&pRecord->prefetchInfo);

    kmem_cache_free(g_uvmCommitRecordCache, pRecord);
}

static void _mmap_close(struct vm_area_struct *vma)
{
    UvmRegionTracker* pTracking_tree = (UvmRegionTracker*)vma->vm_private_data;
    UVM_DBG_PRINT_RL("vma 0x%p [0x%p, 0x%p)\n",
                     vma, (void*)vma->vm_start, (void*)vma->vm_end);

    if (pTracking_tree)
    {
        DriverPrivate *pDriverPriv = pTracking_tree->osPrivate;
        //
        // This should never happen as the vm_private_data is reset
        // in mmap_open().
        //
        UVM_PANIC_ON(pTracking_tree->vma != vma);

        // Destroy the tree and all the commit associated to the vma
        down_write(&pDriverPriv->uvmPrivLock);
        // clear the original vma's private field
        pTracking_tree->vma->vm_private_data = NULL;
        uvm_destroy_region_tracker(pTracking_tree, uvmlite_destroy_commit_record);
        up_write(&pDriverPriv->uvmPrivLock);
    }
}

//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock, before
// calling this routine.
//
static void _disconnect_mig_completely(UvmPerProcessGpuMigs * pMig,
                                       UvmCommitRecord * pRecord)
{
    memset(pMig, 0, sizeof(*pMig));

    if (pRecord != NULL)
        pRecord->cachedHomeGpuPerProcessIndex = UVM_INVALID_HOME_GPU_INDEX;
}

static void
_delete_all_session_info_table_entries(UvmProcessRecord *pProcessRecord)
{
    int i;

    down_write(&pProcessRecord->sessionInfoLock);

    for (i = 0; i < UVM_MAX_SESSIONS_PER_PROCESS; ++i)
    {
        if (pProcessRecord->sessionInfoArray[i].pidSessionOwner !=
            UVM_PID_INIT_VALUE)
        {
            UvmSessionInfo *pSession = &pProcessRecord->sessionInfoArray[i];
            uvm_unref_counter_container(pSession->pCounterContainer);
            uvm_unref_event_container(pSession->pEventContainer);
            uvm_init_session_info(pSession);
        }
    }

    up_write(&pProcessRecord->sessionInfoLock);
}

//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock, before
// calling this routine.
//
static void
_delete_all_migration_resources(DriverPrivate* pPriv)
{
    int index;
    UvmPerProcessGpuMigs * pMig;
    char uuidBuffer[UVM_GPU_UUID_TEXT_BUFFER_LENGTH];

    for (index = 0; index < UVM_MAX_GPUS; ++index)
    {
        pMig = &pPriv->processRecord.gpuMigs[index];
        if (pMig->migTracker != NULL)
        {
            down_read(&g_attached_uuid_lock);
            format_uuid_to_buffer(uuidBuffer, sizeof(uuidBuffer),
                                  &g_attached_uuid_list[index].gpuUuid);
            up_read(&g_attached_uuid_lock);

            UVM_DBG_PRINT_RL("%s: (channelClass: 0x%0x, ceClass: 0x%0x)\n",
                uuidBuffer, pMig->migTracker->channelInfo.channelClassNum,
                pMig->migTracker->ceClassNumber);

            if (!_is_mps_client(&pPriv->processRecord))
            {
                _destroy_migration_resources(pMig->migTracker);
                kmem_cache_free(g_uvmMigTrackerCache, pMig->migTracker);
            }
            _disconnect_mig_completely(pMig, NULL);
        }
    }
}

//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock, before
// calling this routine.
//
static NV_STATUS _create_or_check_channel(UvmCommitRecord * pRecord)
{
    UvmPerProcessGpuMigs *pMig;
    unsigned index;
    NV_STATUS rmStatus;

    if (uvmlite_find_gpu_index(&pRecord->homeGpuUuid, &index) != NV_OK)
    {
        rmStatus = NV_ERR_OBJECT_NOT_FOUND;
        goto fail;
    }

    pRecord->cachedHomeGpuPerProcessIndex = index;

    pMig = &pRecord->osPrivate->processRecord.gpuMigs[index];
    if (pMig->migTracker != NULL)
    {
        // Re-using an already created mig tracker
        return NV_OK;
    }

    if (_is_mps_client(&pRecord->osPrivate->processRecord))
    {
        UvmProcessRecord *mpsServerProcess = NULL;

        if (!_lock_mps_server(&pRecord->osPrivate->processRecord))
        {
            rmStatus = NV_ERR_OBJECT_NOT_FOUND;
            goto fail;
        }

        mpsServerProcess = pRecord->osPrivate->processRecord.mpsServer->processRecord;

        // Use the MIG tracker that the server created
        if (!mpsServerProcess->gpuMigs[index].migTracker)
        {
            _unlock_mps_server(&pRecord->osPrivate->processRecord);
            rmStatus = NV_ERR_OBJECT_NOT_FOUND;
            goto fail;
        }

        pMig->migTracker = mpsServerProcess->gpuMigs[index].migTracker;
        _unlock_mps_server(&pRecord->osPrivate->processRecord);

        return NV_OK;
    }

    // Got a free slot, need to create the first mig tracker for this gpu
    pMig->migTracker = kmem_cache_zalloc(g_uvmMigTrackerCache,
                                         NV_UVM_GFP_FLAGS);
    if (pMig->migTracker == NULL)
    {
        rmStatus = NV_ERR_NO_MEMORY;
        goto fail;
    }

    rmStatus = _create_migration_resources(&pRecord->homeGpuUuid,
                                           pMig->migTracker);
    if (NV_OK != rmStatus)
        goto fail_and_cleanup_mig;

    return rmStatus;

fail_and_cleanup_mig:
    kmem_cache_free(g_uvmMigTrackerCache, pMig->migTracker);
    _disconnect_mig_completely(pMig, pRecord);
fail:
    pRecord->cachedHomeGpuPerProcessIndex = UVM_INVALID_HOME_GPU_INDEX;
    return rmStatus;
}

static UvmStreamRecord * _stream_find_in_cache(UvmProcessRecord *processRecord,
                                               UvmStream streamId)
{
    UvmStreamRecord *stream;
    int cacheIndex = streamId % UVM_STREAMS_CACHE_SIZE;

    stream = processRecord->streamsCache[cacheIndex];

    if (stream && stream->streamId == streamId)
        return stream;

    return NULL;
}

static void _stream_remove_from_cache(UvmStreamRecord *pStream)
{
    int cacheIndex = pStream->streamId % UVM_STREAMS_CACHE_SIZE;
    if (pStream->processRecord->streamsCache[cacheIndex] == pStream)
        pStream->processRecord->streamsCache[cacheIndex] = NULL;
}

static void _stream_save_in_cache(UvmStreamRecord *pStream)
{
    int cacheIndex;

    if (!pStream)
        return;

    cacheIndex = pStream->streamId % UVM_STREAMS_CACHE_SIZE;
    pStream->processRecord->streamsCache[cacheIndex] = pStream;
}

static UvmStreamRecord * _stream_find_in_list(UvmProcessRecord *processRecord,
                                              UvmStream streamId)
{
    struct list_head *pos;
    UvmStreamRecord *pStream = NULL;

    list_for_each(pos, &processRecord->allStreamList)
    {
        pStream = list_entry(pos, UvmStreamRecord, allStreamListNode);
        if (pStream->streamId == streamId)
            return pStream;
    }

    return NULL;
}

static UvmStreamRecord * _stream_alloc(UvmProcessRecord *processRecord,
                                       UvmStream streamId)
{
    UvmStreamRecord *pStream = NULL;

    UVM_DBG_PRINT_RL("stream %lld\n", streamId);

    pStream = (UvmStreamRecord*)kmem_cache_zalloc(g_uvmStreamRecordCache,
                                                  NV_UVM_GFP_FLAGS);
    if (pStream == NULL)
        return NULL;

    INIT_LIST_HEAD(&pStream->allStreamListNode);
    INIT_LIST_HEAD(&pStream->commitRecordsList);

    pStream->processRecord = processRecord;
    pStream->streamId       = streamId;
    pStream->isRunning      = NV_FALSE;

    list_add_tail(&pStream->allStreamListNode, &processRecord->allStreamList);

    return pStream;
}

static UvmStreamRecord * _stream_find(UvmProcessRecord *processRecord,
                                      UvmStream streamId)
{
    UvmStreamRecord *pStream = _stream_find_in_cache(processRecord, streamId);

    if (pStream == NULL)
        pStream = _stream_find_in_list(processRecord, streamId);

    return pStream;
}

static UvmStreamRecord * _stream_find_or_alloc(UvmProcessRecord *processRecord,
                                               UvmStream streamId)
{
    UvmStreamRecord *pStream = _stream_find(processRecord, streamId);

    if (pStream == NULL)
        pStream = _stream_alloc(processRecord, streamId);

    if (pStream == NULL)
        return NULL;

    _stream_save_in_cache(pStream);

    return pStream;
}

static void _record_detach_from_stream(UvmCommitRecord *pRecord)
{
    list_del(&pRecord->streamRegionsListNode);
    _stream_destroy_if_empty(pRecord->pStream);
    pRecord->pStream = NULL;
}

NV_STATUS
uvmlite_attach_record_portion_to_stream(UvmCommitRecord *pRecord,
                                        UvmStream newStreamId,
                                        UvmRegionTracker *pRegionTracker,
                                        unsigned long long start,
                                        unsigned long long length)
{
    NV_STATUS rmStatus = NV_OK;

    UvmCommitRecord * pSubRecord = pRecord;
    if (pRecord->hasChildren)
    {
        // In this case the user is asking UVM to do the following action:
        // from: [------------S1----------][-----------S2---------]
        // to:   [------S1------][---New Stream----][-------S2----]
        unsigned long long ptr = start;
        unsigned long long end = start + length;
        while (ptr < end && rmStatus == NV_OK)
        {
            rmStatus = uvm_get_owner_from_address(pRegionTracker,
                                                  ptr,
                                                  &pSubRecord);

            if (rmStatus != NV_OK || !pSubRecord)
                goto done;

            if (pSubRecord->baseAddress >= start &&
                pSubRecord->baseAddress + pSubRecord->length <= end)
            {
                // from: [------S1------][---New Stream----][-----S2------]
                // to:   [------S1------][---Old Stream----][-----S2------]
                // Nothing special to do just set the stream at the end
            }
            else if (pSubRecord->baseAddress <= start &&
                     pSubRecord->baseAddress + pSubRecord->length <= end)
            {
                // from: [-----------Old Stream------------]
                // to:   [---Old Stream---][---New Stream--]
                rmStatus = uvmlite_split_commit_record(
                    pSubRecord,
                    pRegionTracker,
                    pSubRecord->baseAddress + pSubRecord->length - start,
                    NULL, &pSubRecord);
            }
            else if (pSubRecord->baseAddress >= start &&
                     pSubRecord->baseAddress + pSubRecord->length >= end)
            {
                // from: [-----------Old Stream------------]
                // to:   [---New Stream---][---Old Stream--]
                rmStatus = uvmlite_split_commit_record(
                    pSubRecord,
                    pRegionTracker,
                    pSubRecord->baseAddress - end,
                    &pSubRecord, NULL);
            }
            if (rmStatus == NV_OK)
                rmStatus = uvmlite_region_set_stream(pSubRecord, newStreamId);
            ptr += pSubRecord->length;
        }
    }
    else
    {
        // In this case the user is asking UVM to do the following action:
        // from: [-------------------Old Stream-------------------]
        // to:   [--Old Stream--][---New Stream----][-Old Stream--]
        if (start != pRecord->baseAddress)
            rmStatus = uvmlite_split_commit_record(
                pSubRecord,
                pRegionTracker,
                length,
                NULL, &pSubRecord);
        if (start + length != pRecord->baseAddress + pRecord->length)
            rmStatus = uvmlite_split_commit_record(
                pSubRecord,
                pRegionTracker,
                length,
                &pSubRecord, NULL);
        rmStatus = uvmlite_region_set_stream(pSubRecord, newStreamId);
    }
done:
    return rmStatus;
}

static NV_STATUS _record_attach_to_stream(UvmCommitRecord *pRecord,
                                          UvmStreamRecord *pNewStream)
{
    NV_STATUS status = NV_OK;
    UvmStreamRecord *pOldStream = pRecord->pStream;
    NvBool runningStateChanged = NV_TRUE;

    if (pOldStream && (pOldStream->isRunning == pNewStream->isRunning))
    {
        // No need to change the state if the record's old stream is in
        // the same state as the new stream.
        runningStateChanged = NV_FALSE;
    }

    if (runningStateChanged)
    {
        if (pNewStream->isRunning)
        {
            // Mark the record as inaccessible.
            _set_record_inaccessible(pRecord);
            //
            // Attaching to a running stream from a stopped stream needs
            // to trigger migration to the gpu.
            //
            status = uvmlite_migrate_to_gpu(pRecord->baseAddress,
                                               pRecord->length,
                                               0, // flags
                                               pRecord->vma,
                                               pRecord);
        }
        else
            _set_record_accessible(pRecord);
    }

    list_del(&pRecord->streamRegionsListNode);
    list_add_tail(&pRecord->streamRegionsListNode,
        &pNewStream->commitRecordsList);
    pRecord->pStream = pNewStream;

    _stream_destroy_if_empty(pOldStream);

    return status;
}

//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock
// and mmap_sem, before calling this routine.
//
UvmCommitRecord *
uvmlite_create_commit_record(unsigned long long  requestedBase,
                             unsigned long long  length,
                             DriverPrivate *pPriv,
                             struct vm_area_struct *vma)

{
    NV_STATUS rmStatus;
    UvmCommitRecord *pRecord = NULL;
    // The commitRecordPages array stores one pointer to each page:
    NvLength arrayByteLen = sizeof(pRecord->commitRecordPages[0])
                                    * (PAGE_ALIGN(length) / PAGE_SIZE);

    pRecord = (UvmCommitRecord*)kmem_cache_zalloc(g_uvmCommitRecordCache,
                                                  NV_UVM_GFP_FLAGS);
    if (NULL == pRecord)
    {
        UVM_ERR_PRINT("kmem_cache_zalloc(g_uvmCommitRecordCache) failed.\n");
        goto fail;
    }

    // Be sure to initialize the list, so that
    // uvmlite_destroy_commit_record always works:
    INIT_LIST_HEAD(&pRecord->streamRegionsListNode);
    pRecord->baseAddress = requestedBase;
    pRecord->length      = length;
    pRecord->osPrivate   = pPriv;
    pRecord->vma         = vma;
    pRecord->cachedHomeGpuPerProcessIndex = UVM_INVALID_HOME_GPU_INDEX;
    pRecord->isAccessible = NV_TRUE;
    pRecord->isChild = NV_FALSE;
    pRecord->hasChildren = NV_FALSE;

    pRecord->commitRecordPages = vmalloc(arrayByteLen);
    if (!pRecord->commitRecordPages)
    {
        UVM_ERR_PRINT("vmalloc(%llu) for commitRecordPages failed.\n",
                      (unsigned long long)arrayByteLen);
        goto fail;
    }

    memset(pRecord->commitRecordPages, 0, arrayByteLen);

    rmStatus = uvmlite_init_prefetch_info(&pRecord->prefetchInfo, pRecord);
    if (rmStatus != NV_OK)
        goto fail;

    UVM_DBG_PRINT_RL("vma: 0x%p: pRecord: 0x%p, length: %llu\n",
                     vma, pRecord, length);
    return pRecord;

fail:
    uvmlite_destroy_commit_record(pRecord);
    return NULL;
}

//
// This routine assigns streamID and GPU UUID to the pRecord, and sets up a Copy
// Engine channel as well. (The Copy Engine is what actually does the memory
// migration to and from CPU and GPU.)
//
// Locking: you must already have acquired a write lock on these:
//      mmap_sem
//      DriverPrivate.uvmPrivLock
//
NV_STATUS uvmlite_update_commit_record(UvmCommitRecord *pRecord,
                                       UvmStream streamId,
                                       NvProcessorUuid *pUuid,
                                       DriverPrivate *pPriv)
{
    UvmStreamRecord *pStream = NULL;
    NV_STATUS rmStatus;

    UVM_PANIC_ON(!pRecord);
    pRecord->cachedHomeGpuPerProcessIndex = UVM_INVALID_HOME_GPU_INDEX;

    memcpy(pRecord->homeGpuUuid.uuid, pUuid, sizeof(pRecord->homeGpuUuid.uuid));
    //
    // The resulting resources from this call are cleaned up at process exit, as
    // part of the _destroy_migration_resources() call:
    //
    rmStatus = _create_or_check_channel(pRecord);
    if (NV_OK != rmStatus)
        goto fail;

    pStream = _stream_find_or_alloc(&pPriv->processRecord, streamId);
    if (!pStream)
    {
        rmStatus = NV_ERR_NO_MEMORY;
        goto fail;
    }

    UVM_DBG_PRINT_RL("vma: 0x%p: updated pRecord: 0x%p, stream: %llu\n",
                     pRecord->vma, pRecord, streamId);

    _record_attach_to_stream(pRecord, pStream);
    return rmStatus;

fail:
    uvmlite_destroy_commit_record(pRecord);
    return rmStatus;
}

NV_STATUS
uvmlite_split_commit_record(UvmCommitRecord *pRecord,
                            UvmRegionTracker *pTracker,
                            unsigned long long splitPoint,
                            UvmCommitRecord **outRecordLeft,
                            UvmCommitRecord **outRecordRight)
{
    UvmCommitRecord *pRecordLeft;
    UvmCommitRecord *pRecordRight;
    unsigned long long splitPointAlign = PAGE_ALIGN(splitPoint);
    unsigned long long splitPageIndex = splitPointAlign >> PAGE_SHIFT;

    if (!pRecord)
        return NV_ERR_INVALID_ARGUMENT;

    UVM_DBG_PRINT_RL("split: [0x%llx, 0x%llx) [0x%llx, 0x%llx)\n",
                    pRecord->baseAddress,
                    pRecord->baseAddress + splitPoint,
                    pRecord->baseAddress + splitPoint,
                    pRecord->baseAddress + pRecord->length);

    pRecordLeft = uvmlite_create_commit_record(pRecord->baseAddress,
                                               splitPointAlign,
                                               pRecord->osPrivate,
                                               pRecord->vma);
    if (!pRecordLeft)
        return NV_ERR_NO_MEMORY;
    pRecordLeft->isChild = NV_TRUE;
    pRecordLeft->pStream = pRecord->pStream;
    memcpy(&pRecordLeft->homeGpuUuid, &pRecord->homeGpuUuid,
           sizeof(pRecordLeft->homeGpuUuid));
    pRecordLeft->cachedHomeGpuPerProcessIndex =
        pRecord->cachedHomeGpuPerProcessIndex;

    // Use the page tracker of the parent commit
    vfree(pRecordLeft->commitRecordPages);
    pRecordLeft->commitRecordPages =
        &pRecord->commitRecordPages[0];

    pRecordRight =
        uvmlite_create_commit_record(pRecord->baseAddress + splitPointAlign,
                                     pRecord->length - (splitPointAlign),
                                     pRecord->osPrivate, pRecord->vma);
    if (!pRecordRight)
    {
        uvmlite_destroy_commit_record(pRecordLeft);
        return NV_ERR_NO_MEMORY;
    }
    pRecordRight->isChild = NV_TRUE;
    pRecordRight->pStream = pRecord->pStream;
    memcpy(&pRecordRight->homeGpuUuid, &pRecord->homeGpuUuid,
           sizeof(pRecordRight->homeGpuUuid));
    pRecordRight->cachedHomeGpuPerProcessIndex =
        pRecord->cachedHomeGpuPerProcessIndex;

    // Use the page tracker of the parent commit
    vfree(pRecordRight->commitRecordPages);
    pRecordRight->commitRecordPages =
        &pRecord->commitRecordPages[splitPageIndex];

    // Detach the stream from the parent commit
    list_del(&pRecord->streamRegionsListNode);
    pRecord->pStream = NULL;

    // Attach the stream to the two subcommit
    list_add_tail(&pRecordLeft->streamRegionsListNode,
                  &pRecordLeft->pStream->commitRecordsList);
    list_add_tail(&pRecordRight->streamRegionsListNode,
                  &pRecordRight->pStream->commitRecordsList);

    pRecord->hasChildren = NV_TRUE;

    // Add the new regions to the tree
    uvm_track_region(pTracker,
                     pRecordLeft->baseAddress,
                     pRecordLeft->baseAddress + pRecordLeft->length,
                     NULL, pRecordLeft);
    uvm_track_region(pTracker,
                     pRecordRight->baseAddress,
                     pRecordRight->baseAddress + pRecordRight->length,
                     NULL, pRecordRight);

    if (outRecordLeft)
        *outRecordLeft = pRecordLeft;
    if (outRecordRight)
        *outRecordRight = pRecordRight;

    return NV_OK;
}
//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock, before
// calling this routine.
//
// CAUTION: returns NULL upon failure. This failure can occur if:
//
//     a) there is no valid migration tracking resource available.
//
//     b) The UvmCommitRecord argument is corrupt (NULL osPrivate pointer).
//
static UvmGpuMigrationTracking * _get_mig_tracker(UvmCommitRecord * pRecord)
{
    unsigned migIndex;

    UVM_PANIC_ON((pRecord == NULL) || (pRecord->osPrivate == NULL));

    if ((pRecord == NULL) || (pRecord->osPrivate == NULL))
        return NULL;

    migIndex = pRecord->cachedHomeGpuPerProcessIndex;
    if (migIndex == UVM_INVALID_HOME_GPU_INDEX)
        return NULL;

    return pRecord->osPrivate->processRecord.gpuMigs[migIndex].migTracker;
}

static NV_STATUS _prefetch_alloc_pages(UvmCommitRecord *pRecord,
                                       NvLength begin,
                                       NvLength count,
                                       NvLength *allocPages)
{
    DriverPrivate *pPriv;

    NvLength pageIndex;
    UvmPageTracking *pTracking;

    UVM_PANIC_ON(!allocPages);

    pPriv = pRecord->osPrivate;

    *allocPages = 0;

    for (pageIndex = begin; pageIndex < begin + count; ++pageIndex)
    {
        pTracking = pRecord->commitRecordPages[pageIndex];
        if (!pTracking)
        {
            pTracking = uvm_page_cache_alloc_page(pPriv);
            if (!pTracking)
                return NV_ERR_NO_MEMORY;

            pRecord->commitRecordPages[pageIndex] = pTracking;
            UVM_PAGE_SET_OUTDATED(pTracking->uvmPage);
            ++(*allocPages);
        }
    }

    return NV_OK;
}


//
// This routine queries the prefetcher and implements page prefetching.
// Prefetching is performed in 3 steps:
//
//   1. Allocate those pages that are needed. Set the outdated bit.
//   2. Migrate pages from GPU to CPU memory. Only pages with the outdated bit
//   set are transferred.
//   3. Clear the outdated bit and insert VM mappings (by calling
//   vm_insert_page).
//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock and
// a read lock on mmap_sem, before calling this routine.
//
static void prefetch_on_fault(UvmCommitRecord *pRecord,
                              UvmGpuMigrationTracking *pMigTracker,
                              unsigned homeGpu,
                              unsigned long pageFaultIndex)
{
    NV_STATUS rmStatus;
    DriverPrivate *pPriv;

    int ret;
    UvmPageTracking *pTracking;
    unsigned long pageIndex;
    NvLength allocPages = 0;
    NvLength maxPages;

    NvBool prefetch;
    UvmPrefetchHint prefetchHint;

    NvLength migratedPages;

    if (!uvm_prefetch)
        return;

    pPriv = pRecord->osPrivate;

    // Test the prefetcher
    prefetch = uvmlite_prefetch_log_major_fault(&pRecord->prefetchInfo, pRecord,
                                                pageFaultIndex, &prefetchHint);
    if (!prefetch)
        return;

    maxPages = pRecord->length >> PAGE_SHIFT;
    UVM_PANIC_ON(prefetchHint.baseEntry + prefetchHint.count > maxPages);

    //
    // Phase 1: alloc pages
    //
    rmStatus = _prefetch_alloc_pages(pRecord, prefetchHint.baseEntry,
                                     prefetchHint.count, &allocPages);
    if (rmStatus != NV_OK)
        goto fail;

    //
    // Phase 2: migrate pages
    //

    // Copy the pages to CPU synchronously
    rmStatus = migrate_gpu_to_cpu(pMigTracker, pRecord, prefetchHint.baseEntry,
                                  prefetchHint.count, UVM_MIGRATE_OUTDATED_ONLY,
                                  &migratedPages);
    if (rmStatus != NV_OK)
        goto fail;
    // Update counters. Increment both general transfer and prefetching counters
    uvm_increment_process_counters(homeGpu,
                                   pPriv->processRecord.pCounterContainer,
                                   UvmCounterNamePrefetchBytesXferDtH,
                                   PAGE_SIZE * migratedPages);

    uvm_increment_process_counters(homeGpu,
                                   pPriv->processRecord.pCounterContainer,
                                   UvmCounterNameBytesXferDtH,
                                   PAGE_SIZE * migratedPages);

    UVM_PANIC_ON(migratedPages != allocPages);

    //
    // Phase 3: insert vm mappings
    //
    for (pageIndex = prefetchHint.baseEntry;
         pageIndex < prefetchHint.baseEntry + prefetchHint.count; ++pageIndex)
    {
        pTracking = pRecord->commitRecordPages[pageIndex];
        if (pTracking && UVM_PAGE_OUTDATED(pTracking->uvmPage))
        {
            if (!uvm_prefetch_stats)
            {
                // Register the mapping
                ret = vm_insert_page(pRecord->vma,
                                     pRecord->baseAddress + (pageIndex << PAGE_SHIFT),
                                     pTracking->uvmPage);
                if (ret != 0)
                    goto fail;

                // Decrement because the current page will not be freed on a failure
                --allocPages;
            }
            UVM_PAGE_CLEAR_OUTDATED(pTracking->uvmPage);
            // Inform the prefetcher we were able to prefetch this page
            uvmlite_prefetch_page_ack(&pRecord->prefetchInfo, pageIndex);
        }
    }

    return;

fail:
    // Free all pages allocated in this routine (we use the Checked bit)
    for (pageIndex = prefetchHint.baseEntry;
         pageIndex < prefetchHint.baseEntry + prefetchHint.count;
         ++pageIndex)
    {
        pTracking = pRecord->commitRecordPages[pageIndex];
        if (pTracking && UVM_PAGE_OUTDATED(pTracking->uvmPage))
        {
            UVM_PAGE_CLEAR_OUTDATED(pTracking->uvmPage);
            // Free the allocated page
            uvm_page_cache_free_page(pTracking, __FUNCTION__);
            // Do not fail, since the fault was correctly handled
            pRecord->commitRecordPages[pageIndex] = NULL;

            --allocPages;
        }
    }
    UVM_PANIC_ON(allocPages != 0);
}

//
// Page fault handler for UVM-Lite
//
// Locking:
//     1. the kernel calls into this routine with the mmap_sem lock taken.
//     2. this routine acquires a write lock on the DriverPrivate.uvmPrivLock.
//
// Write fault algorithm:
//
//      1. Lookup address in page tracking
//      2. If page exists, mark as dirty, and we are done.
//      3. else, do "read fault" (below), then mark the page as dirty.
//
// Read fault algorithm:
//
//      1. Lookup which stream owns the address
//      2. See if that stream is active: if so, UVM-Lite rules are violated, so
//         return SIGBUS.
//      3. Otherwise, map in a page from the cache, and allow access.
//      4. Trigger page prefetching
//
int _fault_common(struct vm_area_struct *vma, unsigned long vaddr,
                  struct page **ppage, unsigned vmfFlags)
{
    int retValue = VM_FAULT_SIGBUS;
    UvmCommitRecord *pRecord;
    DriverPrivate *pPriv;
    NV_STATUS rmStatus;
    UvmGpuMigrationTracking *pMigTracker;
    unsigned long pageIndex = 0;
    unsigned homeGpu = 0;
    UvmPageTracking *pTracking = NULL;
    NvLength migratedPages = 0;
    UvmRegionTracker* tracking_tree = (UvmRegionTracker*)vma->vm_private_data;
    NvU8 accessType;
    NvU64 faultTime = 0;

    if (!tracking_tree)
        goto done; // vma has been shut down

    if (NV_OK != uvm_get_owner_from_address(tracking_tree, vaddr, &pRecord))
        goto done;

    pPriv = tracking_tree->osPrivate;
    UVM_PANIC_ON(!pPriv);
    if (!pPriv)
        goto done;

    down_write(&pPriv->uvmPrivLock);
    if (_is_mps_client(&pPriv->processRecord))
    {
        if (!_lock_mps_server(&pPriv->processRecord))
            goto fail_release_lock;
    }

    if (_is_record_included_in_vma(pRecord) == NV_FALSE)
    {
        //
        // The VMA has been modified since the record was created, skip it.
        // This should never be possible as we destroy the records with
        // modified vmas in mmap_open().
        //
        UVM_PANIC();
        goto fail;
    }

    pageIndex = (vaddr - pRecord->baseAddress) >> PAGE_SHIFT;
    pTracking = pRecord->commitRecordPages[pageIndex];

    // The record is not accessible so we have to exit and return a SIGBUS
    if (!pRecord->isAccessible)
    {
        UVM_DBG_PRINT_RL("FAULT_INACCESSIBLE: vaddr: 0x%p, vma: 0x%p\n",
                         (void*)vaddr, vma);

        if (uvm_is_event_enabled(
                pRecord->osPrivate->processRecord.pEventContainer,
                UvmEventTypeMemoryViolation))
        {
            faultTime = NV_GETTIME();

            accessType = (vmfFlags & NV_FAULT_FLAG_WRITE) ?
                         UvmEventMemoryAccessTypeWrite:
                         UvmEventMemoryAccessTypeRead;

            rmStatus = uvm_record_memory_violation_event(
                           pRecord->osPrivate->processRecord.pEventContainer,
                           accessType,
                           vaddr,
                           faultTime,
                           uvm_get_stale_process_id(),
                           uvm_get_stale_thread_id());

            if (rmStatus != NV_OK)
            {
                UVM_ERR_PRINT("Failed to record memory violation event at 0x%p,"
                              " rmStatus: 0x%0x\n", (void*)vaddr, rmStatus);
            }
        }

        goto fail;
    }

    UVM_DBG_PRINT_RL("FAULT_ENTRY: vaddr: 0x%p, vma: 0x%p\n",
                     (void*)vaddr, vma);

    if (pRecord->cachedHomeGpuPerProcessIndex == UVM_INVALID_HOME_GPU_INDEX)
        goto fail;

    homeGpu = pRecord->cachedHomeGpuPerProcessIndex;

    if (!pTracking)
    {
        pTracking = uvm_page_cache_alloc_page(pPriv);
        if (!pTracking)
        {
            retValue = VM_FAULT_OOM;
            goto fail;
        }
        UVM_DBG_PRINT_RL("FAULT_ALLOC: vaddr: 0x%p, vma: 0x%p, pRecord: 0x%p\n",
                         (void*)vaddr, vma, pRecord);

        pRecord->commitRecordPages[pageIndex] = pTracking;

        pMigTracker = _get_mig_tracker(pRecord);
        UVM_PANIC_ON(!pMigTracker);
        if (!pMigTracker)
            goto fail;

        rmStatus = migrate_gpu_to_cpu(pMigTracker, pRecord, pageIndex, 1,
                                      UVM_MIGRATE_DEFAULT, &migratedPages);
        if (rmStatus != NV_OK)
        {
            UVM_ERR_PRINT("FAULT: failed to copy from gpu to cpu:"
                          " vaddr:0x%p, vma: 0x%p, rmStatus: 0x%0x\n",
                          (void*)vaddr, vma, rmStatus);
            goto fail;
        }
        UVM_PANIC_ON(migratedPages != 1);
        prefetch_on_fault(pRecord, pMigTracker, homeGpu, pageIndex);

        retValue = VM_FAULT_MAJOR;

        uvm_increment_process_counters(homeGpu,
                                       pPriv->processRecord.pCounterContainer,
                                       UvmCounterNameCpuPageFaultCount,
                                       1);
        uvm_increment_process_counters(homeGpu,
                                       pPriv->processRecord.pCounterContainer,
                                       UvmCounterNameBytesXferDtH,
                                       PAGE_SIZE);
    }
    else
    {
        if (uvm_prefetch_stats)
        {
            // Notify the prefetcher that the page has had a minor fault
            uvmlite_prefetch_log_minor_fault(&pRecord->prefetchInfo, pageIndex);
        }

        //
        // If we already have the page, then we must have earlier copied in the
        // data from the GPU. Therefore, avoid migrating.
        //

        // We used to return VM_FAULT_MINOR here. However, that was deprecated
        // in the kernel, and the new guideline is to return 0 in case of a
        // minor fault. The VM_FAULT_MINOR symbol itself was removed in
        // March, 2016 with commit 0e8fb9312fbaf1a687dd731b04d8ab3121c4ff5a.
        retValue = 0;
    }

    // Increment the page usage count since the kernel automatically
    // decrements it.
    get_page(pTracking->uvmPage);

    pRecord->isMapped = NV_TRUE;

    UVM_DBG_PRINT_RL("FAULT HANDLED: vaddr: 0x%p, vma: 0x%p, "
                     "pfn:0x%lx, refcount: %d\n",
                     (void*)vaddr, vma, page_to_pfn(pTracking->uvmPage),
                     page_count(pTracking->uvmPage));

    *ppage = pTracking->uvmPage;
    if (_is_mps_client(&pPriv->processRecord))
    {
        _unlock_mps_server(&pPriv->processRecord);
    }
    up_write(&pPriv->uvmPrivLock);
    return retValue;

fail:
    if (pTracking)
    {
        uvm_page_cache_free_page(pTracking, __FUNCTION__);
        pRecord->commitRecordPages[pageIndex] = NULL;
    }

    if (_is_mps_client(&pPriv->processRecord))
    {
        _unlock_mps_server(&pPriv->processRecord);
    }
fail_release_lock:
    up_write(&pPriv->uvmPrivLock);
done:
    return retValue;
}

#if defined(NV_VM_OPERATIONS_STRUCT_HAS_FAULT)
int _fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
    unsigned long vaddr = (unsigned long)vmf->virtual_address;
    struct page *page = NULL;
    int retval;

    retval = _fault_common(vma, vaddr, &page, vmf->flags);

    vmf->page = page;

    return retval;
}

#else
struct page * _fault_old_style(struct vm_area_struct *vma,
                               unsigned long address, int *type)
{
    unsigned long vaddr = address;
    struct page *page = NULL;

    *type = _fault_common(vma, vaddr, &page, FAULT_FLAG_FROM_OLD_KERNEL);

    return page;
}
#endif

static struct vm_operations_struct uvmlite_vma_ops =
{
    .open = _mmap_open,
    .close = _mmap_close,

#if defined(NV_VM_OPERATIONS_STRUCT_HAS_FAULT)
    .fault = _fault,
#else
    .nopage = _fault_old_style,
#endif
};

//
// Counters feature doesn't support fault handler. However,
// without setting vma_ops and fault handler, Linux kernel assumes
// it's dealing with anonymous mapping (see handle_pte_fault).
//
#if defined(NV_VM_OPERATIONS_STRUCT_HAS_FAULT)
int _sigbus_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
    vmf->page = NULL;
    return VM_FAULT_SIGBUS;
}

#else
struct page * _sigbus_fault_old_style(struct vm_area_struct *vma,
                                      unsigned long address, int *type)
{
    *type = VM_FAULT_SIGBUS;
    return NULL;
}
#endif

static struct vm_operations_struct counters_vma_ops =
{
#if defined(NV_VM_OPERATIONS_STRUCT_HAS_FAULT)
    .fault = _sigbus_fault,
#else
    .nopage = _sigbus_fault_old_style,
#endif
};

//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock, before
// calling this routine.
//
NV_STATUS uvmlite_migrate_to_gpu(unsigned long long baseAddress,
                                 NvLength length,
                                 unsigned migrateFlags,
                                 struct vm_area_struct *vma,
                                 UvmCommitRecord * pRecord)
{
    NV_STATUS rmStatus = NV_OK;
    UvmGpuMigrationTracking *pMigTracker;
    NvLength migratedPages = 0;

    if (!pRecord || !vma)
        return NV_ERR_INVALID_ARGUMENT;

    UVM_PANIC_ON(pRecord->vma != vma); // Serious object model corruption occurred.
    UVM_PANIC_ON(pRecord->baseAddress != baseAddress);
    UVM_PANIC_ON(PAGE_ALIGN(pRecord->length) != PAGE_ALIGN(length));

    pMigTracker = _get_mig_tracker(pRecord);

    if (pMigTracker == NULL)
    {
        return NV_ERR_GPU_DMA_NOT_INITIALIZED;
    }

    UVM_PANIC_ON(NULL == pRecord->osPrivate);
    UVM_PANIC_ON(NULL == pRecord->osPrivate->privFile);
    UVM_PANIC_ON(NULL == pRecord->osPrivate->privFile->f_mapping);

    // If this record has no pages mapped, then we can early out
    if (!pRecord->isMapped)
    {
        return NV_OK;
    }

    if (pRecord->length > 0)
    {
        unmap_mapping_range(pRecord->osPrivate->privFile->f_mapping,
                            pRecord->baseAddress, pRecord->length, 1);
        pRecord->isMapped = NV_FALSE;
    }
    //
    // Copy required pages from CPU to GPU. We try to pipeline these copies
    // to get maximum performance from copy engine.
    //
    if (_is_mps_client(&pRecord->osPrivate->processRecord))
    {
        if (!_lock_mps_server(&pRecord->osPrivate->processRecord))
            return NV_ERR_GENERIC;
    }
    rmStatus = migrate_cpu_to_gpu(pMigTracker, pRecord,
                                  0, pRecord->length >> PAGE_SHIFT,
                                  &migratedPages);

    if (_is_mps_client(&pRecord->osPrivate->processRecord))
    {
        _unlock_mps_server(&pRecord->osPrivate->processRecord);
    }
    if (rmStatus == NV_OK)
    {
        _update_gpu_migration_counters(pRecord, migratedPages);

        rmStatus = _clear_cache(pRecord);
        if (rmStatus != NV_OK)
        {
            UVM_DBG_PRINT_RL("Failed to _clear_cache: rmStatus: 0x%0x\n",
                             rmStatus);
        }

        // Because the entire commit record has been migrated to the gpu, reset
        // the prefetch info:
        uvmlite_reset_prefetch_info(&pRecord->prefetchInfo, pRecord);
    }

    return rmStatus;
}

//
// SetStreamRunning (cuda kernel launch) steps:
//
//    For each region attached to the stream ID, or to the all-stream:
//
//        1. unmap page range from user space
//
//        2. copy cpu to gpu, for dirty pages only (lots of room for
//        optimization here)
//
//        3. Free pages from page cache
//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock
// and mmap_sem, before calling this routine.
//
static NV_STATUS _set_stream_running(DriverPrivate* pPriv, UvmStream streamId)
{
    struct list_head * pos;
    UvmCommitRecord * pRecord;
    UvmStreamRecord * pStream;
    NV_STATUS rmStatus = NV_OK;
    UvmProcessRecord *processRecord = &pPriv->processRecord;

    UVM_DBG_PRINT_RL("stream %lld\n", streamId);

    // This might be the first time we see this streamId
    pStream = _stream_find_or_alloc(processRecord, streamId);

    if (pStream->isRunning)
    {
        // Stream is in running state already
        return NV_OK;
    }

    list_for_each(pos, &pStream->commitRecordsList)
    {
        pRecord = list_entry(pos, UvmCommitRecord, streamRegionsListNode);
        UVM_DBG_PRINT_RL("committed region baseAddr: 0x%p, len: 0x%llx\n",
                         (void*)pRecord->baseAddress, pRecord->length);

        if (!_is_record_included_in_vma(pRecord))
        {
            //
            // The VMA has been modified since the record was created, skip it.
            // This should never be possible as we destroy the records with
            // modified vmas in mmap_open().
            //
            UVM_PANIC();
            continue;
        }
        // Mark the record as inaccessible.
        _set_record_inaccessible(pRecord);

        rmStatus = uvmlite_migrate_to_gpu(pRecord->baseAddress,
                                           pRecord->length,
                                           0, // flags
                                           pRecord->vma,
                                           pRecord);
        if (rmStatus != NV_OK)
            goto done;
    }

    if (streamId != UVM_STREAM_ALL)
    {
        // Increment the running streams count
        ++processRecord->runningStreams;
        if (processRecord->runningStreams == 1)
        {
            // First stream to be started needs to also start the all stream
            rmStatus = _set_stream_running(pPriv, UVM_STREAM_ALL);
            if (rmStatus != NV_OK)
                goto done;
        }
    }

    pStream->isRunning = NV_TRUE;

done:
    return rmStatus;
}

static NvBool _is_special_stream(UvmStream streamId)
{
    return streamId == UVM_STREAM_INVALID ||
           streamId == UVM_STREAM_ALL     ||
           streamId == UVM_STREAM_NONE;
}

//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock
// and mmap_sem, before calling this routine.
//
NV_STATUS uvmlite_set_stream_running(DriverPrivate* pPriv, UvmStream streamId)
{
    if (_is_special_stream(streamId))
        return NV_ERR_INVALID_ARGUMENT;

    return _set_stream_running(pPriv, streamId);
}

static NV_STATUS _set_stream_stopped(DriverPrivate *pPriv,
                                     UvmStream streamId)
{
    struct list_head * pos;
    UvmCommitRecord * pRecord;
    UvmStreamRecord * pStream;
    NV_STATUS rmStatus = NV_OK;
    UvmProcessRecord *processRecord = &pPriv->processRecord;

    UVM_DBG_PRINT_RL("stream %lld\n", streamId);

    pStream = _stream_find(processRecord, streamId);
    if (!pStream)
    {
        // The stream has never been started
        return NV_ERR_INVALID_ARGUMENT;
    }

    _stream_save_in_cache(pStream);

    if (!pStream->isRunning)
    {
        // Stream is in stopped state already
        return NV_OK;
    }

    list_for_each(pos, &pStream->commitRecordsList)
    {
        pRecord = list_entry(pos, UvmCommitRecord, streamRegionsListNode);
        UVM_DBG_PRINT_RL("committed region baseAddr: 0x%p, len: 0x%llx\n",
                         (void*)pRecord->baseAddress, pRecord->length);

        if (_is_record_included_in_vma(pRecord) == NV_FALSE)
        {
            //
            // The VMA has been modified since the record was created, skip it.
            // This should never be possible as we destroy the records with
            // modified vmas in mmap_open().
            //
            UVM_PANIC();
            continue;
        }

        // Mark the record as inaccessible.
        _set_record_accessible(pRecord);
    }

    if (streamId != UVM_STREAM_ALL)
    {
        // Decrement the running stream count
        --processRecord->runningStreams;
        if (processRecord->runningStreams == 0)
        {
            // Last stream to be stopped needs to also stop the all stream
            rmStatus = _set_stream_stopped(pPriv, UVM_STREAM_ALL);
            if (rmStatus != NV_OK)
                goto done;
        }
    }

    pStream->isRunning = NV_FALSE;

    _stream_destroy_if_empty(pStream);

done:
    return rmStatus;
}

//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock
// and mmap_sem, before calling this routine.
//
NV_STATUS uvmlite_set_streams_stopped(DriverPrivate* pPriv,
                                      UvmStream * streamIdArray,
                                      NvLength nStreams)
{
    NvLength i;
    NV_STATUS rmStatus = NV_OK;

    for (i = 0; i < nStreams; ++i)
    {
        if (_is_special_stream(streamIdArray[i]))
            return NV_ERR_INVALID_ARGUMENT;
    }

    for (i = 0; i < nStreams; ++i)
    {
        rmStatus = _set_stream_stopped(pPriv, streamIdArray[i]);
        if (rmStatus != NV_OK)
            break;
    }

    return rmStatus;
}

static void _stream_destroy_if_empty(UvmStreamRecord *pStream)
{
    if (!pStream)
        return;

    if (pStream->isRunning)
    {
        //
        // Don't destroy running streams even if they are empty as a record
        // might be attached before they are stopped.
        //
        return;
    }

    if (!list_empty(&pStream->commitRecordsList))
    {
        // Don't destroy streams with attached records
        return;
    }

    _stream_destroy(pStream);
}

NV_STATUS uvmlite_region_set_stream(UvmCommitRecord *pRecord,
                                    UvmStream newStreamId)
{
    UvmStreamRecord *pNewStream;
    NV_STATUS status;

    if (pRecord == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    if ((pRecord->pStream) && (pRecord->pStream->streamId == newStreamId))
        return NV_OK;

    pNewStream = _stream_find_or_alloc(&pRecord->osPrivate->processRecord,
                                       newStreamId);
    if (!pNewStream)
        return NV_ERR_INSUFFICIENT_RESOURCES;

    status = _record_attach_to_stream(pRecord, pNewStream);

    return status;
}

//
// Locking: this routine initializes the DriverPrivate.uvmPrivLock.
// thread safety: uvmlite_open has to be used by a single thread,
//
static int uvmlite_open(struct inode *inode, struct file *filp)
{
    DriverPrivate * pUvmPrivate = NULL;
    int retval = -ENOMEM;

    struct address_space *pMapping =
        kmem_cache_alloc(g_uvmMappingCache, NV_UVM_GFP_FLAGS);

    if (!pMapping)
        goto fail;

    pUvmPrivate = (DriverPrivate*)
                  kmem_cache_zalloc(g_uvmPrivateCache, NV_UVM_GFP_FLAGS);

    if (!pUvmPrivate)
        goto fail;
    //
    // UVM-Lite calls unmap_mapping_range, but UVM-Lite has only a single device
    // node, through which all user-space processes do their mmap() calls. In
    // order to avoid interference among unrelated processes, we must set up a
    // separate mapping object for each struct file.
    //
    address_space_init_once(pMapping);
    pMapping->host = inode;
#if defined(NV_ADDRESS_SPACE_HAS_BACKING_DEV_INFO)
    pMapping->backing_dev_info = inode->i_mapping->backing_dev_info;
#endif

    INIT_LIST_HEAD(&pUvmPrivate->pageList);
    INIT_LIST_HEAD(&pUvmPrivate->processRecord.allStreamList);
    INIT_LIST_HEAD(&pUvmPrivate->driverPrivateNode);
    init_rwsem(&pUvmPrivate->uvmPrivLock);
    pUvmPrivate->privFile = filp;
    pUvmPrivate->processRecord.euid = NV_CURRENT_EUID();
    pUvmPrivate->processRecord.mpsProcessType = MPS_NOT_ACTIVE;
    pUvmPrivate->processRecord.mpsServer = NULL;
    pUvmPrivate->processRecord.pCounterContainer = NULL;
    pUvmPrivate->processRecord.pEventContainer = NULL;

    if (NV_OK != uvm_alloc_counter_container(
            &pUvmPrivate->processRecord.pCounterContainer))
       goto fail;
    if (NV_OK != uvm_alloc_event_container(
            &pUvmPrivate->processRecord.pEventContainer))
       goto fail;

    init_rwsem(&pUvmPrivate->processRecord.sessionInfoLock);
    memset(pUvmPrivate->processRecord.sessionInfoArray, 0,
           sizeof (*pUvmPrivate->processRecord.sessionInfoArray));

    filp->private_data = pUvmPrivate;
    filp->f_mapping = pMapping;

    pUvmPrivate->processRecord.pid = uvm_get_stale_process_id();
    // register to global process record table after initialization
    down_write(&g_uvmDriverPrivateTableLock);
    list_add(&pUvmPrivate->driverPrivateNode,
             &g_uvmDriverPrivateTable);
    up_write(&g_uvmDriverPrivateTableLock);

    UVM_DBG_PRINT("pPriv: 0x%p, f_mapping: 0x%p\n",
                  filp->private_data, filp->f_mapping);

    return 0;

fail:
    if (pMapping)
        kmem_cache_free(g_uvmMappingCache, pMapping);

    if (pUvmPrivate)
    {
        uvm_unref_counter_container(
            pUvmPrivate->processRecord.pCounterContainer);
        uvm_unref_event_container(
            pUvmPrivate->processRecord.pEventContainer);
        kmem_cache_free(g_uvmPrivateCache, pUvmPrivate);
    }

    return retval;
}

//
// Locking: you must hold processRecordTableLock read lock
// when you access UvmProcessRecord.
//
static UvmProcessRecord* _find_process_record(unsigned pid)
{
    struct list_head * pos;
    UvmProcessRecord *pProcessRecord;

    list_for_each(pos, &g_uvmDriverPrivateTable)
    {
        pProcessRecord = &list_entry(pos, DriverPrivate,
                                    driverPrivateNode)->processRecord;
        if (pid == pProcessRecord->pid)
            return pProcessRecord;
    }
    return NULL;
}


//
// On success, this routine increments refcount on UmvCounterContainer and on
// UvmEventContainer before returning them.
// Locking:
//     1. This routine acquires the read g_uvmDriverPrivateTableLock.
//
NV_STATUS uvmlite_secure_get_process_containers
(
    unsigned pidTarget,
    UvmCounterContainer **ppCounterContainer,
    UvmEventContainer **ppEventContainer,
    uid_t *pEuid
)
{
    UvmProcessRecord *pProcRec;


    //
    // uvmlite_close can't decrement refcount / remove ProcessCounterInfo
    // structure before grabbing g_uvmDriverPrivateTableLock and removing itself
    // from g_uvmDriverPrivateTable.
    //
    down_read(&g_uvmDriverPrivateTableLock);

    pProcRec = _find_process_record(pidTarget);

    if (pProcRec == NULL)
    {
        up_read(&g_uvmDriverPrivateTableLock);
        return NV_ERR_PID_NOT_FOUND;
    }

    *pEuid = pProcRec->euid;

    if (!uvm_user_id_security_check(*pEuid))
    {
        up_read(&g_uvmDriverPrivateTableLock);
        return NV_ERR_INSUFFICIENT_PERMISSIONS;
    }

    *ppCounterContainer = pProcRec->pCounterContainer;
    *ppEventContainer = pProcRec->pEventContainer;
    uvm_ref_counter_container(*ppCounterContainer);
    uvm_ref_event_container(*ppEventContainer);
    up_read(&g_uvmDriverPrivateTableLock);

    return NV_OK;
}

// The caller must hold a read lock on g_uvmDriverPrivateTableLock.
NV_STATUS uvmlite_get_process_record
(
    unsigned pidTarget,
    UvmProcessRecord **pProcessRecord
)
{
    *pProcessRecord = _find_process_record(pidTarget);

    if (*pProcessRecord == NULL)
        return NV_ERR_PID_NOT_FOUND;

    return NV_OK;
}

//
// Locking:
//     1. This routine acquires the g_uvmDriverPrivateTableLock.
//
static int uvmlite_close(struct inode *inode, struct file *filp)
{
    DriverPrivate* pPriv = (DriverPrivate*)filp->private_data;

    // If it was an MPS server, remove any reference to it before
    // deleting its internal resources
    if (_is_mps_server(&pPriv->processRecord))
    {
        down_write(&g_uvmMpsServersListLock);
        list_del(&pPriv->processRecord.mpsServer->driverPrivateNode);
        up_write(&g_uvmMpsServersListLock);

        down_write(&pPriv->processRecord.mpsServer->mpsLock);
        pPriv->processRecord.mpsServer->dying = NV_TRUE;
        up_write(&pPriv->processRecord.mpsServer->mpsLock);

        kref_put(&pPriv->processRecord.mpsServer->kref, _delete_mps_server);
    }
    else if (_is_mps_client(&pPriv->processRecord))
    {
        kref_put(&pPriv->processRecord.mpsServer->kref, _delete_mps_server);
    }

    // unregister from global process record table before cleanup
    down_write(&g_uvmDriverPrivateTableLock);
    list_del(&pPriv->driverPrivateNode);
    _delete_all_migration_resources(pPriv);
    up_write(&g_uvmDriverPrivateTableLock);

    uvm_unref_counter_container(pPriv->processRecord.pCounterContainer);
    uvm_unref_event_container(pPriv->processRecord.pEventContainer);

    //
    // At this point all the regions have been removed, but there might be some
    // leftover streams in running state.
    //
    _stop_and_destroy_leftover_streams(&pPriv->processRecord);
    UVM_PANIC_ON(!list_empty(&pPriv->processRecord.allStreamList));
    //
    // Pages are freed when each commit record is destroyed, and those in turn
    // are destroyed when their vmas go away. All that happens during process
    // teardown, in the kernel core, before the fd's are closed. That all means
    // that there should not be any pages remaining by the time we get here.
    //
    uvm_page_cache_verify_page_list_empty(pPriv, __FUNCTION__);
    _delete_all_session_info_table_entries(&pPriv->processRecord);

    kmem_cache_free(g_uvmMappingCache, filp->f_mapping);
    kmem_cache_free(g_uvmPrivateCache, pPriv);
    UVM_DBG_PRINT("done\n");

    return 0;
}

static int uvmlite_mmap(struct file * filp, struct vm_area_struct * vma)
{
    // vm_end and vm_start are already aligned to page boundary
    unsigned long nPages = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
    unsigned long counterLowestPage = UVM_COUNTERS_OFFSET_BASE >> PAGE_SHIFT;
    unsigned long eventsLowestPage = UVM_EVENTS_OFFSET_BASE >> PAGE_SHIFT;
    int ret = -EINVAL;
    UvmRegionTracker *pRegionTracker = NULL;
    UvmCommitRecord  *pRecord = NULL;
    DriverPrivate *pPriv = (DriverPrivate*)filp->private_data;

    UVM_PANIC_ON(!pPriv);

    // verify mapping is not within UVM privileged region
    if ((unsigned long)vma->vm_start >= g_uvmKernelPrivRegionStart &&
        (unsigned long)vma->vm_start < g_uvmKernelPrivRegionStart +
                                       g_uvmKernelPrivRegionLength)
    {
        return -EINVAL;
    }

    if (vma->vm_pgoff + nPages < counterLowestPage)
    {
        //
        // UVM mappings must set the offset to the virtual address of the
        // mapping. Also check VA alignment
        //
        if (vma->vm_start != (vma->vm_pgoff << PAGE_SHIFT))
            return -EINVAL;

        down_write(&pPriv->uvmPrivLock);

        pRegionTracker = uvm_create_region_tracker(vma);
        if (NULL == pRegionTracker)
            return -ENOMEM;

        vma->vm_private_data = pRegionTracker;
        pRegionTracker->osPrivate = pPriv;
        up_write(&pPriv->uvmPrivLock);
        vma->vm_ops   = &uvmlite_vma_ops;

        // Prohibit copying the vma on fork().
        vma->vm_flags |= VM_DONTCOPY;
        // Prohibt mremap() that would expand the vma.
        vma->vm_flags |= VM_DONTEXPAND;

        // Other cases of vma modification are detected in _mmap_open().

        vma->vm_private_data  = pRegionTracker;

        // create the associated commit record
        down_write(&pPriv->uvmPrivLock);
        pRecord = uvmlite_create_commit_record(vma->vm_start,
                                            vma->vm_end - vma->vm_start,
                                            pPriv, vma);

        if (!pRecord)
        {
            UVM_ERR_PRINT("failed to create a commit record for the region\n");
            ret = -EINVAL; 
        }
        else
        {
            uvm_track_region(pRegionTracker,
                            vma->vm_start,
                            vma->vm_end,
                            NULL,
                            pRecord);

            _set_record_inaccessible(pRecord);

            ret = 0;
        }
        up_write(&pPriv->uvmPrivLock);
    }
    else if (vma->vm_pgoff >= counterLowestPage &&
             vma->vm_pgoff + nPages < eventsLowestPage)
    {
        // mapping for counters (read only)
        if (vma->vm_flags & VM_WRITE)
            return -EINVAL;

        vma->vm_ops = &counters_vma_ops;
        vma->vm_flags &= ~VM_MAYWRITE;
        ret = 0;
    }
    else if (vma->vm_pgoff >= eventsLowestPage)
    {
        vma->vm_ops = &counters_vma_ops;
        //
        // No access until the backing store is plugged in during the MAP_EVENT_QUEUE
        // ioctl.
        //
        ret = 0;
    }

    // prevent vm_insert_page from modifying the vma's flags:
    vma->vm_flags |= VM_MIXEDMAP;
    UVM_DBG_PRINT_RL("vma 0x%p (vm_start:0x%p) pgoff: %lu, nPages: %lu\n",
                     vma, (void*)vma->vm_start,
                     vma->vm_pgoff, nPages);

    return ret;
}

static long uvmlite_unlocked_ioctl(struct file *filp,
                                   unsigned int cmd,
                                   unsigned long arg)
{
    //
    // The following macro is only intended for use in this routine. That's why
    // it is declared inside the function (even though, of course, the
    // preprocessor ignores such scoping).
    //
    #define UVM_ROUTE_CMD(cmd,functionName)                                 \
        case cmd:                                                           \
        {                                                                   \
            cmd##_PARAMS params;                                            \
            if (copy_from_user(&params, (void __user*)arg, sizeof(params))) \
                return -EFAULT;                                             \
                                                                            \
            params.rmStatus = functionName(&params, filp);                 \
            if (copy_to_user((void __user*)arg, &params, sizeof(params)))   \
                return -EFAULT;                                             \
                                                                            \
            break;                                                          \
        }

    switch (cmd)
    {
        case UVM_DEINITIALIZE:
            UVM_DBG_PRINT("cmd: UVM_DEINITIALIZE\n");
            break;
        UVM_ROUTE_CMD(UVM_INITIALIZE,             uvm_api_initialize);
        UVM_ROUTE_CMD(UVM_RESERVE_VA,             uvm_api_reserve_va);
        UVM_ROUTE_CMD(UVM_RELEASE_VA,             uvm_api_release_va);
        UVM_ROUTE_CMD(UVM_REGION_COMMIT,          uvm_api_region_commit);
        UVM_ROUTE_CMD(UVM_REGION_DECOMMIT,        uvm_api_region_decommit);
        UVM_ROUTE_CMD(UVM_REGION_SET_STREAM,      uvm_api_region_set_stream);
        UVM_ROUTE_CMD(UVM_SET_STREAM_RUNNING,     uvm_api_region_set_stream_running);
        UVM_ROUTE_CMD(UVM_SET_STREAM_STOPPED,     uvm_api_region_set_stream_stopped);
        UVM_ROUTE_CMD(UVM_MIGRATE_TO_GPU,         uvm_api_migrate_to_gpu);
        UVM_ROUTE_CMD(UVM_RUN_TEST,               uvm_api_run_test);
        UVM_ROUTE_CMD(UVM_ADD_SESSION,            uvm_api_add_session);
        UVM_ROUTE_CMD(UVM_REMOVE_SESSION,         uvm_api_remove_session);
        UVM_ROUTE_CMD(UVM_MAP_COUNTER,            uvm_api_map_counter);
        UVM_ROUTE_CMD(UVM_ENABLE_COUNTERS,        uvm_api_enable_counters);
        UVM_ROUTE_CMD(UVM_REGISTER_MPS_SERVER,    uvm_api_register_mps_server);
        UVM_ROUTE_CMD(UVM_REGISTER_MPS_CLIENT,    uvm_api_register_mps_client);
        UVM_ROUTE_CMD(UVM_CREATE_EVENT_QUEUE,     uvm_api_create_event_queue);
        UVM_ROUTE_CMD(UVM_MAP_EVENT_QUEUE,        uvm_api_map_event_queue);
        UVM_ROUTE_CMD(UVM_REMOVE_EVENT_QUEUE,     uvm_api_remove_event_queue);
        UVM_ROUTE_CMD(UVM_EVENT_CTRL,             uvm_api_event_ctrl);
        UVM_ROUTE_CMD(UVM_GET_GPU_UUID_TABLE,     uvm_api_get_gpu_uuid_table);
        UVM_ROUTE_CMD(UVM_IS_8_SUPPORTED,         uvm_api_is_8_supported_lite);
        UVM_ROUTE_CMD(UVM_PAGEABLE_MEM_ACCESS,    uvm_api_pageable_mem_access_lite);
        default:
            UVM_ERR_PRINT("Unknown: cmd: 0x%0x\n", cmd);
            return -EINVAL;
            break;
    }

    #undef UVM_ROUTE_CMD

    return 0;
}

static unsigned int uvmlite_poll (struct file *filep, poll_table *wait)
{
    unsigned int mask = 0;
    DriverPrivate *pPriv = (DriverPrivate*)filep->private_data;
    UvmProcessRecord *pProcessRecord;
    wait_queue_head_t *wait_queue;

    down_read(&pPriv->uvmPrivLock);

    pProcessRecord = &pPriv->processRecord;

    wait_queue = &pProcessRecord->pEventContainer->wait_queue;

    up_read(&pPriv->uvmPrivLock);

    poll_wait(filep, wait_queue, wait);

    // Post-check to see if the caller was woken up because events were
    // available:
    UVM_DBG_PRINT_RL("post-check\n");
    down_read(&pPriv->uvmPrivLock);

    pProcessRecord = &pPriv->processRecord;

    if (uvm_any_event_notifications_pending(pProcessRecord->pEventContainer))
    {
        mask = (POLLPRI | POLLIN);
        up_read(&pPriv->uvmPrivLock);
        UVM_DBG_PRINT_RL("found events\n");
        return mask;
    }

    up_read(&pPriv->uvmPrivLock);

    return mask;
}

static const struct file_operations uvmlite_fops = {
    .open            = uvmlite_open,
    .release         = uvmlite_close,
    .mmap            = uvmlite_mmap,
    .unlocked_ioctl  = uvmlite_unlocked_ioctl,
#if NVCPU_IS_X86_64 && defined(NV_FILE_OPERATIONS_HAS_COMPAT_IOCTL)
    .compat_ioctl    = uvmlite_unlocked_ioctl,
#endif
    .poll            = uvmlite_poll,
    .owner           = THIS_MODULE,
};

//
// Locking: this initializes the g_uvmDriverPrivateTableLock, and doesn't take
// or acquire any other locks.
//
int uvmlite_init(dev_t uvmBaseDev)
{
    dev_t uvmliteDev = MKDEV(MAJOR(uvmBaseDev),
                             NVIDIA_UVM_PRIMARY_MINOR_NUMBER);
    NV_STATUS rmStatus;
    int ret = 0;

    // TODO: Bug 1381188: uvm8: Linux: shrinkable kernel cache support
    // Also:
    // TODO: Bug 1766109: uvm8: delete UVM-Lite files and remove -lite mode
    //  ...just delete this entire file, instead of the original to-do: which was:
    //
    // call register_shrinker()
    init_rwsem(&g_uvmDriverPrivateTableLock);
    init_rwsem(&g_uvmMpsServersListLock);

    init_rwsem(&g_attached_uuid_lock);
    memset(g_attached_uuid_list, 0, sizeof (g_attached_uuid_list));
    g_attached_uuid_num = 0;

    rmStatus = uvm_initialize_events_api();
    if (NV_OK != rmStatus)
    {
        UVM_ERR_PRINT_NV_STATUS(
            "Could not initialize events api.\n",
            rmStatus);
        goto fail;
    }

    rmStatus = uvm_initialize_counters_api();
    if (NV_OK != rmStatus)
    {
        UVM_ERR_PRINT_NV_STATUS(
            "Could not initialize counters api.\n",
            rmStatus);
        goto fail;
    }

    // Debugging hint: kmem_cache_create objects are visible in /proc/slabinfo:
    ret = -ENOMEM;
    g_uvmPrivateCache = NV_KMEM_CACHE_CREATE("uvm_private_t", struct DriverPrivate_tag);
    if (!g_uvmPrivateCache)
        goto fail;

    g_uvmCommitRecordCache = NV_KMEM_CACHE_CREATE("uvm_commit_record_t",
                                                  struct UvmCommitRecord_tag);
    if (!g_uvmCommitRecordCache)
        goto fail;

    g_uvmMigTrackerCache = NV_KMEM_CACHE_CREATE("uvm_mig_tracker_t",
                                                struct UvmGpuMigrationTracking_tag);

    if (!g_uvmMigTrackerCache)
        goto fail;

    g_uvmStreamRecordCache = NV_KMEM_CACHE_CREATE("uvm_stream_record_t",
                                                  struct UvmStreamRecord_tag);
    if (!g_uvmStreamRecordCache)
        goto fail;

    g_uvmMappingCache = NV_KMEM_CACHE_CREATE("uvm_mapping_t", struct address_space);
    if (!g_uvmMappingCache)
        goto fail;

    g_uvmMpsServerCache = NV_KMEM_CACHE_CREATE("uvm_mps_server_t",
                                               struct UvmMpsServer_tag);
    if (!g_uvmMpsServerCache)
        goto fail;

    rmStatus = uvmlite_prefetch_init();
    if (rmStatus != NV_OK)
        goto fail;

    if (NV_OK != uvm_regiontracker_init())
        goto fail;

    if (0 != uvm_page_cache_init())
        goto fail;

    if (NV_OK != nvUvmInterfaceGetUvmPrivRegion(&g_uvmKernelPrivRegionStart,
                                                &g_uvmKernelPrivRegionLength))
        goto fail;

    // Add the device to the system as a last step to avoid any race condition.
    cdev_init(&g_uvmlite_cdev, &uvmlite_fops);
    g_uvmlite_cdev.owner = THIS_MODULE;

    ret = cdev_add(&g_uvmlite_cdev, uvmliteDev, 1);
    if (ret)
    {
        UVM_ERR_PRINT("cdev_add (major %u, minor %u) failed: %d\n",
                      MAJOR(uvmliteDev), MINOR(uvmliteDev), ret);
        goto fail;
    }

    return 0;

fail:
    kmem_cache_destroy_safe(&g_uvmMpsServerCache);
    kmem_cache_destroy_safe(&g_uvmMappingCache);
    kmem_cache_destroy_safe(&g_uvmStreamRecordCache);
    kmem_cache_destroy_safe(&g_uvmMigTrackerCache);
    kmem_cache_destroy_safe(&g_uvmCommitRecordCache);
    kmem_cache_destroy_safe(&g_uvmPrivateCache);

    uvmlite_prefetch_exit();

    uvm_regiontracker_exit();

    uvm_deinitialize_events_api();
    uvm_deinitialize_counters_api();

    UVM_ERR_PRINT("Failed\n");
    return ret;

}

int uvmlite_setup_gpu_list()
{
    NvU8 *pUuidList;
    NV_STATUS status;
    unsigned i = 0;
    unsigned numAttachedGpus = 0;
    int result = 0;

    // get the list of gpus
    pUuidList = vmalloc(sizeof(NvU8) * UVM_MAX_GPUS * UVM_UUID_LEN);
    if (!pUuidList)
        return -ENOMEM;

    down_write(&g_attached_uuid_lock);

    status = nvUvmInterfaceGetAttachedUuids(pUuidList, &numAttachedGpus);
    if (status != NV_OK || (numAttachedGpus > UVM_MAX_GPUS))
    {
        UVM_ERR_PRINT("ERROR: Error in finding GPUs\n");
        result = -ENODEV;
        goto cleanup;
    }

    UVM_DBG_PRINT("Attached GPUs number = %u\n", numAttachedGpus);
    // construct the uuid list
    for (i = 0; i < numAttachedGpus; i++)
    {
        NvProcessorUuid *gpuUuid = (NvProcessorUuid *) (pUuidList + (i * UVM_UUID_LEN));
        NV_STATUS status;
        unsigned index;

        UVM_DBG_PRINT_UUID("Found attached GPU", gpuUuid);
        status = _find_or_add_gpu_index(gpuUuid, &index);
        if (status != NV_OK)
        {
            result = -ENOMEM;
            goto cleanup;
        }

        g_attached_uuid_list[index].isEnabled = NV_TRUE;

    }

cleanup:
    up_write(&g_attached_uuid_lock);

    vfree(pUuidList);
    return result;
}

void uvmlite_exit(void)
{
    //
    // No extra cleanup of regions or data structures is necessary here, because
    // that is done by the file release routine, and the kernel won't allow the
    // module to be unloaded while it's device file open count remains >0.
    //
    // However, this is still a good place for:
    //
    //     check for resource leaks here, just in case.
    //

    cdev_del(&g_uvmlite_cdev);

    kmem_cache_destroy(g_uvmMpsServerCache);
    kmem_cache_destroy(g_uvmMappingCache);
    kmem_cache_destroy(g_uvmStreamRecordCache);
    kmem_cache_destroy(g_uvmMigTrackerCache);
    kmem_cache_destroy(g_uvmCommitRecordCache);
    kmem_cache_destroy(g_uvmPrivateCache);

    uvmlite_prefetch_exit();

    uvm_regiontracker_exit();

    uvm_deinitialize_events_api();
    uvm_deinitialize_counters_api();

    uvm_page_cache_destroy();
}

//
// This function sets up the CopyEngine and its channel.
//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock, before
// calling this routine.
//
static NV_STATUS
_create_migration_resources(NvProcessorUuid * pGpuUuidStruct,
                            UvmGpuMigrationTracking *pMigTracking)
{
    NV_STATUS rmStatus;
    unsigned ceInstance = 1;

    if (!pMigTracking)
        return NV_ERR_INVALID_ARGUMENT;

    UVM_DBG_PRINT_UUID("Entering", pGpuUuidStruct);

    rmStatus = nvUvmInterfaceSessionCreate(&pMigTracking->hSession);
    if (rmStatus != NV_OK)
    {
        UVM_ERR_PRINT("ERROR: could not create a session\n");
        goto cleanup;
    }

    rmStatus = nvUvmInterfaceAddressSpaceCreateMirrored(
                               pMigTracking->hSession,
                               pGpuUuidStruct,
                               &pMigTracking->hVaSpace);
    if (rmStatus != NV_OK)
    {
        UVM_ERR_PRINT("ERROR: could not create an address space\n");
        goto cleanup_session;
    }

    // Get GPU caps like ECC support on GPU, big page size, small page size etc
    rmStatus = nvUvmInterfaceQueryCaps(pMigTracking->hVaSpace,
                                       &pMigTracking->gpuCaps);

    if (rmStatus != NV_OK)
    {
        UVM_ERR_PRINT("ERROR: could not lookup GPU capabilities\n");
        goto cleanup_address_space;
    }

    rmStatus = nvUvmInterfaceChannelAllocate(pMigTracking->hVaSpace,
                                             &pMigTracking->hChannel,
                                             &pMigTracking->channelInfo);
    if (rmStatus != NV_OK)
    {
        UVM_ERR_PRINT("ERROR: could not allocate a channel\n");
        goto cleanup_address_space;
    }

    // Reset rmStatus, in case there are no loop iterations to set it:
    rmStatus = NV_ERR_GENERIC;

    for (ceInstance = 1; ceInstance <= MAX_NUM_COPY_ENGINES; ++ceInstance)
    {
        rmStatus = nvUvmInterfaceCopyEngineAllocate(
                                                  pMigTracking->hChannel,
                                                  ceInstance,
                                                  &pMigTracking->ceClassNumber,
                                                  &pMigTracking->hCopyEngine);

        if ((rmStatus == NV_ERR_INVALID_INDEX) || (rmStatus == NV_OK))
            break;
    }

    if (rmStatus != NV_OK)
    {
        UVM_ERR_PRINT("ERROR: could not allocate OBJCE\n");
        goto cleanup_address_space;
    }

    // allocate a semaphore page
    rmStatus = nvUvmInterfaceMemoryAllocSys(pMigTracking->hVaSpace,
                                            SEMAPHORE_SIZE,
                                            &pMigTracking->gpuSemaPtr,
                                            NULL);
    if (NV_OK != rmStatus)
    {
        UVM_ERR_PRINT("ERROR: could not allocate GPU memory for PB\n");
        goto cleanup_address_space;
    }
    rmStatus = nvUvmInterfaceMemoryCpuMap(pMigTracking->hVaSpace,
                                          pMigTracking->gpuSemaPtr,
                                          SEMAPHORE_SIZE,
                                          &pMigTracking->cpuSemaPtr,
                                          UVM_PAGE_SIZE_DEFAULT);
    if (NV_OK != rmStatus)
    {
        UVM_ERR_PRINT("ERROR: could not map PB to CPU VA\n");
        goto cleanup_address_space;
    }

    // allocate a Push Buffer segment
    rmStatus = nvUvmInterfaceMemoryAllocSys(pMigTracking->hVaSpace,
                                            PUSHBUFFER_SIZE,
                                            &pMigTracking->gpuPushBufferPtr,
                                            NULL);
    if (NV_OK != rmStatus)
    {
        UVM_ERR_PRINT("ERROR: could not allocate GPU memory for PB\n");
        goto cleanup_address_space;
    }
    // Map Push Buffer
    rmStatus = nvUvmInterfaceMemoryCpuMap(pMigTracking->hVaSpace,
                                          pMigTracking->gpuPushBufferPtr,
                                          PUSHBUFFER_SIZE,
                                          &pMigTracking->cpuPushBufferPtr,
                                          UVM_PAGE_SIZE_DEFAULT);
    if (NV_OK != rmStatus)
    {
        UVM_ERR_PRINT("ERROR: could not map PB to CPU VA\n");
        goto cleanup_address_space;
    }

    // setup CE Ops
    rmStatus = NvUvmHalInit(pMigTracking->ceClassNumber,
                            pMigTracking->channelInfo.channelClassNum,
                            &pMigTracking->ceOps);

    if (NV_OK != rmStatus)
    {
        UVM_ERR_PRINT("ERROR: could not find a CE HAL to use\n");
        goto cleanup_address_space;
    }

    UVM_DBG_PRINT("Done. channelClassNum: 0x%0x, ceClassNum: 0x%0x\n",
                  pMigTracking->channelInfo.channelClassNum,
                  pMigTracking->ceClassNumber);

    return 0;

cleanup_address_space:
    nvUvmInterfaceAddressSpaceDestroy(pMigTracking->hVaSpace);
cleanup_session:
    nvUvmInterfaceSessionDestroy(pMigTracking->hSession);
cleanup:
    return rmStatus;
}

//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock, before
// calling this routine.
//
static void _destroy_migration_resources(UvmGpuMigrationTracking *pMigTracking)
{
    if (!pMigTracking)
        return;

    UVM_DBG_PRINT("Entering\n");

    // destroy the channel and the engines under it
    if (pMigTracking->hChannel != 0)
        nvUvmInterfaceChannelDestroy(pMigTracking->hChannel);

    if (pMigTracking->hVaSpace != 0)
    {
        nvUvmInterfaceAddressSpaceDestroy(pMigTracking->hVaSpace);
        //
        // TODO: Bug 1766109: uvm8: delete UVM-Lite files and remove -lite mode
        //  ...just delete this entire file, instead of the original to-do: which was:
        //
        // Fix the RM bug where it thinks allocating a bogus session
        // is ok when there is no valid device.
        //
        nvUvmInterfaceSessionDestroy(pMigTracking->hSession);
    }

    UVM_DBG_PRINT("Done\n");

    return;
}

//
// Locking:
//     1. This routine acquires the g_uvmDriverPrivateTableLock.
//
//     2. This routine acquires the processRecord.uvmPrivLock.
//
void umvlite_destroy_per_process_gpu_resources(NvProcessorUuid *gpuUuidStruct)
{
    struct list_head *pos;
    unsigned index;

    down_read(&g_attached_uuid_lock);
    index = _find_gpu_index(gpuUuidStruct);
    up_read(&g_attached_uuid_lock);

    if (index == UVM_INVALID_HOME_GPU_INDEX)
        return;

    down_write(&g_uvmDriverPrivateTableLock);
    list_for_each(pos, &g_uvmDriverPrivateTable)
    {

        UvmPerProcessGpuMigs * pMig;
        DriverPrivate *pPriv = list_entry(pos, DriverPrivate,
                                          driverPrivateNode);

        down_write(&pPriv->uvmPrivLock);

        pMig = &pPriv->processRecord.gpuMigs[index];

        if (pMig->migTracker != NULL)
        {
            if (!_is_mps_client(&pPriv->processRecord))
            {
                _destroy_migration_resources(pMig->migTracker);
                kmem_cache_free(g_uvmMigTrackerCache, pMig->migTracker);
            }
            _disconnect_mig_completely(pMig, NULL);
        }

        up_write(&pPriv->uvmPrivLock);
    }
    up_write(&g_uvmDriverPrivateTableLock);
}

//
// Function to check for ECC errors and returns true if an ECC DBE error
// has happened.
//
static NV_STATUS _check_ecc_errors(UvmGpuMigrationTracking *pMigTracker,
                                   NvBool *pIsEccErrorSet)
{
    struct timeval eccErrorStartTime = {0};
    struct timeval eccErrorCurrentTime = {0};
    struct timeval eccTimeout = {0};
    NvBool bEccErrorTimeout = NV_FALSE;
    NvBool bEccIncomingError = NV_FALSE;
    unsigned rmInterruptSet = 0;

    if (!pIsEccErrorSet || !pMigTracker ||
        !(pMigTracker->gpuCaps.eccErrorNotifier))
        return NV_ERR_INVALID_ARGUMENT;

    *pIsEccErrorSet = NV_FALSE;

    // Checking for ECC error after semaphore has been released.
    do
    {
        if (!!(rmInterruptSet) && !bEccIncomingError)
        {
            do_gettimeofday(&eccErrorStartTime);
            _set_timeout_in_usec(&eccErrorStartTime, &eccTimeout,
                                 UVM_ECC_ERR_TIMEOUT_USEC);

            //
            // Call RM to service interrupts to make sure we don't loop too much
            // for upcoming ECC interrupt to get reset before checking the
            // notifier.
            //
            if (NV_OK == nvUvmInterfaceServiceDeviceInterruptsRM(
                    pMigTracker->hVaSpace))
                bEccIncomingError = NV_TRUE;
        }
        //
        // Read any incoming ECC interrupt. If true, then we need to wait for it
        // to get  reset before we read notifier to make sure it was an ECC
        // interrupt only.
        //
        if (pMigTracker->gpuCaps.eccReadLocation)
        {
            rmInterruptSet = MEM_RD32((NvU8*)pMigTracker->gpuCaps.eccReadLocation +
                                       pMigTracker->gpuCaps.eccOffset);
            rmInterruptSet = rmInterruptSet & pMigTracker->gpuCaps.eccMask;
        }

        //
        // Make sure you have an ECC Interrupt pending and you have taken the
        // current time before checking for timeout else you will end up always
        // getting a timeout.
        //
        if (!!(rmInterruptSet) && (eccErrorStartTime.tv_usec != 0))
        {
            do_gettimeofday(&eccErrorCurrentTime);
            if ((eccErrorCurrentTime.tv_sec > eccTimeout.tv_sec) ||
                ((eccErrorCurrentTime.tv_sec == eccTimeout.tv_sec) &&
                (eccErrorCurrentTime.tv_usec >= eccTimeout.tv_usec)))
            {
                bEccErrorTimeout = NV_TRUE;
            }
        }

    } while (!!(rmInterruptSet) && pMigTracker->gpuCaps.eccErrorNotifier &&
             !*(pMigTracker->gpuCaps.eccErrorNotifier) &&
             !bEccErrorTimeout);

    // Check if an interrupt is still set and notifier has not been reset.
    if (!!(rmInterruptSet) && pMigTracker->gpuCaps.eccErrorNotifier &&
        !*(pMigTracker->gpuCaps.eccErrorNotifier))
    {
        // Read interrupt one more and then call slow path check.
        if (pMigTracker->gpuCaps.eccReadLocation)
        {
            rmInterruptSet = MEM_RD32((NvU8*)pMigTracker->gpuCaps.eccReadLocation +
                                      pMigTracker->gpuCaps.eccOffset);
            rmInterruptSet = rmInterruptSet & pMigTracker->gpuCaps.eccMask;
        }

        if (!!(rmInterruptSet))
        {
            nvUvmInterfaceCheckEccErrorSlowpath(pMigTracker->hChannel,
                                                (NvBool *)&bEccIncomingError);

            if (bEccIncomingError)
            {
                *pIsEccErrorSet = NV_TRUE;
                bEccIncomingError = NV_FALSE;
            }

            return NV_OK;
        }
    }

    //
    // If we are here this means interrupt is reset. Just return notifier value
    // as ECC error.
    //
    if (pMigTracker->gpuCaps.eccErrorNotifier)
    {
        *pIsEccErrorSet = *(pMigTracker->gpuCaps.eccErrorNotifier);
    }

    return NV_OK;
}

//
// This function will enqueue semaphore release and wait for the
// previously enqueued copies to complete. This will be called
// in both CPU->GPU and GPU->CPU copies.
//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock, before
// calling this routine.
//
static
NV_STATUS _wait_for_migration_completion(UvmGpuMigrationTracking *pMigTracker,
                                         UvmCommitRecord *pRecord,
                                         UvmGpuPointer pageVirtualAddr,
                                         UvmGpuPointer cpuPhysAddr,
                                         char ** cpuPbPointer,
                                         char * cpuPbEnd,
                                         NvLength * numMethods)
{
    NV_STATUS rmStatus = NV_OK;
    UvmCopyOps *pCopyOps;
    unsigned semaVal = 0;
    NvBool bEccError = NV_FALSE;

    if (!pMigTracker || !pRecord || (pMigTracker->ceOps.launchDma == NULL) ||
       (pMigTracker->ceOps.writeGpEntry == NULL))
        return NV_ERR_INVALID_ARGUMENT;

    pCopyOps  = &pMigTracker->ceOps;

    // reset the semaphore payload.
    *(unsigned *)pMigTracker->cpuSemaPtr = UVM_SEM_INIT;

    // push methods to release the semaphore.
    *numMethods += pCopyOps->semaphoreRelease((unsigned **)cpuPbPointer,
                                              (unsigned*)cpuPbEnd,
                                              pMigTracker->gpuSemaPtr,
                                              UVM_SEM_DONE);

    // wrap around gpFifoOffset if needed.
    if (pMigTracker->channelInfo.numGpFifoEntries ==
            pMigTracker->currentGpFifoOffset + 1)
    {
        pMigTracker->currentGpFifoOffset = 0;
    }

    // write the GP entry.
    pCopyOps->writeGpEntry(pMigTracker->channelInfo.gpFifoEntries,
                           pMigTracker->currentGpFifoOffset,
                           pMigTracker->gpuPushBufferPtr,
                           *numMethods);
    // launch the copy.
    NvUvmChannelWriteGpPut(pMigTracker->channelInfo.GPPut,
                           pMigTracker->currentGpFifoOffset+1);
    pMigTracker->currentGpFifoOffset++;

    //
    // spin on the semaphore before returning
    //
    // TODO: Bug 1766109: uvm8: delete UVM-Lite files and remove -lite mode
    //  ...just delete this entire file, instead of the original to-do: which was:
    //
    // consider busy-waiting vs. sleeping, probably depending on copy
    // size.
    //
    UVM_DBG_PRINT_RL("Waiting for semaphore at virt addr: 0x%p\n",
                     (void *)pageVirtualAddr);

    semaVal = 0;
    while (semaVal != UVM_SEM_DONE)
    {
        semaVal = MEM_RD32(pMigTracker->cpuSemaPtr);

        if (fatal_signal_pending(current))
        {
            UVM_ERR_PRINT("Caught a fatal signal, so killing the channel and "
                          "bailing out early\n");
            rmStatus = nvUvmInterfaceKillChannel(pMigTracker->hChannel);
            if (rmStatus != NV_OK)
            {
                UVM_DBG_PRINT_RL("Failed to reset the channel - hChannel: "
                             "0x%p, rmStatus: 0x%0x\n",
                             pMigTracker->hChannel, rmStatus);
            }
            pRecord->cachedHomeGpuPerProcessIndex = UVM_INVALID_HOME_GPU_INDEX;
            return NV_ERR_SIGNAL_PENDING;
        }

        //
        // If we hit RC error we simply bail out else we will keep looping
        // till we hit copy timeout.
        //
        if (pMigTracker->channelInfo.errorNotifier &&
            MEM_RD16(&(pMigTracker->channelInfo.errorNotifier->status)) != 0)
        {
            UVM_ERR_PRINT("RC Error during page migration for virt "
                          "addr: 0x%p\n", (void *)pageVirtualAddr);

            pRecord->cachedHomeGpuPerProcessIndex = UVM_INVALID_HOME_GPU_INDEX;
            return NV_ERR_RC_ERROR;
        }
        cpu_relax();
    }

    // Handle any ECC error if  ECC is enabled.
    if (pMigTracker->gpuCaps.bEccEnabled)
    {
        _check_ecc_errors(pMigTracker, &bEccError);
        if (bEccError)
        {
            // In case of an ECC error we can't use this GPU for any other work.
            UVM_ERR_PRINT("ECC Error detected during page migration for"
                          " CPU physical-GPU Virtual address: 0x%p - 0x%p\n",
                          (void*)cpuPhysAddr, (void *)pageVirtualAddr);

            pRecord->cachedHomeGpuPerProcessIndex = UVM_INVALID_HOME_GPU_INDEX;
            return NV_ERR_ECC_ERROR;
        }
    }

    return NV_OK;
}

//
// This function will migrate pages from GPU video memory to CPU sysmem to in
// pipelined manner. The CPU pointer is physical and the GPU pointer is virtual.
// If UVM_MIGRATE_OUTDATED_ONLY is set, only pages marked as outdated are
// transferred.
//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock, before
// calling this routine.
//
// Locking: If called from an MPS client, you must hold a read lock on the server
// UvmMpsServer.mpsLock and a write lock on the server DriverPrivate.uvmPrivLock
//
// Notes:
//
// 1. Enqueue copying of as many pages as possible(limited by push-buffer size)
// as copy engine supports pipelining and we want to get maximum performance
// from HW.
//
// TODO: Bug 1766109: uvm8: delete UVM-Lite files and remove -lite mode
//  ...just delete this entire file, instead of the original to-do: which was:
//
// 2. handle vma splitting, mismatches between what the vma and pRecord
// have for baseAddress and length.
//
// 3. Caller is responsible for updating the counters.
//
NV_STATUS migrate_gpu_to_cpu(UvmGpuMigrationTracking *pMigTracker,
                             UvmCommitRecord *pRecord,
                             NvLength startPage,
                             NvLength numPages,
                             int migrationFlags,
                             NvLength *migratedPages)
{
    NV_STATUS rmStatus = NV_OK;
    UvmPageTracking *pTracking = NULL;
    UvmCopyOps *pCopyOps;
    char *cpuPbPointer = NULL;
    char *cpuPbCopyEnd = NULL;
    char *cpuPbEnd = NULL;
    NvUPtr cpuPhysAddr = 0;
    NvUPtr pageVirtualAddr = 0;
    NvLength pagesInRecord;
    NvLength pageIndex;
    NvLength methods = 0;
    NvLength numMethods = 0;
    NvBool recordMigrationEvent = NV_FALSE;
    NvU64 beginTime = 0;
    NvU64 endTime = 0;

    if (!pMigTracker || !pRecord || (pMigTracker->ceOps.launchDma == NULL))
        return NV_ERR_INVALID_ARGUMENT;

    if (uvm_is_event_enabled(pRecord->osPrivate->processRecord.pEventContainer,
                             UvmEventTypeMigration))
    {
        recordMigrationEvent = NV_TRUE;
        beginTime = NV_GETTIME();
    }

    // If any RC or ECC error has happened check it before starting any copy.
    rmStatus = _preexisting_error_on_channel(pMigTracker, pRecord);
    if (rmStatus != NV_OK)
        return rmStatus;

    pagesInRecord = pRecord->length >> PAGE_SHIFT;

    UVM_PANIC_ON(startPage            >= pagesInRecord);
    UVM_PANIC_ON(startPage + numPages >  pagesInRecord);

    pCopyOps     = &pMigTracker->ceOps;
    cpuPbPointer = pMigTracker->cpuPushBufferPtr;
    cpuPbEnd     = pMigTracker->cpuPushBufferPtr + PUSHBUFFER_SIZE;
    //
    // Send a dummy Semaphore release to get the size of pushBuffer required
    // for release method. We need to reserve this while pushing copies to make
    // sure we have enough space remaining for pushing semaphore release.
    //
    methods = pCopyOps->semaphoreRelease((unsigned **)&cpuPbPointer,
                                         (unsigned*)cpuPbEnd,
                                         pMigTracker->gpuSemaPtr,
                                         UVM_SEM_DONE);

    // Copy pushBuffer limit should take care of release methods.
    cpuPbPointer = pMigTracker->cpuPushBufferPtr;
    cpuPbCopyEnd = (char *) cpuPbEnd - methods;
    methods = 0;

    if (migratedPages != NULL)
        *migratedPages = 0;

    for (pageIndex = startPage; pageIndex < startPage + numPages; ++pageIndex)
    {
        pTracking = pRecord->commitRecordPages[pageIndex];
        if (!pTracking ||
            ((migrationFlags & UVM_MIGRATE_OUTDATED_ONLY) &&
             !UVM_PAGE_OUTDATED(pTracking->uvmPage)))
            continue;

        pageVirtualAddr = pRecord->baseAddress + (pageIndex << PAGE_SHIFT);
        cpuPhysAddr = page_to_phys(pTracking->uvmPage);
        // The common case takes the break. If the PB is full, we flush the
        // previous copies and retry
        while (1)
        {
            methods = pCopyOps->launchDma((unsigned **)&cpuPbPointer,
                                          (unsigned *) cpuPbCopyEnd,
                                          (UvmGpuPointer)pageVirtualAddr,
                                          NV_UVM_COPY_SRC_LOCATION_FB,
                                          (UvmGpuPointer)cpuPhysAddr,
                                          NV_UVM_COPY_DST_LOCATION_SYSMEM,
                                          PAGE_SIZE,
                                          NV_UVM_COPY_DST_TYPE_PHYSICAL |
                                            NV_UVM_COPY_SRC_TYPE_VIRTUAL );
            if (methods)
                break;

            rmStatus = _wait_for_migration_completion(pMigTracker, pRecord,
                                            (UvmGpuPointer)pageVirtualAddr,
                                            (UvmGpuPointer)cpuPhysAddr,
                                            &cpuPbPointer, cpuPbEnd,
                                            &numMethods);
            if (rmStatus != NV_OK)
            {
                UVM_DBG_PRINT_RL("Failed to copy from gpu to cpu - vma: "
                                 "0x%p, rmStatus: 0x%0x\n",
                                 pRecord->vma, rmStatus);
                break;
            }
            // Reset push buffer pointer to start again from top.
            cpuPbPointer = pMigTracker->cpuPushBufferPtr;
            numMethods = 0;
        }
        numMethods += methods;

        if (migratedPages != NULL)
            ++(*migratedPages);
    }

    // Trigger completion of all copies which didn't completely fill PB.
    if ((numMethods != 0) && (rmStatus == NV_OK))
    {
        UVM_PANIC_ON(0 == cpuPhysAddr);

        // Change address to current page as we have incremented it above
        rmStatus = _wait_for_migration_completion(pMigTracker, pRecord,
                                        (UvmGpuPointer)pageVirtualAddr,
                                        (UvmGpuPointer)cpuPhysAddr,
                                        &cpuPbPointer, cpuPbEnd,
                                        &numMethods);
        if (rmStatus != NV_OK)
            UVM_DBG_PRINT_RL("Failed to copy from gpu to cpu: vma: 0x%p, "
                             "rmStatus: 0x%0x\n", pRecord->vma, rmStatus);
    }

    if (recordMigrationEvent && *migratedPages > 0)
    {
        endTime = NV_GETTIME();
        rmStatus = uvm_record_migration_event(
                       pRecord->osPrivate->processRecord.pEventContainer,
                       UvmEventMigrationDirectionGpuToCpu,
                       pRecord->cachedHomeGpuPerProcessIndex,
                       -1,
                       pageVirtualAddr,
                       (*migratedPages) * PAGE_SIZE,
                       beginTime,
                       endTime,
                       pRecord->pStream->streamId);
    }

    return rmStatus;
}

//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock, before
// calling this routine.
//
static
NV_STATUS _clear_cache(UvmCommitRecord *pRecord)
{
    UvmPageTracking * pTracking = NULL;
    NvUPtr end;
    NvUPtr pageVirtualAddr;
    unsigned long pageIndex = 0;

    if (!pRecord)
        return NV_ERR_INVALID_ARGUMENT;

    end = pRecord->baseAddress + pRecord->length;
    //
    // Mark the pages as no longer resident on CPU, by removing their pointer
    // from the array. We do this for all pages irrespective of copy being
    // succeeded.
    //
    for (pageVirtualAddr = pRecord->baseAddress, pageIndex = 0;
         pageVirtualAddr < end; pageVirtualAddr += PAGE_SIZE, ++pageIndex)
    {
        pTracking = pRecord->commitRecordPages[pageIndex];
        if (!pTracking)
        {
            // The page is not resident on the CPU, so it doesn't get migrated.
            continue;
        }
        pRecord->commitRecordPages[pageIndex] = NULL;
        uvm_page_cache_free_page(pTracking, __FUNCTION__);
    }

    return NV_OK;
}

static
void _update_gpu_migration_counters(UvmCommitRecord *pRecord,
                                    unsigned long long migratedPages)
{
    UVM_PANIC_ON(pRecord->cachedHomeGpuPerProcessIndex ==
                 UVM_INVALID_HOME_GPU_INDEX);

    uvm_increment_process_counters(
            pRecord->cachedHomeGpuPerProcessIndex,
            pRecord->osPrivate->processRecord.pCounterContainer,
            UvmCounterNameBytesXferHtD,
            migratedPages * PAGE_SIZE);
}

//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock, before
// calling this routine.
//
static
NV_STATUS _preexisting_error_on_channel(UvmGpuMigrationTracking *pMigTracker,
                                        UvmCommitRecord *pRecord)
{
    if (!pMigTracker || !pRecord)
        return NV_ERR_INVALID_ARGUMENT;

    if (pMigTracker->gpuCaps.bEccEnabled &&
        (pMigTracker->gpuCaps.eccErrorNotifier) &&
        *(pMigTracker->gpuCaps.eccErrorNotifier))
    {
        UVM_ERR_PRINT("ECC Error while starting migration from CPU->GPU\n");

        pRecord->cachedHomeGpuPerProcessIndex = UVM_INVALID_HOME_GPU_INDEX;
        return NV_ERR_ECC_ERROR;
    }

    // Check for a RC notifier before starting any transaction.
    if (pMigTracker->channelInfo.errorNotifier &&
        pMigTracker->channelInfo.errorNotifier->status != 0)
    {
        UVM_ERR_PRINT("RC Error while starting migration from CPU->GPU\n");

        pRecord->cachedHomeGpuPerProcessIndex = UVM_INVALID_HOME_GPU_INDEX;
        return NV_ERR_RC_ERROR;
    }

    return NV_OK;
}

//
// This function will migrate pages from CPU sysmem to GPU video memory in
// pipelined manner. The CPU pointer is physical and the GPU pointer is virtual.
//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock, before
// calling this routine.
//
// Locking: If called from an MPS client, you must hold a read lock on the
// server UvmMpsServer.mpsLock and a write lock on the server
// DriverPrivate.uvmPrivLock
//
// Notes:
//
// 1. Enqueue copying of as many pages as possible(limited by push-buffer size)
// as copy engine supports pipelining and we want to get maximum performance
// from HW.
//
// TODO: Bug 1766109: uvm8: delete UVM-Lite files and remove -lite mode
//  ...just delete this entire file, instead of the original to-do: which was:
//
// 2. handle vma splitting, mismatches between what the vma and pRecord
// have for baseAddress and length.
//
// 3. Caller is responsible for updating the counters.
//
NV_STATUS migrate_cpu_to_gpu(UvmGpuMigrationTracking *pMigTracker,
                             UvmCommitRecord *pRecord,
                             NvLength startPage,
                             NvLength numPages,
                             NvLength *migratedPages)
{
    NV_STATUS rmStatus = NV_OK;
    UvmPageTracking *pTracking = NULL;
    UvmCopyOps *pCopyOps;
    char *cpuPbPointer = NULL;
    char *cpuPbCopyEnd = NULL;
    char *cpuPbEnd = NULL;
    NvUPtr cpuPhysAddr = 0;
    NvUPtr pageVirtualAddr = 0;
    NvLength pagesInRecord;
    NvLength pageIndex;
    NvLength methods = 0;
    NvLength numMethods = 0;
    NvBool recordMigrationEvent = NV_FALSE;
    NvU64 beginTime = 0;
    NvU64 endTime = 0;

    if (!pMigTracker || !pRecord || (pMigTracker->ceOps.launchDma == NULL))
        return NV_ERR_INVALID_ARGUMENT;

    if (uvm_is_event_enabled(pRecord->osPrivate->processRecord.pEventContainer,
                             UvmEventTypeMigration))
    {
        recordMigrationEvent = NV_TRUE;
        beginTime = NV_GETTIME();
    }

    pagesInRecord = pRecord->length >> PAGE_SHIFT;

    UVM_PANIC_ON(startPage            >= pagesInRecord);
    UVM_PANIC_ON(startPage + numPages >  pagesInRecord);

    pCopyOps     = &pMigTracker->ceOps;
    cpuPbPointer = pMigTracker->cpuPushBufferPtr;
    cpuPbEnd     = pMigTracker->cpuPushBufferPtr + PUSHBUFFER_SIZE;
    //
    // Send a dummy Semaphore release to get the size of pushBuffer required
    // for release method. We need to reserve this while pushing copies to make
    // sure we have enough space remaining for pushing semaphore release.
    //
    methods = pCopyOps->semaphoreRelease((unsigned **)&cpuPbPointer,
                                         (unsigned*)cpuPbEnd,
                                         pMigTracker->gpuSemaPtr,
                                         UVM_SEM_DONE);

    // Copy pushBuffer limit should take care of release methods.
    cpuPbPointer = pMigTracker->cpuPushBufferPtr;
    cpuPbCopyEnd = (char *) cpuPbEnd - methods;
    methods = 0;

    if (migratedPages != NULL)
        *migratedPages = 0;

    for (pageIndex = startPage; pageIndex < startPage + numPages; ++pageIndex)
    {
        pTracking = pRecord->commitRecordPages[pageIndex];
        if (!pTracking || !PageDirty(pTracking->uvmPage))
            continue;

        pageVirtualAddr = pRecord->baseAddress + (pageIndex << PAGE_SHIFT);
        cpuPhysAddr = page_to_phys(pTracking->uvmPage);
        // The common case takes the break. If the PB is full, we flush the
        // previous copies and retry
        while (1)
        {
            methods = pCopyOps->launchDma((unsigned **)&cpuPbPointer,
                                          (unsigned *) cpuPbCopyEnd,
                                          (UvmGpuPointer)cpuPhysAddr,
                                          NV_UVM_COPY_SRC_LOCATION_SYSMEM,
                                          (UvmGpuPointer)pageVirtualAddr,
                                          NV_UVM_COPY_DST_LOCATION_FB,
                                          PAGE_SIZE,
                                          NV_UVM_COPY_DST_TYPE_VIRTUAL |
                                            NV_UVM_COPY_SRC_TYPE_PHYSICAL);
            if (methods)
                break;

            rmStatus = _wait_for_migration_completion(pMigTracker, pRecord,
                                            (UvmGpuPointer)pageVirtualAddr,
                                            (UvmGpuPointer)cpuPhysAddr,
                                            &cpuPbPointer, cpuPbEnd,
                                            &numMethods);
            if (rmStatus != NV_OK)
            {
                UVM_DBG_PRINT_RL("Failed to copy from cpu to gpu - vma: "
                                 "0x%p, rmStatus: 0x%0x\n",
                                 pRecord->vma, rmStatus);
                break;
            }
            // Reset push buffer pointer to start again from top.
            cpuPbPointer = pMigTracker->cpuPushBufferPtr;
            numMethods = 0;
        }
        numMethods += methods;

        if (migratedPages != NULL)
            ++(*migratedPages);
    }

    // Trigger completion of all copies which didn't completely fill PB.
    if ((numMethods != 0) && (rmStatus == NV_OK))
    {
        UVM_PANIC_ON(0 == cpuPhysAddr);

        // Change address to current page as we have incremented it above
        rmStatus = _wait_for_migration_completion(pMigTracker, pRecord,
                                        (UvmGpuPointer)pageVirtualAddr,
                                        (UvmGpuPointer)cpuPhysAddr,
                                        &cpuPbPointer, cpuPbEnd,
                                        &numMethods);
        if (rmStatus != NV_OK)
            UVM_DBG_PRINT_RL("Failed to copy from cpu to gpu - vma: "
                             "0x%p, rmStatus: 0x%0x\n",
                             pRecord->vma, rmStatus);
    }

    if (recordMigrationEvent && *migratedPages > 0)
    {
        endTime = NV_GETTIME();

        rmStatus = uvm_record_migration_event(
              pRecord->osPrivate->processRecord.pEventContainer,
              UvmEventMigrationDirectionCpuToGpu,
              -1,
              pRecord->cachedHomeGpuPerProcessIndex,
              pageVirtualAddr,
              (*migratedPages) * PAGE_SIZE,
              beginTime,
              endTime,
              pRecord->pStream->streamId);
    }

    return rmStatus;
}

//
// Locking: you must hold a write lock on the mmap_sem.
//
static void _set_record_accessible(UvmCommitRecord *pRecord)
{
    pRecord->isAccessible = NV_TRUE;
}

//
// Locking: you must hold a write lock on the mmap_sem.
//
static void _set_record_inaccessible(UvmCommitRecord *pRecord)
{
    pRecord->isAccessible = NV_FALSE;
}

//
// Locking: you must hold a read lock on the mmap_sem
// and DriverPrivate.uvmPrivLock.
//
static int _is_record_included_in_vma(UvmCommitRecord *pRecord)
{
    return (pRecord->baseAddress >= pRecord->vma->vm_start) &&
           (PAGE_ALIGN(pRecord->length) <= (pRecord->vma->vm_end - pRecord->baseAddress));
}

//
// returns GPU index of matching record, otherwise a first free index
//
// Locking: you must hold g_attached_uuid_lock before calling this routine
//
static unsigned _find_gpu_index(NvProcessorUuid *gpuUuidStruct)
{
    unsigned index;
    for (index = 0; index < g_attached_uuid_num; ++index)
    {
        if (memcmp(gpuUuidStruct, &g_attached_uuid_list[index].gpuUuid,
                   sizeof(NvProcessorUuid)) == 0)
        {
            return index;
        }
    }

    return UVM_INVALID_HOME_GPU_INDEX;
}

//
// Locking: you must hold g_attached_uuid_lock before calling this routine
//
static NV_STATUS _find_or_add_gpu_index(NvProcessorUuid *gpuUuidStruct,
                                        unsigned *pIndex)
{
    NV_STATUS status;
    UvmGpuInfo gpuInfo;
    unsigned index = _find_gpu_index(gpuUuidStruct);

    memset(&gpuInfo, 0, sizeof(UvmGpuInfo));
    if (index == UVM_INVALID_HOME_GPU_INDEX)
    {
        UVM_PANIC_ON(g_attached_uuid_num >= UVM_MAX_GPUS);
        if (g_attached_uuid_num >= UVM_MAX_GPUS)
            return NV_ERR_INSUFFICIENT_RESOURCES;

        // Fetch this GPU's architecture
        status = nvUvmInterfaceGetGpuInfo(gpuUuidStruct, &gpuInfo);
        if (status != NV_OK)
            return status;

        index = g_attached_uuid_num;

        memcpy(&g_attached_uuid_list[index].gpuUuid,
               gpuUuidStruct, sizeof(NvProcessorUuid));

        g_attached_uuid_list[index].gpuArch = gpuInfo.gpuArch;

        ++g_attached_uuid_num;
    }

    *pIndex = index;
    return NV_OK;

}

//
// Locking: you must hold g_attached_uuid_lock before calling this routine
//
static NvBool _is_gpu_kepler_and_above(unsigned index)
{
    NvU32 dGpuArch = g_attached_uuid_list[index].gpuArch;

    //
    // Make sure the arch number is kepler or above and smaller
    // than Tegra arch numbers.
    //
    if (dGpuArch >= NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GK100 &&
        dGpuArch < NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_T13X)
        return NV_TRUE;

    return NV_FALSE;
}

NV_STATUS uvmlite_enable_gpu_uuid(NvProcessorUuid *gpuUuidStruct)
{
    NV_STATUS status;
    unsigned index;

    down_write(&g_attached_uuid_lock);
    status = _find_or_add_gpu_index(gpuUuidStruct, &index);
    if (status == NV_OK)
        g_attached_uuid_list[index].isEnabled = NV_TRUE;

    up_write(&g_attached_uuid_lock);

    return status;
}

NV_STATUS uvmlite_disable_gpu_uuid(NvProcessorUuid *gpuUuidStruct)
{
    NV_STATUS status;
    unsigned index;

    down_write(&g_attached_uuid_lock);
    status = _find_or_add_gpu_index(gpuUuidStruct, &index);
    if (status == NV_OK)
        g_attached_uuid_list[index].isEnabled = NV_FALSE;

    up_write(&g_attached_uuid_lock);

    return status;
}

NV_STATUS uvmlite_find_gpu_index(NvProcessorUuid *gpuUuidStruct, unsigned *pIndex)
{
    NV_STATUS status = NV_OK;
    unsigned index;
    down_read(&g_attached_uuid_lock);

    index = _find_gpu_index(gpuUuidStruct);
    if (index == UVM_INVALID_HOME_GPU_INDEX ||
        !g_attached_uuid_list[index].isEnabled)
    {
        index = UVM_INVALID_HOME_GPU_INDEX;
        status = NV_ERR_GPU_UUID_NOT_FOUND;
    }

    up_read(&g_attached_uuid_lock);

    *pIndex = index;
    return status;
}

NvBool uvmlite_is_gpu_kepler_and_above(NvProcessorUuid *gpuUuidStruct)
{
    NvBool result = NV_FALSE;
    unsigned index;

    down_read(&g_attached_uuid_lock);

    index = _find_gpu_index(gpuUuidStruct);

    if (index != UVM_INVALID_HOME_GPU_INDEX)
        result = _is_gpu_kepler_and_above(index);

    up_read(&g_attached_uuid_lock);

    return result;
}

//
// Locking: you must already have acquired a write lock on these:
//      DriverPrivate.uvmPrivLock
//      g_uvmMpsServersListLock
//
static void _create_unique_mps_handle(NvU64 *pHandle)
{
    struct list_head *pos;
    UvmMpsServer *mpsServer = NULL;

generate_new_handle:
    get_random_bytes(pHandle, 8);

    // loop on existing handles to make sure this is not a duplicate
    list_for_each(pos, &g_uvmMpsServersList)
    {
        mpsServer = list_entry(pos, UvmMpsServer, driverPrivateNode);
        if (*pHandle == mpsServer->handle)
            goto generate_new_handle;
    }
}

//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock, before
// calling this routine.
//
NV_STATUS uvmlite_register_mps_server(DriverPrivate *pPriv,
                                      NvProcessorUuid *gpuUuidArray,
                                      NvLength numGpus,
                                      NvU64 *serverId)
{
    NV_STATUS rmStatus;
    UvmProcessRecord *serverProcess;
    UvmMpsServer *mpsServer = NULL;
    UvmPerProcessGpuMigs *pMig;
    unsigned gpuIdx, resetGpuIdx;

    serverProcess = &pPriv->processRecord;

    // Already an MPS client/server
    if (serverProcess->mpsProcessType != MPS_NOT_ACTIVE)
        return NV_ERR_INVALID_ARGUMENT;

    // MPS server already registered
    if (serverProcess->mpsServer)
        return NV_ERR_INVALID_ARGUMENT;

    // loop around gpus and create migration trackers
    for (gpuIdx = 0; gpuIdx < numGpus; ++gpuIdx)
    {
        unsigned index;
        if (uvmlite_find_gpu_index(&gpuUuidArray[gpuIdx], &index) != NV_OK)
        {
            rmStatus = NV_ERR_OBJECT_NOT_FOUND;
            goto fail;
        }

        pMig = &serverProcess->gpuMigs[index];

        pMig->migTracker = kmem_cache_zalloc(g_uvmMigTrackerCache,
                                             NV_UVM_GFP_FLAGS);

        if (!pMig->migTracker)
        {
            rmStatus = NV_ERR_NO_MEMORY;
            goto fail;
        }

        rmStatus = _create_migration_resources(&gpuUuidArray[gpuIdx],
                                               pMig->migTracker);

        if (NV_OK != rmStatus)
        {
            kmem_cache_free(g_uvmMigTrackerCache, pMig->migTracker);
            pMig->migTracker = NULL;
            UVM_ERR_PRINT_UUID("_create_migration_resource failed for MPS "
                               "server. NV_STATUS: 0x%x\n",
                               &gpuUuidArray[gpuIdx], rmStatus);
            goto fail;
        }
    }

    mpsServer = kmem_cache_zalloc(g_uvmMpsServerCache, NV_UVM_GFP_FLAGS);
    if (!mpsServer)
    {
        rmStatus = NV_ERR_NO_MEMORY;
        goto fail;
    }

    mpsServer->processRecord = serverProcess;
    kref_init(&mpsServer->kref);
    init_rwsem(&mpsServer->mpsLock);
    mpsServer->dying = NV_FALSE;

    down_write(&g_uvmMpsServersListLock);
    _create_unique_mps_handle(&mpsServer->handle);
    list_add(&mpsServer->driverPrivateNode,
             &g_uvmMpsServersList);
    up_write(&g_uvmMpsServersListLock);

    serverProcess->mpsProcessType = MPS_SERVER;
    serverProcess->mpsServer = mpsServer;
    *serverId = mpsServer->handle;

    UVM_DBG_PRINT("Registered MPS server (pid %d)\n", serverProcess->pid);

    return NV_OK;

fail:
    // reset the values we changed
    for (resetGpuIdx = 0; resetGpuIdx < gpuIdx; ++resetGpuIdx)
    {
        unsigned index;
        if (uvmlite_find_gpu_index(&gpuUuidArray[resetGpuIdx], &index) != NV_OK)
            continue;

        pMig = &serverProcess->gpuMigs[index];
        if (pMig->migTracker)
        {
            _destroy_migration_resources(pMig->migTracker);
            kmem_cache_free(g_uvmMigTrackerCache, pMig->migTracker);
            pMig->migTracker = NULL;
        }
    }

    if (mpsServer)
    {
        kmem_cache_free(g_uvmMpsServerCache, mpsServer);
    }
    return rmStatus;
}

// Locking: Acquires the server mpsLock and UvmPrivLock
// If the server is dying, this function will return NV_FALSE
static NvBool _lock_mps_server(UvmProcessRecord *mpsClientProcess)
{
    DriverPrivate *pPriv;

    down_read(&mpsClientProcess->mpsServer->mpsLock);

    if (mpsClientProcess->mpsServer->dying)
    {
        up_read(&mpsClientProcess->mpsServer->mpsLock);
        return NV_FALSE;
    }

    UVM_PANIC_ON(!mpsClientProcess->mpsServer->processRecord);

    pPriv = container_of(mpsClientProcess->mpsServer->processRecord,
                         DriverPrivate, processRecord);
    down_write(&pPriv->uvmPrivLock);
    return NV_TRUE;
}

// Locking: Releases the server mpsLock and UvmPrivLock
static void _unlock_mps_server(UvmProcessRecord *mpsClientProcess)
{
    DriverPrivate *pPriv;

    UVM_PANIC_ON(!mpsClientProcess->mpsServer->processRecord);

    pPriv = container_of(mpsClientProcess->mpsServer->processRecord,
                         DriverPrivate, processRecord);
    up_write(&pPriv->uvmPrivLock);
    up_read(&mpsClientProcess->mpsServer->mpsLock);
}

static NvBool _is_mps_server(UvmProcessRecord *processRecord)
{
    return (processRecord->mpsProcessType == MPS_SERVER);
}

static NvBool _is_mps_client(UvmProcessRecord *processRecord)
{
    return (processRecord->mpsProcessType == MPS_CLIENT);
}

static void _delete_mps_server(struct kref *kref)
{
    UvmMpsServer *mpsServer = container_of(kref, UvmMpsServer, kref);
    kmem_cache_free(g_uvmMpsServerCache, mpsServer);
}

//
// Locking: you must hold g_uvmMpsServersListLock before calling this routine
//
static UvmMpsServer * _find_mps_server(NvU64 handle)
{
    struct list_head *pos;
    UvmMpsServer *mpsServer = NULL;

    list_for_each(pos, &g_uvmMpsServersList)
    {
        mpsServer = list_entry(pos, UvmMpsServer, driverPrivateNode);
        if (handle == mpsServer->handle)
            return mpsServer;
    }

    return NULL;
}

//
// Locking: you must hold a write lock on the DriverPrivate.uvmPrivLock, before
// calling this routine.
//
NV_STATUS uvmlite_register_mps_client(DriverPrivate *pPriv,
                                      NvU64 serverId)
{
    UvmMpsServer *mpsServer = NULL;

    // Already an MPS client/server
    if (pPriv->processRecord.mpsProcessType != MPS_NOT_ACTIVE)
        return NV_ERR_INVALID_ARGUMENT;

    down_read(&g_uvmMpsServersListLock);
    mpsServer = _find_mps_server(serverId);
    up_read(&g_uvmMpsServersListLock);

    if (!mpsServer)
        return NV_ERR_INVALID_ARGUMENT;

    // Allow if the server and the client have the same user id
    if (mpsServer->processRecord->euid != pPriv->processRecord.euid)
        return NV_ERR_INSUFFICIENT_PERMISSIONS;

    pPriv->processRecord.mpsProcessType = MPS_CLIENT;
    pPriv->processRecord.mpsServer = mpsServer;
    kref_get(&mpsServer->kref);

    return NV_OK;
}

NV_STATUS uvmlite_get_gpu_uuid_list(NvProcessorUuid *gpuUuidArray,
                                         unsigned *validCount)
{
    unsigned index;

    down_read(&g_attached_uuid_lock);

    for (index = 0; index < g_attached_uuid_num; ++index)
    {
        memcpy(&gpuUuidArray[index], &g_attached_uuid_list[index].gpuUuid,
               sizeof(NvProcessorUuid));
    }

    *validCount = g_attached_uuid_num;

    up_read(&g_attached_uuid_lock);

    return NV_OK;
}
