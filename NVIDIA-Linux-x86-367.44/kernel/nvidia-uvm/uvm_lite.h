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

//
// uvm_lite.h
//
// This file contains declarations for UVM-Lite code.
//
//

#ifndef _UVM_LITE_H
#define _UVM_LITE_H

#include "uvmtypes.h"
#include "uvm_ioctl.h"
#include "uvm_linux.h"
#include "uvm_kernel_events.h"
#include "uvm_kernel_counters.h"
#include "uvm_debug_session.h"
#include "nvstatus.h"
#include "uvm_page_migration.h"
#include "nv_uvm_interface.h"
#include "uvm_linux.h"

#define UVM_INVALID_HOME_GPU_INDEX     0xDEADBEEF

// Forward declarations:
struct UvmCommitRecord_tag;
struct UvmPerProcessGpuMigs_tag;
struct UvmGpuMigrationTracking_tag;
struct UvmPrefetchInfo_tag;
struct UvmRegionAccess_tag;

#define UVM_MAX_STREAMS 256
#define UVM_STREAMS_CACHE_SIZE 1024

#define UVM_ECC_ERR_TIMEOUT_USEC 100

#define SEMAPHORE_SIZE    4*1024
#define PUSHBUFFER_SIZE   0x4000

#define MEM_RD32(a) (*(const volatile NvU32 *)(a))
#define MEM_RD16(a) (*(const volatile NvU16 *)(a))

//
// UvmPerProcessGpuMigs struct: there is a one-to-one relationship between
// processes that call into the UVM-Lite kernel driver, and this data structure.
//
//
typedef struct UvmPerProcessGpuMigs_tag
{
    struct UvmGpuMigrationTracking_tag * migTracker;
}UvmPerProcessGpuMigs;

typedef struct UvmStreamRecord_tag UvmStreamRecord;

typedef enum
{
    MPS_NOT_ACTIVE = 0,
    MPS_SERVER,
    MPS_CLIENT,
} UvmMpsProcessType_t;

struct UvmMpsServer_tag;

typedef struct UvmProcessRecord_tag
{
    //
    // TODO: Bug 1766109: uvm8: delete UVM-Lite files and remove -lite mode
    //  ...just delete this entire file, instead of the original to-do: which was:
    //
    //      1. "Non-idle" stream list should be stored here.
    //          This in turns allows all "Regions" associated with this process
    //          to be enumerated. Probably going to be useful for the (shared)
    //          bits of the debug/profiler infrastructure
    //      2. Complete list of all GpuRecords for this process
    //

    // indexed according to g_attached_uuid_list
    UvmPerProcessGpuMigs gpuMigs[UVM_MAX_GPUS];

    // List of all streams
    //
    // TODO: Bug 1766109: uvm8: delete UVM-Lite files and remove -lite mode
    //  ...just delete this entire file, instead of the original to-do: which was:
    //
    // should probably be moved to a hashmap
    struct list_head allStreamList;
    // Trivial cache storing recently used streams at streamId mod cache size
    UvmStreamRecord *streamsCache[UVM_STREAMS_CACHE_SIZE];

    //
    // Number of streams (other than NO and ALL streams) in the running state
    // Used for tracking when to start/stop the ALL stream
    //
    NvLength runningStreams;

    UvmMpsProcessType_t mpsProcessType;

    // Pointer to the UvmMpsServer linked to this process.
    // Is only valid if mpsProcessType is MPS_SERVER or MPS_CLIENT.
    struct UvmMpsServer_tag *mpsServer;

    // Per Process Debug Session array.
    UvmSessionInfo sessionInfoArray[UVM_MAX_SESSIONS_PER_PROCESS];
    // This lock is used to protect sessionInfoArray
    struct rw_semaphore sessionInfoLock;

    UvmCounterContainer *pCounterContainer;
    UvmEventContainer   *pEventContainer;

    // effective user id of this process, for security check
    uid_t euid;

    unsigned pid;
}UvmProcessRecord;

typedef struct UvmMpsServer_tag
{
    // Unique handle to identify the server
    NvU64 handle;

    // Pointer to the server uvmProcessRecord structure
    UvmProcessRecord *processRecord;

    // Number of references to this structure
    struct kref kref;

    // Entry in g_uvmMpsServersList
    struct list_head driverPrivateNode;

    // Lock to protect dying state
    struct rw_semaphore mpsLock;

    // Dying state: set to NV_TRUE when the userland process died
    // When set, processRecord is considered invalid
    NvBool dying;
} UvmMpsServer;

