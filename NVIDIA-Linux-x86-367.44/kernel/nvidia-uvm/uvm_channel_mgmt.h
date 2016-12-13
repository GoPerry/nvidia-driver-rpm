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

#ifndef _UVM_CHANNEL_MGMT_H_
#define _UVM_CHANNEL_MGMT_H_

#include "uvm_linux.h"
#include "uvmtypes.h"
#include "uvm_page_migration.h"
#include "uvm_mmu_mgmt.h"
#include "mmu/gmmu_fmt.h"

// 
// A pushbuffer needs to accomodate all possible operations on
// a 2 Mb Va region per gpu. The longest sequence of operations would be:
// Acquire 3 + 32 trackers:
// replay tracker, instancePtr tracker, 2Mb descriptor tracker and 32 trackers
// one each for 64Kb of phys mem.
// Each tracker can have ~64 tracker items  (35 x 64 x 20 bytes acquire  = 45k)
// Unmap 4k ptes for 2Mb va                 (Inline pte data + header    = ~4k)
// Invalidate for every 4k                  (512 * 20 bytes              = 10k)
// Migrate data worth 2 Mb                  (512 * 48 bytes to do copy   = 24k)
// Map 4k ptes for 2Mb va                   (4k inline pte data + header = ~4k)
// Invalidate for every 4k                  (512 * 20 bytes              = 10k)
// Total                                                            Total= ~100k
//
// TODO: Bug 1766104: uvm8: delete uvmfull/ subdirectory before release
// ...just delete this entire file, instead of the original to-do: which was:
//
// Optimize acquire, launchDma etc methods to use auto-incrmenting versions
// The above calculations are based on auto-incrementing methods.
//
#define UVM_PUSHBUFFER_RESERVATION_SIZE      (128 * 1024u)

// Same as number of Copy channels on the gpu.
#define UVM_RINGBUFFER_POOL_DEFAULT_SIZE     2u
#define UVM_RINGBUFFER_DEFAULT_SIZE         (2 * UVM_PUSHBUFFER_RESERVATION_SIZE)

#define UVM_CHANNEL_POOL_DEFAULT_SIZE        UVM_RINGBUFFER_POOL_DEFAULT_SIZE

#define UVM_READ_SEMA(sema) (*(volatile NvU32*)sema)

//
// Example usage:
//
//   NvBool isPushSuccess;
//
//   UVM_PUSH_METHOD(
//       isPushSuccess, pushbuffer, ceOps.launchDma,
//       // ... arguments);
//
//   if (!isPushSuccess)
//       goto cleanup;
//
// Continue to use CE HAL methods as-is, just omit the first two arugments:
// 1. unsigned **pbPut
// 2. unsigned *pbEnd
//
#define UVM_PUSH_METHOD(ret, pb, func, ...) \
    do { \
        NvU64 numMethods = func( \
                (unsigned**)&pb->pbOffset, \
                (unsigned*)((NvU64)pb->cpuBegin + \
                            UVM_PUSHBUFFER_RESERVATION_SIZE), \
                __VA_ARGS__); \
        if (numMethods) \
        { \
            pb->curOffset += numMethods; \
            ret = NV_TRUE; \
        } \
        else \
        { \
            ret = NV_FALSE; \
        } \
    } while(0)

