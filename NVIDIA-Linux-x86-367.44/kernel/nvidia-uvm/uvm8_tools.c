/*******************************************************************************
    Copyright (c) 2016 NVIDIA Corporation

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
#include "uvm8_gpu.h"
#include "uvm8_tools.h"
#include "uvm8_va_space.h"
#include "uvm8_init.h"
#include "uvm8_api.h"
#include "uvm8_hal_types.h"
#include "uvm8_va_block.h"
#include "uvm8_va_range.h"
#include "uvm8_push.h"
#include "uvm8_forward_decl.h"
#include "uvm8_range_group.h"

// We limit the number of times a page can be retained by the kernel
// to prevent the user from maliciously passing UVM tools the same page
// over and over again in an attempt to overflow the refcount.
#define MAX_PAGE_COUNT (1 << 20)

typedef struct
{
    NvU32 get_ahead;
    NvU32 get_behind;
    NvU32 put_ahead;
    NvU32 put_behind;
} uvm_tools_queue_snapshot_t;

typedef struct
{
    uvm_spinlock_t lock;
    NvU64 subscribed_queues;
    struct list_head queue_nodes[UvmEventNumTypes];

    struct page **queue_buffer_pages;
    UvmEventEntry *queue;
    NvU32 queue_buffer_count;
    NvU32 notification_threshold;

    struct page **control_buffer_pages;
    UvmToolsEventControlData *control;

    wait_queue_head_t wait_queue;
    bool is_wakeup_get_valid;
    NvU32 wakeup_get;
} uvm_tools_queue_t;

typedef struct
{
    struct list_head counter_nodes[UVM_TOTAL_COUNTERS];
    NvU64 subscribed_counters;

    struct page **counter_buffer_pages;
    NvU64 *counters;

    bool all_processors;
    NvProcessorUuid processor;
} uvm_tools_counter_t;

// private_data for /dev/nvidia-uvm-tools
typedef struct
{
    bool is_queue;
    struct file *uvm_file;
    union
    {
        uvm_tools_queue_t queue;
        uvm_tools_counter_t counter;
    };
} uvm_tools_event_tracker_t;

typedef struct
{
    // Part of a list rooted at va_space->tools.channel_list
    // which is a list of channels with pending pushes that have events associated with them
    struct list_head channel_list_node;
    uvm_channel_t *channel;

    // The lifetime of this object depends on two things:
    // 1) whether pending_event_count is zero.  If it is, then this
    //    object does not need to be in the list channels with pending events.
    // 2) whether the parent block_migration_data_t has been fully
    //    processed, resulting in parent_alive being cleared.
    // Iff both of these conditions are true, the object can be freed.
    // These objects are allocated together for efficiency.
    NvU64 pending_event_count;
    bool parent_alive;
} tools_channel_entry_t;

typedef struct
{
    nv_kthread_q_item_t queue_item;
    uvm_processor_id_t dst;
    uvm_processor_id_t src;
    uvm_va_space_t *va_space;

    // The block_migration_data_t is used as a channel entry if it is the first
    // entry for that channel when it is enqueued.  In this situation, its self_channel_entry
    // field becomes the channel entry, and its channel_entry field points to self_channel_entry.
    // This migration will become the channel_entry for all subsequent events for this channel.
    // Otherwise, there is an existing channel entry for that channel, so this
    // block_migration_data_t's channel_entry field points to the existing entry.
    tools_channel_entry_t *channel_entry;
    tools_channel_entry_t self_channel_entry;
    struct list_head events;
    NvU64 start_timestamp_cpu;
    NvU64 end_timestamp_cpu;
    NvU64 *start_timestamp_gpu_addr;
    NvU64 start_timestamp_gpu;
    NvU64 range_group_id;
    UvmEventMigrationCause cause;
} block_migration_data_t;

typedef struct
{
    struct list_head events_node;
    NvU64 bytes;
    NvU64 address;
    NvU64 *end_timestamp_gpu_addr;
    NvU64 end_timestamp_gpu;
} migration_data_t;

static struct cdev g_uvm_tools_cdev;
static struct kmem_cache *g_tools_event_tracker_cache __read_mostly = NULL;
static LIST_HEAD(g_tools_va_space_list);
static uvm_rw_semaphore_t g_tools_va_space_list_lock;
static struct kmem_cache *g_tools_block_migration_data_cache __read_mostly = NULL;
static struct kmem_cache *g_tools_migration_data_cache __read_mostly = NULL;
static nv_kthread_q_t g_tools_queue;

static void uvm_tools_record_fault(uvm_perf_event_t event_id, uvm_perf_event_data_t *event_data);
static void uvm_tools_record_migration(uvm_perf_event_t event_id, uvm_perf_event_data_t *event_data);
static NV_STATUS tools_update_status(uvm_va_space_t *va_space);
static void uvm_tools_record_block_migration_begin(uvm_perf_event_t event_id, uvm_perf_event_data_t *event_data);

static uvm_tools_event_tracker_t *tools_event_tracker(struct file *filp)
{
    long event_tracker = atomic_long_read((atomic_long_t *)&filp->private_data);

    smp_read_barrier_depends();
    return (uvm_tools_event_tracker_t *)event_tracker;
}

static bool tracker_is_queue(uvm_tools_event_tracker_t *event_tracker)
{
    return event_tracker != NULL && event_tracker->is_queue;
}

static bool tracker_is_counter(uvm_tools_event_tracker_t *event_tracker)
{
    return event_tracker != NULL && !event_tracker->is_queue;
}

static bool file_is_nvidia_uvm(struct file *filp)
{
    return (filp != NULL) && (filp->f_op == &uvm_fops);
}

static void put_user_pages(struct page **pages, NvU64 page_count)
{
    NvU64 i;
    for (i = 0; i < page_count; i++)
        put_page(pages[i]);
}

static void unmap_user_pages(struct page **pages, void *addr, NvU64 size)
{
    size = DIV_ROUND_UP(size, PAGE_SIZE);
    vunmap((NvU8 *)addr);
    put_user_pages(pages, size);
    uvm_kvfree(pages);
}

// Map virtual memory of data from [user_va, user_va + size) of current process into kernel.
// Sets *addr to kernel mapping and *pages to the array of struct pages that contain the memory.
static NV_STATUS map_user_pages(NvU64 user_va, NvU64 size, void **addr, struct page ***pages)
{
    NV_STATUS status = NV_OK;
    long ret = 0;
    long num_pages;
    long i;
    struct vm_area_struct **vmas = NULL;

    *addr = NULL;
    *pages = NULL;
    num_pages = DIV_ROUND_UP(size, PAGE_SIZE);

    if (uvm_api_range_invalid(user_va, num_pages * PAGE_SIZE)) {
        status = NV_ERR_INVALID_ADDRESS;
        goto fail;
    }

    *pages = uvm_kvmalloc(sizeof(struct page *) * num_pages);
    if (*pages == NULL) {
        status = NV_ERR_NO_MEMORY;
        goto fail;
    }

    vmas = uvm_kvmalloc(sizeof(struct vm_area_struct *) * num_pages);
    if (vmas == NULL) {
        status = NV_ERR_NO_MEMORY;
        goto fail;
    }

    down_read(&current->mm->mmap_sem);
    ret = NV_GET_USER_PAGES(user_va, num_pages, 1, 0, *pages, vmas);
    up_read(&current->mm->mmap_sem);
    if (ret != num_pages) {
        status = NV_ERR_INVALID_ARGUMENT;
        goto fail;
    }

    for (i = 0; i < num_pages; i++) {
        if (page_count((*pages)[i]) > MAX_PAGE_COUNT || file_is_nvidia_uvm(vmas[i]->vm_file)) {
            status = NV_ERR_INVALID_ARGUMENT;
            goto fail;
        }
    }

    *addr = vmap(*pages, num_pages, VM_MAP, PAGE_KERNEL);
    if (*addr == NULL)
        goto fail;

    uvm_kvfree(vmas);
    return NV_OK;

fail:
    if (*pages == NULL)
        return status;

    uvm_kvfree(vmas);

    if (ret > 0)
        put_user_pages(*pages, ret);
    else if (ret < 0)
        status = errno_to_nv_status(ret);

    uvm_kvfree(*pages);
    *pages = NULL;
    return status;
}

static void insert_event_tracker(struct list_head *node,
                                 NvU32 list_count,
                                 NvU64 list_mask,
                                 NvU64 *subscribed_mask,
                                 struct list_head *lists)
{
    NvU32 i;
    NvU64 insertable_lists = list_mask & ~*subscribed_mask;

    for (i = 0; i < list_count; i++) {
        if (insertable_lists & (1ULL << i))
            list_add(node + i, lists + i);
    }

    *subscribed_mask |= list_mask;
}

static void remove_event_tracker(struct list_head *node,
                                 NvU32 list_count,
                                 NvU64 list_mask,
                                 NvU64 *subscribed_mask)
{
    NvU32 i;
    NvU64 removable_lists = list_mask & *subscribed_mask;
    for (i = 0; i < list_count; i++) {
        if (removable_lists & (1ULL << i))
            list_del(node + i);
    }

    *subscribed_mask &= ~list_mask;
}

static bool queue_needs_wakeup(uvm_tools_queue_t *queue, uvm_tools_queue_snapshot_t *sn)
{
    NvU32 queue_mask = queue->queue_buffer_count - 1;

    uvm_assert_spinlock_locked(&queue->lock);
    return ((queue->queue_buffer_count + sn->put_behind - sn->get_ahead) & queue_mask) >= queue->notification_threshold;
}

static void destroy_event_tracker(uvm_tools_event_tracker_t *event_tracker)
{
    if (event_tracker->uvm_file != NULL) {
        NV_STATUS status;
        uvm_va_space_t *va_space = uvm_va_space_get(event_tracker->uvm_file);

        uvm_down_write(&g_tools_va_space_list_lock);
        uvm_down_write(&va_space->perf_events.lock);

        if (event_tracker->is_queue) {
            uvm_tools_queue_t *queue = &event_tracker->queue;

            remove_event_tracker(queue->queue_nodes,
                                 UvmEventNumTypes,
                                 queue->subscribed_queues,
                                 &queue->subscribed_queues);

            if (queue->queue != NULL) {
                unmap_user_pages(queue->queue_buffer_pages,
                                 queue->queue,
                                 queue->queue_buffer_count * sizeof(UvmEventEntry));
            }

            if (queue->control != NULL) {
                unmap_user_pages(queue->control_buffer_pages,
                                 queue->control,
                                 sizeof(UvmToolsEventControlData));
            }
        }
        else {
            uvm_tools_counter_t *counters = &event_tracker->counter;

            remove_event_tracker(counters->counter_nodes,
                                 UVM_TOTAL_COUNTERS,
                                 counters->subscribed_counters,
                                 &counters->subscribed_counters);

            if (counters->counters != NULL) {
                unmap_user_pages(counters->counter_buffer_pages,
                                 counters->counters,
                                 UVM_TOTAL_COUNTERS * sizeof(NvU64));
            }
        }

        // de-registration should not fail
        status = tools_update_status(va_space);
        UVM_ASSERT(status == NV_OK);

        uvm_up_write(&va_space->perf_events.lock);
        uvm_up_write(&g_tools_va_space_list_lock);

        fput(event_tracker->uvm_file);
    }
    kmem_cache_free(g_tools_event_tracker_cache, event_tracker);
}

static void enqueue_event(UvmEventEntry *entry, uvm_tools_queue_t *queue)
{
    UvmToolsEventControlData *ctrl = queue->control;
    uvm_tools_queue_snapshot_t sn;
    NvU32 queue_size = queue->queue_buffer_count;
    NvU32 queue_mask = queue_size - 1;

    uvm_spin_lock(&queue->lock);

    // ctrl is mapped into user space with read and write permissions,
    // so its values cannot be trusted.
    sn.get_behind = atomic_read((atomic_t *)&ctrl->get_behind) & queue_mask;
    sn.put_behind = atomic_read((atomic_t *)&ctrl->put_behind) & queue_mask;
    sn.put_ahead = (sn.put_behind + 1) & queue_mask;

    // one free element means that the queue is full
    if (((queue_size + sn.get_behind - sn.put_behind) & queue_mask) == 1) {
        atomic64_inc((atomic64_t *)&ctrl->dropped + entry->eventData.eventType);
        goto unlock;
    }

    memcpy(queue->queue + sn.put_behind, entry, sizeof(*entry));

    sn.put_behind = sn.put_ahead;
    // put_ahead and put_behind will always be the same outside of queue->lock
    // this allows the user-space consumer to choose either a 2 or 4 pointer synchronization approach
    atomic_set((atomic_t *)&ctrl->put_ahead, sn.put_behind);
    atomic_set((atomic_t *)&ctrl->put_behind, sn.put_behind);

    sn.get_ahead = atomic_read((atomic_t *)&ctrl->get_ahead);
    // if the queue needs to be woken up, only signal if we haven't signaled before for this value of get_ahead
    if (queue_needs_wakeup(queue, &sn) && !(queue->is_wakeup_get_valid && queue->wakeup_get == sn.get_ahead)) {
        queue->is_wakeup_get_valid = true;
        queue->wakeup_get = sn.get_ahead;
        wake_up_all(&queue->wait_queue);
    }

unlock:
    uvm_spin_unlock(&queue->lock);
}

static void uvm_tools_record_event(uvm_va_space_t *va_space, UvmEventEntry *entry)
{
    NvU8 eventType = entry->eventData.eventType;
    uvm_tools_queue_t *queue;

    UVM_ASSERT(eventType < UvmEventNumTypes);

    uvm_assert_rwsem_locked(&va_space->perf_events.lock);

    list_for_each_entry(queue, va_space->tools.queues + eventType, queue_nodes[eventType])
        enqueue_event(entry, queue);
}

static void uvm_tools_broadcast_event(UvmEventEntry *entry)
{
    uvm_va_space_t *va_space;

    uvm_down_read(&g_tools_va_space_list_lock);
    list_for_each_entry(va_space, &g_tools_va_space_list, tools.node) {
        uvm_down_read(&va_space->perf_events.lock);
        uvm_tools_record_event(va_space, entry);
        uvm_up_read(&va_space->perf_events.lock);
    }
    uvm_up_read(&g_tools_va_space_list_lock);
}

static bool counter_matches_processor(UvmCounterName counter, const NvProcessorUuid *processor)
{
    // For compatibility with older counters, CPU faults for memory with a preferred location are reported
    // for their preferred location as well as for the CPU device itself.
    // This check prevents double counting in the aggregate count.
    if (counter == UvmCounterNameCpuPageFaultCount)
        return uvm_processor_uuid_eq(processor, &NV_PROCESSOR_UUID_CPU_DEFAULT);
    return true;
}

static void uvm_tools_inc_counter(uvm_va_space_t *va_space,
                                  UvmCounterName counter,
                                  NvU64 amount,
                                  const NvProcessorUuid *processor)
{
    UVM_ASSERT((NvU32)counter < UVM_TOTAL_COUNTERS);
    uvm_assert_rwsem_locked(&va_space->perf_events.lock);

    if (amount > 0) {
        uvm_tools_counter_t *counters;

        list_for_each_entry(counters, va_space->tools.counters + counter, counter_nodes[counter]) {
            if ((counters->all_processors && counter_matches_processor(counter, processor)) ||
                uvm_processor_uuid_eq(&counters->processor, processor)) {
                atomic64_add(amount, (atomic64_t *)(counters->counters + counter));
            }
        }
    }
}

static bool tools_are_enabled(uvm_va_space_t *va_space)
{
    NvU32 i;

    for (i = 0; i < ARRAY_SIZE(va_space->tools.counters); i++) {
        if (!list_empty(va_space->tools.counters + i))
            return true;
    }
    for (i = 0; i < ARRAY_SIZE(va_space->tools.queues); i++) {
        if (!list_empty(va_space->tools.queues + i))
            return true;
    }
    return false;
}

static int uvm_tools_open(struct inode *inode, struct file *filp)
{
    filp->private_data = NULL;
    return -nv_status_to_errno(uvm_global_get_status());
}

static int uvm_tools_release(struct inode *inode, struct file *filp)
{
    uvm_tools_event_tracker_t *event_tracker = tools_event_tracker(filp);
    if (event_tracker != NULL) {
        destroy_event_tracker(event_tracker);
        filp->private_data = NULL;
    }
    return -nv_status_to_errno(uvm_global_get_status());
}

static long uvm_tools_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
        UVM_ROUTE_CMD_STACK(UVM_TOOLS_INIT_EVENT_TRACKER,         uvm_api_tools_init_event_tracker);
        UVM_ROUTE_CMD_STACK(UVM_TOOLS_SET_NOTIFICATION_THRESHOLD, uvm_api_tools_set_notification_threshold);
        UVM_ROUTE_CMD_STACK(UVM_TOOLS_EVENT_QUEUE_ENABLE_EVENTS,  uvm_api_tools_event_queue_enable_events);
        UVM_ROUTE_CMD_STACK(UVM_TOOLS_EVENT_QUEUE_DISABLE_EVENTS, uvm_api_tools_event_queue_disable_events);
        UVM_ROUTE_CMD_STACK(UVM_TOOLS_ENABLE_COUNTERS,            uvm_api_tools_enable_counters);
        UVM_ROUTE_CMD_STACK(UVM_TOOLS_DISABLE_COUNTERS,           uvm_api_tools_disable_counters);
    }
    return -EINVAL;
}

static unsigned int uvm_tools_poll(struct file *filp, poll_table *wait)
{
    int flags = 0;
    uvm_tools_queue_snapshot_t sn;
    uvm_tools_event_tracker_t *event_tracker;
    UvmToolsEventControlData *ctrl;

    if (uvm_global_get_status() != NV_OK)
        return POLLERR;

    event_tracker = tools_event_tracker(filp);
    if (!tracker_is_queue(event_tracker))
        return POLLERR;

    uvm_spin_lock(&event_tracker->queue.lock);

    event_tracker->queue.is_wakeup_get_valid = false;
    ctrl = event_tracker->queue.control;
    sn.get_ahead = atomic_read((atomic_t *)&ctrl->get_ahead);
    sn.put_behind = atomic_read((atomic_t *)&ctrl->put_behind);

    if (queue_needs_wakeup(&event_tracker->queue, &sn))
        flags = POLLIN | POLLRDNORM;

    uvm_spin_unlock(&event_tracker->queue.lock);

    poll_wait(filp, &event_tracker->queue.wait_queue, wait);
    return flags;
}

static UvmEventFaultType g_hal_to_tools_fault_type_table[UVM_FAULT_TYPE_MAX] = {
    [UVM_FAULT_TYPE_INVALID_PDE]          = UvmFaultTypeInvalidPde,
    [UVM_FAULT_TYPE_INVALID_PTE]          = UvmFaultTypeInvalidPte,
    [UVM_FAULT_TYPE_ATOMIC]               = UvmFaultTypeAtomic,
    [UVM_FAULT_TYPE_WRITE]                = UvmFaultTypeWrite,
    [UVM_FAULT_TYPE_PDE_SIZE]             = UvmFaultTypeInvalidPdeSize,
    [UVM_FAULT_TYPE_VA_LIMIT_VIOLATION]   = UvmFaultTypeLimitViolation,
    [UVM_FAULT_TYPE_UNBOUND_INST_BLOCK]   = UvmFaultTypeUnboundInstBlock,
    [UVM_FAULT_TYPE_PRIV_VIOLATION]       = UvmFaultTypePrivViolation,
    [UVM_FAULT_TYPE_PITCH_MASK_VIOLATION] = UvmFaultTypePitchMaskViolation,
    [UVM_FAULT_TYPE_WORK_CREATION]        = UvmFaultTypeWorkCreation,
    [UVM_FAULT_TYPE_UNSUPPORTED_APERTURE] = UvmFaultTypeUnsupportedAperture,
    [UVM_FAULT_TYPE_COMPRESSION_FAILURE]  = UvmFaultTypeCompressionFailure,
    [UVM_FAULT_TYPE_UNSUPPORTED_KIND]     = UvmFaultTypeUnsupportedKind,
    [UVM_FAULT_TYPE_REGION_VIOLATION]     = UvmFaultTypeRegionViolation,
    [UVM_FAULT_TYPE_POISONED]             = UvmFaultTypePoison,
};

static UvmEventMemoryAccessType g_hal_to_tools_fault_access_type_table[UVM_FAULT_ACCESS_TYPE_MAX] = {
    [UVM_FAULT_ACCESS_TYPE_ATOMIC]   = UvmEventMemoryAccessTypeAtomic,
    [UVM_FAULT_ACCESS_TYPE_WRITE]    = UvmEventMemoryAccessTypeWrite,
    [UVM_FAULT_ACCESS_TYPE_READ]     = UvmEventMemoryAccessTypeRead,
    [UVM_FAULT_ACCESS_TYPE_PREFETCH] = UvmEventMemoryAccessTypePrefetch
};

static void uvm_tools_record_fault(uvm_perf_event_t event_id, uvm_perf_event_data_t *event_data)
{
    UvmEventEntry entry;
    uvm_va_space_t *va_space;

    UVM_ASSERT(event_id == UVM_PERF_EVENT_FAULT);
    UVM_ASSERT(event_data->fault.space);

    va_space = event_data->fault.space;

    uvm_assert_rwsem_locked(&va_space->lock);
    uvm_assert_rwsem_locked(&va_space->perf_events.lock);
    UVM_ASSERT(va_space->tools.enabled);

    memset(&entry, 0, sizeof(entry));

    if (event_data->fault.proc_id == UVM_CPU_ID) {
        uvm_processor_id_t preferred_location;
        UvmEventCpuFaultInfo *info = &entry.eventData.cpuFault;

        info->eventType = UvmEventTypeCpuFault;
        if (event_data->fault.cpu.is_write)
            info->accessType = UvmEventMemoryAccessTypeWrite;
        else
            info->accessType = UvmEventMemoryAccessTypeRead;

        info->address = event_data->fault.cpu.fault_va;
        info->timeStamp = NV_GETTIME();
        // assume that current owns va_space
        info->pid = uvm_get_stale_process_id();
        info->threadId = uvm_get_stale_thread_id();

        // The UVM Lite tools interface did not represent the CPU as a UVM device.
        // It reported CPU faults against the corresponding allocation's 'home location'.
        // Though this driver's tools interface does include a CPU device, for compatibility,
        // the driver still reports faults against a buffer's preferred location,
        // in addition to the CPU.
        uvm_tools_inc_counter(va_space, UvmCounterNameCpuPageFaultCount, 1, &NV_PROCESSOR_UUID_CPU_DEFAULT);

        preferred_location = event_data->fault.block->va_range->preferred_location;
        if (preferred_location != UVM8_MAX_PROCESSORS && preferred_location != UVM_CPU_ID) {
            uvm_gpu_t *gpu = uvm_gpu_get(preferred_location);
            uvm_tools_inc_counter(va_space, UvmCounterNameCpuPageFaultCount, 1, &gpu->uuid);
        }
    }
    else {
        uvm_fault_buffer_entry_t *buffer_entry = event_data->fault.gpu.buffer_entry;
        UvmEventGpuFaultInfo *info = &entry.eventData.gpuFault;
        uvm_gpu_t *gpu = uvm_gpu_get(event_data->fault.proc_id);

        UVM_ASSERT(gpu);

        info->eventType    = UvmEventTypeGpuFault;
        info->gpuIndex     = gpu->id;
        info->faultType    = g_hal_to_tools_fault_type_table[buffer_entry->fault_type];
        info->accessType   = g_hal_to_tools_fault_access_type_table[buffer_entry->fault_access_type];
        info->gpcId        = info->gpcId;
        info->tpcId        = info->tpcId;
        info->address      = buffer_entry->fault_address;
        info->timeStamp    = NV_GETTIME();
        info->timeStampGpu = buffer_entry->timestamp;
        info->batchId      = event_data->fault.gpu.batch_id;

        uvm_tools_inc_counter(va_space, UvmCounterNameGpuPageFaultCount, 1, &gpu->uuid);
    }

    uvm_tools_record_event(va_space, &entry);
}

static void add_pending_event_for_channel(uvm_va_space_t *va_space, block_migration_data_t *block_mig)
{
    tools_channel_entry_t *channel_entry;

    uvm_assert_spinlock_locked(&va_space->tools.channel_list_lock);

    // If this channel already has pending events, just increment the count
    list_for_each_entry(channel_entry, &va_space->tools.channel_list, channel_list_node) {
        if (channel_entry->channel == block_mig->self_channel_entry.channel)
            goto done;
    }

    // otherwise, use the channel list from within the block migration
    channel_entry = &block_mig->self_channel_entry;
    list_add_tail(&channel_entry->channel_list_node, &va_space->tools.channel_list);

done:
    block_mig->channel_entry = channel_entry;
    channel_entry->pending_event_count++;
}

static void remove_pending_event_for_channel(uvm_va_space_t *va_space, tools_channel_entry_t *channel_entry)
{
    uvm_assert_spinlock_locked(&va_space->tools.channel_list_lock);
    UVM_ASSERT(channel_entry->pending_event_count > 0);
    channel_entry->pending_event_count--;
    if (channel_entry->pending_event_count == 0) {
        list_del(&channel_entry->channel_list_node);

        if (!channel_entry->parent_alive) {
            block_migration_data_t *block_mig = container_of(channel_entry, block_migration_data_t, self_channel_entry);
            kmem_cache_free(g_tools_block_migration_data_cache, block_mig);
        }
    }
}


void record_migration_events(void *args)
{
    block_migration_data_t *block_mig = (block_migration_data_t *)args;
    migration_data_t *mig;
    migration_data_t *next;
    UvmEventEntry entry;
    UvmEventMigrationInfo *info = &entry.eventData.migration;
    uvm_va_space_t *va_space = block_mig->va_space;

    NvU64 gpu_timestamp = block_mig->start_timestamp_gpu;

    UVM_ASSERT(block_mig->self_channel_entry.parent_alive);

    // Initialize fields that are constant throughout the whole block
    memset(&entry, 0, sizeof(entry));
    info->srcIndex = block_mig->src;
    info->dstIndex = block_mig->dst;
    info->beginTimeStamp = block_mig->start_timestamp_cpu;
    info->endTimeStamp = block_mig->end_timestamp_cpu;
    info->rangeGroupId = block_mig->range_group_id;
    info->migrationCause = block_mig->cause;

    uvm_down_read(&va_space->perf_events.lock);
    list_for_each_entry_safe(mig, next, &block_mig->events, events_node) {

        UVM_ASSERT(mig->bytes > 0);
        list_del(&mig->events_node);

        info->eventType = UvmEventTypeMigration;
        info->address = mig->address;
        info->migratedBytes = mig->bytes;
        info->beginTimeStampGpu = gpu_timestamp;
        info->endTimeStampGpu = mig->end_timestamp_gpu;
        gpu_timestamp = mig->end_timestamp_gpu;
        kmem_cache_free(g_tools_migration_data_cache, mig);

        uvm_tools_record_event(block_mig->va_space, &entry);
    }
    uvm_up_read(&va_space->perf_events.lock);

    uvm_spin_lock(&va_space->tools.channel_list_lock);
    block_mig->self_channel_entry.parent_alive = false;
    if (block_mig->self_channel_entry.pending_event_count == 0)
        kmem_cache_free(g_tools_block_migration_data_cache, block_mig);

    uvm_spin_unlock(&va_space->tools.channel_list_lock);
}

void on_block_migration_complete(void *ptr)
{
    migration_data_t *mig;
    block_migration_data_t *block_mig = (block_migration_data_t *)ptr;

    block_mig->end_timestamp_cpu = NV_GETTIME();
    block_mig->start_timestamp_gpu = *block_mig->start_timestamp_gpu_addr;
    list_for_each_entry(mig, &block_mig->events, events_node)
        mig->end_timestamp_gpu = *mig->end_timestamp_gpu_addr;

    nv_kthread_q_item_init(&block_mig->queue_item, record_migration_events, block_mig);

    // The UVM driver may notice that work in a channel is complete in a variety of situations
    // and the va_space lock is not always held in all of them, nor can it always be taken safely on them.
    // Dispatching events requires the va_space lock to be held in at least read mode, so
    // this callback simply enqueues the dispatching onto a queue, where the
    // va_space lock is always safe to acquire.
    uvm_spin_lock(&block_mig->va_space->tools.channel_list_lock);
    remove_pending_event_for_channel(block_mig->va_space, block_mig->channel_entry);
    nv_kthread_q_schedule_q_item(&g_tools_queue, &block_mig->queue_item);
    uvm_spin_unlock(&block_mig->va_space->tools.channel_list_lock);
}

static void uvm_tools_record_migration(uvm_perf_event_t event_id, uvm_perf_event_data_t *event_data)
{
    uvm_va_block_t *va_block = event_data->migration.block;
    uvm_va_space_t *va_space = va_block->va_range->va_space;

    UVM_ASSERT(event_id == UVM_PERF_EVENT_MIGRATION);

    uvm_assert_mutex_locked(&va_block->lock);
    uvm_assert_rwsem_locked(&va_space->perf_events.lock);

    if (va_space->tools.enabled) {
        UvmEventEntry entry;
        migration_data_t *mig;
        uvm_push_info_t *push_info = uvm_push_info_from_push(event_data->migration.push);
        block_migration_data_t *block_mig = (block_migration_data_t *)push_info->on_complete_data;
        NvU64 begin_time_stamp = NV_GETTIME();
        size_t page_index;
        uvm_va_block_region_t region = uvm_va_block_region_from_start_size(va_block,
                                                                           event_data->migration.address,
                                                                           event_data->migration.bytes);

        // Increment counters
        if (event_data->migration.src == UVM_CPU_ID) {
            uvm_gpu_t *gpu = uvm_gpu_get(event_data->migration.dst);
            uvm_tools_inc_counter(va_space, UvmCounterNameBytesXferHtD, event_data->migration.bytes, &gpu->uuid);
        }
        else if (event_data->migration.dst == UVM_CPU_ID) {
            uvm_gpu_t *gpu = uvm_gpu_get(event_data->migration.src);
            uvm_tools_inc_counter(va_space, UvmCounterNameBytesXferDtH, event_data->migration.bytes, &gpu->uuid);
        }

        if (push_info->on_complete != NULL) {
            mig = kmem_cache_alloc(g_tools_migration_data_cache, NV_UVM_GFP_FLAGS);
            if (mig == NULL)
                return;

            mig->address = event_data->migration.address;
            mig->bytes = event_data->migration.bytes;
            mig->end_timestamp_gpu_addr = uvm_push_timestamp(event_data->migration.push);

            list_add_tail(&mig->events_node, &block_mig->events);
        }

        // Read-duplication events
        if (event_data->migration.transfer_mode == UVM_VA_BLOCK_TRANSFER_MODE_COPY) {
            UvmEventReadDuplicateInfo *info_read_duplicate = &entry.eventData.readDuplicate;
            memset(&entry, 0, sizeof(entry));

            info_read_duplicate->eventType = UvmEventTypeReadDuplicate;
            info_read_duplicate->size      = PAGE_SIZE;
            info_read_duplicate->timeStamp = begin_time_stamp;

            for_each_va_block_page_in_region(page_index, region) {
                uvm_processor_id_t id;
                uvm_processor_mask_t resident_processors;

                info_read_duplicate->address    = va_block->start + page_index * PAGE_SIZE;
                info_read_duplicate->processors = 0;

                uvm_va_block_page_resident_processors(va_block, page_index, &resident_processors);
                for_each_id_in_mask(id, &resident_processors)
                    info_read_duplicate->processors |= (1 < id);

                uvm_tools_record_event(va_space, &entry);
            }
        }
        else {
            UvmEventReadDuplicateInvalidateInfo *info = &entry.eventData.readDuplicateInvalidate;
            memset(&entry, 0, sizeof(entry));

            info->eventType     = UvmEventTypeReadDuplicateInvalidate;
            info->residentIndex = event_data->migration.dst;
            info->size          = PAGE_SIZE;
            info->timeStamp     = begin_time_stamp;

            for_each_va_block_page_in_region(page_index, region) {
                if (test_bit(page_index, va_block->read_duplicated_pages)) {
                    info->address = va_block->start + page_index * PAGE_SIZE;

                    uvm_tools_record_event(va_space, &entry);
                }
            }
        }
    }
}

void uvm_tools_broadcast_replay(uvm_gpu_id_t gpu_id, NvU32 batch_id)
{
    UvmEventEntry entry;
    UvmEventGpuFaultReplayInfo *info = &entry.eventData.gpuFaultReplay;
    memset(&entry, 0, sizeof(entry));

    info->eventType = UvmEventTypeGpuFaultReplay;
    info->gpuIndex  = gpu_id;
    info->batchId   = batch_id;
    info->timeStamp = NV_GETTIME();

    uvm_tools_broadcast_event(&entry);
}

static void uvm_tools_record_block_migration_begin(uvm_perf_event_t event_id, uvm_perf_event_data_t *event_data)
{
    uvm_va_space_t *va_space;
    uvm_range_group_range_t *range;
    UVM_ASSERT(event_id == UVM_PERF_EVENT_BLOCK_MIGRATION_BEGIN);

    va_space = event_data->migration.block->va_range->va_space;
    uvm_assert_rwsem_locked(&va_space->perf_events.lock);

    if (va_space->tools.enabled) {
        block_migration_data_t *block_mig;
        uvm_push_info_t *push_info = uvm_push_info_from_push(event_data->migration.push);

        UVM_ASSERT(push_info->on_complete == NULL && push_info->on_complete_data == NULL);

        block_mig = kmem_cache_alloc(g_tools_block_migration_data_cache, NV_UVM_GFP_FLAGS);
        if (block_mig == NULL)
            return;

        block_mig->start_timestamp_gpu_addr = uvm_push_timestamp(event_data->migration.push);
        block_mig->start_timestamp_cpu = NV_GETTIME();
        block_mig->dst = event_data->migration.dst;
        block_mig->src = event_data->migration.src;
        block_mig->range_group_id = UVM_RANGE_GROUP_ID_NONE;

        // During evictions, it is not safe to uvm_range_group_range_find() because the va_space lock is not held.
        if (event_data->migration.cause != UvmEventMigrationCauseEviction) {
            range = uvm_range_group_range_find(va_space, event_data->migration.address);
            if (range != NULL)
                block_mig->range_group_id = range->range_group->id;
        }
        block_mig->cause = event_data->migration.cause;
        block_mig->va_space = va_space;

        INIT_LIST_HEAD(&block_mig->events);
        push_info->on_complete_data = block_mig;
        push_info->on_complete = on_block_migration_complete;

        // Set-up channel-oriented state
        block_mig->self_channel_entry.parent_alive = true;
        block_mig->self_channel_entry.pending_event_count = 0;
        block_mig->self_channel_entry.channel = event_data->migration.push->channel;

        uvm_spin_lock(&va_space->tools.channel_list_lock);
        add_pending_event_for_channel(va_space, block_mig);
        uvm_spin_unlock(&va_space->tools.channel_list_lock);
    }
}

void uvm_tools_schedule_completed_events(uvm_va_space_t *va_space)
{
    tools_channel_entry_t *channel_entry;
    tools_channel_entry_t *next_channel_entry;
    NvU64 channel_count = 0;
    NvU64 i;

    uvm_assert_rwsem_locked(&va_space->lock);

    uvm_spin_lock(&va_space->tools.channel_list_lock);

    // retain every channel list entry currently in the list and keep track of their count.
    list_for_each_entry(channel_entry, &va_space->tools.channel_list, channel_list_node) {
        channel_entry->pending_event_count++;
        channel_count++;
    }
    uvm_spin_unlock(&va_space->tools.channel_list_lock);

    if (channel_count == 0)
        return;

    // new entries always appear at the end, and all the entries seen in the first loop have been retained
    // so it is safe to go through them
    channel_entry = list_first_entry(&va_space->tools.channel_list, tools_channel_entry_t, channel_list_node);
    for (i = 0; i < channel_count; i++) {
        uvm_channel_update_progress_all(channel_entry->channel);
        channel_entry = list_next_entry(channel_entry, channel_list_node);
    }

    // now release all the entries we retained in the beginning
    i = 0;
    uvm_spin_lock(&va_space->tools.channel_list_lock);
    list_for_each_entry_safe(channel_entry, next_channel_entry, &va_space->tools.channel_list, channel_list_node) {
        if (i++ == channel_count)
            break;

        remove_pending_event_for_channel(va_space, channel_entry);
    }
    uvm_spin_unlock(&va_space->tools.channel_list_lock);
}

// TODO: Bug 1760246: Temporary workaround to start recording replay events. The
//       final implementation should provide a VA space broadcast mechanism.
void uvm_tools_record_replay(uvm_gpu_id_t gpu_id, uvm_va_space_t *va_space, NvU32 batch_id)
{
    uvm_assert_rwsem_locked(&va_space->lock);

    if (va_space->tools.enabled) {
        UvmEventEntry entry;
        UvmEventGpuFaultReplayInfo *info = &entry.eventData.gpuFaultReplay;

        memset(&entry, 0, sizeof(entry));

        info->eventType = UvmEventTypeGpuFaultReplay;
        info->gpuIndex  = gpu_id;
        info->batchId   = batch_id;
        info->timeStamp = NV_GETTIME();

        uvm_down_read(&va_space->perf_events.lock);
        uvm_tools_record_event(va_space, &entry);
        uvm_up_read(&va_space->perf_events.lock);
    }
}

void uvm_tools_record_cpu_fatal_fault(uvm_va_space_t *va_space, NvU64 address, bool is_write,
                                      UvmEventFatalReason reason)
{
    uvm_assert_rwsem_locked(&va_space->lock);

    if (va_space->tools.enabled) {
        UvmEventEntry entry;
        UvmEventFatalFaultInfo *info = &entry.eventData.fatalFault;
        memset(&entry, 0, sizeof(entry));

        info->eventType = UvmEventTypeFatalFault;
        info->processorIndex = UVM_CPU_ID;
        info->timeStamp = NV_GETTIME();
        info->address = address;
        info->accessType = is_write? UvmEventMemoryAccessTypeWrite: UvmEventMemoryAccessTypeRead;
        // info->faultType is not valid for cpu faults
        info->reason = reason;

        uvm_down_read(&va_space->perf_events.lock);
        uvm_tools_record_event(va_space, &entry);
        uvm_up_read(&va_space->perf_events.lock);
    }
}

void uvm_tools_record_gpu_fatal_fault(uvm_gpu_id_t gpu_id,
                                      uvm_va_space_t *va_space,
                                      uvm_fault_buffer_entry_t *buffer_entry,
                                      UvmEventFatalReason reason)
{
    uvm_assert_rwsem_locked(&va_space->lock);

    if (va_space->tools.enabled) {
        UvmEventEntry entry;
        UvmEventFatalFaultInfo *info = &entry.eventData.fatalFault;
        memset(&entry, 0, sizeof(entry));

        info->eventType = UvmEventTypeFatalFault;
        info->processorIndex = gpu_id;
        info->timeStamp = NV_GETTIME();
        info->address = buffer_entry->fault_address;
        info->accessType = g_hal_to_tools_fault_access_type_table[buffer_entry->fault_access_type];
        info->faultType = g_hal_to_tools_fault_type_table[buffer_entry->fault_type];
        info->reason = reason;

        uvm_down_read(&va_space->perf_events.lock);
        uvm_tools_record_event(va_space, &entry);
        uvm_up_read(&va_space->perf_events.lock);
    }
}

void uvm_tools_record_thrashing(uvm_va_block_t *va_block, NvU64 address, size_t region_size,
                                const uvm_processor_mask_t *processors)
{
    uvm_va_space_t *va_space = va_block->va_range->va_space;

    UVM_ASSERT(address);
    UVM_ASSERT(PAGE_ALIGNED(address));
    UVM_ASSERT(region_size > 0);

    uvm_assert_rwsem_locked(&va_space->lock);
    uvm_assert_rwsem_locked(&va_space->perf_events.lock);

    if (va_space->tools.enabled) {
        UvmEventEntry entry;
        UvmEventThrashingDetectedInfo *info = &entry.eventData.thrashing;
        memset(&entry, 0, sizeof(entry));

        info->eventType = UvmEventTypeThrashingDetected;
        info->address   = address;
        info->size      = region_size;
        info->timeStamp = NV_GETTIME();
        bitmap_copy((long unsigned *)&info->processors, processors->bitmap, UVM8_MAX_PROCESSORS);

        uvm_tools_record_event(va_space, &entry);
    }
}

void uvm_tools_record_throttling_start(uvm_va_block_t *va_block, NvU64 address, uvm_processor_id_t processor)
{
    uvm_va_space_t *va_space = va_block->va_range->va_space;

    UVM_ASSERT(address);
    UVM_ASSERT(PAGE_ALIGNED(address));
    UVM_ASSERT(processor < UVM8_MAX_PROCESSORS);

    uvm_assert_rwsem_locked(&va_space->lock);

    if (va_space->tools.enabled) {
        UvmEventEntry entry;
        UvmEventThrottlingStartInfo *info = &entry.eventData.throttlingStart;
        memset(&entry, 0, sizeof(entry));

        info->eventType      = UvmEventTypeThrottlingStart;
        info->processorIndex = processor;
        info->address        = address;
        info->timeStamp      = NV_GETTIME();

        uvm_down_read(&va_space->perf_events.lock);
        uvm_tools_record_event(va_space, &entry);
        uvm_up_read(&va_space->perf_events.lock);
    }
}

void uvm_tools_record_throttling_end(uvm_va_block_t *va_block, NvU64 address, uvm_processor_id_t processor)
{
    uvm_va_space_t *va_space = va_block->va_range->va_space;

    UVM_ASSERT(address);
    UVM_ASSERT(PAGE_ALIGNED(address));
    UVM_ASSERT(processor < UVM8_MAX_PROCESSORS);

    uvm_assert_rwsem_locked(&va_space->lock);

    if (va_space->tools.enabled) {
        UvmEventEntry entry;
        UvmEventThrottlingEndInfo *info = &entry.eventData.throttlingEnd;
        memset(&entry, 0, sizeof(entry));

        info->eventType      = UvmEventTypeThrottlingEnd;
        info->processorIndex = processor;
        info->address        = address;
        info->timeStamp      = NV_GETTIME();

        uvm_down_read(&va_space->perf_events.lock);
        uvm_tools_record_event(va_space, &entry);
        uvm_up_read(&va_space->perf_events.lock);
    }
}

void uvm_tools_record_map_remote(uvm_va_block_t *va_block, uvm_processor_id_t processor, uvm_processor_id_t residency,
                                 NvU64 address, size_t region_size, UvmEventMapRemoteCause cause)
{
    uvm_va_space_t *va_space = va_block->va_range->va_space;

    UVM_ASSERT(processor < UVM8_MAX_PROCESSORS);
    UVM_ASSERT(residency < UVM8_MAX_PROCESSORS);

    uvm_assert_rwsem_locked(&va_space->lock);

    if (va_space->tools.enabled) {
        UvmEventEntry entry;
        UvmEventMapRemoteInfo *info = &entry.eventData.mapRemote;
        memset(&entry, 0, sizeof(entry));

        info->eventType      = UvmEventTypeMapRemote;
        info->srcIndex       = processor;
        info->dstIndex       = residency;
        info->address        = address;
        info->mapRemoteCause = cause;
        info->size           = region_size;
        info->timeStamp      = NV_GETTIME();
        // TODO: Bug 200194638: compute GPU time stamp reliably
        info->timeStampGpu   = 0;

        uvm_down_read(&va_space->perf_events.lock);
        uvm_tools_record_event(va_space, &entry);
        uvm_up_read(&va_space->perf_events.lock);
    }
}

NV_STATUS uvm_api_tools_init_event_tracker(UVM_TOOLS_INIT_EVENT_TRACKER_PARAMS *params, struct file *filp)
{
    NV_STATUS status = NV_OK;
    uvm_tools_event_tracker_t *event_tracker;

    event_tracker = kmem_cache_zalloc(g_tools_event_tracker_cache, NV_UVM_GFP_FLAGS);
    if (event_tracker == NULL)
        return NV_ERR_NO_MEMORY;

    event_tracker->uvm_file = fget(params->uvmFd);
    if (event_tracker->uvm_file == NULL) {
        status = NV_ERR_INSUFFICIENT_PERMISSIONS;
        goto fail;
    }

    if (!file_is_nvidia_uvm(event_tracker->uvm_file)) {
        fput(event_tracker->uvm_file);
        event_tracker->uvm_file = NULL;
        status = NV_ERR_INSUFFICIENT_PERMISSIONS;
        goto fail;
    }

    event_tracker->is_queue = params->queueBufferSize != 0;
    if (event_tracker->is_queue) {
        uvm_tools_queue_t *queue = &event_tracker->queue;
        uvm_spin_lock_init(&queue->lock, UVM_LOCK_ORDER_LEAF);
        init_waitqueue_head(&queue->wait_queue);

        if (params->queueBufferSize > UINT_MAX) {
            status = NV_ERR_INVALID_ARGUMENT;
            goto fail;
        }

        queue->queue_buffer_count = (NvU32)params->queueBufferSize;
        queue->notification_threshold = queue->queue_buffer_count / 2;

        // queue_buffer_count must be a power of 2, of at least 2
        if (!is_power_of_2(queue->queue_buffer_count) || queue->queue_buffer_count < 2) {
            status = NV_ERR_INVALID_ARGUMENT;
            goto fail;
        }

        status = map_user_pages(params->queueBuffer,
                                queue->queue_buffer_count * sizeof(UvmEventEntry),
                                (void **)&queue->queue,
                                &queue->queue_buffer_pages);
        if (status != NV_OK)
            goto fail;

        status = map_user_pages(params->controlBuffer,
                                sizeof(UvmToolsEventControlData),
                                (void **)&queue->control,
                                &queue->control_buffer_pages);

        if (status != NV_OK)
            goto fail;
    }
    else {
        uvm_tools_counter_t *counter = &event_tracker->counter;
        counter->all_processors = params->allProcessors;
        counter->processor = params->processor;
        status = map_user_pages(params->controlBuffer,
                                sizeof(NvU64) * UVM_TOTAL_COUNTERS,
                                (void **)&counter->counters,
                                &counter->counter_buffer_pages);
        if (status != NV_OK)
            goto fail;
    }

    if (nv_atomic_long_cmpxchg((atomic_long_t *)&filp->private_data, 0, (long)event_tracker) != 0) {
        status = NV_ERR_INVALID_ARGUMENT;
        goto fail;
    }

    return NV_OK;

fail:
    destroy_event_tracker(event_tracker);
    return status;
}

NV_STATUS uvm_api_tools_set_notification_threshold(UVM_TOOLS_SET_NOTIFICATION_THRESHOLD_PARAMS *params, struct file *filp)
{
    UvmToolsEventControlData *ctrl;
    uvm_tools_queue_snapshot_t sn;
    uvm_tools_event_tracker_t *event_tracker = tools_event_tracker(filp);

    if (!tracker_is_queue(event_tracker))
        return NV_ERR_INVALID_ARGUMENT;

    uvm_spin_lock(&event_tracker->queue.lock);

    event_tracker->queue.notification_threshold = params->notificationThreshold;

    ctrl = event_tracker->queue.control;
    sn.put_behind = atomic_read((atomic_t *)&ctrl->put_behind);
    sn.get_ahead = atomic_read((atomic_t *)&ctrl->get_ahead);

    if (queue_needs_wakeup(&event_tracker->queue, &sn))
        wake_up_all(&event_tracker->queue.wait_queue);

    uvm_spin_unlock(&event_tracker->queue.lock);

    return NV_OK;
}

static NV_STATUS tools_register_perf_events(uvm_va_space_t *va_space)
{
    NV_STATUS status;

    uvm_assert_rwsem_locked_write(&va_space->perf_events.lock);

    status = uvm_perf_register_event_callback_locked(&va_space->perf_events,
                                                     UVM_PERF_EVENT_FAULT,
                                                     uvm_tools_record_fault);
    if (status != NV_OK)
        return status;

    status = uvm_perf_register_event_callback_locked(&va_space->perf_events,
                                                     UVM_PERF_EVENT_MIGRATION,
                                                     uvm_tools_record_migration);
    if (status != NV_OK)
        return status;

    status = uvm_perf_register_event_callback_locked(&va_space->perf_events,
                                                     UVM_PERF_EVENT_BLOCK_MIGRATION_BEGIN,
                                                     uvm_tools_record_block_migration_begin);

    if (status != NV_OK)
        return status;

    return NV_OK;
}

static void tools_unregister_perf_events(uvm_va_space_t *va_space)
{
    uvm_assert_rwsem_locked_write(&va_space->perf_events.lock);

    uvm_perf_unregister_event_callback_locked(&va_space->perf_events,
                                              UVM_PERF_EVENT_FAULT,
                                              uvm_tools_record_fault);

    uvm_perf_unregister_event_callback_locked(&va_space->perf_events,
                                              UVM_PERF_EVENT_MIGRATION,
                                              uvm_tools_record_migration);

    uvm_perf_unregister_event_callback_locked(&va_space->perf_events,
                                              UVM_PERF_EVENT_BLOCK_MIGRATION_BEGIN,
                                              uvm_tools_record_block_migration_begin);

}

static NV_STATUS tools_update_status(uvm_va_space_t *va_space)
{
    bool should_be_enabled;
    uvm_assert_rwsem_locked_write(&g_tools_va_space_list_lock);
    uvm_assert_rwsem_locked_write(&va_space->perf_events.lock);

    should_be_enabled = tools_are_enabled(va_space);
    if (should_be_enabled != va_space->tools.enabled) {
        if (should_be_enabled) {
            NV_STATUS status = tools_register_perf_events(va_space);
            if (status != NV_OK)
                return status;

            list_add(&va_space->tools.node, &g_tools_va_space_list);
        }
        else {
            tools_unregister_perf_events(va_space);
            list_del(&va_space->tools.node);
        }
        va_space->tools.enabled = should_be_enabled;
    }

    return NV_OK;
}

NV_STATUS uvm_api_tools_event_queue_enable_events(UVM_TOOLS_EVENT_QUEUE_ENABLE_EVENTS_PARAMS *params, struct file *filp)
{
    uvm_va_space_t *va_space;
    uvm_tools_event_tracker_t *event_tracker = tools_event_tracker(filp);
    NV_STATUS status = NV_OK;

    if (!tracker_is_queue(event_tracker))
        return NV_ERR_INVALID_ARGUMENT;

    va_space = uvm_va_space_get(event_tracker->uvm_file);

    uvm_down_write(&g_tools_va_space_list_lock);
    uvm_down_write(&va_space->perf_events.lock);
    insert_event_tracker(event_tracker->queue.queue_nodes,
                         UvmEventNumTypes,
                         params->eventTypeFlags,
                         &event_tracker->queue.subscribed_queues,
                         va_space->tools.queues);

    // perform any necessary registration
    status = tools_update_status(va_space);

    uvm_up_write(&va_space->perf_events.lock);
    uvm_up_write(&g_tools_va_space_list_lock);

    return status;
}

NV_STATUS uvm_api_tools_event_queue_disable_events(UVM_TOOLS_EVENT_QUEUE_DISABLE_EVENTS_PARAMS *params, struct file *filp)
{
    NV_STATUS status;
    uvm_va_space_t *va_space;
    uvm_tools_event_tracker_t *event_tracker = tools_event_tracker(filp);

    if (!tracker_is_queue(event_tracker))
        return NV_ERR_INVALID_ARGUMENT;

    va_space = uvm_va_space_get(event_tracker->uvm_file);

    uvm_down_write(&g_tools_va_space_list_lock);
    uvm_down_write(&va_space->perf_events.lock);
    remove_event_tracker(event_tracker->queue.queue_nodes,
                         UvmEventNumTypes,
                         params->eventTypeFlags,
                         &event_tracker->queue.subscribed_queues);

    // de-registration should not fail
    status = tools_update_status(va_space);
    UVM_ASSERT(status == NV_OK);

    uvm_up_write(&va_space->perf_events.lock);
    uvm_up_write(&g_tools_va_space_list_lock);
    return NV_OK;
}

NV_STATUS uvm_api_tools_enable_counters(UVM_TOOLS_ENABLE_COUNTERS_PARAMS *params, struct file *filp)
{
    uvm_va_space_t *va_space;
    uvm_tools_event_tracker_t *event_tracker = tools_event_tracker(filp);
    NV_STATUS status = NV_OK;

    if (!tracker_is_counter(event_tracker))
        return NV_ERR_INVALID_ARGUMENT;

    va_space = uvm_va_space_get(event_tracker->uvm_file);

    uvm_down_write(&g_tools_va_space_list_lock);
    uvm_down_write(&va_space->perf_events.lock);
    insert_event_tracker(event_tracker->counter.counter_nodes,
                         UVM_TOTAL_COUNTERS,
                         params->counterTypeFlags,
                         &event_tracker->counter.subscribed_counters,
                         va_space->tools.counters);

    status = tools_update_status(va_space);

    uvm_up_write(&va_space->perf_events.lock);
    uvm_up_write(&g_tools_va_space_list_lock);

    return status;
}

NV_STATUS uvm_api_tools_disable_counters(UVM_TOOLS_DISABLE_COUNTERS_PARAMS *params, struct file *filp)
{
    NV_STATUS status;
    uvm_va_space_t *va_space;
    uvm_tools_event_tracker_t *event_tracker = tools_event_tracker(filp);

    if (!tracker_is_counter(event_tracker))
        return NV_ERR_INVALID_ARGUMENT;

    va_space = uvm_va_space_get(event_tracker->uvm_file);

    uvm_down_write(&g_tools_va_space_list_lock);
    uvm_down_write(&va_space->perf_events.lock);
    remove_event_tracker(event_tracker->counter.counter_nodes,
                         UVM_TOTAL_COUNTERS,
                         params->counterTypeFlags,
                         &event_tracker->counter.subscribed_counters);

    // de-registration should not fail
    status = tools_update_status(va_space);
    UVM_ASSERT(status == NV_OK);

    uvm_up_write(&va_space->perf_events.lock);
    uvm_up_write(&g_tools_va_space_list_lock);

    return NV_OK;
}

static NV_STATUS tools_access_va_block(uvm_va_block_t *va_block,
                                       NvU64 target_va,
                                       NvU64 size,
                                       bool is_write,
                                       void *stage)
{
    NV_STATUS status;

    if (is_write)
        status = UVM_VA_BLOCK_LOCK_RETRY(va_block, NULL,
                     uvm_va_block_write_from_cpu(va_block, target_va, stage, size));
    else
        status = UVM_VA_BLOCK_LOCK_RETRY(va_block, NULL,
                     uvm_va_block_read_to_cpu(va_block, stage, target_va, size));

    return status;
}

static NV_STATUS tools_access_process_memory(uvm_va_space_t *va_space,
                                             NvU64 target_va,
                                             NvU64 size,
                                             NvU64 user_va,
                                             NvU64 *bytes,
                                             bool is_write)
{
    NV_STATUS status = NV_OK;
    uvm_va_block_t *block;
    void *stage_addr;

    struct page *stage = alloc_page(NV_UVM_GFP_FLAGS | __GFP_ZERO);
    if (stage == NULL)
        return NV_ERR_NO_MEMORY;

    stage_addr = kmap(stage);
    *bytes = 0;

    while (*bytes < size) {
        NvU64 user_va_start = user_va + *bytes;
        NvU64 target_va_start = target_va + *bytes;
        NvU64 bytes_left = size - *bytes;
        NvU64 page_offset = target_va_start & (PAGE_SIZE - 1);
        NvU64 bytes_now = min(bytes_left, (NvU64)(PAGE_SIZE - page_offset));

        if (is_write) {
            NvU64 remaining = copy_from_user(stage_addr, (void *)user_va_start, bytes_now);
            if (remaining != 0)  {
                status = NV_ERR_INVALID_ARGUMENT;
                break;
            }

        }

        // The RM flavor of the lock is needed to perform ECC checks.
        uvm_va_space_down_read_rm(va_space);
        status = uvm_va_block_find_create(va_space, target_va_start, &block);
        if (status != NV_OK) {
            uvm_va_space_up_read_rm(va_space);
            break;
        }
        status = tools_access_va_block(block,
                                       target_va_start,
                                       bytes_now,
                                       is_write,
                                       stage_addr);

        // For simplicity, check for ECC errors on all GPUs registered in the VA
        // space as tools read/write is not on a perf critical path.
        if (status == NV_OK)
            status = uvm_gpu_check_ecc_error_mask(&va_space->registered_gpus);

        uvm_va_space_up_read_rm(va_space);
        if (status != NV_OK)
            break;

        if (!is_write) {
            NvU64 remaining = copy_to_user((void *)user_va_start, stage_addr, bytes_now);
            if (remaining > 0) {
                status = NV_ERR_INVALID_ARGUMENT;
                break;
            }
        }

        *bytes += bytes_now;
    }
    kunmap(stage);
    put_page(stage);

    return status;
}

NV_STATUS uvm_api_tools_read_process_memory(UVM_TOOLS_READ_PROCESS_MEMORY_PARAMS *params, struct file *filp)
{
    return tools_access_process_memory(uvm_va_space_get(filp),
                                       params->targetVa,
                                       params->size,
                                       params->buffer,
                                       &params->bytesRead,
                                       false);
}

NV_STATUS uvm_api_tools_write_process_memory(UVM_TOOLS_WRITE_PROCESS_MEMORY_PARAMS *params, struct file *filp)
{
    return tools_access_process_memory(uvm_va_space_get(filp),
                                       params->targetVa,
                                       params->size,
                                       params->buffer,
                                       &params->bytesWritten,
                                       true);
}

NV_STATUS uvm8_test_inject_tools_event(UVM_TEST_INJECT_TOOLS_EVENT_PARAMS *params, struct file *filp)
{
    NvU32 i;
    uvm_va_space_t *va_space = uvm_va_space_get(filp);

    if (params->entry.eventData.eventType >= UvmEventNumTypes)
        return NV_ERR_INVALID_ARGUMENT;

    uvm_down_read(&va_space->perf_events.lock);
    for (i = 0; i < params->count; i++)
        uvm_tools_record_event(va_space, &params->entry);
    uvm_up_read(&va_space->perf_events.lock);
    return NV_OK;
}

NV_STATUS uvm8_test_increment_tools_counter(UVM_TEST_INCREMENT_TOOLS_COUNTER_PARAMS *params, struct file *filp)
{
    NvU32 i;
    uvm_va_space_t *va_space = uvm_va_space_get(filp);

    if (params->counter >= UVM_TOTAL_COUNTERS)
        return NV_ERR_INVALID_ARGUMENT;

    uvm_down_read(&va_space->perf_events.lock);
    for (i = 0; i < params->count; i++)
        uvm_tools_inc_counter(va_space, params->counter, params->amount, &params->processor);
    uvm_up_read(&va_space->perf_events.lock);

    return NV_OK;
}

NV_STATUS uvm_api_tools_get_processor_uuid_table(UVM_TOOLS_GET_PROCESSOR_UUID_TABLE_PARAMS *params, struct file *filp)
{
    NvProcessorUuid *uuids;
    NvU64 remaining;
    uvm_gpu_t *gpu;
    uvm_va_space_t *va_space = uvm_va_space_get(filp);

    uuids = uvm_kvmalloc_zero(sizeof(NvProcessorUuid) * UVM_MAX_PROCESSORS);
    if (uuids == NULL)
        return NV_ERR_NO_MEMORY;

    uvm_processor_uuid_copy(&uuids[UVM_CPU_ID], &NV_PROCESSOR_UUID_CPU_DEFAULT);
    params->count = 1;

    uvm_va_space_down_read(va_space);
    for_each_va_space_gpu(gpu, va_space) {
       uvm_processor_uuid_copy(&uuids[gpu->id], &gpu->uuid);
       if (gpu->id + 1 > params->count)
           params->count = gpu->id + 1;
    }
    uvm_va_space_up_read(va_space);

    remaining = copy_to_user((void *)params->tablePtr, uuids, sizeof(NvProcessorUuid) * params->count);
    uvm_kvfree(uuids);

    if (remaining != 0)
        return NV_ERR_INVALID_ADDRESS;

    return NV_OK;
}

void uvm_tools_flush_events(uvm_va_space_t *va_space)
{
    uvm_va_space_down_read(va_space);
    uvm_tools_schedule_completed_events(va_space);
    uvm_va_space_up_read(va_space);

    nv_kthread_q_flush(&g_tools_queue);
}

NV_STATUS uvm_api_tools_flush_events(UVM_TOOLS_FLUSH_EVENTS_PARAMS *params, struct file *filp)
{
    uvm_tools_flush_events(uvm_va_space_get(filp));
    return NV_OK;
}

static const struct file_operations uvm_tools_fops =
{
    .open            = uvm_tools_open,
    .release         = uvm_tools_release,
    .unlocked_ioctl  = uvm_tools_unlocked_ioctl,
#if NVCPU_IS_X86_64 && defined(NV_FILE_OPERATIONS_HAS_COMPAT_IOCTL)
    .compat_ioctl    = uvm_tools_unlocked_ioctl,
#endif
    .poll            = uvm_tools_poll,
    .owner           = THIS_MODULE,
};

// on failure, the caller should call uvm_tools_exit()
int uvm_tools_init(dev_t uvm_base_dev)
{
    dev_t uvm_tools_dev = MKDEV(MAJOR(uvm_base_dev), NVIDIA_UVM_TOOLS_MINOR_NUMBER);
    int ret;

    uvm_init_rwsem(&g_tools_va_space_list_lock, UVM_LOCK_ORDER_TOOLS_VA_SPACE_LIST);

    g_tools_event_tracker_cache = NV_KMEM_CACHE_CREATE("uvm_tools_event_tracker_t",
                                                        uvm_tools_event_tracker_t);
    if (!g_tools_event_tracker_cache)
        return -ENOMEM;

    g_tools_block_migration_data_cache = NV_KMEM_CACHE_CREATE("uvm_tools_block_migration_data_t",
                                                              block_migration_data_t);
    if (!g_tools_block_migration_data_cache)
        return -ENOMEM;

    g_tools_migration_data_cache = NV_KMEM_CACHE_CREATE("uvm_tools_migration_data_t",
                                                        migration_data_t);
    if (!g_tools_migration_data_cache)
        return -ENOMEM;

    ret = nv_kthread_q_init(&g_tools_queue, "UVM Tools Event Queue");
    if (ret < 0)
        return ret;

    uvm_init_character_device(&g_uvm_tools_cdev, &uvm_tools_fops);
    ret = cdev_add(&g_uvm_tools_cdev, uvm_tools_dev, 1);
    if (ret != 0)
        UVM_ERR_PRINT("cdev_add (major %u, minor %u) failed: %d\n", MAJOR(uvm_tools_dev),
                      MINOR(uvm_tools_dev), ret);

    return ret;
}

void uvm_tools_exit(void)
{
    cdev_del(&g_uvm_tools_cdev);

    nv_kthread_q_stop(&g_tools_queue);

    kmem_cache_destroy_safe(&g_tools_event_tracker_cache);
    kmem_cache_destroy_safe(&g_tools_block_migration_data_cache);
    kmem_cache_destroy_safe(&g_tools_migration_data_cache);
}