struct UvmStreamRecord_tag
{
    UvmProcessRecord *processRecord;
    UvmStream streamId;
    NvBool isRunning;
    struct list_head allStreamListNode;
    struct list_head commitRecordsList;
};

typedef struct UvmGpuState_tag
{
    NvProcessorUuid gpuUuid;
    NvBool isEnabled;
    NvU32 gpuArch;
}UvmGpuState;

// Prefetch command returned by the prefetcher
typedef struct UvmPrefetchHint_tag
{
    NvLength baseEntry;
    NvLength count;
}UvmPrefetchHint;

// Prefetch statistics to allow self-adaptive policies
typedef struct UvmPrefetchRegionCounters_tag
{
    NvLength faults;
    NvLength nprefetch;
}UvmPrefetchRegionCounters;

// Prefetch information attached to a UvmCommitRecord
typedef struct UvmPrefetchInfo_tag
{
    // Allocation counters
    NvLength faultRegions;
    UvmPrefetchRegionCounters counters;

    unsigned threshold;

    NvLength regions;
    struct UvmRegionAccess_tag ** regionPtrs;
}UvmPrefetchInfo;

//
//  Tracks committed regions of memory
//
typedef struct UvmCommitRecord_tag
{
    //
    // Invariant: The home GPU might stop running, or get an RC recovery that
    // kills the channel that this commit record uses. However, the home GPU's
    // identity will not change.
    //
    // As it turns out, all we actually need is the UUID of this home GPU.
    //
    NvProcessorUuid homeGpuUuid; // GPU that owns the allocation

    //
    // This is used to index into UvmProcessRecord.gpuMigs[].  Allowed values
    // are:
    //
    //     1. Zero through (UVM_MAX_GPUS-1)
    //
    //     2. UVM_INVALID_HOME_GPU_INDEX
    //
    // If it is UVM_INVALID_HOME_GPU_INDEX, then it must not be used.
    //
    unsigned         cachedHomeGpuPerProcessIndex;

    NvUPtr           baseAddress;   // immutable, must be page aligned
    NvLength         length;        // immutable, must be page aligned

    // This flag is used to indicate that at least one page in the record
    // is mapped for CPU access
    NvBool isMapped;

    // This flag is used to indicated if the memory represented by the
    // record is accessible or not
    NvBool isAccessible;

    NvBool hasChildren;
    NvBool isChild;

    struct DriverPrivate_tag * osPrivate;
    struct vm_area_struct * vma;
    struct UvmPageTracking_tag ** commitRecordPages;

    UvmStreamRecord * pStream;
    struct list_head streamRegionsListNode;

    UvmPrefetchInfo prefetchInfo;
}UvmCommitRecord;

typedef struct DriverPrivate_tag
{
    struct list_head       pageList;
    UvmProcessRecord  processRecord;
    struct rw_semaphore    uvmPrivLock;
    struct file            *privFile;
    // Entry in g_uvmDriverPrivateTable
    struct list_head       driverPrivateNode;
}DriverPrivate;

//
// These are the UVM datastructures that we need. This should be allocated
// per-process, per-GPU, when setting up a new channel for a process during the
// first call to UvmRegionCommit.
//
typedef struct UvmGpuMigrationTracking_tag
{
    uvmGpuSessionHandle      hSession;
    uvmGpuAddressSpaceHandle hVaSpace;
    uvmGpuChannelHandle      hChannel;
    uvmGpuCopyEngineHandle   hCopyEngine;
    unsigned                 ceClassNumber;
    UvmGpuChannelPointers    channelInfo;
    UvmCopyOps               ceOps;
    UvmGpuCaps               gpuCaps;

    // per channel allocations
    // pushbuffer
    UvmGpuPointer            gpuPushBufferPtr;
    void                    *cpuPushBufferPtr;
    unsigned                 currentPbOffset;
    unsigned                 currentGpFifoOffset;
    // semaphore
    UvmGpuPointer            gpuSemaPtr;
    void                    *cpuSemaPtr;
    // need to add a lock per push buffer
}UvmGpuMigrationTracking;