//
// Channel Mgmt API
//
// The Api provides channel management functionalities for Linux.
//
// 1. Start-up/Shut-down channel mgmt api.
// 2. Get/Submit pushbuffers.
// 3. Track/Wait on submitted work.
//
// uvm_* APIs are external APIs that the rest of UVM uses.
//
// uvm_channel_manager_* APIs are intenral APIs that only channel API should access.
//
// Uvm tracker and pushbuffer api usage:
//
// UvmTrackerItem trackerItem;
// UvmTracker* tempTracker = uvm_allocate_tracker();
// uvm_grow_tracker(tempTracker, BITCOUNT(activeGpuMask));
//
// for each channelManager in activeGpuMask
//     pushbuffer[i] = uvm_get_pushbuffer(channelManager);
//     lock(va_range);
//
// for each gpu in va_range
//      uvm_reserve_acquire_space(pushbuffer[i], va_range.tracker);
//      PUSH_METHODS(pushbuffer[i]);
//      uvm_submit_pushbuffer(pushbuffer[i], va_range.tracker, &trackerItem);
//      uvm_merge_tracker_item(tempTracker, trackerItem);
//
// uvm_move_tracker(va_range.tracker, tempTracker);
// Optional: uvm_wait_for_tracker(va_range.tracker);
// unlock(va_range);
// Optional: uvm_free_tracker(tempTracker);
// Optional: uvm_reset_tracker(tempTracker);
//
// Channel Mgmt API Locking
//
// The goal is to make locking in this API orthogonal to the rest of UVM.
// Taking locks elsewhere in other UVM components shouldn't affect locking
// inside the channel mgmt API.
//
// Lock abstraction:
//
//  uvm_lock_channel_manager
//  uvm_unlock_channel_manager
//
// Above functions are provided to lock/unlock UvmChannelManager.
//
// uvm_* functions use the above APIs, thus the caller doesn't need to
// grab locks before calling them.
//
// uvm_channel_manager* functions do not acquire/release
// locks. Caller is solely responsible to ensure proper locking before
// calling them.
//
// The only exception is: uvm_wait_for_tracker.
// * No lock needed to call this function.
//
// Channel Management Memory Object:
// The channel management api houses a global memory object which tracks all 
// active channelpools. This object is also needed to manage the global pool of 
// semaphores mapped by all the channelpools. Anytime a new channelpool is being
// added to the system; it gets an unique id from the memory object and based on
// that indexes into a segment of semaphores to use from the global pool.
// The creation and deletion of channelmanager/channelpool results in momentary 
// acquire of the memory object lock for bookkeeping.
//
// The channel management memory object lock is acquired whenever a channelpool/
// channelmanager is created or destroyed. This lock is different from the per 
// channelManager lock. The thread destroying the channelMangaer will unlink the
// associated channel pool from the global memory object in the process.
//

// Size with timestamp
#define UVM_SEMAPHORE_SIZE_BYTES                (16u)
// This should point to max number of gpus
#define UVM_MAX_NUM_CHANNEL_POOLS               (64u)
#define UVM_SEMAPHORE_POOL_SIZE_BYTES           UVM_MAX_NUM_CHANNEL_POOLS * \
                                                UVM_CHANNEL_POOL_DEFAULT_SIZE *\
                                                UVM_SEMAPHORE_SIZE_BYTES

#define UVM_SEMAPHORE_POOL_SIZE_PAGES     \
                           (((UVM_SEMAPHORE_POOL_SIZE_BYTES - 1)/PAGE_SIZE) + 1)

// Forward declarations:
typedef struct UvmTrackerItem_tag           UvmTrackerItem;
typedef struct UvmTracker_tag               UvmTracker;
typedef struct UvmPushbuffer_tag            UvmPushbuffer;
typedef struct UvmRingbuffer_tag            UvmRingbuffer;
typedef struct UvmRingbufferPool_tag        UvmRingbufferPool;
typedef struct UvmChannel_tag               UvmChannel;
typedef struct UvmChannelTracking_tag       UvmChannelTracking;
typedef struct UvmChannelPool_tag           UvmChannelPool;
typedef struct UvmChannelManager_tag        UvmChannelManager;
typedef struct UvmChannelMgmtMemory_tag     UvmChannelMgmtMemory;
typedef struct UvmChannelSemaphorePool_tag  UvmChannelSemaphorePool;
typedef struct UvmPbInlineRegion_tag       UvmPbInlineRegion;

struct UvmTrackerItem_tag
{
    NvU64               seqNum;
    UvmChannel          *channel;
    struct list_head    list;
};

struct UvmTracker_tag
{
    size_t nTotalItems;
    size_t nUsedItems;
    struct list_head itemHead;
    struct list_head *usedTail;    // usedTail->next is free if not last.
};

//
// UVM Pushbuffer Structure
//
// Pushbuffer structure is either owned by:
//   1. the pbFreeListHead in UvmRingbufferPool, or
//   2. the pbListHead in UvmRingbuffer.
//
// Use pbList to traverse the list that owns this pushbuffer structure.
//
struct UvmPushbuffer_tag
{
    //
    // Pushbuffer address and offset
    //
    // 1. CPU begin address of this pb segment
    // 2. GPU begin address of this pb segment
    // 3. The address which you can write methods to
    // 4. The current pushbuffer offset from the beginning
    //
    NvUPtr        cpuBegin;
    UvmGpuPointer gpuBegin;
    NvUPtr        pbOffset;
    NvU64         curOffset;

    UvmRingbuffer *ringbuffer;
    UvmChannel    *channel;

    NvU32 gpFifoOffset;
    NvU64 seqNum;

