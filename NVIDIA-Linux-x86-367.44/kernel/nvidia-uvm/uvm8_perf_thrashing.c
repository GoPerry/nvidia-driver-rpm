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
    LIABILITY, WHETHER IN AN hint OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

*******************************************************************************/

#include "uvm8_perf_events.h"
#include "uvm8_perf_module.h"
#include "uvm8_perf_thrashing.h"
#include "uvm8_perf_utils.h"
#include "uvm8_va_block.h"
#include "uvm8_va_range.h"
#include "uvm8_kvmalloc.h"
#include "uvm8_tools.h"

// Number of bits for page-granularity time stamps. Currently we ignore the first 6 bits
// of the timestamp (i.e. we have 64ns resolution, which is good enough)
#define PAGE_THRASHING_LAST_TIME_STAMP_BITS 58
#define PAGE_NUM_THRASHING_EVENTS_BITS      3

// Per-page thrashing detection structure
typedef struct
{
    struct
    {
        // Last time stamp when a thrashing-related event was recorded
        NvU64                        last_time_stamp : PAGE_THRASHING_LAST_TIME_STAMP_BITS;

        bool                    has_migration_events : 1;

        bool                   has_revocation_events : 1;

        // Number of consecutive "thrashing" events (within the configured
        // thrashing lapse)
        NvU8                    num_thrashing_events : PAGE_NUM_THRASHING_EVENTS_BITS;

        bool                                  pinned : 1;
    };

    // Number of times a processor has been throttled
    NvU8                            throttling_count;

    // Processors accessing this page
    uvm_processor_mask_t                  processors;

    // Processors that have been throttled
    uvm_processor_mask_t        throttled_processors;
} page_thrashing_info_t;

// Global cache to allocate the per-VA block thrashing detection structures
static struct kmem_cache *g_thrashing_info_cache __read_mostly;

// Per-VA block thrashing detection structure
typedef struct
{
    page_thrashing_info_t                     *pages;

    NvU16                        num_thrashing_pages;

    NvU8                       thrashing_reset_count;

    uvm_processor_id_t                last_processor;

    NvU64                            last_time_stamp;

    NvU64                  last_thrashing_time_stamp;

    // Stats
    NvU32                           throttling_count;

    NvU32                                  pin_count;

    DECLARE_BITMAP(thrashing_pages, PAGES_PER_UVM_VA_BLOCK);
} block_thrashing_info_t;

//
// Tunables for thrashing detection/prevention (configurable via module parameters)
//

// Enable/disable thrashing performance heuristics
static unsigned uvm_perf_thrashing_enable = 1;

#define UVM_PERF_THRASHING_THRESHOLD_DEFAULT 3
#define UVM_PERF_THRASHING_THRESHOLD_MAX     ((1 << PAGE_NUM_THRASHING_EVENTS_BITS) - 1)

// Number of consecutive thrashing events to initiate thrashing prevention
//
// Maximum value is UVM_PERF_THRASHING_THRESHOLD_MAX
static unsigned uvm_perf_thrashing_threshold = UVM_PERF_THRASHING_THRESHOLD_DEFAULT;

#define UVM_PERF_THRASHING_PIN_THRESHOLD_DEFAULT 10
#define UVM_PERF_THRASHING_PIN_THRESHOLD_MAX     1000

// Number of consecutive throttling operations before trying to map remotely
//
// Maximum value is UVM_PERF_THRASHING_PIN_THRESHOLD_MAX
static unsigned uvm_perf_thrashing_pin_threshold = UVM_PERF_THRASHING_PIN_THRESHOLD_DEFAULT;

// TODO: Bug 1768615: [uvm8] Automatically tune default values for thrashing
// detection/prevention parameters
#define UVM_PERF_THRASHING_LAPSE_USEC_DEFAULT 100

// Lapse of time in microseconds that determines if two consecutive events on
// the same page can be considered thrashing
static unsigned uvm_perf_thrashing_lapse_usec = UVM_PERF_THRASHING_LAPSE_USEC_DEFAULT;

#define UVM_PERF_THRASHING_NAP_USEC_DEFAULT (UVM_PERF_THRASHING_LAPSE_USEC_DEFAULT * 8)
#define UVM_PERF_THRASHING_NAP_USEC_MAX     (250*1000)