#define UVM_MIGRATE_DEFAULT       0
#define UVM_MIGRATE_OUTDATED_ONLY 1

NV_STATUS migrate_gpu_to_cpu(UvmGpuMigrationTracking *pMigTracker,
                              UvmCommitRecord *pRecord,
                              NvLength startPage,
                              NvLength numPages,
                              int migrationFlags,
                              NvLength *migratedPages);

NV_STATUS migrate_cpu_to_gpu(UvmGpuMigrationTracking *pMigTracker,
                              UvmCommitRecord *pRecord,
                              NvLength startPage,
                              NvLength numPages,
                              NvLength *migratedPages);

struct file;

//
//
// UVM-Lite char driver entry points:
//
int uvmlite_init(dev_t uvmBaseDev);
void uvmlite_exit(void);
int uvmlite_setup_gpu_list(void);

NV_STATUS uvmlite_gpu_event_start_device(NvProcessorUuid *gpuUuidStruct);
NV_STATUS uvmlite_gpu_event_stop_device(NvProcessorUuid *gpuUuidStruct);

NV_STATUS uvmlite_set_stream_running(DriverPrivate* pPriv, UvmStream streamId);
NV_STATUS uvmlite_set_streams_stopped(DriverPrivate* pPriv,
                                      UvmStream * streamIdArray,
                                      NvLength nStreams);
NV_STATUS uvmlite_region_set_stream(UvmCommitRecord *pRecord,
    UvmStream streamId);
NV_STATUS uvmlite_migrate_to_gpu(unsigned long long baseAddress,
                                  NvLength length,
                                  unsigned migrateFlags,
                                  struct vm_area_struct *vma,
                                  UvmCommitRecord * pRecord);
//
// UVM-Lite core functionality:
//
struct s_UvmRegionTracker;
UvmCommitRecord *
uvmlite_create_commit_record(unsigned long long  requestedBase,
                             unsigned long long  length,
                             DriverPrivate *pPriv,
                             struct vm_area_struct *vma);

NV_STATUS uvmlite_update_commit_record(UvmCommitRecord *pRecord,
                                       UvmStream streamId,
                                       NvProcessorUuid *pUuid,
                                       DriverPrivate *pPriv);
NV_STATUS uvmlite_split_commit_record(UvmCommitRecord *pRecord,
                                      struct s_UvmRegionTracker *pTracker,
                                      unsigned long long splitPoint,
                                      UvmCommitRecord **outRecordLeft,
                                      UvmCommitRecord **outRecordRight);
NV_STATUS uvmlite_attach_record_portion_to_stream(UvmCommitRecord *pRecord,
                                                  UvmStream newStreamId,
                                                  struct s_UvmRegionTracker *pRegionTracker,
                                                  unsigned long long start,
                                                  unsigned long long length);
void uvmlite_destroy_commit_record(UvmCommitRecord *pRecord);

//
// UVM-Lite page cache:
//
typedef struct UvmPageTracking_tag
{
    struct page *uvmPage;
    struct list_head pageListNode;
}UvmPageTracking;

int uvm_page_cache_init(void);
void uvm_page_cache_destroy(void);

UvmPageTracking * uvm_page_cache_alloc_page(DriverPrivate* pPriv);
void uvm_page_cache_free_page(UvmPageTracking *pTracking, const char *caller);
void uvm_page_cache_verify_page_list_empty(DriverPrivate* pPriv,
                                           const char * caller);
//
// UVM-Lite: MPS support
//
NV_STATUS uvmlite_register_mps_server(DriverPrivate *pPriv,
                                      NvProcessorUuid *gpuUuidArray,
                                      NvLength numGpus,
                                      NvU64 *serverId);
NV_STATUS uvmlite_register_mps_client(DriverPrivate *pPriv,
                                      NvU64 serverId);

//
// UVM-Lite outer layer: UVM API calls that are issued via ioctl() call:
//
NV_STATUS uvm_api_reserve_va(UVM_RESERVE_VA_PARAMS *pParams, struct file *filp);
NV_STATUS uvm_api_release_va(UVM_RELEASE_VA_PARAMS *pParams, struct file *filp);
NV_STATUS uvm_api_region_commit(UVM_REGION_COMMIT_PARAMS *pParams,
                               struct file *filp);