    // Signifies that a PB region is pending and needs to be closed before
    // opening a new one.
    NvBool bRegionPending;

    NvBool acquireSpaceRsvd;
    size_t nRsvdAcquires;

    struct list_head pbList;
    struct list_head pbSubmittedList;   // Connected to other pb pending completion
                                        // in the same channel.
};

//
// UVM Ringbuffer Structure
//
struct UvmRingbuffer_tag
{
    //
    // Ringbuffer address and offset
    //
    NvUPtr        cpuBegin;         // inclusive
    NvUPtr        cpuEnd;           // exclusive
    UvmGpuPointer gpuBegin;
    NvU64         curOffset;        // where we can start the next pushbuffer

    struct list_head pbListHead;
    struct list_head ringbufferList;
    struct list_head ringbufferFreeList;
};

struct UvmRingbufferPool_tag
{
    // Begin addresses of ringbuffer pool memory area
    UvmGpuPointer gpuPtr;
    NvUPtr        cpuPtr;

    unsigned      numRingbuffers;

    struct list_head ringbufferListHead;
    struct list_head ringbufferFreeListHead;
    struct list_head pbFreeListHead;    // free pb structures
};

struct UvmChannelTracking_tag
{
    NvU32 curGpFifoOffset;
    NvU32 numReservedGpFifoEntries;
    NvU32 numFreeGpFifoEntries;

    UvmGpuPointer semaGpuPointer;
    NvUPtr        semaCpuPointer;

    // seqNumDone should always be accessed atomically/using spinlock.
    NV_ATOMIC64 seqNumDone;
    NvU64 seqNumPending;

    // Lock protecting the seqNumDone which emulates the hw semaphore.
    spinlock_t lock;
};

struct UvmChannel_tag
{
    uvmGpuChannelHandle    hChannel;
    UvmGpuChannelPointers  channelInfo;
    uvmGpuCopyEngineHandle hCopyEngine;
    unsigned               ceClassNumber;
    UvmCopyOps             ceOps;
    UvmMemOps              memOps;
    // Num of bytes an acquire method takes.
    size_t                 acquireBytes;
    NvU32                  id;

    // Channel status tracking information
    UvmChannelTracking trackingInfo;

    // Back pointer to channelPool
    UvmChannelPool        *pool;

    struct list_head pbSubmittedListHead;       // List of pbs pending completion
                                                // in this channel.
    struct list_head channelList;
};

struct UvmChannelSemaphorePool_tag
{
    UvmGpuPointer semaGpuPointerBase;
    NvUPtr        semaCpuPointerBase;
};

struct UvmChannelPool_tag
{
    uvmGpuSessionHandle      hSession;
    uvmGpuAddressSpaceHandle hVaSpace;
    UvmGpuCaps               gpuCaps;

    // Initialized and used by idenity
    // map setup - (PASCAL only)
    GMMU_FMT                *pGmmuFmt;

    NvU32    numChannels;
    NvU32    poolId;

    // Offset of the semaphores for the channels in pool.
    NvU32    semaOffset;

    // Gpu and Cpu mappings to the semaphore pool.
    UvmChannelSemaphorePool semaPool;

    // Back pointer to channelManager
    UvmChannelManager      *manager;

    // Node in the list of pools in the memory object
    struct list_head    poolList;
    struct list_head    channelListHead;
};

struct UvmChannelManager_tag
{
    UvmChannelPool    channelPool;
    UvmRingbufferPool ringbufferPool;

    struct rw_semaphore channelManagerLock;
};

struct UvmChannelMgmtMemory_tag
{
    // Num of channel pools allocated. Each channel has a dedicated sema loc.
    // The count cannot decrement and provides a unique id to the pool.
    size_t          poolCount;

    // Num of mappings to the semaphore pool
    size_t          mapCount;

    NvBool          bSemaPoolAllocated;

    // List of active channel pools
    struct list_head            activePoolsHead;

    // Lock protecting the semaphore allocations
    struct rw_semaphore         lock;
};

//
// An inline region can be used by the user to create a NOP section in the pb.
// The region can be used for staging a copy (in which case the user will close
// the region with copy params passed into the api) or just to create a carveout
// in the pushbuffer that the gpu would not parse.
//
struct UvmPbInlineRegion_tag
{
    NvUPtr          nopLocation;
    void            *regionStart; // start of user data
    size_t          size;         // Filled by user when ending the region.
                                  // User specific data size.