// Time that the processor being throttled is forbidden to work on the thrashing
// page. Time is counted in microseconds
static unsigned uvm_perf_thrashing_nap_usec   = UVM_PERF_THRASHING_NAP_USEC_DEFAULT;

// Time lapse after which we consider thrashing is no longer happening. Time is
// counted in milliseconds
#define UVM_PERF_THRASHING_EPOCH_MSEC_DEFAULT 1000

static unsigned uvm_perf_thrashing_epoch_msec = UVM_PERF_THRASHING_EPOCH_MSEC_DEFAULT;

// Number of times a VA block can be reset back to non-thrashing. This
// mechanism tries to avoid performing optimizations on a block that periodically
// causes thrashing
#define THRASHING_MAX_RESETS_DEFAULT 4

static unsigned uvm_perf_thrashing_max_resets = THRASHING_MAX_RESETS_DEFAULT;

// Module parameters for the tunables
module_param(uvm_perf_thrashing_enable, uint, S_IRUGO);
module_param(uvm_perf_thrashing_threshold, uint, S_IRUGO);
module_param(uvm_perf_thrashing_pin_threshold, uint, S_IRUGO);
module_param(uvm_perf_thrashing_lapse_usec, uint, S_IRUGO);
module_param(uvm_perf_thrashing_nap_usec, uint, S_IRUGO);
module_param(uvm_perf_thrashing_epoch_msec, uint, S_IRUGO);
module_param(uvm_perf_thrashing_max_resets, uint, S_IRUGO);

bool g_uvm_perf_thrashing_enable;
unsigned g_uvm_perf_thrashing_threshold;
unsigned g_uvm_perf_thrashing_pin_threshold;
NvU64 g_uvm_perf_thrashing_lapse_ns;
NvU64 g_uvm_perf_thrashing_nap_ns;
NvU64 g_uvm_perf_thrashing_epoch_ns;
unsigned g_uvm_perf_thrashing_max_resets;

// Helpers to get/set the time stamp
static NvU64 page_thrashing_get_time_stamp(page_thrashing_info_t *entry)
{
    return entry->last_time_stamp << (64 - PAGE_THRASHING_LAST_TIME_STAMP_BITS);
}

static void page_thrashing_set_time_stamp(page_thrashing_info_t *entry, NvU64 time_stamp)
{
    entry->last_time_stamp = time_stamp >> (64 - PAGE_THRASHING_LAST_TIME_STAMP_BITS);
}

// Performance heuristics module for thrashing
static uvm_perf_module_t g_module_thrashing;

// Callback declaration for the performance heuristics events
static void thrashing_event_cb(uvm_perf_event_t event_id, uvm_perf_event_data_t *event_data);
static void thrashing_block_destroy_cb(uvm_perf_event_t event_id, uvm_perf_event_data_t *event_data);

static uvm_perf_module_event_callback_desc_t g_callbacks_thrashing[] = {
    { UVM_PERF_EVENT_BLOCK_DESTROY, thrashing_block_destroy_cb },
    { UVM_PERF_EVENT_MODULE_UNLOAD, thrashing_block_destroy_cb },
    { UVM_PERF_EVENT_BLOCK_SHRINK , thrashing_block_destroy_cb },
    { UVM_PERF_EVENT_MIGRATION,     thrashing_event_cb         },
    { UVM_PERF_EVENT_REVOCATION,    thrashing_event_cb         }
};

// Get the thrashing detection struct for the given block
static block_thrashing_info_t *thrashing_info_get(uvm_va_block_t *va_block)
{
    return uvm_perf_module_type_data(va_block->perf_modules_data, UVM_PERF_MODULE_TYPE_THRASHING);
}

// Get the thrashing detection struct for the given block or create it if it
// does not exist
static block_thrashing_info_t *thrashing_info_get_create(uvm_va_block_t *va_block)
{
    block_thrashing_info_t *thrashing_info = thrashing_info_get(va_block);

    BUILD_BUG_ON((1 << 8 * sizeof(thrashing_info->num_thrashing_pages)) < PAGES_PER_UVM_VA_BLOCK);

    if (!thrashing_info) {
        thrashing_info = kmem_cache_zalloc(g_thrashing_info_cache, NV_UVM_GFP_FLAGS);
        if (!thrashing_info)
            goto done;

        thrashing_info->last_processor = UVM8_MAX_PROCESSORS;

        uvm_perf_module_type_set_data(va_block->perf_modules_data, thrashing_info, UVM_PERF_MODULE_TYPE_THRASHING);
    }

done:
    return thrashing_info;
}