NV_STATUS uvm_api_region_decommit(UVM_REGION_DECOMMIT_PARAMS *pParams,
                                 struct file *filp);
NV_STATUS uvm_api_region_set_stream(UVM_REGION_SET_STREAM_PARAMS *pParams,
                                  struct file *filp);
NV_STATUS uvm_api_region_set_stream_running(UVM_SET_STREAM_RUNNING_PARAMS *pParams,
                                   struct file *filp);
NV_STATUS uvm_api_region_set_stream_stopped(UVM_SET_STREAM_STOPPED_PARAMS *pParams,
                                   struct file *filp);
NV_STATUS uvm_api_migrate_to_gpu(UVM_MIGRATE_TO_GPU_PARAMS *pParams,
                               struct file *filp);
NV_STATUS uvm_api_run_test(UVM_RUN_TEST_PARAMS *pParams, struct file *filp);
NV_STATUS uvm_api_add_session(UVM_ADD_SESSION_PARAMS *pParams,
                             struct file *filp);
NV_STATUS uvm_api_remove_session(UVM_REMOVE_SESSION_PARAMS *pParams,
                                struct file *filp);
NV_STATUS uvm_api_enable_counters(UVM_ENABLE_COUNTERS_PARAMS *pParams,
                                 struct file *filp);
NV_STATUS uvm_api_map_counter(UVM_MAP_COUNTER_PARAMS *pParams,
                             struct file *filp);
NV_STATUS uvm_api_register_mps_server(UVM_REGISTER_MPS_SERVER_PARAMS *pParams,
                                      struct file *filp);
NV_STATUS uvm_api_register_mps_client(UVM_REGISTER_MPS_CLIENT_PARAMS *pParams,
                                      struct file *filp);
NV_STATUS uvm_api_create_event_queue(UVM_CREATE_EVENT_QUEUE_PARAMS *pParams,
                                     struct file *filp);
NV_STATUS uvm_api_remove_event_queue(UVM_REMOVE_EVENT_QUEUE_PARAMS *pParams,
                                     struct file *filp);
NV_STATUS uvm_api_map_event_queue(UVM_MAP_EVENT_QUEUE_PARAMS *pParams,
                                  struct file *filp);

NV_STATUS uvm_api_event_ctrl(UVM_EVENT_CTRL_PARAMS *pParams,
                             struct file *filp);

NV_STATUS uvm_api_get_gpu_uuid_table(UVM_GET_GPU_UUID_TABLE_PARAMS *pParams,
                                     struct file *filp);

NV_STATUS uvm_api_is_8_supported_lite(UVM_IS_8_SUPPORTED_PARAMS *pParams,
                                      struct file *filp);

NV_STATUS uvm_api_pageable_mem_access_lite(UVM_PAGEABLE_MEM_ACCESS_PARAMS *params,
                                           struct file *filp);

NV_STATUS uvmlite_secure_get_process_containers(
        unsigned pidTarget,
        UvmCounterContainer **ppCounterContainer,
        UvmEventContainer **ppEventContainer,
        uid_t *pEuid);

void umvlite_destroy_per_process_gpu_resources(NvProcessorUuid *gpuUuidStruct);

NV_STATUS uvmlite_gpu_event_start_device(NvProcessorUuid *gpuUuidStruct);
NV_STATUS uvmlite_gpu_event_stop_device(NvProcessorUuid *gpuUuidStruct);

NV_STATUS uvmlite_find_gpu_index(NvProcessorUuid *gpuUuidStruct, unsigned *pIndex);

NV_STATUS uvmlite_enable_gpu_uuid(NvProcessorUuid *gpuUuidStruct);

NV_STATUS uvmlite_disable_gpu_uuid(NvProcessorUuid *gpuUuidStruct);

NvBool uvmlite_is_gpu_kepler_and_above(NvProcessorUuid *gpuUuidStruct);

NV_STATUS uvmlite_get_process_record(unsigned pidTarget,
                                     UvmProcessRecord **pProcessRecord);

NV_STATUS uvmlite_get_gpu_uuid_list(NvProcessorUuid *gpuUuidArray,
                                         unsigned *validCount);

#endif // _UVM_LITE_H