    struct
    {
        NvBool      bValid;        // To be updated by the user
        size_t      copySize;      // Size to be copied from the region start.
        NvU64       dstAddr;
        NvU32       dstAperture;   // NV_UVM_COPY_DST_LOCATION _SYSMEM | _FB
        NvU32       dstCopyFlags;  // NV_UVM_COPY_DST_TYPE _VIRTUAL | _PHYSICAL
    } copy;
};

//
// Initalize channel API global resources.
//
// Thread Safety: must be called only in a single thread.
//
NV_STATUS uvm_initialize_channel_mgmt_api(void);

//
// Deinitalize/destroy channel API global resources.
//
// Thread Safety: must be called only in a single thread.
//
void uvm_deinitialize_channel_mgmt_api(void);

//
// Create channel/pushbuffer resources associated with this GPU.
//
NV_STATUS uvm_create_channel_manager(NvProcessorUuid *gpuUuid,
                                     UvmChannelManager **channelManager);

//
// Destroy channel/pushbuffer resources tied to this channel manager.
//
// Caller must ensure proper sychronization.
//
// i.e. No one else should be touching this object during and after
//      calling this function.
//
void uvm_destroy_channel_manager(UvmChannelManager *channelManager);

//
// Lock UvmChannelManager
//
// This function should be called before using any fields in UvmChannelManager.
//
void uvm_lock_channel_manager(UvmChannelManager *channelManager);

//
// Unlock UvmChannelManager
//
// This function should be called after using UvmChannelManager
//
void uvm_unlock_channel_manager(UvmChannelManager *channelManager);

//
// Grab a pushbuffer from this channel manager.
//
// Lock channel manager before calling.
//
NV_STATUS uvm_get_pushbuffer(
        UvmChannelManager *channelManager, UvmPushbuffer **pushbuffer);

//
// Cancel a pushbuffer in this channel manager.
//
// Lock channel manager before calling.
//
void uvm_cancel_pushbuffer(
        UvmChannelManager *channelManager, UvmPushbuffer *pushbuffer);

//
// Submit this pushbuffer and provide the new acquire information in the
// given trackerItem.
//
NV_STATUS uvm_submit_pushbuffer(
        UvmChannelManager *channelManager, UvmPushbuffer *pushbuffer,
        UvmTracker *trackerToAcquire, UvmTrackerItem *trackerItem);

//
// The client needs to use this function to make sure enough space is reserved
// at the start of the pushbuffer which can be used at submit time to push
// acquires. The function will also update the pbOffset as if acquires were
// pushed.
//
void uvm_reserve_acquire_space(
        UvmChannelManager *channelManager, UvmPushbuffer *pushBuffer,
        UvmTracker *tracker);

//
// Wait for tracker on the cpu side
//
// No lock required for this call.
//
NV_STATUS uvm_wait_for_tracker(UvmTracker *tracker);

//
// Uvm Channel Mgmt Internal APIs
//
// Hold a lock to UvmChannelManager before calling any of the below.
//

//
// Find a freely available pushbuffer structure for this particular
// channel manager. If we can't find any, allocate a new pushbuffer
// structure for this particular channel manager.
//
NV_STATUS uvm_channel_manager_alloc_pushbuffer_structure(
        UvmChannelManager *channelManager, UvmPushbuffer **pushbuffer);

//
// Free this pushbuffer structure into the freelist.
//
// This does not actually free the memory.
//
void uvm_channel_manager_free_pushbuffer_structure(
        UvmChannelManager *channelManager, UvmPushbuffer *pushbuffer);

//
// Get an available ringbuffer from the pool.
//
NV_STATUS uvm_channel_manager_get_ringbuffer(
        UvmChannelManager *channelManager, UvmRingbuffer **ringbuffer);

//
// Put a ringbuffer back to the pol's free list.
//
void uvm_channel_manager_put_ringbuffer(
        UvmChannelManager *channelManager, UvmRingbuffer *ringbuffer);

//
// Get a channel with spare GPFIFOs from the pool.
// Also reserve a GPFIFO entry
//
NV_STATUS uvm_channel_manager_get_channel(
        UvmChannelManager *channelManager, UvmChannel **channel);

//
// Call this function when you cancel a pushbuffer, and want to
// restore the channel to its previous state.
//
// Put a channel back to the beginning the pool.
// Also unreserve a GPFIFO entry.
//
void uvm_channel_manager_put_channel(
        UvmChannelManager *channelManager, UvmChannel *channel);