// Destroy the thrashing detection struct for the given block
static void thrashing_info_destroy(uvm_va_block_t *va_block)
{
    block_thrashing_info_t *thrashing_info;

    thrashing_info = thrashing_info_get(va_block);
    if (thrashing_info) {
        uvm_kvfree(thrashing_info->pages);

        kmem_cache_free(g_thrashing_info_cache, thrashing_info);
        uvm_perf_module_type_unset_data(va_block->perf_modules_data, UVM_PERF_MODULE_TYPE_THRASHING);
    }
}

void thrashing_block_destroy_cb(uvm_perf_event_t event_id, uvm_perf_event_data_t *event_data)
{
    uvm_va_block_t *va_block;

    UVM_ASSERT(g_uvm_perf_thrashing_enable);

    UVM_ASSERT(event_id == UVM_PERF_EVENT_BLOCK_DESTROY ||
               event_id == UVM_PERF_EVENT_BLOCK_SHRINK ||
               event_id == UVM_PERF_EVENT_MODULE_UNLOAD);

    if (event_id == UVM_PERF_EVENT_BLOCK_DESTROY)
        va_block = event_data->block_destroy.block;
    else if (event_id == UVM_PERF_EVENT_BLOCK_SHRINK)
        va_block = event_data->block_shrink.block;
    else
        va_block = event_data->module_unload.block;

    if (!va_block)
        return;

    thrashing_info_destroy(va_block);
}

static void thrashing_reset_page(uvm_va_block_t *va_block,
                                 block_thrashing_info_t *block_thrashing,
                                 NvU64 address,
                                 size_t page_index,
                                 page_thrashing_info_t *page_thrashing)
{
    uvm_processor_id_t gpu_id;

    UVM_ASSERT(block_thrashing->num_thrashing_pages > 0);
    UVM_ASSERT(test_bit(page_index, block_thrashing->thrashing_pages));
    UVM_ASSERT(page_thrashing->num_thrashing_events > 0);

    __clear_bit(page_index, block_thrashing->thrashing_pages);
    --block_thrashing->num_thrashing_pages;

    for_each_gpu_id_in_mask(gpu_id, &page_thrashing->throttled_processors)
        uvm_tools_record_throttling_end(va_block, address, gpu_id);

    page_thrashing->last_time_stamp       = 0;
    page_thrashing->has_migration_events  = 0;
    page_thrashing->has_revocation_events = 0;
    page_thrashing->num_thrashing_events  = 0;
    uvm_processor_mask_zero(&page_thrashing->throttled_processors);
}

void thrashing_event_cb(uvm_perf_event_t event_id, uvm_perf_event_data_t *event_data)
{
    block_thrashing_info_t *block_thrashing;
    uvm_va_block_t *va_block;
    NvU64 address;
    NvU64 bytes;
    uvm_processor_id_t processor_id;
    size_t page_index;
    NvU64 time_stamp;
    uvm_va_block_region_t region;

    UVM_ASSERT(g_uvm_perf_thrashing_enable);

    UVM_ASSERT(event_id == UVM_PERF_EVENT_MIGRATION || event_id == UVM_PERF_EVENT_REVOCATION);

    if (event_id == UVM_PERF_EVENT_MIGRATION) {
        // We only care about migrations due to page faults and page prefetching
        if (event_data->migration.cause != UvmEventMigrationCauseCoherence &&
            event_data->migration.cause != UvmEventMigrationCausePrefetch) {
            return;
        }

        va_block     = event_data->migration.block;
        address      = event_data->migration.address;
        bytes        = event_data->migration.bytes;
        processor_id = event_data->migration.dst;
    }
    else {
        va_block     = event_data->revocation.block;
        address      = event_data->revocation.address;
        bytes        = event_data->revocation.bytes;
        processor_id = event_data->revocation.proc_id;
    }

    block_thrashing = thrashing_info_get_create(va_block);
    if (!block_thrashing)
        return;

    time_stamp = NV_GETTIME();

    if (!block_thrashing->pages) {
        // Don't create the per-page tracking structure unless there is some potential thrashing within the block
        NvU16 num_block_pages;

        if (block_thrashing->last_time_stamp == 0 ||
            block_thrashing->last_processor == processor_id ||
            time_stamp - block_thrashing->last_time_stamp > g_uvm_perf_thrashing_lapse_ns) {
            goto done;
        }

        num_block_pages = uvm_va_block_size(va_block) / PAGE_SIZE;

        block_thrashing->pages = uvm_kvmalloc_zero(sizeof(*block_thrashing->pages) * num_block_pages);
        if (!block_thrashing->pages)
            goto done;
    }

    region = uvm_va_block_region_from_start_size(va_block, address, bytes);

    // Update all pages in the region
    for_each_va_block_page_in_region(page_index, region) {
        page_thrashing_info_t *page_thrashing = &block_thrashing->pages[page_index];
        NvU64 last_time_stamp = page_thrashing_get_time_stamp(page_thrashing);

        if (!uvm_processor_mask_test(&page_thrashing->processors, processor_id))
            page_thrashing->pinned = false;

        uvm_processor_mask_set(&page_thrashing->processors, processor_id);
        page_thrashing_set_time_stamp(page_thrashing, time_stamp);

        if (last_time_stamp == 0)
            continue;

        if (time_stamp - last_time_stamp <= g_uvm_perf_thrashing_lapse_ns) {
            UVM_PERF_SATURATING_INC(page_thrashing->num_thrashing_events);
            if (page_thrashing->num_thrashing_events == g_uvm_perf_thrashing_threshold) {
                // Thrashing detected, record the event
                uvm_tools_record_thrashing(va_block, address, bytes, &page_thrashing->processors);
                __set_bit(page_index, block_thrashing->thrashing_pages);
                ++block_thrashing->num_thrashing_pages;
            }

            if (page_thrashing->num_thrashing_events >= g_uvm_perf_thrashing_threshold)
                block_thrashing->last_thrashing_time_stamp = time_stamp;

            if (event_id == UVM_PERF_EVENT_MIGRATION)
                page_thrashing->has_migration_events = true;
            else
                page_thrashing->has_revocation_events = true;
        }
        else if (page_thrashing->num_thrashing_events >= g_uvm_perf_thrashing_threshold) {
            thrashing_reset_page(va_block, block_thrashing, address, page_index, page_thrashing);
        }
    }

done:
    block_thrashing->last_time_stamp = time_stamp;
    block_thrashing->last_processor  = processor_id;
}