//
// Update all channels progress information.
//
void uvm_update_all_channel_progress_for_manager(
        UvmChannelManager *channelManager);

//
// Update channel progress information. Returns the latest semaphore value.
//
NvU64 uvm_update_channel_progress(UvmChannel *channel);

//
// Reclaim pushbuffer memory and GPFIFO entries in this channel manager.
//
NV_STATUS uvm_channel_manager_reclaim(UvmChannelManager *channelManager);

//
// Allocate a UvmTracker.
// The function will allocate memory.
// Can be followed up by a resize_tracker() call to add trackerItems.
//
UvmTracker* uvm_allocate_tracker(void);

//
// Initializes a tracker with zero items.
// Example: This function needs to be called for trackers on stack.
//
void uvm_init_tracker(UvmTracker *tracker);

//
// Deallocate the UvmTracker.
// Deletes memory for tracker and all trackerItems.
//
void uvm_free_tracker(UvmTracker *tracker);

//
// Resets the tracker items associated with the tracker. No deallocation.
// Moves all tracker items to free list.
// Not thread safe.
//
void uvm_reset_tracker(UvmTracker *tracker);

//
// Grows the tracker to have nItems tracker items.
// May allocate blocks to free list in the tracker. Does not do anything if
// the tracker already has items more or equal to nItems.
// Not thread safe.
//
NV_STATUS uvm_grow_tracker(UvmTracker *tracker, size_t nItems);


//
// Shrinks the tracker to only used items.
// Frees all blocks from free list in the tracker.
// Returns num of used items(= total items) after freeing all the unused items.
// Not thread safe.
//
size_t uvm_shrink_tracker(UvmTracker *tracker);

//
// This is a destructive copy operation that may steal memory from src. The
// previous contents of dst are overwritten with those of src. If dst does not
// have enough item space to complete the copy, the necessary storage blocks are
// taken from src. In all cases when this returns src behaves as if
// uvm_reset_tracker was called on it.
//
void uvm_move_tracker(UvmTracker *dst, UvmTracker *src);

//
// This functions checks whether all the valid tracker items in the given
// tracker have had their channels attain the acquire-seq-num.
// Returns
//      - RM_OK if all tracker items are done
//      - RM_WARN_MORE_PROCESSING_REQUIRED if items are pending
//      - RM_ERR_ECC_ERROR if an ECC error happened
//      - RM_ERR_RC_ERROR if a RC happened
//
NV_STATUS uvm_query_tracker(UvmTracker *tracker);

//
// This functions merges a item's information into the tracker.
// The function expects the tracker to have enough free space to accomodate the
// item info. (ie a free tracker item).
//
// Returns
//      - NV_OK if merge is successful
//      - NV_ERR_NO_MEMORY if there are no free items left.
//
NV_STATUS uvm_merge_tracker_item(UvmTracker *tracker, UvmTrackerItem *item);

//
// This functions starts an inline region in the given pushbuffer.
// The region can be used to fill user private data that would be ignored by
// by the gpu when parsing the pushbuffer.
//
void uvm_pushbuffer_inline_start(UvmPushbuffer *pb,
                                 UvmPbInlineRegion *region);
//
// This function ends the inline region in the given pushbuffer.
// The user cannot have mutiple outstanding regions opened at any given time.
//
void uvm_pushbuffer_inline_end(UvmPushbuffer *pb,
                                    UvmPbInlineRegion *region);

//
// This function allows the user to stage a copy from the given inline
// region as src buffer. The destination copy parameters need to be filled by
// the user in the "region->copy" structure. The region needs to be "ended" 
// before launching any copy out of it. The function pushes a method to
// do a gpu-virtual src copy from pushbuffer(mostly sysmem) to the
// user-defined destination location and params.
//
size_t uvm_pushbuffer_copy_region(UvmPushbuffer *pb,
                                      UvmPbInlineRegion *region);

//
// This functions starts an inline region in the given pushbuffer which would
// be used to stage a copy as soon as the region is closed/ended.
//
void uvm_pushbuffer_inline_copy_region_start(UvmPushbuffer *pb,
                                             UvmPbInlineRegion *region);

//
// This function ends the inline region in the given pushbuffer and launches a
// copy out of it based on the params provided by the user in the region->copy
// structure.
//
size_t uvm_pushbuffer_inline_copy_region_end(UvmPushbuffer *pb,
                                           UvmPbInlineRegion *region);

#endif // _UVM_CHANNEL_MGMT_H_