static uvm_perf_thrashing_hint_t
get_hint_for_migration_thrashing(uvm_va_block_t *va_block,
                                 NvU64 address,
                                 size_t page_index,
                                 block_thrashing_info_t *block_thrashing,
                                 page_thrashing_info_t *page_thrashing,
                                 uvm_processor_id_t requester)
{
    uvm_perf_thrashing_hint_t hint;
    uvm_processor_id_t closest_resident_id;
    uvm_va_range_t *va_range = va_block->va_range;
    uvm_va_space_t *va_space = va_range->va_space;

    hint.type = UVM_PERF_THRASHING_HINT_TYPE_NONE;

    closest_resident_id = uvm_va_block_page_get_closest_resident_in_mask(va_block,
                                                                         page_index,
                                                                         requester,
                                                                         &page_thrashing->processors);

    // 1) If preferred_location is set, try to map to it (throttle if that's not possible)
    // 2) If NVLINK map
    // 3) Else first throttle, then map (if processors do not have access,
    //    migrate, if necessary, and map to sysmem).
    if (va_range->preferred_location != UVM8_MAX_PROCESSORS) {
        if (!uvm_processor_mask_test(&va_space->accessible_from[va_range->preferred_location], requester)) {
            hint.type = UVM_PERF_THRASHING_HINT_TYPE_THROTTLE;
        }
        else {
            hint.type = UVM_PERF_THRASHING_HINT_TYPE_PIN;
            hint.pin.residency = va_range->preferred_location;
        }
    }
    else if (closest_resident_id != UVM8_MAX_PROCESSORS &&
             uvm_processor_mask_subset(&page_thrashing->processors,
                                       &va_space->has_nvlink_from[closest_resident_id])) {
        hint.type = UVM_PERF_THRASHING_HINT_TYPE_PIN;
        hint.pin.residency = closest_resident_id;
    }
    else {
        if (page_thrashing->throttling_count >= g_uvm_perf_thrashing_pin_threshold &&
            !page_thrashing->pinned) {
            hint.type = UVM_PERF_THRASHING_HINT_TYPE_PIN;

            if (closest_resident_id != UVM8_MAX_PROCESSORS &&
                uvm_processor_mask_test(&va_space->accessible_from[closest_resident_id], requester)) {
                hint.pin.residency = closest_resident_id;
            }
            else {
                hint.pin.residency = requester;
            }
        }
        else {
            hint.type = UVM_PERF_THRASHING_HINT_TYPE_THROTTLE;
        }
    }

    if (hint.type == UVM_PERF_THRASHING_HINT_TYPE_PIN) {
        uvm_processor_mask_copy(&hint.pin.processors, &page_thrashing->processors);
        ++block_thrashing->pin_count;

        page_thrashing->pinned = true;
    }
    else if (hint.type == UVM_PERF_THRASHING_HINT_TYPE_THROTTLE) {
        if (!uvm_processor_mask_test(&page_thrashing->throttled_processors, requester)) {
            uvm_processor_mask_set(&page_thrashing->throttled_processors, requester);
            UVM_PERF_SATURATING_INC(page_thrashing->throttling_count);
            UVM_PERF_SATURATING_INC(block_thrashing->throttling_count);

            if (requester != UVM_CPU_ID)
                uvm_tools_record_throttling_start(va_block, address, requester);
        }
    }

    return hint;
}

// When we get pure revocation thrashing, this is due to system-wide atomics downgrading
// the permissions of other processors. Revocations only happen when several processors
// are mapping the same page and there are no migrations. In this case, the only thing
// we can do is to throttle the execution of the processors.
static uvm_perf_thrashing_hint_t
get_hint_for_revocation_thrashing(uvm_va_block_t *va_block,
                                  NvU64 address,
                                  size_t page_index,
                                  block_thrashing_info_t *block_thrashing,
                                  page_thrashing_info_t *page_thrashing,
                                  uvm_processor_id_t requester)
{
    uvm_perf_thrashing_hint_t hint;

    hint.type = UVM_PERF_THRASHING_HINT_TYPE_THROTTLE;
    if (!uvm_processor_mask_test(&page_thrashing->throttled_processors, requester)) {
        uvm_processor_mask_set(&page_thrashing->throttled_processors, requester);
        UVM_PERF_SATURATING_INC(page_thrashing->throttling_count);
        UVM_PERF_SATURATING_INC(block_thrashing->throttling_count);

        if (requester != UVM_CPU_ID)
            uvm_tools_record_throttling_start(va_block, address, requester);
    }

    return hint;
}

uvm_perf_thrashing_hint_t uvm_perf_thrashing_get_hint(uvm_va_block_t *va_block,
                                                      NvU64 address,
                                                      uvm_processor_id_t requester)
{
    block_thrashing_info_t *block_thrashing = NULL;
    page_thrashing_info_t *page_thrashing = NULL;
    uvm_perf_thrashing_hint_t hint;
    size_t page_index = uvm_va_block_cpu_page_index(va_block, address);
    NvU64 time_stamp;
    NvU64 last_time_stamp;

    hint.type = UVM_PERF_THRASHING_HINT_TYPE_NONE;

    if (!g_uvm_perf_thrashing_enable)
        goto done;

    // If we don't have enough memory to store thrashing information, we assume
    // no thrashing
    block_thrashing = thrashing_info_get(va_block);
    if (!block_thrashing)
        goto done;

    // If the per-page tracking structure has not been created yet, we assume
    // no thrashing
    if (!block_thrashing->pages)
        goto done;

    time_stamp = NV_GETTIME();

    if (block_thrashing->last_thrashing_time_stamp != 0 &&
        block_thrashing->thrashing_reset_count < g_uvm_perf_thrashing_max_resets &&
        time_stamp - block_thrashing->last_thrashing_time_stamp > g_uvm_perf_thrashing_epoch_ns) {
        ++block_thrashing->thrashing_reset_count;

        // Reset per-page tracking structure
        // TODO: Bug 1769904 [uvm8] Speculatively unpin pages that were pinned on a specific memory due to thrashing
        uvm_kvfree(block_thrashing->pages);
        block_thrashing->pages                     = NULL;
        block_thrashing->num_thrashing_pages       = 0;
        block_thrashing->last_processor            = UVM8_MAX_PROCESSORS;
        block_thrashing->last_time_stamp           = 0;
        block_thrashing->last_thrashing_time_stamp = 0;
        uvm_page_mask_zero(block_thrashing->thrashing_pages);

        goto done;
    }

    page_thrashing = &block_thrashing->pages[page_index];

    // Not enough thrashing events yet
    if (page_thrashing->num_thrashing_events < g_uvm_perf_thrashing_threshold)
        goto done;

    last_time_stamp = page_thrashing_get_time_stamp(page_thrashing);

    // If the lapse since the last thrashing event is longer than a thrashing
    // lapse we are no longer thrashing
    if (time_stamp - last_time_stamp > g_uvm_perf_thrashing_lapse_ns) {
        thrashing_reset_page(va_block, block_thrashing, address, page_index, page_thrashing);
        goto done;
    }

    // Set the requesting processor in the thrashing processors mask
    uvm_processor_mask_set(&page_thrashing->processors, requester);

    UVM_ASSERT(page_thrashing->has_migration_events || page_thrashing->has_revocation_events);

    if (page_thrashing->has_revocation_events && !page_thrashing->has_migration_events)
        hint = get_hint_for_revocation_thrashing(va_block, address, page_index, block_thrashing, page_thrashing,
                                                 requester);
    else
        hint = get_hint_for_migration_thrashing(va_block, address, page_index, block_thrashing, page_thrashing,
                                                requester);

done:
    return hint;
}

uvm_processor_mask_t *uvm_perf_thrashing_get_thrashing_processors(uvm_va_block_t *va_block, NvU64 address)
{
    block_thrashing_info_t *block_thrashing = NULL;
    page_thrashing_info_t *page_thrashing = NULL;
    size_t page_index = uvm_va_block_cpu_page_index(va_block, address);

    UVM_ASSERT(g_uvm_perf_thrashing_enable);

    block_thrashing = thrashing_info_get(va_block);
    UVM_ASSERT(block_thrashing);

    UVM_ASSERT(block_thrashing->pages);

    page_thrashing = &block_thrashing->pages[page_index];

    return &page_thrashing->processors;
}

const long unsigned *uvm_perf_thrashing_get_thrashing_pages(uvm_va_block_t *va_block)
{
    block_thrashing_info_t *block_thrashing = NULL;

    if (!g_uvm_perf_thrashing_enable)
        return NULL;

    block_thrashing = thrashing_info_get(va_block);
    if (!block_thrashing)
        return NULL;

    if (block_thrashing->num_thrashing_pages == 0)
        return NULL;

    return block_thrashing->thrashing_pages;
}

bool uvm_perf_thrashing_is_block_thrashing(uvm_va_block_t *va_block)
{
    block_thrashing_info_t *block_thrashing = NULL;

    if (!g_uvm_perf_thrashing_enable)
        return false;

    block_thrashing = thrashing_info_get(va_block);
    if (!block_thrashing)
        return false;

    return block_thrashing->num_thrashing_pages > 0;
}

NV_STATUS uvm_perf_thrashing_load(uvm_va_space_t *va_space)
{
    if (!g_uvm_perf_thrashing_enable)
        return NV_OK;

    return uvm_perf_module_load(&g_module_thrashing, va_space);
}

void uvm_perf_thrashing_unload(uvm_va_space_t *va_space)
{
    if (!g_uvm_perf_thrashing_enable)
        return;

    uvm_perf_module_unload(&g_module_thrashing, va_space);
}

NV_STATUS uvm_perf_thrashing_init()
{
    g_uvm_perf_thrashing_enable = uvm_perf_thrashing_enable != 0;

    if (!g_uvm_perf_thrashing_enable)
        return NV_OK;

    uvm_perf_module_init("perf_thrashing", UVM_PERF_MODULE_TYPE_THRASHING, g_callbacks_thrashing,
                         ARRAY_SIZE(g_callbacks_thrashing), &g_module_thrashing);

    g_thrashing_info_cache = NV_KMEM_CACHE_CREATE("block_thrashing_info_t", block_thrashing_info_t);
    if (!g_thrashing_info_cache)
        return NV_ERR_NO_MEMORY;

    if (uvm_perf_thrashing_threshold != 0 && uvm_perf_thrashing_threshold <= UVM_PERF_THRASHING_THRESHOLD_MAX) {
        g_uvm_perf_thrashing_threshold = uvm_perf_thrashing_threshold;
    }
    else {
        pr_info("Invalid value %u for uvm_perf_thrashing_threshold. Using %u instead\n",
                uvm_perf_thrashing_threshold, UVM_PERF_THRASHING_THRESHOLD_DEFAULT);

        g_uvm_perf_thrashing_threshold = UVM_PERF_THRASHING_THRESHOLD_DEFAULT;
    }

    if (uvm_perf_thrashing_pin_threshold != 0 && uvm_perf_thrashing_pin_threshold <= UVM_PERF_THRASHING_PIN_THRESHOLD_MAX) {
        g_uvm_perf_thrashing_pin_threshold = uvm_perf_thrashing_pin_threshold;
    }
    else {
        pr_info("Invalid value %u for uvm_perf_thrashing_pin_threshold. Using %u instead\n",
                uvm_perf_thrashing_pin_threshold, UVM_PERF_THRASHING_PIN_THRESHOLD_DEFAULT);

        g_uvm_perf_thrashing_pin_threshold = UVM_PERF_THRASHING_PIN_THRESHOLD_DEFAULT;
    }


    if (uvm_perf_thrashing_lapse_usec != 0) {
        g_uvm_perf_thrashing_lapse_ns = ((NvU64)uvm_perf_thrashing_lapse_usec) * 1000;
    }
    else {
        pr_info("Invalid value %u for uvm_perf_thrashing_lapse_usec. Using %u instead\n",
                uvm_perf_thrashing_lapse_usec, UVM_PERF_THRASHING_LAPSE_USEC_DEFAULT);

        g_uvm_perf_thrashing_lapse_ns = ((NvU64)UVM_PERF_THRASHING_LAPSE_USEC_DEFAULT) * 1000;
    }

    if (uvm_perf_thrashing_nap_usec != 0 && uvm_perf_thrashing_nap_usec <= UVM_PERF_THRASHING_NAP_USEC_MAX) {
        g_uvm_perf_thrashing_nap_ns   = ((NvU64)uvm_perf_thrashing_nap_usec) * 1000;
    }
    else {
        pr_info("Invalid value %u for uvm_perf_thrashing_nap_usec. Using %u instead\n",
                uvm_perf_thrashing_nap_usec, UVM_PERF_THRASHING_NAP_USEC_DEFAULT);

        g_uvm_perf_thrashing_nap_ns = ((NvU64)UVM_PERF_THRASHING_NAP_USEC_DEFAULT) * 1000;
    }

    if (uvm_perf_thrashing_epoch_msec != 0 && uvm_perf_thrashing_epoch_msec * 1000 > uvm_perf_thrashing_lapse_usec) {
        g_uvm_perf_thrashing_epoch_ns = ((NvU64)uvm_perf_thrashing_epoch_msec) * 1000 * 1000;
    }
    else {
        pr_info("Invalid value %u for uvm_perf_thrashing_epoch_msec. Using %u instead\n",
                uvm_perf_thrashing_epoch_msec, UVM_PERF_THRASHING_EPOCH_MSEC_DEFAULT);

        g_uvm_perf_thrashing_epoch_ns = ((NvU64)UVM_PERF_THRASHING_EPOCH_MSEC_DEFAULT) * 1000 * 1000;
    }

    g_uvm_perf_thrashing_max_resets = uvm_perf_thrashing_max_resets;

    return NV_OK;
}

void uvm_perf_thrashing_exit()
{
    if (!g_uvm_perf_thrashing_enable)
        return;

    kmem_cache_destroy_safe(&g_thrashing_info_cache);
}
