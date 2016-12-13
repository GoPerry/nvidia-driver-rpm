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

#include "uvm_linux.h"
#include "uvm_common.h"
#include "uvm8_gpu.h"
#include "uvm8_va_space.h"
#include "uvm8_va_range.h"
#include "uvm8_va_block.h"
#include "uvm8_hal_types.h"
#include "uvm8_kvmalloc.h"
#include "uvm8_tools.h"
#include "uvm8_push.h"
#include "uvm8_hal.h"
#include "uvm8_perf_thrashing.h"
#include "uvm8_perf_prefetch.h"

typedef enum
{
    BLOCK_PTE_OP_MAP,
    BLOCK_PTE_OP_REVOKE,
    BLOCK_PTE_OP_COUNT
} block_pte_op_t;

static int uvm_fault_force_sysmem = 0;
module_param(uvm_fault_force_sysmem, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(uvm_fault_force_sysmem, "Force (1) using sysmem storage for pages that faulted. Default: 0.");

static struct kmem_cache *g_uvm_va_block_cache __read_mostly;
static struct kmem_cache *g_uvm_va_block_gpu_state_cache __read_mostly;
static struct kmem_cache *g_uvm_page_mask_cache __read_mostly;
static struct kmem_cache *g_uvm_va_block_context_cache __read_mostly;

// List of fault service contexts for CPU faults
static LIST_HEAD(g_cpu_fault_service_context_list);

static uvm_spinlock_t g_cpu_fault_service_context_list_lock;

static void block_deferred_accessed_by(void *args);

static bool is_uvm_fault_force_sysmem_set(void)
{
    // Only enforce this during testing
    return uvm_enable_builtin_tests && uvm_fault_force_sysmem != 0;
}

void uvm_va_block_retry_init(uvm_va_block_retry_t *retry)
{
    if (!retry)
        return;

    retry->gpu = NULL;
    uvm_tracker_init(&retry->tracker);
    INIT_LIST_HEAD(&retry->used_chunks);
    INIT_LIST_HEAD(&retry->free_chunks);
}

// Frees any left-over free chunks and unpins all the used chunks
void uvm_va_block_retry_deinit(uvm_va_block_retry_t *retry, uvm_va_block_t *va_block)
{
    uvm_gpu_t *gpu;
    uvm_gpu_chunk_t *gpu_chunk;
    uvm_gpu_chunk_t *next_chunk;

    if (!retry)
        return;

    gpu = retry->gpu;

    if (!gpu) {
        UVM_ASSERT(uvm_tracker_is_empty(&retry->tracker));
        UVM_ASSERT(list_empty(&retry->free_chunks));
        UVM_ASSERT(list_empty(&retry->used_chunks));
        return;
    }

    uvm_tracker_deinit(&retry->tracker);

    // Free any unused chunks
    list_for_each_entry_safe(gpu_chunk, next_chunk, &retry->free_chunks, list) {
        list_del_init(&gpu_chunk->list);
        uvm_pmm_gpu_free(&gpu->pmm, gpu_chunk, NULL);
    }

    // Unpin all the used chunks now that we are done
    list_for_each_entry_safe(gpu_chunk, next_chunk, &retry->used_chunks, list) {
        list_del_init(&gpu_chunk->list);
        uvm_pmm_gpu_unpin_temp(&gpu->pmm, gpu_chunk, va_block);
    }
}

static void block_retry_add_free_chunk(uvm_va_block_retry_t *retry, uvm_gpu_t *gpu, uvm_gpu_chunk_t *gpu_chunk)
{
    if (!retry->gpu)
        retry->gpu = gpu;
    else
        UVM_ASSERT_MSG(retry->gpu == gpu, "Present GPU %s, new GPU %s\n", retry->gpu->name, gpu->name);

    list_add_tail(&gpu_chunk->list, &retry->free_chunks);
}

static void block_retry_add_used_chunk(uvm_va_block_retry_t *retry, uvm_gpu_t *gpu, uvm_gpu_chunk_t *gpu_chunk)
{
    if (!retry->gpu)
        retry->gpu = gpu;
    else
        UVM_ASSERT_MSG(retry->gpu == gpu, "Present GPU %s, new GPU %s\n", retry->gpu->name, gpu->name);

    list_add_tail(&gpu_chunk->list, &retry->used_chunks);
}

static uvm_gpu_chunk_t *block_retry_get_free_chunk(uvm_va_block_retry_t *retry, uvm_gpu_t *gpu, uvm_chunk_size_t size)
{
    uvm_gpu_chunk_t *gpu_chunk;

    if (retry->gpu)
        UVM_ASSERT_MSG(retry->gpu == gpu, "Present GPU %s, new GPU %s\n", retry->gpu->name, gpu->name);

    list_for_each_entry(gpu_chunk, &retry->free_chunks, list) {
        if (uvm_gpu_chunk_get_size(gpu_chunk) == size) {
            list_del(&gpu_chunk->list);
            return gpu_chunk;
        }
    }

    return NULL;
}

// Encapsulates a reference to a physical page belonging to a specific processor
// within a VA block.
typedef struct
{
    // Processor the page is on
    uvm_processor_id_t processor;

    // The page index
    NvU32 page_index;
} block_phys_page_t;

static block_phys_page_t block_phys_page(uvm_processor_id_t processor, NvU32 page_index)
{
    return (block_phys_page_t){ processor, page_index };
}

// Get a fault service context from the global list or allocate a new one if there are no
// available entries
static uvm_fault_service_block_context_t *get_cpu_fault_service_context(void)
{
    uvm_fault_service_block_context_t *service_context;

    uvm_spin_lock(&g_cpu_fault_service_context_list_lock);

    service_context = list_first_entry_or_null(&g_cpu_fault_service_context_list, uvm_fault_service_block_context_t,
                                               cpu_service_context_list);

    if (service_context)
        list_del(&service_context->cpu_service_context_list);

    uvm_spin_unlock(&g_cpu_fault_service_context_list_lock);

    if (!service_context)
        service_context = uvm_kvmalloc(sizeof(*service_context));

    return service_context;
}

// Put a fault service context in the global list
static void put_cpu_fault_service_context(uvm_fault_service_block_context_t *service_context)
{
    uvm_spin_lock(&g_cpu_fault_service_context_list_lock);

    list_add(&service_context->cpu_service_context_list, &g_cpu_fault_service_context_list);

    uvm_spin_unlock(&g_cpu_fault_service_context_list_lock);
}

static void free_cpu_fault_service_context_list(void)
{
    uvm_fault_service_block_context_t *service_context, *service_context_tmp;

    // Free fault service contexts for the CPU and add clear the global list
    list_for_each_entry_safe(service_context, service_context_tmp, &g_cpu_fault_service_context_list,
                             cpu_service_context_list) {
        uvm_kvfree(service_context);
    }
    INIT_LIST_HEAD(&g_cpu_fault_service_context_list);
}

NV_STATUS uvm_va_block_init(void)
{
    unsigned num_preallocated_contexts = 4;

    g_uvm_va_block_cache = NV_KMEM_CACHE_CREATE("uvm_va_block_t", uvm_va_block_t);
    if (!g_uvm_va_block_cache)
        return NV_ERR_NO_MEMORY;

    g_uvm_va_block_gpu_state_cache = NV_KMEM_CACHE_CREATE("uvm_va_block_gpu_state_t",
                                                          uvm_va_block_gpu_state_t);
    if (!g_uvm_va_block_gpu_state_cache)
        return NV_ERR_NO_MEMORY;

    g_uvm_page_mask_cache = NV_KMEM_CACHE_CREATE("uvm_page_mask",
                                                 struct { DECLARE_BITMAP(dummy, PAGES_PER_UVM_VA_BLOCK); });
    if (!g_uvm_page_mask_cache)
        return NV_ERR_NO_MEMORY;

    g_uvm_va_block_context_cache = NV_KMEM_CACHE_CREATE("uvm_va_block_context_t", uvm_va_block_context_t);
    if (!g_uvm_va_block_context_cache)
        return NV_ERR_NO_MEMORY;

    uvm_spin_lock_init(&g_cpu_fault_service_context_list_lock, UVM_LOCK_ORDER_LEAF);

    // Pre-allocate some fault service contexts for the CPU and add them to the global list
    while (num_preallocated_contexts-- > 0) {
        uvm_fault_service_block_context_t *service_context = uvm_kvmalloc(sizeof(*service_context));
        if (!service_context)
            return NV_ERR_NO_MEMORY;

        list_add(&service_context->cpu_service_context_list, &g_cpu_fault_service_context_list);
    }

    return NV_OK;
}

void uvm_va_block_exit(void)
{
    free_cpu_fault_service_context_list();

    kmem_cache_destroy_safe(&g_uvm_va_block_context_cache);
    kmem_cache_destroy_safe(&g_uvm_page_mask_cache);
    kmem_cache_destroy_safe(&g_uvm_va_block_gpu_state_cache);
    kmem_cache_destroy_safe(&g_uvm_va_block_cache);
}

uvm_va_block_context_t *uvm_va_block_context_alloc(void)
{
    return kmem_cache_alloc(g_uvm_va_block_context_cache, NV_UVM_GFP_FLAGS);
}

void uvm_va_block_context_free(uvm_va_block_context_t *va_block_context)
{
    if (va_block_context)
        kmem_cache_free(g_uvm_va_block_context_cache, va_block_context);
}

size_t uvm_va_block_cpu_page_index(uvm_va_block_t *va_block, NvU64 addr)
{
    UVM_ASSERT(addr >= va_block->start);
    UVM_ASSERT(addr <= va_block->end);
    return (addr - va_block->start) / PAGE_SIZE;
}

// Convert from page_index to chunk_index. The goal is for each system page in
// the region [start, start + size) to be covered by the largest naturally-
// aligned user chunk size.
size_t uvm_va_block_gpu_chunk_index_range(NvU64 start,
                                          NvU64 size,
                                          uvm_gpu_t *gpu,
                                          size_t page_index,
                                          uvm_chunk_size_t *out_chunk_size)
{
    uvm_chunk_sizes_mask_t chunk_sizes = uvm_mmu_user_chunk_sizes(gpu);
    uvm_chunk_size_t chunk_size, final_chunk_size;
    size_t num_chunks, num_chunks_total;
    NvU64 addr, end, aligned_start, aligned_addr, aligned_end, temp_size;

    UVM_ASSERT(PAGE_ALIGNED(start));
    UVM_ASSERT(PAGE_ALIGNED(size));
    UVM_ASSERT(size > 0);
    UVM_ASSERT(size <= UVM_CHUNK_SIZE_2M);
    UVM_ASSERT(UVM_ALIGN_DOWN(start, UVM_CHUNK_SIZE_2M) == UVM_ALIGN_DOWN(start + size - 1, UVM_CHUNK_SIZE_2M));
    BUILD_BUG_ON(UVM_VA_BLOCK_SIZE != UVM_CHUNK_SIZE_2M);

    // PAGE_SIZE needs to be the lowest natively-supported chunk size in the
    // mask, since we never deal with chunk sizes smaller than that (although we
    // may have PTEs mapping pages smaller than that).
    UVM_ASSERT(uvm_chunk_find_first_size(chunk_sizes) == PAGE_SIZE);

    // Optimize the ideal Pascal+ case: the whole block is covered by a single
    // 2M page.
    if ((chunk_sizes & UVM_CHUNK_SIZE_2M) && size == UVM_CHUNK_SIZE_2M) {
        UVM_ASSERT(IS_ALIGNED(start, UVM_CHUNK_SIZE_2M));
        final_chunk_size = UVM_CHUNK_SIZE_2M;
        num_chunks_total = 0;
        goto out;
    }

    // Only one 2M chunk can fit within a VA block on any GPU architecture, so
    // remove that size from consideration.
    chunk_sizes &= ~UVM_CHUNK_SIZE_2M;

    // Next common case: the whole block is aligned and sized to perfectly fit
    // the largest page size.
    //
    // TODO: Bug 1750144: This might not be the common case for HMM. Verify that
    //       this helps performance more than it hurts.
    final_chunk_size = uvm_chunk_find_last_size(chunk_sizes);
    if (IS_ALIGNED(start, final_chunk_size) && IS_ALIGNED(size, final_chunk_size)) {
        num_chunks_total = (size_t)uvm_div_pow2_64(page_index * PAGE_SIZE, final_chunk_size);
        goto out;
    }

    // We didn't hit our special paths. Do it the hard way.

    num_chunks_total = 0;
    addr = start + page_index * PAGE_SIZE;
    end = start + size;
    final_chunk_size = 0;
    UVM_ASSERT(addr < end);

    // The below loop collapses almost completely when chunk_size == PAGE_SIZE
    // since in that lowest-common-denominator case everything is already
    // aligned. Skip it and handle that specially after the loop.
    //
    // Note that since we removed 2M already above, this loop will only iterate
    // once on x86 Pascal+ since only 64K is left.
    chunk_sizes &= ~PAGE_SIZE;

    // This loop calculates the number of chunks between start and addr by
    // calculating the number of whole chunks of each size between them,
    // starting with the largest allowed chunk size. This requires fewer
    // iterations than if we began from start and kept calculating the next
    // larger chunk size boundary.
    for_each_chunk_size_rev(chunk_size, chunk_sizes) {
        aligned_start = UVM_ALIGN_UP(start, chunk_size);
        aligned_addr  = UVM_ALIGN_DOWN(addr, chunk_size);
        aligned_end   = UVM_ALIGN_DOWN(end, chunk_size);

        // If addr and start are within the same chunk, try smaller
        if (aligned_start > aligned_addr)
            continue;

        // If addr and end are not in the same chunk, then addr is covered by a
        // single chunk of the current size. Ignore smaller boundaries between
        // addr and aligned_addr.
        if (aligned_addr < aligned_end && final_chunk_size == 0) {
            addr = aligned_addr;
            final_chunk_size = chunk_size;
        }

        // How many chunks of this size are between start and addr? Note that
        // this might be 0 since aligned_addr and aligned_start could be in the
        // same chunk.
        num_chunks = uvm_div_pow2_32(((NvU32)aligned_addr - aligned_start), chunk_size);
        num_chunks_total += num_chunks;

        // We've already accounted for these chunks, so "remove" them by
        // bringing start, addr, and end closer together to calculate the
        // remaining chunk sizes.
        temp_size = num_chunks * chunk_size;
        addr -= temp_size;
        end -= temp_size;

        // Once there's no separation between addr and start, and we've
        // successfully found the right chunk size when taking end into account,
        // we're done.
        if (addr == start && final_chunk_size)
            break;
    }

    // Handle PAGE_SIZE cleanup since we skipped it in the loop
    num_chunks_total += (addr - start) / PAGE_SIZE;
    if (final_chunk_size == 0)
        final_chunk_size = PAGE_SIZE;

out:
    if (out_chunk_size)
        *out_chunk_size = final_chunk_size;

    return num_chunks_total;
}

static size_t block_gpu_chunk_index(uvm_va_block_t *block,
                                    uvm_gpu_t *gpu,
                                    size_t page_index,
                                    uvm_chunk_size_t *out_chunk_size)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_chunk_size_t size;
    uvm_gpu_chunk_t *chunk;

    size_t index = uvm_va_block_gpu_chunk_index_range(block->start, uvm_va_block_size(block), gpu, page_index, &size);

    UVM_ASSERT(size >= PAGE_SIZE);

    if (gpu_state) {
        UVM_ASSERT(gpu_state->chunks);
        chunk = gpu_state->chunks[index];
        if (chunk) {
            UVM_ASSERT(uvm_gpu_chunk_get_size(chunk) == size);
            UVM_ASSERT(uvm_gpu_chunk_get_state(chunk) != UVM_PMM_GPU_CHUNK_STATE_PMA_OWNED);
            UVM_ASSERT(uvm_gpu_chunk_get_state(chunk) != UVM_PMM_GPU_CHUNK_STATE_FREE);
        }
    }

    if (out_chunk_size)
        *out_chunk_size = size;

    return index;
}

// Compute the size of the chunk known to start at start_page_index
static uvm_chunk_size_t block_gpu_chunk_size(uvm_va_block_t *block, uvm_gpu_t *gpu, size_t start_page_index)
{
    uvm_chunk_sizes_mask_t chunk_sizes = uvm_mmu_user_chunk_sizes(gpu);
    uvm_chunk_sizes_mask_t start_alignments, pow2_leq_size, allowed_sizes;
    NvU64 start = block->start + start_page_index * PAGE_SIZE;
    NvU64 size = block->end - start + 1;

    // Create a mask of all sizes for which start is aligned. x ^ (x-1) yields a
    // mask of the rightmost 1 bit in x, as well as all trailing 0 bits in x.
    // Example: 1011000 -> 0001111
    start_alignments = (uvm_chunk_sizes_mask_t)(start ^ (start - 1));

    // Next, compute all sizes (powers of two) which are <= size.
    pow2_leq_size = (uvm_chunk_sizes_mask_t)rounddown_pow_of_two(size);
    pow2_leq_size |= pow2_leq_size - 1;

    // Now and them all together to get our list of GPU-supported chunk sizes
    // which are aligned to start and will fit within size.
    allowed_sizes = chunk_sizes & start_alignments & pow2_leq_size;

    // start and size must always be aligned to at least the smallest supported
    // chunk size (PAGE_SIZE).
    UVM_ASSERT(allowed_sizes >= PAGE_SIZE);

    // Take the largest allowed size
    return uvm_chunk_find_last_size(allowed_sizes);
}

static size_t block_num_gpu_chunks(uvm_va_block_t *block, uvm_gpu_t *gpu)
{
    return block_gpu_chunk_index(block, gpu, uvm_va_block_cpu_page_index(block, block->end), NULL) + 1;
}

static size_t block_num_gpu_chunks_range(NvU64 start, NvU64 size, uvm_gpu_t *gpu)
{
    size_t last_page_index = (size_t)((size / PAGE_SIZE) - 1);
    return uvm_va_block_gpu_chunk_index_range(start, size, gpu, last_page_index, NULL) + 1;
}

// Return the block region covered by the given chunk size. page_index must be
// any page within the block known to be covered by the chunk.
static uvm_va_block_region_t block_gpu_chunk_region(uvm_va_block_t *block,
                                                    uvm_chunk_size_t chunk_size,
                                                    size_t page_index)
{
    NvU64 page_addr = block->start + page_index * PAGE_SIZE;
    NvU64 chunk_start_addr = UVM_ALIGN_DOWN(page_addr, chunk_size);
    NvU32 first = (NvU32)((chunk_start_addr - block->start) / PAGE_SIZE);
    return uvm_va_block_region(first, first + (chunk_size / PAGE_SIZE));
}

uvm_gpu_chunk_t *uvm_va_block_lookup_gpu_chunk(uvm_va_block_t *va_block, uvm_gpu_t *gpu, NvU64 address)
{
    size_t chunk_index;
    uvm_va_block_gpu_state_t *gpu_state = va_block->gpus[gpu->id - 1];
    size_t page_index = uvm_va_block_cpu_page_index(va_block, address);

    uvm_assert_mutex_locked(&va_block->lock);

    if (!gpu_state)
        return NULL;

    chunk_index = block_gpu_chunk_index(va_block, gpu, page_index, NULL);

    return gpu_state->chunks[chunk_index];
}

NV_STATUS uvm_va_block_create(uvm_va_range_t *va_range,
                              NvU64 start,
                              NvU64 end,
                              uvm_va_block_t **out_block)
{
    uvm_va_block_t *block = NULL;
    NvU64 size = end - start + 1;
    NV_STATUS status;

    UVM_ASSERT(PAGE_ALIGNED(start));
    UVM_ASSERT(PAGE_ALIGNED(end + 1));
    UVM_ASSERT(PAGE_ALIGNED(size));
    UVM_ASSERT(size > 0);
    UVM_ASSERT(size <= UVM_VA_BLOCK_SIZE);
    UVM_ASSERT(start >= va_range->node.start);
    UVM_ASSERT(end <= va_range->node.end);
    UVM_ASSERT(va_range->type == UVM_VA_RANGE_TYPE_MANAGED);

    // Blocks can't span a block alignment boundary
    UVM_ASSERT(UVM_VA_BLOCK_ALIGN_DOWN(start) == UVM_VA_BLOCK_ALIGN_DOWN(end));

    block = kmem_cache_zalloc(g_uvm_va_block_cache, NV_UVM_GFP_FLAGS);
    if (!block) {
        status = NV_ERR_NO_MEMORY;
        goto error;
    }

    kref_init(&block->kref);
    uvm_mutex_init(&block->lock, UVM_LOCK_ORDER_VA_BLOCK);
    block->start = start;
    block->end = end;
    block->va_range = va_range;
    uvm_tracker_init(&block->tracker);

    nv_kthread_q_item_init(&block->accessed_by_mappings_q_item,
                           block_deferred_accessed_by,
                           block);

    block->cpu.pages = uvm_kvmalloc_zero((size / PAGE_SIZE) * sizeof(block->cpu.pages[0]));
    if (!block->cpu.pages) {
        status = NV_ERR_NO_MEMORY;
        goto error;
    }

    *out_block = block;
    return NV_OK;

error:
    uvm_va_block_release(block);
    return status;
}

// Retrieves the gpu_state for the given GPU, allocating it if it doesn't exist
static uvm_va_block_gpu_state_t *block_gpu_state_get_alloc(uvm_va_block_t *block, uvm_gpu_t *gpu)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];

    if (gpu_state)
        return gpu_state;

    gpu_state = kmem_cache_zalloc(g_uvm_va_block_gpu_state_cache, NV_UVM_GFP_FLAGS);
    if (!gpu_state)
        return NULL;

    gpu_state->chunks = uvm_kvmalloc_zero(block_num_gpu_chunks(block, gpu) * sizeof(gpu_state->chunks[0]));
    if (!gpu_state->chunks)
        goto error;

    block->gpus[gpu->id - 1] = gpu_state;

    return gpu_state;

error:
    if (gpu_state)
        kmem_cache_free(g_uvm_va_block_gpu_state_cache, gpu_state);

    return NULL;
}

// Allocates the input page in the block, if it doesn't already exist
static NV_STATUS block_populate_page_cpu(uvm_va_block_t *block, size_t page_index, bool zero)
{
    struct page *page;
    gfp_t gfp_flags;

    if (block->cpu.pages[page_index])
        return NV_OK;

    UVM_ASSERT(!test_bit(page_index, block->cpu.resident));

    gfp_flags = NV_UVM_GFP_FLAGS | GFP_HIGHUSER;
    if (zero)
        gfp_flags |= __GFP_ZERO;

    page = alloc_pages(gfp_flags, 0);
    if (!page)
        return NV_ERR_NO_MEMORY;

    // the kernel has 'written' zeros to this page, so it is dirty
    if (zero)
        SetPageDirty(page);

    block->cpu.pages[page_index] = page;
    return NV_OK;
}

// Try allocating a chunk. If eviction was required,
// NV_ERR_MORE_PROCESSING_REQUIRED will be returned since the block's lock was
// unlocked and relocked. The caller is responsible for adding the chunk to the
// retry used_chunks list.
static NV_STATUS block_alloc_gpu_chunk(uvm_va_block_t *block,
                                       uvm_va_block_retry_t *retry,
                                       uvm_gpu_t *gpu,
                                       uvm_chunk_size_t size,
                                       uvm_gpu_chunk_t **out_gpu_chunk)
{
    NV_STATUS status = NV_OK;
    uvm_gpu_chunk_t *gpu_chunk;

    // First try getting a free chunk from previously-made allocations.
    gpu_chunk = block_retry_get_free_chunk(retry, gpu, size);
    if (!gpu_chunk) {
        if (block->user_pages_allocation_retry_force_count > 0) {
            // Force eviction by pretending the allocation failed with no memory
            --block->user_pages_allocation_retry_force_count;
            status = NV_ERR_NO_MEMORY;
        }
        else {
            // Try allocating a new one without eviction
            status = uvm_pmm_gpu_alloc_user(&gpu->pmm, 1, size, UVM_PMM_ALLOC_FLAGS_NONE, &gpu_chunk, &retry->tracker);
        }

        if (status == NV_ERR_NO_MEMORY) {
            // If that fails with no memory, try allocating with eviction and
            // return back to the caller immediately so that the operation can
            // be restarted.
            uvm_mutex_unlock(&block->lock);

            status = uvm_pmm_gpu_alloc_user(&gpu->pmm, 1, size, UVM_PMM_ALLOC_FLAGS_EVICT, &gpu_chunk, &retry->tracker);
            if (status == NV_OK) {
                block_retry_add_free_chunk(retry, gpu, gpu_chunk);
                status = NV_ERR_MORE_PROCESSING_REQUIRED;
            }

            uvm_mutex_lock(&block->lock);
            return status;
        }
        else if (status != NV_OK) {
            return status;
        }
    }

    *out_gpu_chunk = gpu_chunk;
    return NV_OK;
}

static bool block_gpu_has_page_tables(uvm_va_block_t *block, uvm_gpu_t *gpu)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];

    if (!gpu_state)
        return false;

    return gpu_state->page_table_range_4k.table  ||
           gpu_state->page_table_range_big.table ||
           gpu_state->page_table_range_2m.table;
}

// A helper to get a known-to-be-present GPU VA space given a VA block that's
// locked. In order to use this function, the caller must know that at least one
// of these conditions is true:
//
// 1) The VA space lock is held
// 2) The VA block has active page tables for the GPU
//
// If the VA space lock is held (#1), then the gpu_va_space obviously can't go
// away.
//
// On the eviction path, we don't have a lock on the VA space state. However,
// since remove_gpu_va_space walks each block to unmap the GPU and free GPU page
// tables before destroying the gpu_va_space, we're guaranteed that if this GPU
// has page tables (#2), the gpu_va_space can't go away while we're holding the
// block lock.
static uvm_gpu_va_space_t *uvm_va_block_get_gpu_va_space(uvm_va_block_t *va_block, uvm_gpu_t *gpu)
{
    uvm_gpu_va_space_t *gpu_va_space;
    uvm_va_space_t *va_space;

    UVM_ASSERT(gpu);
    UVM_ASSERT(va_block->va_range);

    va_space = va_block->va_range->va_space;

    if (!block_gpu_has_page_tables(va_block, gpu))
        uvm_assert_rwsem_locked(&va_space->lock);

    UVM_ASSERT(uvm_processor_mask_test(&va_space->registered_gpu_va_spaces, gpu->id));

    gpu_va_space = va_space->gpu_va_spaces[gpu->id - 1];

    UVM_ASSERT(gpu_va_space->va_space == va_space);
    UVM_ASSERT(gpu_va_space->gpu == gpu);

    return gpu_va_space;
}

static bool block_gpu_supports_2m(uvm_va_block_t *block, uvm_gpu_t *gpu)
{
    uvm_gpu_va_space_t *gpu_va_space;

    if (uvm_va_block_size(block) < UVM_PAGE_SIZE_2M)
        return false;

    UVM_ASSERT(uvm_va_block_size(block) == UVM_PAGE_SIZE_2M);

    gpu_va_space = uvm_va_block_get_gpu_va_space(block, gpu);
    return uvm_mmu_page_size_supported(&gpu_va_space->page_tables, UVM_PAGE_SIZE_2M);
}

NvU32 uvm_va_block_gpu_big_page_size(uvm_va_block_t *va_block, uvm_gpu_t *gpu)
{
    uvm_gpu_va_space_t *gpu_va_space;

    // For GPUs which swizzle, we have to associate the big page size with
    // physical memory, not just with page table mappings. This means we may
    // need to know the big page size when we don't have a gpu_va_space. Handle
    // this by taking advantage of the fact that for GPUs which support
    // swizzling, the big page size is fixed for all VA spaces globally so our
    // internal size can't differ from the user's size.
    if (gpu->big_page.swizzling)
        return gpu->big_page.internal_size;

    gpu_va_space = uvm_va_block_get_gpu_va_space(va_block, gpu);
    return gpu_va_space->page_tables.big_page_size;
}

static uvm_va_block_region_t range_big_page_region_all(NvU64 start, NvU64 end, NvU32 big_page_size)
{
    NvU64 first_addr = UVM_ALIGN_UP(start, big_page_size);
    NvU64 outer_addr = UVM_ALIGN_DOWN(end + 1, big_page_size);

    // The range must fit within a VA block
    UVM_ASSERT(UVM_VA_BLOCK_ALIGN_DOWN(start) == UVM_VA_BLOCK_ALIGN_DOWN(end));

    if (outer_addr <= first_addr)
        return uvm_va_block_region(0, 0);

    return uvm_va_block_region((first_addr - start) / PAGE_SIZE, (outer_addr - start) / PAGE_SIZE);
}

static size_t range_num_big_pages(NvU64 start, NvU64 end, NvU32 big_page_size)
{
    uvm_va_block_region_t region = range_big_page_region_all(start, end, big_page_size);
    return (size_t)uvm_div_pow2_64(uvm_va_block_region_size(region), big_page_size);
}

uvm_va_block_region_t uvm_va_block_big_page_region_all(uvm_va_block_t *va_block, NvU32 big_page_size)
{
    return range_big_page_region_all(va_block->start, va_block->end, big_page_size);
}

size_t uvm_va_block_num_big_pages(uvm_va_block_t *va_block, NvU32 big_page_size)
{
    return range_num_big_pages(va_block->start, va_block->end, big_page_size);
}

NvU64 uvm_va_block_big_page_addr(uvm_va_block_t *va_block, size_t big_page_index, NvU32 big_page_size)
{
    NvU64 addr = UVM_ALIGN_UP(va_block->start, big_page_size) + (big_page_index * big_page_size);
    UVM_ASSERT(addr >= va_block->start);
    UVM_ASSERT(addr < va_block->end);
    return addr;
}

uvm_va_block_region_t uvm_va_block_big_page_region(uvm_va_block_t *va_block, size_t big_page_index, NvU32 big_page_size)
{
    NvU64 page_addr = uvm_va_block_big_page_addr(va_block, big_page_index, big_page_size);

    // Assume that we don't have to handle multiple big PTEs per system page.
    // It's not terribly difficult to implement, but we don't currently have a
    // use case.
    UVM_ASSERT(big_page_size >= PAGE_SIZE);

    return uvm_va_block_region_from_start_size(va_block, page_addr, big_page_size);
}

// Returns the big page index (the bit index within
// uvm_va_block_gpu_state_t::big_ptes) corresponding to page_index. If
// page_index cannot be covered by a big PTE due to alignment or block size,
// MAX_BIG_PAGES_PER_UVM_VA_BLOCK is returned.
size_t uvm_va_block_big_page_index(uvm_va_block_t *va_block, size_t page_index, NvU32 big_page_size)
{
    uvm_va_block_region_t big_region_all = uvm_va_block_big_page_region_all(va_block, big_page_size);
    size_t big_index;

    // Note that this condition also handles the case of having no big pages in
    // the block, in which case .first >= .outer.
    if (page_index < big_region_all.first || page_index >= big_region_all.outer)
        return MAX_BIG_PAGES_PER_UVM_VA_BLOCK;

    big_index = (size_t)uvm_div_pow2_64((page_index - big_region_all.first) * PAGE_SIZE, big_page_size);

    UVM_ASSERT(uvm_va_block_big_page_addr(va_block, big_index, big_page_size) >= va_block->start);
    UVM_ASSERT(uvm_va_block_big_page_addr(va_block, big_index, big_page_size) + big_page_size <= va_block->end + 1);

    return big_index;
}

static bool block_gpu_page_is_swizzled(uvm_va_block_t *block, uvm_gpu_t *gpu, size_t page_index)
{
    NvU32 big_page_size;
    size_t big_page_index;
    uvm_va_block_gpu_state_t *gpu_state;

    if (!gpu->big_page.swizzling)
        return false;

    gpu_state = block->gpus[gpu->id - 1];
    UVM_ASSERT(gpu_state);

    big_page_size = uvm_va_block_gpu_big_page_size(block, gpu);
    big_page_index = uvm_va_block_big_page_index(block, page_index, big_page_size);

    return big_page_index != MAX_BIG_PAGES_PER_UVM_VA_BLOCK && test_bit(big_page_index, gpu_state->big_pages_swizzled);
}

static void uvm_page_mask_init_from_big_ptes(uvm_va_block_t *block,
                                             uvm_gpu_t *gpu,
                                             unsigned long *mask_out,
                                             const unsigned long *big_ptes_in)
{
    uvm_va_block_region_t big_region;
    size_t big_page_index;
    NvU32 big_page_size = uvm_va_block_gpu_big_page_size(block, gpu);

    uvm_page_mask_zero(mask_out);

    for_each_set_bit(big_page_index, big_ptes_in, MAX_BIG_PAGES_PER_UVM_VA_BLOCK) {
        big_region = uvm_va_block_big_page_region(block, big_page_index, big_page_size);
        uvm_page_mask_region_fill(mask_out, big_region);
    }
}

NvU32 uvm_va_block_page_size_cpu(uvm_va_block_t *va_block, size_t page_index)
{
    if (!test_bit(page_index, va_block->cpu.pte_bits[UVM_PTE_BITS_CPU_READ]))
        return 0;

    UVM_ASSERT(uvm_processor_mask_test(&va_block->mapped, UVM_CPU_ID));
    return PAGE_SIZE;
}

NvU32 uvm_va_block_page_size_gpu(uvm_va_block_t *va_block, uvm_gpu_id_t gpu_id, size_t page_index)
{
    uvm_va_block_gpu_state_t *gpu_state = va_block->gpus[gpu_id - 1];
    size_t big_page_size, big_page_index;

    if (!gpu_state)
        return 0;

    if (!test_bit(page_index, gpu_state->pte_bits[UVM_PTE_BITS_GPU_READ]))
        return 0;

    UVM_ASSERT(uvm_processor_mask_test(&va_block->mapped, gpu_id));

    if (gpu_state->pte_is_2m)
        return UVM_PAGE_SIZE_2M;

    big_page_size = uvm_va_block_gpu_big_page_size(va_block, uvm_gpu_get(gpu_id));
    big_page_index = uvm_va_block_big_page_index(va_block, page_index, big_page_size);
    if (big_page_index != MAX_BIG_PAGES_PER_UVM_VA_BLOCK && test_bit(big_page_index, gpu_state->big_ptes))
        return big_page_size;

    return UVM_PAGE_SIZE_4K;
}

// Get the size of the physical allocation backing the page, or 0 if not
// resident. Note that this is different from uvm_va_block_page_size_* because
// those return the size of the PTE which maps the page index, which may be
// smaller than the physical allocation.
static NvU32 block_phys_page_size(uvm_va_block_t *va_block, block_phys_page_t page)
{
    uvm_va_block_gpu_state_t *gpu_state;
    uvm_chunk_size_t chunk_size;

    if (page.processor == UVM_CPU_ID) {
        if (!test_bit(page.page_index, va_block->cpu.resident))
            return 0;

        UVM_ASSERT(uvm_processor_mask_test(&va_block->resident, UVM_CPU_ID));
        return PAGE_SIZE;
    }

    gpu_state = va_block->gpus[page.processor - 1];
    if (!gpu_state || !test_bit(page.page_index, gpu_state->resident))
        return 0;

    UVM_ASSERT(uvm_processor_mask_test(&va_block->resident, page.processor));
    block_gpu_chunk_index(va_block, uvm_gpu_get(page.processor), page.page_index, &chunk_size);
    return (NvU32)chunk_size;
}

unsigned long *uvm_va_block_resident_mask_get(uvm_va_block_t *block, uvm_processor_id_t processor)
{
    uvm_va_block_gpu_state_t *gpu_state;

    if (processor == UVM_CPU_ID)
        return block->cpu.resident;

    gpu_state = block->gpus[processor - 1];

    UVM_ASSERT(gpu_state);
    return gpu_state->resident;
}

// Get the page residency mask for a processor
//
// Notably this will allocate GPU state if not yet present and if that fails
// NULL is returned.
static unsigned long *block_resident_mask_get_alloc(uvm_va_block_t *block, uvm_processor_id_t processor)
{
    uvm_va_block_gpu_state_t *gpu_state;

    if (processor == UVM_CPU_ID)
        return block->cpu.resident;

    gpu_state = block_gpu_state_get_alloc(block, uvm_gpu_get(processor));
    if (!gpu_state)
        return NULL;

    return gpu_state->resident;
}

// Get the page mapped mask for a processor
unsigned long *uvm_va_block_map_mask_get(uvm_va_block_t *block, uvm_processor_id_t processor)
{
    uvm_va_block_gpu_state_t *gpu_state;

    if (processor == UVM_CPU_ID)
        return block->cpu.pte_bits[UVM_PTE_BITS_CPU_READ];

    gpu_state = block->gpus[processor - 1];

    UVM_ASSERT(gpu_state);
    return gpu_state->pte_bits[UVM_PTE_BITS_GPU_READ];
}

static bool block_is_page_resident_anywhere(uvm_va_block_t *block, size_t page_index)
{
    uvm_processor_id_t id;
    for_each_id_in_mask(id, &block->resident) {
        if (test_bit(page_index, uvm_va_block_resident_mask_get(block, id)))
            return true;
    }

    return false;
}

static bool block_processor_page_is_populated(uvm_va_block_t *block, uvm_processor_id_t proc, size_t page_index)
{
    uvm_va_block_gpu_state_t *gpu_state;
    size_t chunk_index;

    if (proc == UVM_CPU_ID)
        return block->cpu.pages[page_index] != NULL;

    gpu_state = block->gpus[proc - 1];
    if (!gpu_state)
        return false;

    chunk_index = block_gpu_chunk_index(block, uvm_gpu_get(proc), page_index, NULL);
    return gpu_state->chunks[chunk_index] != NULL;
}

static bool block_processor_page_is_resident_on(uvm_va_block_t *block, uvm_processor_id_t proc, size_t page_index)
{
    unsigned long *resident_mask;

    if (proc == UVM_CPU_ID) {
        resident_mask = block->cpu.resident;
    }
    else {
        uvm_va_block_gpu_state_t *gpu_state = block->gpus[proc - 1];
        if (!gpu_state)
            return false;

        resident_mask = gpu_state->resident;
    }

    return test_bit(page_index, resident_mask);
}

static uvm_pte_bits_cpu_t get_cpu_pte_bit_index(uvm_prot_t prot)
{
    uvm_pte_bits_cpu_t pte_bit_index = UVM_PTE_BITS_CPU_MAX;

    // ATOMIC and WRITE are synonyms for the CPU
    if (prot == UVM_PROT_READ_WRITE_ATOMIC || prot == UVM_PROT_READ_WRITE)
        pte_bit_index = UVM_PTE_BITS_CPU_WRITE;
    else if (prot == UVM_PROT_READ_ONLY)
        pte_bit_index = UVM_PTE_BITS_CPU_READ;
    else
        UVM_ASSERT_MSG(false, "Invalid access permissions %s\n", uvm_prot_string(prot));

    return pte_bit_index;
}

static uvm_pte_bits_gpu_t get_gpu_pte_bit_index(uvm_prot_t prot)
{
    uvm_pte_bits_gpu_t pte_bit_index = UVM_PTE_BITS_GPU_MAX;

    if (prot == UVM_PROT_READ_WRITE_ATOMIC)
        pte_bit_index = UVM_PTE_BITS_GPU_ATOMIC;
    else if (prot == UVM_PROT_READ_WRITE)
        pte_bit_index = UVM_PTE_BITS_GPU_WRITE;
    else if (prot == UVM_PROT_READ_ONLY)
        pte_bit_index = UVM_PTE_BITS_GPU_READ;
    else
        UVM_ASSERT_MSG(false, "Invalid access permissions %s\n", uvm_prot_string(prot));

    return pte_bit_index;
}

void uvm_va_block_region_authorized_gpus(uvm_va_block_t *va_block,
                                         uvm_va_block_region_t region,
                                         uvm_prot_t access_permission,
                                         uvm_processor_mask_t *authorized_processors)
{
    uvm_gpu_id_t gpu_id;
    uvm_pte_bits_gpu_t search_gpu_bit = get_gpu_pte_bit_index(access_permission);

    uvm_processor_mask_zero(authorized_processors);

    // Test all GPUs with mappings on the block
    for_each_gpu_id_in_mask(gpu_id, &va_block->mapped) {
        uvm_va_block_gpu_state_t *gpu_state = va_block->gpus[gpu_id - 1];
        if (gpu_state && !uvm_page_mask_region_empty(gpu_state->pte_bits[search_gpu_bit], region))
            uvm_processor_mask_set(authorized_processors, gpu_id);
    }
}

void uvm_va_block_region_authorized_processors(uvm_va_block_t *va_block,
                                               uvm_va_block_region_t region,
                                               uvm_prot_t access_permission,
                                               uvm_processor_mask_t *authorized_processors)
{
    uvm_pte_bits_cpu_t search_cpu_bit = get_cpu_pte_bit_index(access_permission);

    // Compute GPUs
    uvm_va_block_region_authorized_gpus(va_block, region, access_permission, authorized_processors);

    // Test CPU
    if (uvm_processor_mask_test(&va_block->mapped, UVM_CPU_ID) &&
        !uvm_page_mask_region_empty(va_block->cpu.pte_bits[search_cpu_bit], region)) {
        uvm_processor_mask_set(authorized_processors, UVM_CPU_ID);
    }
}

bool uvm_va_block_is_gpu_authorized_on_whole_region(uvm_va_block_t *va_block,
                                                    uvm_va_block_region_t region,
                                                    uvm_gpu_id_t gpu_id,
                                                    uvm_prot_t required_prot)
{
    uvm_pte_bits_gpu_t search_gpu_bit = get_gpu_pte_bit_index(required_prot);
    uvm_va_block_gpu_state_t *gpu_state = va_block->gpus[gpu_id - 1];

    if (!gpu_state)
        return false;

    return uvm_page_mask_region_full(gpu_state->pte_bits[search_gpu_bit], region);
}

bool uvm_va_block_is_processor_authorized_on_whole_region(uvm_va_block_t *va_block,
                                                          uvm_va_block_region_t region,
                                                          uvm_processor_id_t processor_id,
                                                          uvm_prot_t required_prot)
{
    if (processor_id == UVM_CPU_ID) {
        uvm_pte_bits_cpu_t search_cpu_bit = get_cpu_pte_bit_index(required_prot);

        return uvm_page_mask_region_full(va_block->cpu.pte_bits[search_cpu_bit], region);
    }
    else {
        return uvm_va_block_is_gpu_authorized_on_whole_region(va_block, region, processor_id, required_prot);
    }
}

void uvm_va_block_page_resident_gpus(uvm_va_block_t *va_block,
                                     size_t page_index,
                                     uvm_processor_mask_t *resident_gpus)
{
    uvm_gpu_id_t id;
    uvm_processor_mask_zero(resident_gpus);

    for_each_gpu_id_in_mask(id, &va_block->resident) {
        if (test_bit(page_index, uvm_va_block_resident_mask_get(va_block, id))) {
            UVM_ASSERT(block_processor_page_is_populated(va_block, id, page_index));
            uvm_processor_mask_set(resident_gpus, id);
        }
    }
}

void uvm_va_block_page_resident_processors(uvm_va_block_t *va_block,
                                           size_t page_index,
                                           uvm_processor_mask_t *resident_processors)
{
    uvm_va_block_page_resident_gpus(va_block, page_index, resident_processors);

    if (test_bit(page_index, uvm_va_block_resident_mask_get(va_block, UVM_CPU_ID))) {
        UVM_ASSERT(block_processor_page_is_populated(va_block, UVM_CPU_ID, page_index));
        uvm_processor_mask_set(resident_processors, UVM_CPU_ID);
    }
}

NvU32 uvm_va_block_page_resident_processors_count(uvm_va_block_t *va_block, size_t page_index)
{
    uvm_processor_mask_t resident_processors;
    uvm_va_block_page_resident_processors(va_block, page_index, &resident_processors);

    return uvm_processor_mask_get_count(&resident_processors);
}

uvm_processor_id_t uvm_va_block_page_get_closest_resident(uvm_va_block_t *va_block,
                                                          size_t page_index,
                                                          uvm_processor_id_t processor)
{
    return uvm_va_block_page_get_closest_resident_in_mask(va_block, page_index, processor, NULL);
}

uvm_processor_id_t uvm_va_block_page_get_closest_resident_in_mask(uvm_va_block_t *va_block,
                                                                  size_t page_index,
                                                                  uvm_processor_id_t processor,
                                                                  const uvm_processor_mask_t *processor_mask)
{
    uvm_va_space_t *va_space = va_block->va_range->va_space;
    uvm_processor_mask_t search_mask;
    uvm_processor_id_t id;

    if (processor_mask)
        uvm_processor_mask_and(&search_mask, processor_mask, &va_block->resident);
    else
        uvm_processor_mask_copy(&search_mask, &va_block->resident);

    for_each_closest_id(id, &search_mask, processor, va_space) {
        if (test_bit(page_index, uvm_va_block_resident_mask_get(va_block, id)))
            return id;
    }

    return UVM8_MAX_PROCESSORS;
}

// We don't track the specific aperture of each mapped page. Instead, we assume
// that each virtual mapping from a given processor always targets the closest
// processor on which that page is resident (with special rules for UVM-Lite).
//
// This function verifies that assumption: before a page becomes resident on a
// new location, assert that no processor has a valid mapping to a farther
// processor on that page.
static bool block_check_resident_proximity(uvm_va_block_t *block, size_t page_index, uvm_processor_id_t new_residency)
{
    uvm_processor_mask_t resident_procs, mapped_procs;
    uvm_processor_id_t mapped_id, closest_id;

    uvm_processor_mask_andnot(&mapped_procs, &block->mapped, &block->va_range->uvm_lite_gpus);

    for_each_id_in_mask(mapped_id, &mapped_procs) {
        if (!test_bit(page_index, uvm_va_block_map_mask_get(block, mapped_id)))
            continue;

        uvm_va_block_page_resident_processors(block, page_index, &resident_procs);
        UVM_ASSERT(!uvm_processor_mask_empty(&resident_procs));
        UVM_ASSERT(!uvm_processor_mask_test(&resident_procs, new_residency));
        uvm_processor_mask_set(&resident_procs, new_residency);
        closest_id = uvm_processor_mask_find_closest_id(block->va_range->va_space, &resident_procs, mapped_id);
        UVM_ASSERT(closest_id != new_residency);
    }

    return true;
}

// Returns the processor to which page_index should be mapped on gpu
static uvm_processor_id_t block_gpu_get_processor_to_map(uvm_va_block_t *block, uvm_gpu_t *gpu, size_t page_index)
{
    uvm_va_range_t *va_range = block->va_range;
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_processor_id_t dest_id;

    UVM_ASSERT(gpu_state);

    // UVM-Lite GPUs can only map pages on the preferred location
    if (uvm_processor_mask_test(&va_range->uvm_lite_gpus, gpu->id))
        return va_range->preferred_location;

    // Otherwise we always map the closest resident processor
    dest_id = uvm_va_block_page_get_closest_resident(block, page_index, gpu->id);
    UVM_ASSERT(dest_id != UVM8_MAX_PROCESSORS);
    return dest_id;
}

// Returns the processor to which page_index should be mapped on mapping_id
static uvm_processor_id_t block_get_processor_to_map(uvm_va_block_t *block,
                                                     uvm_processor_id_t mapping_id,
                                                     size_t page_index)
{

    if (mapping_id == UVM_CPU_ID) {
        // The CPU can only map its own memory
        UVM_ASSERT(uvm_va_block_page_get_closest_resident(block, page_index, mapping_id) == UVM_CPU_ID);
        return UVM_CPU_ID;
    }

    return block_gpu_get_processor_to_map(block, uvm_gpu_get(mapping_id), page_index);
}

static void block_get_mapped_processors(uvm_va_block_t *block,
                                        uvm_processor_id_t resident_id,
                                        size_t page_index,
                                        uvm_processor_mask_t *mapped_procs)
{
    uvm_processor_id_t mapped_id;

    uvm_processor_mask_zero(mapped_procs);

    for_each_id_in_mask(mapped_id, &block->mapped) {
        if (test_bit(page_index, uvm_va_block_map_mask_get(block, mapped_id)) &&
            block_get_processor_to_map(block, mapped_id, page_index) == resident_id)
            uvm_processor_mask_set(mapped_procs, mapped_id);
    }
}

// We use block_gpu_get_processor_to_map to find the destination processor of a
// given GPU mapping. This function is called when the mapping is established to
// sanity check that the destination of the mapping matches the query.
static bool block_check_mapping_residency_region(uvm_va_block_t *block,
                                                 uvm_gpu_t *gpu,
                                                 uvm_processor_id_t mapping_dest,
                                                 uvm_va_block_region_t region,
                                                 const unsigned long *page_mask)
{
    size_t page_index;
    uvm_processor_id_t proc_to_map;
    for_each_va_block_page_in_mask(page_index, page_mask, region) {
        proc_to_map = block_gpu_get_processor_to_map(block, gpu, page_index);
        UVM_ASSERT_MSG(mapping_dest == proc_to_map,
                       "VA 0x%llx on %s: mapping %s, supposed to map %s",
                       block->start + page_index * PAGE_SIZE,
                       gpu->name,
                       uvm_processor_name(mapping_dest),
                       uvm_processor_name(proc_to_map));
    }
    return true;
}

static bool block_check_mapping_residency(uvm_va_block_t *block,
                                          uvm_gpu_t *gpu,
                                          uvm_processor_id_t mapping_dest,
                                          const unsigned long *page_mask)
{
    return block_check_mapping_residency_region(block,
                                                gpu,
                                                mapping_dest,
                                                uvm_va_block_region_from_block(block),
                                                page_mask);
}

// Check that there are no mappings targeting resident_id from any processor in
// the block.
static bool block_check_processor_not_mapped(uvm_va_block_t *block, uvm_processor_id_t resident_id)
{
    uvm_va_block_region_t region = uvm_va_block_region_from_block(block);
    const unsigned long *map_mask;
    uvm_processor_id_t mapped_id;
    size_t page_index;

    for_each_id_in_mask(mapped_id, &block->mapped) {
        map_mask = uvm_va_block_map_mask_get(block, mapped_id);

        for_each_va_block_page_in_mask(page_index, map_mask, region)
            UVM_ASSERT(block_get_processor_to_map(block, mapped_id, page_index) != resident_id);
    }

    return true;
}

// Zero all pages of the newly-populated chunk which are not resident anywhere
// else in the system, adding that work to the block's tracker. In all cases,
// this function adds a dependency on passed in tracker to the block's tracker.
static NV_STATUS block_zero_new_gpu_chunk(uvm_va_block_t *block,
                                          uvm_gpu_t *gpu,
                                          uvm_gpu_chunk_t *chunk,
                                          uvm_va_block_region_t chunk_region,
                                          uvm_tracker_t *tracker)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    NV_STATUS status;
    uvm_gpu_address_t memset_addr_base, memset_addr, phys_addr;
    uvm_push_t push;
    uvm_gpu_id_t id;
    uvm_va_block_region_t subregion, big_region_all;
    unsigned long *zero_mask = kmem_cache_alloc(g_uvm_page_mask_cache, NV_UVM_GFP_FLAGS);
    bool big_page_swizzle = false;
    NvU32 big_page_size = 0;

    UVM_ASSERT(uvm_va_block_region_size(chunk_region) == uvm_gpu_chunk_get_size(chunk));

    if (!zero_mask)
        return NV_ERR_NO_MEMORY;

    if (gpu->big_page.swizzling) {
        // When populating we don't yet know what the mapping will be, so we
        // don't know whether this will be initially mapped as a big page (which
        // must be swizzled) or as 4k pages (which must not). Our common case
        // for first populate on swizzled GPUs (UVM-Lite) is full migration and
        // mapping of an entire block, so assume that we will swizzle if the
        // block is large enough to fit a big page. If we're wrong, the big page
        // will be deswizzled at map time.
        //
        // Note also that this chunk might be able to fit more than one big
        // page.
        big_page_size = uvm_va_block_gpu_big_page_size(block, gpu);
        big_region_all = uvm_va_block_big_page_region_all(block, big_page_size);

        // Note that this condition also handles the case of having no big pages
        // in the block, in which case big_region_all is {0. 0}.
        if (uvm_va_block_region_contains_region(big_region_all, chunk_region)) {
            big_page_swizzle = true;
            UVM_ASSERT(uvm_gpu_chunk_get_size(chunk) >= big_page_size);
        }
    }

    phys_addr = uvm_gpu_address_physical(UVM_APERTURE_VID, chunk->address);
    if (big_page_swizzle)
        memset_addr_base = uvm_mmu_gpu_address_for_big_page_physical(phys_addr, gpu);
    else
        memset_addr_base = phys_addr;

    memset_addr = memset_addr_base;

    // Tradeoff: zeroing entire chunk vs zeroing only the pages needed for the
    // operation.
    //
    // We may over-zero the page with this approach. For example, we might be
    // populating a 2MB chunk because only a single page within that chunk needs
    // to be made resident. If we also zero non-resident pages outside of the
    // strict region, we could waste the effort if those pages are populated on
    // another processor later and migrated here.
    //
    // We zero all non-resident pages in the chunk anyway for two reasons:
    //
    // 1) Efficiency. It's better to do all zeros as pipelined transfers once
    //    rather than scatter them around for each populate operation.
    //
    // 2) Optimizing the common case of block_populate_gpu_chunk being called
    //    for already-populated chunks. If we zero once at initial populate, we
    //    can simply check whether the chunk is present in the array. Otherwise
    //    we'd have to recompute the "is any page resident" mask every time.

    // Roll up all pages in chunk_region which are resident somewhere
    uvm_page_mask_zero(zero_mask);
    for_each_id_in_mask(id, &block->resident)
        uvm_page_mask_or(zero_mask, zero_mask, uvm_va_block_resident_mask_get(block, id));

    // If all pages in the chunk are resident somewhere, we don't need to clear
    // anything. Just make sure the chunk is tracked properly.
    if (uvm_page_mask_region_full(zero_mask, chunk_region)) {
        status = uvm_tracker_add_tracker_safe(&block->tracker, tracker);
        goto out;
    }

    // Complement to get the pages which are not resident anywhere. These
    // are the pages which must be zeroed.
    uvm_page_mask_complement(zero_mask, zero_mask);

    status = uvm_push_begin_acquire(gpu->channel_manager, UVM_CHANNEL_TYPE_GPU_INTERNAL, tracker, &push,
                                    "Zero out chunk [0x%llx, 0x%llx) for region [0x%llx, 0x%llx) in va block [0x%llx, 0x%llx)",
                                    chunk->address, chunk->address + uvm_gpu_chunk_get_size(chunk),
                                    uvm_va_block_region_start(block, chunk_region),
                                    uvm_va_block_region_end(block, chunk_region) + 1,
                                    block->start, block->end + 1);
    if (status != NV_OK)
        goto out;

    for_each_va_block_subregion_in_mask(subregion, zero_mask, chunk_region) {
        // Pipeline the memsets since they never overlap with each other
        uvm_push_set_flag(&push, UVM_PUSH_FLAG_CE_NEXT_PIPELINED);

        // We'll push one membar later for all memsets in this loop
        uvm_push_set_flag(&push, UVM_PUSH_FLAG_CE_NEXT_MEMBAR_NONE);

        memset_addr.address = memset_addr_base.address + (subregion.first - chunk_region.first) * PAGE_SIZE;
        gpu->ce_hal->memset_8(&push, memset_addr, 0, uvm_va_block_region_size(subregion));
    }

    // A membar from this GPU is required between this memset and any PTE write
    // pointing this or another GPU to this chunk. Otherwise an engine could
    // read the PTE then access the page before the memset write is visible to
    // that engine.
    //
    // This memset writes GPU memory, so local mappings need only a GPU-local
    // membar. We can't easily determine here whether a peer GPU will ever map
    // this page in the future, so always use a sysmembar. uvm_push_end provides
    // one by default.
    //
    // TODO: Bug 1766424: Use GPU-local membars if no peer can currently map
    //       this page. When peer access gets enabled, do a MEMBAR_SYS at that
    //       point.
    uvm_push_end(&push);
    status = uvm_tracker_add_push_safe(&block->tracker, &push);

out:
    if (big_page_swizzle && status == NV_OK) {
        // Set big_pages_swizzled for each big page region covered by the new
        // chunk. We do this regardless of whether we actually wrote anything,
        // since this controls how the initial data will be copied into the
        // page later. See the above comment on big_page_swizzle.
        bitmap_set(gpu_state->big_pages_swizzled,
                   uvm_va_block_big_page_index(block, chunk_region.first, big_page_size),
                   (size_t)uvm_div_pow2_64(uvm_va_block_region_size(chunk_region), big_page_size));
    }

    if (zero_mask)
        kmem_cache_free(g_uvm_page_mask_cache, zero_mask);
    return status;
}

static NV_STATUS block_populate_gpu_chunk(uvm_va_block_t *block,
                                          uvm_va_block_retry_t *retry,
                                          uvm_gpu_t *gpu,
                                          size_t chunk_index,
                                          uvm_va_block_region_t chunk_region)
{
    uvm_va_block_gpu_state_t *gpu_state = block_gpu_state_get_alloc(block, gpu);
    uvm_gpu_chunk_t *chunk = NULL;
    uvm_chunk_size_t chunk_size = uvm_va_block_region_size(chunk_region);
    NV_STATUS status;

    if (!gpu_state)
        return NV_ERR_NO_MEMORY;

    UVM_ASSERT(chunk_index < block_num_gpu_chunks(block, gpu));
    UVM_ASSERT(chunk_size & uvm_mmu_user_chunk_sizes(gpu));

    // We zero chunks as necessary at initial population, so if the chunk is
    // already populated we're done. See the comment in
    // block_zero_new_gpu_chunk.
    if (gpu_state->chunks[chunk_index])
        return NV_OK;

    UVM_ASSERT(uvm_page_mask_region_empty(gpu_state->resident, chunk_region));

    status = block_alloc_gpu_chunk(block, retry, gpu, chunk_size, &chunk);
    if (status != NV_OK)
        return status;

    status = block_zero_new_gpu_chunk(block, gpu, chunk, chunk_region, &retry->tracker);
    if (status != NV_OK) {
        uvm_pmm_gpu_free(&gpu->pmm, chunk, NULL);
        return status;
    }

    // Record the used chunk so that it can be unpinned at the end of the whole
    // operation.
    block_retry_add_used_chunk(retry, gpu, chunk);
    gpu_state->chunks[chunk_index] = chunk;
    return NV_OK;
}

// Populate all chunks which cover the given region and page mask.
static NV_STATUS block_populate_pages_gpu(uvm_va_block_t *block,
                                          uvm_va_block_retry_t *retry,
                                          uvm_gpu_t *gpu,
                                          uvm_va_block_region_t region,
                                          unsigned long *populate_mask)
{
    uvm_va_block_region_t chunk_region, check_region;
    size_t chunk_index, page_index;
    uvm_chunk_size_t chunk_size;
    NV_STATUS status;

    page_index = uvm_va_block_first_page_in_mask(region, populate_mask);
    if (page_index == region.outer)
        return NV_OK;

    chunk_index = block_gpu_chunk_index(block, gpu, page_index, &chunk_size);
    chunk_region = block_gpu_chunk_region(block, chunk_size, page_index);

    while (1) {
        check_region = uvm_va_block_region(max(chunk_region.first, region.first),
                                           min(chunk_region.outer, region.outer));
        page_index = uvm_va_block_first_page_in_mask(check_region, populate_mask);
        if (page_index != check_region.outer) {
            status = block_populate_gpu_chunk(block, retry, gpu, chunk_index, chunk_region);
            if (status != NV_OK)
                return status;
        }

        if (check_region.outer == region.outer)
            break;

        ++chunk_index;
        chunk_size = block_gpu_chunk_size(block, gpu, chunk_region.outer);
        chunk_region = uvm_va_block_region(chunk_region.outer, chunk_region.outer + (chunk_size / PAGE_SIZE));
    }

    return NV_OK;
}

static NV_STATUS block_populate_pages(uvm_va_block_t *block,
                                      uvm_va_block_retry_t *retry,
                                      uvm_va_block_context_t *block_context,
                                      uvm_processor_id_t dest_id,
                                      uvm_va_block_region_t region,
                                      const unsigned long *page_mask)
{
    NV_STATUS status;
    unsigned long *resident_mask = block_resident_mask_get_alloc(block, dest_id);
    size_t page_index;
    unsigned long *populate_page_mask = block_context->make_resident.page_mask;

    if (!resident_mask)
        return NV_ERR_NO_MEMORY;

    if (page_mask)
        uvm_page_mask_andnot(populate_page_mask, page_mask, resident_mask);
    else
        uvm_page_mask_complement(populate_page_mask, resident_mask);

    if (dest_id != UVM_CPU_ID)
        return block_populate_pages_gpu(block, retry, uvm_gpu_get(dest_id), region, populate_page_mask);

    for_each_va_block_page_in_mask(page_index, populate_page_mask, region) {
        uvm_processor_mask_t resident_on;
        bool resident_somewhere;
        uvm_va_block_page_resident_processors(block, page_index, &resident_on);
        resident_somewhere = !uvm_processor_mask_empty(&resident_on);

        // For pages not resident anywhere, need to populate with zeroed memory
        status = block_populate_page_cpu(block, page_index, !resident_somewhere);
        if (status != NV_OK)
            return status;
    }

    return NV_OK;
}

static uvm_processor_mask_t *block_get_can_copy_from_mask(uvm_va_block_t *block, uvm_processor_id_t from)
{
    return &block->va_range->va_space->can_copy_from[from];
}

static bool block_can_copy_from(uvm_va_block_t *va_block, uvm_processor_id_t from, uvm_processor_id_t to)
{
    return uvm_processor_mask_test(block_get_can_copy_from_mask(va_block, to), from);
}

// Get the physical GPU address of a block's page from the POV of the specified GPU
// This is the address that should be used for making PTEs for the specified GPU.
static uvm_gpu_phys_address_t block_phys_page_address(uvm_va_block_t *block, block_phys_page_t block_page, uvm_gpu_t *gpu)
{
    uvm_va_block_gpu_state_t *gpu_state;
    uvm_aperture_t aperture;
    size_t chunk_index, page_offset;
    uvm_gpu_t *owning_gpu;
    uvm_gpu_chunk_t *chunk;
    uvm_chunk_size_t chunk_size;

    if (block_page.processor == UVM_CPU_ID) {
        // TODO: Bug 1765195: IOMMU support
        NvU64 phys = page_to_phys(block->cpu.pages[block_page.page_index]);
        return uvm_gpu_phys_address(UVM_APERTURE_SYS, phys);
    }

    owning_gpu = uvm_gpu_get(block_page.processor);

    gpu_state = block->gpus[block_page.processor - 1];
    UVM_ASSERT(gpu_state);

    if (block_page.processor == gpu->id) {
        aperture = UVM_APERTURE_VID;
    }
    else {
        UVM_ASSERT(uvm_va_space_peer_enabled(block->va_range->va_space, gpu, owning_gpu));

        aperture = uvm_gpu_peer_aperture(gpu, owning_gpu);
    }

    chunk_index = block_gpu_chunk_index(block, owning_gpu, block_page.page_index, &chunk_size);
    chunk = gpu_state->chunks[chunk_index];
    UVM_ASSERT(chunk);

    page_offset = block_page.page_index - block_gpu_chunk_region(block, chunk_size, block_page.page_index).first;
    return uvm_gpu_phys_address(aperture, chunk->address + page_offset * PAGE_SIZE);
}

// Get the physical GPU address of a block's page from the POV of the specified
// GPU, suitable for accessing the memory from UVM-internal CE channels.
//
// Notably this is may be different from block_phys_page_address() to handle CE
// limitations in addressing physical memory directly.
static uvm_gpu_address_t block_phys_page_copy_address(uvm_va_block_t *block, block_phys_page_t block_page, uvm_gpu_t *gpu)
{
    uvm_gpu_phys_address_t phys = block_phys_page_address(block, block_page, gpu);
    uvm_gpu_t *peer_gpu;

    UVM_ASSERT_MSG(block_can_copy_from(block, gpu->id, block_page.processor),
            "from %s to %s\n", uvm_processor_name(gpu->id), uvm_processor_name(block_page.processor));

    if (block_page.processor != UVM_CPU_ID && block_page.processor != gpu->id) {
        // See the comments on the peer_identity_mappings_supported assignments
        // in the HAL for why we disable direct copies between peers.
        peer_gpu = uvm_gpu_get(block_page.processor);

        // GPUs which swizzle in particular must never have direct copies
        // because then we'd need to create both big and 4k mappings.
        UVM_ASSERT(!gpu->big_page.swizzling);
        UVM_ASSERT(!peer_gpu->big_page.swizzling);

        return uvm_gpu_peer_memory_address(gpu, peer_gpu, phys);
    }

    // If this page is currently in a swizzled big page format, we have to copy
    // using the big page identity mapping in order to deswizzle.
    if (block_page.processor == gpu->id && block_gpu_page_is_swizzled(block, gpu, block_page.page_index))
        return uvm_mmu_gpu_address_for_big_page_physical(uvm_gpu_address_from_phys(phys), gpu);

    return uvm_gpu_address_from_phys(phys);
}

// Begin a push appropriate for copying data from src_id processor to dst_id processor.
// One of src_id and dst_id needs to be a GPU.
static NV_STATUS block_copy_begin_push(uvm_va_block_t *va_block, uvm_processor_id_t dst_id, uvm_processor_id_t src_id,
        uvm_tracker_t *tracker, uvm_push_t *push)
{
    uvm_channel_type_t channel_type;
    uvm_gpu_t *gpu;

    UVM_ASSERT_MSG(src_id != dst_id, "Unexpected copy to self, processor %s\n", uvm_processor_name(src_id));

    if (src_id == UVM_CPU_ID) {
        gpu = uvm_gpu_get(dst_id);
        channel_type = UVM_CHANNEL_TYPE_CPU_TO_GPU;
    }
    else if (dst_id == UVM_CPU_ID) {
        gpu = uvm_gpu_get(src_id);
        channel_type = UVM_CHANNEL_TYPE_GPU_TO_CPU;
    }
    else {
        // For GPU to GPU copies, prefer to "push" the data from the source as
        // that works better at least for P2P over PCI-E.
        gpu = uvm_gpu_get(src_id);

        // TODO: Bug 1764955: Use peer specific channels when available
        channel_type = UVM_CHANNEL_TYPE_GPU_TO_GPU;
    }

    UVM_ASSERT_MSG(block_can_copy_from(va_block, gpu->id, dst_id),
            "GPU %s dst %s src %s\n", uvm_processor_name(gpu->id), uvm_processor_name(dst_id), uvm_processor_name(src_id));
    UVM_ASSERT_MSG(block_can_copy_from(va_block, gpu->id, src_id),
            "GPU %s dst %s src %s\n", uvm_processor_name(gpu->id), uvm_processor_name(dst_id), uvm_processor_name(src_id));

    return uvm_push_begin_acquire(gpu->channel_manager, channel_type, tracker, push,
            "Copy from %s to %s for block [0x%llx, 0x%llx]",
            uvm_processor_name(src_id),
            uvm_processor_name(dst_id),
            va_block->start,
            va_block->end);
}

// A page is clean iff...
// the destination is the preferred location and
// the source is the CPU and
// the destination does not support faults/eviction and
// the CPU page is not dirty
static bool block_page_is_clean(uvm_va_block_t *block,
                                uvm_processor_id_t dst_id,
                                uvm_processor_id_t src_id,
                                size_t page_index)
{
    return dst_id == block->va_range->preferred_location &&
           src_id == UVM_CPU_ID &&
           !uvm_gpu_get(dst_id)->handling_replayable_faults &&
           !PageDirty(block->cpu.pages[page_index]);
}

// When the destination is the CPU...
// if the source is the preferred location, mark as clean
// otherwise, mark as dirty
static void block_update_page_dirty_state(uvm_va_block_t *block,
                                          uvm_processor_id_t dst_id,
                                          uvm_processor_id_t src_id,
                                          size_t page_index)
{
    if (dst_id != UVM_CPU_ID)
        return;

    if (src_id == block->va_range->preferred_location)
        ClearPageDirty(block->cpu.pages[page_index]);
    else
        SetPageDirty(block->cpu.pages[page_index]);
}

static void block_set_resident_processor(uvm_va_block_t *block, uvm_processor_id_t id)
{
    uvm_gpu_t *gpu;

    UVM_ASSERT(!uvm_page_mask_empty(uvm_va_block_resident_mask_get(block, id)));

    if (uvm_processor_mask_test_and_set(&block->resident, id))
        return;

    if (id == UVM_CPU_ID)
        return;

    gpu = uvm_gpu_get(id);

    // If the block is of the max size and the GPU supports eviction, mark the
    // root chunk as used in PMM.
    if (uvm_va_block_size(block) == UVM_CHUNK_SIZE_MAX && uvm_gpu_supports_eviction(gpu)) {
        // The chunk has to be there if this GPU is resident
        uvm_pmm_gpu_mark_root_chunk_used(&gpu->pmm, block->gpus[gpu->id - 1]->chunks[0]);
    }
}

static void block_clear_resident_processor(uvm_va_block_t *block, uvm_processor_id_t id)
{
    uvm_gpu_t *gpu;

    UVM_ASSERT(uvm_page_mask_empty(uvm_va_block_resident_mask_get(block, id)));

    if (!uvm_processor_mask_test_and_clear(&block->resident, id))
        return;

    if (id == UVM_CPU_ID)
        return;

    gpu = uvm_gpu_get(id);

    // If the block is of the max size and the GPU supports eviction, mark the
    // root chunk as unused in PMM.
    if (uvm_va_block_size(block) == UVM_CHUNK_SIZE_MAX && uvm_gpu_supports_eviction(gpu)) {
        // The chunk may not be there any more when residency is cleared.
        uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
        if (gpu_state && gpu_state->chunks[0])
            uvm_pmm_gpu_mark_root_chunk_unused(&gpu->pmm, gpu_state->chunks[0]);
    }
}

typedef enum
{
    BLOCK_TRANSFER_MODE_INTERNAL_MOVE            = 1,
    BLOCK_TRANSFER_MODE_INTERNAL_COPY            = 2,
    BLOCK_TRANSFER_MODE_INTERNAL_MOVE_TO_STAGE   = 3,
    BLOCK_TRANSFER_MODE_INTERNAL_MOVE_FROM_STAGE = 4,
    BLOCK_TRANSFER_MODE_INTERNAL_COPY_TO_STAGE   = 5,
    BLOCK_TRANSFER_MODE_INTERNAL_COPY_FROM_STAGE = 6
} block_transfer_mode_internal_t;

// Copies pages resident on the src_id processor to the dst_id processor
//
// Acquires the block's tracker and adds all of its pushes to the copy_tracker.
static NV_STATUS block_copy_resident_pages_between(uvm_va_block_t *block,
                                                   uvm_va_block_context_t *block_context,
                                                   uvm_processor_id_t dst_id,
                                                   uvm_processor_id_t src_id,
                                                   uvm_va_block_region_t region,
                                                   const unsigned long *page_mask,
                                                   UvmEventMigrationCause cause,
                                                   block_transfer_mode_internal_t transfer_mode,
                                                   unsigned long *migrated_pages,
                                                   NvU32 *copied_pages,
                                                   uvm_tracker_t *copy_tracker)
{
    NV_STATUS tracker_status, status = NV_OK;
    unsigned long *src_resident_mask = uvm_va_block_resident_mask_get(block, src_id);
    unsigned long *dst_resident_mask = uvm_va_block_resident_mask_get(block, dst_id);
    uvm_gpu_t *copying_gpu = NULL;
    uvm_gpu_address_t src_address;
    uvm_gpu_address_t dst_address;
    uvm_push_t push;
    size_t page_index;
    size_t contig_start_index = region.outer;
    size_t last_index = region.outer;
    unsigned long *src_move_mask = block_context->make_resident.copy_resident_pages_between_mask;
    uvm_range_group_range_t *rgr = NULL;
    bool rgr_has_changed = false;

    *copied_pages = 0;

    if (dst_id == src_id)
        return NV_OK;

    uvm_page_mask_andnot(src_move_mask, src_resident_mask, dst_resident_mask);

    if (page_mask)
        uvm_page_mask_and(src_move_mask, src_move_mask, page_mask);

    // uvm_range_group_range_iter_first should only be called when the va_space lock is held, which is always the case
    // unless an eviction is taking place.
    if (cause != UvmEventMigrationCauseEviction) {
        rgr = uvm_range_group_range_iter_first(block->va_range->va_space,
                                               uvm_va_block_region_start(block, region),
                                               uvm_va_block_region_end(block, region));
        rgr_has_changed = true;
    }

    for_each_va_block_page_in_mask(page_index, src_move_mask, region) {
        NvU64 page_start = block->start + PAGE_SIZE * page_index;

        if (dst_id == UVM_CPU_ID) {
            // To support staging through CPU, populate CPU pages on demand.
            // GPU destinations should have their pages populated already, but
            // that might change if we add staging through GPUs.
            status = block_populate_page_cpu(block, page_index, false);
            if (status != NV_OK)
                break;
        }

        UVM_ASSERT(block_processor_page_is_populated(block, dst_id, page_index));

        // If we're not evicting and we're migrating away from the preferred location, then we should add the range
        // group range to the list of migrated ranges in the range group. It's safe to skip this because the use of
        // range_group's migrated_ranges list is a UVM-Lite optimization - eviction is not supported on UVM-Lite GPUs.
        if (cause != UvmEventMigrationCauseEviction && src_id == block->va_range->preferred_location ) {
            // The current page could be after the end of rgr. Iterate over the range group ranges until rgr's
            // end location is greater than or equal to the current page.
            while (rgr && rgr->node.end < page_start) {
                rgr = uvm_range_group_range_iter_next(block->va_range->va_space,
                                                      rgr,
                                                      uvm_va_block_region_end(block, region));
                rgr_has_changed = true;
            }

            // Check whether the current page lies within rgr. A single page must entirely reside within a range group
            // range. Since we've incremented rgr until its end is higher than page_start, we now check if page_start
            // lies within rgr.
            if (rgr && rgr_has_changed && page_start >= rgr->node.start && page_start <= rgr->node.end) {
                uvm_spin_lock(&rgr->range_group->migrated_ranges_lock);
                if (list_empty(&rgr->range_group_migrated_list_node))
                    list_move_tail(&rgr->range_group_migrated_list_node, &rgr->range_group->migrated_ranges);
                uvm_spin_unlock(&rgr->range_group->migrated_ranges_lock);
            }
        }

        // No need to copy pages that haven't changed.  Just clear residency information
        if (block_page_is_clean(block, dst_id, src_id, page_index))
            goto update_bits;

        if (!copying_gpu) {
            uvm_perf_event_data_t event_data =
                {
                    .migration =
                        {
                            .push = &push,
                            .block = block,
                            .src = src_id,
                            .dst = dst_id,
                            .address = page_start,
                            .bytes = 0,
                            .cause = cause
                        }
                };
            status = block_copy_begin_push(block, dst_id, src_id, &block->tracker, &push);
            if (status != NV_OK)
                break;
            copying_gpu = uvm_push_get_gpu(&push);

            // Record all processors involved in the copy
            uvm_processor_mask_set(&block_context->make_resident.all_involved_processors, copying_gpu->id);
            uvm_processor_mask_set(&block_context->make_resident.all_involved_processors, dst_id);
            uvm_processor_mask_set(&block_context->make_resident.all_involved_processors, src_id);

            uvm_perf_event_notify(&block->va_range->va_space->perf_events, UVM_PERF_EVENT_BLOCK_MIGRATION_BEGIN, &event_data);
        }
        else {
            uvm_push_set_flag(&push, UVM_PUSH_FLAG_CE_NEXT_PIPELINED);
        }

        block_update_page_dirty_state(block, dst_id, src_id, page_index);

        src_address = block_phys_page_copy_address(block, block_phys_page(src_id, page_index), copying_gpu);
        dst_address = block_phys_page_copy_address(block, block_phys_page(dst_id, page_index), copying_gpu);

        if (last_index == region.outer) {
            contig_start_index = page_index;
        }
        else if (page_index != last_index + 1) {
            uvm_va_block_region_t contig_region = uvm_va_block_region(contig_start_index, last_index + 1);
            uvm_perf_event_data_t event_data =
                {
                    .migration =
                        {
                            .push          = &push,
                            .block         = block,
                            .src           = src_id,
                            .dst           = dst_id,
                            .address       = uvm_va_block_region_start(block, contig_region),
                            .bytes         = uvm_va_block_region_size(contig_region),
                            .cause         = cause,
                            .transfer_mode = transfer_mode
                        }
                };

            UVM_ASSERT(uvm_va_block_region_contains_region(region, contig_region));
            uvm_perf_event_notify(&block->va_range->va_space->perf_events, UVM_PERF_EVENT_MIGRATION, &event_data);
            contig_start_index = page_index;
        }

        uvm_push_set_flag(&push, UVM_PUSH_FLAG_CE_NEXT_MEMBAR_NONE);
        copying_gpu->ce_hal->memcopy(&push, dst_address, src_address, PAGE_SIZE);

        last_index = page_index;

update_bits:
        *copied_pages += 1;

        UVM_ASSERT(block_check_resident_proximity(block, page_index, dst_id));

        __set_bit(page_index, migrated_pages);
        __set_bit(page_index, dst_resident_mask);

        if (transfer_mode == BLOCK_TRANSFER_MODE_INTERNAL_COPY ||
            transfer_mode == BLOCK_TRANSFER_MODE_INTERNAL_COPY_FROM_STAGE)
            __set_bit(page_index, block->read_duplicated_pages);

        // If we are staging the copy due to read duplication, we keep the copy there
        if (transfer_mode == BLOCK_TRANSFER_MODE_INTERNAL_MOVE_FROM_STAGE)
            __clear_bit(page_index, src_resident_mask);

        rgr_has_changed = false;
    }

    if (*copied_pages) {
        block_set_resident_processor(block, dst_id);

        // Check whether there are any resident pages left on src
        if (transfer_mode == BLOCK_TRANSFER_MODE_INTERNAL_MOVE_FROM_STAGE && uvm_page_mask_empty(src_resident_mask))
            block_clear_resident_processor(block, src_id);
    }

    if (!copying_gpu)
        return status;

    {
        uvm_va_block_region_t contig_region = uvm_va_block_region(contig_start_index, last_index + 1);
        uvm_perf_event_data_t event_data =
            {
                .migration =
                    {
                        .push          = &push,
                        .block         = block,
                        .src           = src_id,
                        .dst           = dst_id,
                        .address       = uvm_va_block_region_start(block, contig_region),
                        .bytes         = uvm_va_block_region_size(contig_region),
                        .cause         = cause,
                        .transfer_mode = transfer_mode

                    }
            };

        UVM_ASSERT(uvm_va_block_region_contains_region(region, contig_region));
        uvm_perf_event_notify(&block->va_range->va_space->perf_events, UVM_PERF_EVENT_MIGRATION, &event_data);
    }

    // TODO: Bug 1766424: If the destination is a GPU and the copy was done by
    //       that GPU, use a GPU-local membar if no peer can currently map this
    //       page. When peer access gets enabled, do a MEMBAR_SYS at that point.
    uvm_push_end(&push);
    tracker_status = uvm_tracker_add_push_safe(copy_tracker, &push);
    return status == NV_OK ? tracker_status : status;
}

// Copy resident pages to the destination from all source processors in the src_processor_mask
static NV_STATUS block_copy_resident_pages_mask(uvm_va_block_t *block,
                                                uvm_va_block_context_t *block_context,
                                                uvm_processor_id_t dst_id,
                                                uvm_processor_mask_t *src_processor_mask,
                                                uvm_va_block_region_t region,
                                                const unsigned long *page_mask,
                                                UvmEventMigrationCause cause,
                                                block_transfer_mode_internal_t transfer_mode,
                                                NvU32 max_pages_to_copy,
                                                unsigned long *migrated_pages,
                                                NvU32 *copied_pages_out,
                                                uvm_tracker_t *tracker_out)
{
    uvm_processor_id_t src_id;

    *copied_pages_out = 0;

    for_each_id_in_mask(src_id, src_processor_mask) {
        NV_STATUS status;
        NvU32 copied_pages_from_src;

        UVM_ASSERT(src_id != dst_id);

        status = block_copy_resident_pages_between(block,
                                                   block_context,
                                                   dst_id,
                                                   src_id,
                                                   region,
                                                   page_mask,
                                                   cause,
                                                   transfer_mode,
                                                   migrated_pages,
                                                   &copied_pages_from_src,
                                                   tracker_out);
        if (status != NV_OK)
            return status;

        *copied_pages_out += copied_pages_from_src;
        UVM_ASSERT(*copied_pages_out <= max_pages_to_copy);

        // Break out once we copied max pages already
        if (*copied_pages_out == max_pages_to_copy)
            break;
    }

    return NV_OK;
}

static void break_read_duplication_in_region(uvm_va_block_t *block,
                                             uvm_va_block_context_t *block_context,
                                             uvm_processor_id_t dst_id,
                                             uvm_va_block_region_t region,
                                             const long unsigned *page_mask)
{
    uvm_processor_id_t id;
    long unsigned *break_pages_in_region = block_context->scratch_page_mask;

    uvm_page_mask_init_from_region(break_pages_in_region, region, page_mask);

    // Clear read_duplicated bit for all pages in region
    uvm_page_mask_andnot(block->read_duplicated_pages, block->read_duplicated_pages, break_pages_in_region);

    // Clear residency bits for all processors other than dst_id
    for_each_id_in_mask(id, &block->resident) {
        unsigned long *other_resident_mask;

        if (id == dst_id)
            continue;

        other_resident_mask = uvm_va_block_resident_mask_get(block, id);

        if (!uvm_page_mask_andnot(other_resident_mask, other_resident_mask, break_pages_in_region))
            block_clear_resident_processor(block, id);
    }
}

// Copy resident pages from other processors to the destination and mark any
// pages not resident anywhere as resident on the destination.
// All the pages on the destination need to be populated by the caller first.
// Pages not resident anywhere else need to be zeroed out as well.
//
// If UVM_VA_BLOCK_TRANSFER_MODE_COPY is passed, processors that already have a
// copy of the page will keep it. Conversely, if UVM_VA_BLOCK_TRANSFER_MODE_MOVE
// is passed, the page will no longer be resident in any processor other than dst_id.
static NV_STATUS block_copy_resident_pages(uvm_va_block_t *block,
                                           uvm_va_block_context_t *block_context,
                                           uvm_processor_id_t dst_id,
                                           uvm_va_block_region_t region,
                                           const unsigned long *page_mask,
                                           UvmEventMigrationCause cause,
                                           uvm_va_block_transfer_mode_t transfer_mode)
{
    NV_STATUS status = NV_OK;
    NV_STATUS tracker_status;
    uvm_tracker_t local_tracker = UVM_TRACKER_INIT();
    unsigned long *resident_mask = uvm_va_block_resident_mask_get(block, dst_id);
    size_t page_index = region.first;
    NvU32 missing_pages_count;
    NvU32 pages_copied;
    NvU32 pages_copied_to_cpu;
    uvm_processor_mask_t src_processor_mask;
    unsigned long *copy_page_mask = block_context->make_resident.page_mask;
    unsigned long *migrated_pages = block_context->make_resident.pages_changed_residency;
    unsigned long *staged_pages = block_context->make_resident.pages_staged;
    uvm_va_space_t *va_space = block->va_range->va_space;
    block_transfer_mode_internal_t transfer_mode_internal;

    if (page_mask) {
        uvm_page_mask_and(copy_page_mask, resident_mask, page_mask);
        missing_pages_count = uvm_page_mask_region_weight(page_mask, region) -
                              uvm_page_mask_region_weight(copy_page_mask, region);
    }
    else {
        missing_pages_count = uvm_va_block_region_num_pages(region) -
                              uvm_page_mask_region_weight(resident_mask, region);
    }

    // If nothing needs to be copied, just check if we need to break
    // read-duplication (i.e. transfer_mode is UVM_VA_BLOCK_TRANSFER_MODE_MOVE)
    if (missing_pages_count == 0)
        goto out;

    // TODO: Bug 1753731: Add P2P2P copies staged through a GPU
    // TODO: Bug 1753731: When a page is resident in multiple locations due to
    //       read-duplication, spread out the source of the copy so we don't
    //       bottleneck on a single location.

    uvm_processor_mask_zero(&src_processor_mask);

    if (dst_id != UVM_CPU_ID) {
        // If the destination is a GPU, first move everything from processors
        // with copy access supported. Notably this will move pages from the CPU
        // as well even if later some extra copies from CPU are required for
        // staged copies.
        uvm_processor_mask_and(&src_processor_mask, block_get_can_copy_from_mask(block, dst_id), &block->resident);
        uvm_processor_mask_clear(&src_processor_mask, dst_id);

        status = block_copy_resident_pages_mask(block,
                                                block_context,
                                                dst_id,
                                                &src_processor_mask,
                                                region,
                                                page_mask,
                                                cause,
                                                transfer_mode == UVM_VA_BLOCK_TRANSFER_MODE_COPY?
                                                    BLOCK_TRANSFER_MODE_INTERNAL_COPY:
                                                    BLOCK_TRANSFER_MODE_INTERNAL_MOVE,
                                                missing_pages_count,
                                                migrated_pages,
                                                &pages_copied,
                                                &local_tracker);
        if (status != NV_OK)
            goto out;

        missing_pages_count -= pages_copied;

        if (missing_pages_count == 0)
            goto out;
    }

    // Now copy from everywhere else to the CPU. This is both for when the
    // destination is the CPU (src_processor_mask empty) and for a staged copy
    // (src_processor_mask containing processors with copy access to dst_id).
    uvm_processor_mask_andnot(&src_processor_mask, &block->resident, &src_processor_mask);
    uvm_processor_mask_clear(&src_processor_mask, dst_id);
    uvm_processor_mask_clear(&src_processor_mask, UVM_CPU_ID);

    uvm_page_mask_zero(staged_pages);

    if (dst_id == UVM_CPU_ID) {
        transfer_mode_internal = transfer_mode == UVM_VA_BLOCK_TRANSFER_MODE_COPY?
                                                    BLOCK_TRANSFER_MODE_INTERNAL_COPY:
                                                    BLOCK_TRANSFER_MODE_INTERNAL_MOVE;
    }
    else {
        transfer_mode_internal = transfer_mode == UVM_VA_BLOCK_TRANSFER_MODE_COPY?
                                                    BLOCK_TRANSFER_MODE_INTERNAL_COPY_TO_STAGE:
                                                    BLOCK_TRANSFER_MODE_INTERNAL_MOVE_TO_STAGE;
    }

    status = block_copy_resident_pages_mask(block,
                                            block_context,
                                            UVM_CPU_ID,
                                            &src_processor_mask,
                                            region,
                                            page_mask,
                                            cause,
                                            transfer_mode_internal,
                                            missing_pages_count,
                                            staged_pages,
                                            &pages_copied_to_cpu,
                                            &local_tracker);
    if (status != NV_OK)
        goto out;

    // If destination is the CPU then we copied everything there above
    if (dst_id == UVM_CPU_ID) {
        if (migrated_pages)
            uvm_page_mask_or(migrated_pages, migrated_pages, staged_pages);

        goto out;
    }

    // Add everything to the block's tracker so that the
    // block_copy_resident_pages_between() call below will acquire it.
    status = uvm_tracker_add_tracker_safe(&block->tracker, &local_tracker);
    if (status != NV_OK)
        goto out;
    uvm_tracker_clear(&local_tracker);

    // Now copy staged pages from the CPU to the destination.
    status = block_copy_resident_pages_between(block,
                                               block_context,
                                               dst_id,
                                               UVM_CPU_ID,
                                               region,
                                               staged_pages,
                                               cause,
                                               transfer_mode == UVM_VA_BLOCK_TRANSFER_MODE_COPY?
                                                   BLOCK_TRANSFER_MODE_INTERNAL_COPY_FROM_STAGE:
                                                   BLOCK_TRANSFER_MODE_INTERNAL_MOVE_FROM_STAGE,
                                               migrated_pages,
                                               &pages_copied,
                                               &local_tracker);
    if (status != NV_OK)
        goto out;

    // If we get here, that means we were staging the copy through the CPU and
    // we should copy as many pages from the CPU as we copied to the CPU.
    UVM_ASSERT(pages_copied == pages_copied_to_cpu);

    // Now copy the rest of pages from the CPU to the destination.
    status = block_copy_resident_pages_between(block,
                                               block_context,
                                               dst_id,
                                               UVM_CPU_ID,
                                               region,
                                               page_mask,
                                               cause,
                                               transfer_mode == UVM_VA_BLOCK_TRANSFER_MODE_COPY?
                                                   BLOCK_TRANSFER_MODE_INTERNAL_COPY:
                                                   BLOCK_TRANSFER_MODE_INTERNAL_MOVE,
                                               migrated_pages,
                                               &pages_copied,
                                               &local_tracker);
    if (status != NV_OK)
        goto out;

out:

    if (transfer_mode == UVM_VA_BLOCK_TRANSFER_MODE_MOVE)
        break_read_duplication_in_region(block, block_context, dst_id, region, page_mask);

    if (page_mask)
        uvm_page_mask_andnot(copy_page_mask, page_mask, resident_mask);
    else
        uvm_page_mask_complement(copy_page_mask, resident_mask);

    // Pages that weren't resident anywhere else were populated at the
    // destination directly. Mark them as resident now.
    for_each_va_block_page_in_mask(page_index, copy_page_mask, region) {
        uvm_perf_event_data_t event_data =
        {
            .first_touch =
                {
                    .block   = block,
                    .dst     = dst_id,
                    .address = block->start + page_index * PAGE_SIZE,
                    .bytes   = PAGE_SIZE,
                }
        };

        UVM_ASSERT(!block_is_page_resident_anywhere(block, page_index));
        UVM_ASSERT(block_processor_page_is_populated(block, dst_id, page_index));
        UVM_ASSERT(block_check_resident_proximity(block, page_index, dst_id));

        __set_bit(page_index, block_context->make_resident.pages_changed_residency);
        __set_bit(page_index, resident_mask);
        block_set_resident_processor(block, dst_id);

        uvm_perf_event_notify(&va_space->perf_events, UVM_PERF_EVENT_FIRST_TOUCH, &event_data);
    }

    // Add everything from the local tracker to the block's tracker.
    // Notably this is also needed for handling block_copy_resident_pages_between()
    // failures in the first loop.
    tracker_status = uvm_tracker_add_tracker_safe(&block->tracker, &local_tracker);
    uvm_tracker_deinit(&local_tracker);

    return status == NV_OK ? tracker_status : status;
}

NV_STATUS uvm_va_block_make_resident(uvm_va_block_t *va_block,
                                     uvm_va_block_retry_t *va_block_retry,
                                     uvm_va_block_context_t *va_block_context,
                                     uvm_processor_id_t dest_id,
                                     uvm_va_block_region_t region,
                                     const unsigned long *page_mask,
                                     UvmEventMigrationCause cause)
{
    NV_STATUS status;
    uvm_va_range_t *va_range = va_block->va_range;
    uvm_processor_mask_t unmap_processor_mask;
    unsigned long *unmap_page_mask = va_block_context->make_resident.page_mask;
    unsigned long *resident_mask;

    uvm_assert_mutex_locked(&va_block->lock);
    UVM_ASSERT(va_block->va_range);
    UVM_ASSERT(va_block->va_range->type == UVM_VA_RANGE_TYPE_MANAGED);

    resident_mask = block_resident_mask_get_alloc(va_block, dest_id);
    if (!resident_mask)
        return NV_ERR_NO_MEMORY;

    // Unmap all mapped processors except for UVM-Lite GPUs as their mappings
    // are largely persistent.
    uvm_processor_mask_andnot(&unmap_processor_mask, &va_block->mapped, &va_range->uvm_lite_gpus);

    if (page_mask)
        uvm_page_mask_andnot(unmap_page_mask, page_mask, resident_mask);
    else
        uvm_page_mask_complement(unmap_page_mask, resident_mask);

    // Unmap all pages not resident on the destination
    status = uvm_va_block_unmap_mask(va_block, va_block_context, &unmap_processor_mask, region, unmap_page_mask);
    if (status != NV_OK)
        return status;

    if (page_mask)
        uvm_page_mask_and(unmap_page_mask, page_mask, va_block->read_duplicated_pages);
    else
        uvm_page_mask_init_from_region(unmap_page_mask, region, va_block->read_duplicated_pages);

    // Also unmap read-duplicated pages excluding dest_id
    uvm_processor_mask_clear(&unmap_processor_mask, dest_id);
    status = uvm_va_block_unmap_mask(va_block, va_block_context, &unmap_processor_mask, region, unmap_page_mask);
    if (status != NV_OK)
        return status;

    // Note that block_populate_pages and block_move_resident_pages also use
    // va_block_context->make_resident.page_mask.
    unmap_page_mask = NULL;

    status = block_populate_pages(va_block, va_block_retry, va_block_context, dest_id, region, page_mask);
    if (status != NV_OK)
        return status;

    status = block_copy_resident_pages(va_block,
                                       va_block_context,
                                       dest_id,
                                       region,
                                       page_mask,
                                       cause,
                                       UVM_VA_BLOCK_TRANSFER_MODE_MOVE);
    if (status != NV_OK)
        return status;

    return NV_OK;
}

NV_STATUS uvm_va_block_make_resident_read_duplicate(uvm_va_block_t *va_block,
                                                    uvm_va_block_retry_t *va_block_retry,
                                                    uvm_va_block_context_t *va_block_context,
                                                    uvm_processor_id_t dest_id,
                                                    uvm_va_block_region_t region,
                                                    const unsigned long *page_mask,
                                                    UvmEventMigrationCause cause)
{
    NV_STATUS status = NV_OK;
    uvm_va_range_t *va_range = va_block->va_range;
    uvm_processor_mask_t unmap_processor_mask;
    union
    {
        unsigned long *revoke;
        unsigned long *unmap;
    } preprocess_page_mask = { va_block_context->make_resident.page_mask };
    uvm_processor_id_t src_id;
    uvm_tracker_t local_tracker = UVM_TRACKER_INIT();

    uvm_assert_mutex_locked(&va_block->lock);
    UVM_ASSERT(va_block->va_range);
    UVM_ASSERT(va_block->va_range->type == UVM_VA_RANGE_TYPE_MANAGED);

    // For pages that are entering read-duplication we need to unmap remote
    // mappings and revoke RW and higher access permissions.
    //
    // The current implementation:
    // - Unmaps pages from all processors but the one with the resident copy
    // - Revokes write access from the processor with the resident copy
    for_each_id_in_mask(src_id, &va_block->resident) {
        uvm_processor_id_t unmap_id;
        unsigned long *resident_mask;

        resident_mask = uvm_va_block_resident_mask_get(va_block, src_id);
        UVM_ASSERT(resident_mask);

        uvm_processor_mask_andnot(&unmap_processor_mask, &va_block->mapped, &va_range->uvm_lite_gpus);

        if (page_mask)
            uvm_page_mask_andnot(preprocess_page_mask.unmap, page_mask, va_block->read_duplicated_pages);
        else
            uvm_page_mask_complement(preprocess_page_mask.unmap, va_block->read_duplicated_pages);

        // If there are no pages that need to be unmapped/revoked, skip to the
        // next processor
        if (!uvm_page_mask_and(preprocess_page_mask.unmap, preprocess_page_mask.unmap, resident_mask))
            continue;

        // Skip the owner of the page
        uvm_processor_mask_clear(&unmap_processor_mask, src_id);

        for_each_id_in_mask(unmap_id, &unmap_processor_mask) {
            status = uvm_va_block_unmap(va_block,
                                        va_block_context,
                                        unmap_id,
                                        region,
                                        preprocess_page_mask.unmap,
                                        &local_tracker);
            if (status != NV_OK)
                break;
        }
        if (status != NV_OK)
            break;

        // Revoke WRITE/ATOMIC access permissions from the processor with the resident copy
        status = uvm_va_block_revoke_prot(va_block,
                                          va_block_context,
                                          src_id,
                                          region,
                                          preprocess_page_mask.revoke,
                                          UVM_PROT_READ_WRITE,
                                          &local_tracker);
        if (status != NV_OK)
            break;
    }

    status = uvm_tracker_add_tracker_safe(&va_block->tracker, &local_tracker);
    uvm_tracker_deinit(&local_tracker);

    if (status != NV_OK)
        return status;

    // Note that block_populate_pages and block_move_resident_pages also use
    // va_block_context->make_resident.page_mask.
    preprocess_page_mask.revoke = NULL;

    status = block_populate_pages(va_block, va_block_retry, va_block_context, dest_id, region, page_mask);
    if (status != NV_OK)
        return status;

    status = block_copy_resident_pages(va_block, va_block_context, dest_id, region, page_mask, cause,
                                       UVM_VA_BLOCK_TRANSFER_MODE_COPY);
    if (status != NV_OK)
        return status;

    return NV_OK;
}

// Looks up the current CPU mapping state of page from the
// block->cpu.pte_bits bitmaps. If write access is enabled,
// UVM_PROT_READ_WRITE_ATOMIC is returned instead of UVM_PROT_READ_WRITE, since
// write access implies atomic access for CPUs.
static uvm_prot_t block_page_prot_cpu(uvm_va_block_t *block, size_t page_index)
{
    uvm_prot_t prot;

    UVM_ASSERT(block->va_range);
    UVM_ASSERT(block->va_range->type == UVM_VA_RANGE_TYPE_MANAGED);

    if (test_bit(page_index, block->cpu.pte_bits[UVM_PTE_BITS_CPU_WRITE]))
        prot = UVM_PROT_READ_WRITE_ATOMIC;
    else if (test_bit(page_index, block->cpu.pte_bits[UVM_PTE_BITS_CPU_READ]))
        prot = UVM_PROT_READ_ONLY;
    else
        prot = UVM_PROT_NONE;


    return prot;
}

// Looks up the current GPU mapping state of page from the
// block->gpus[i]->pte_bits bitmaps.
static uvm_prot_t block_page_prot_gpu(uvm_va_block_t *block, uvm_gpu_t *gpu, size_t page_index)
{
    uvm_va_block_gpu_state_t *gpu_state = block_gpu_state_get_alloc(block, gpu);
    uvm_prot_t prot;

    UVM_ASSERT(block->va_range);
    UVM_ASSERT(block->va_range->type == UVM_VA_RANGE_TYPE_MANAGED);

    if (test_bit(page_index, gpu_state->pte_bits[UVM_PTE_BITS_GPU_ATOMIC]))
        prot = UVM_PROT_READ_WRITE_ATOMIC;
    else if (test_bit(page_index, gpu_state->pte_bits[UVM_PTE_BITS_GPU_WRITE]))
        prot = UVM_PROT_READ_WRITE;
    else if (test_bit(page_index, gpu_state->pte_bits[UVM_PTE_BITS_GPU_READ]))
        prot = UVM_PROT_READ_ONLY;
    else
        prot = UVM_PROT_NONE;

    return prot;
}

static uvm_prot_t block_page_prot(uvm_va_block_t *block, uvm_processor_id_t id, size_t page_index)
{
    if (id == UVM_CPU_ID)
        return block_page_prot_cpu(block, page_index);
    else
        return block_page_prot_gpu(block, uvm_gpu_get(id), page_index);
}

// Returns true if the block has any valid CPU PTE mapping in the block region.
static bool block_has_valid_mapping_cpu(uvm_va_block_t *block, uvm_va_block_region_t region)
{
    size_t valid_page;

    UVM_ASSERT(region.outer <= uvm_va_block_num_cpu_pages(block));

    // Early-out: check whether any address in this block has a CPU mapping
    if (!uvm_processor_mask_test(&block->mapped, UVM_CPU_ID)) {
        UVM_ASSERT(uvm_page_mask_empty(block->cpu.pte_bits[UVM_PTE_BITS_CPU_READ]));
        UVM_ASSERT(uvm_page_mask_empty(block->cpu.pte_bits[UVM_PTE_BITS_CPU_WRITE]));
        return false;
    }

    // All valid mappings have at least read permissions so we only need to
    // inspect the read bits.
    valid_page = find_next_bit(block->cpu.pte_bits[UVM_PTE_BITS_CPU_READ], region.outer, region.first);
    if (valid_page == region.outer)
        return false;

    UVM_ASSERT(block_page_prot_cpu(block, valid_page) != UVM_PROT_NONE);
    return true;
}

// Sanity check the given GPU's chunks array
static bool block_check_chunks(uvm_va_block_t *block, uvm_gpu_id_t id)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[id - 1];
    uvm_gpu_t *gpu;
    size_t i, page_index, num_chunks, chunk_index;
    uvm_gpu_chunk_t *chunk;
    uvm_pmm_gpu_chunk_state_t state;
    uvm_chunk_size_t chunk_size;

    if (!gpu_state)
        return true;

    gpu = uvm_gpu_get(id);

    num_chunks = block_num_gpu_chunks(block, gpu);
    for (page_index = 0, i = 0; i < num_chunks; i++) {
        chunk = gpu_state->chunks[i];

        chunk_index = block_gpu_chunk_index(block, gpu, page_index, &chunk_size);
        if (chunk_index != i) {
            UVM_ERR_PRINT("chunk index mismatch: calculated %zu, is in %zu. VA block [0x%llx, 0x%llx) GPU %u page_index: %zu\n",
                           chunk_index, i, block->start, block->end + 1, id, page_index);
            return false;
        }

        if (chunk) {
            if (chunk_size != uvm_gpu_chunk_get_size(chunk)) {
                UVM_ERR_PRINT("chunk size mismatch: calc %u, actual %u. VA block [0x%llx, 0x%llx) GPU: %u page_index: %zu chunk index: %zu\n",
                              chunk_size, uvm_gpu_chunk_get_size(chunk), block->start, block->end + 1, id,
                              page_index, i);
                return false;
            }

            state = uvm_gpu_chunk_get_state(chunk);
            if (state != UVM_PMM_GPU_CHUNK_STATE_ALLOCATED) {
                UVM_ERR_PRINT("Invalid chunk state %s. VA block [0x%llx, 0x%llx) GPU: %u page_index: %zu chunk index: %zu chunk_size: %u\n",
                              uvm_pmm_gpu_chunk_state_string(state), block->start, block->end + 1, id,
                              page_index, i, chunk_size);
                return false;
            }
        }

        page_index += chunk_size / PAGE_SIZE;
    }

    return true;
}

// Sanity checks for page mappings
static bool block_check_mappings_page(uvm_va_block_t *block, size_t page_index)
{
    uvm_processor_mask_t atomic_mappings, write_mappings, read_mappings;
    uvm_processor_mask_t lite_read_mappings, lite_atomic_mappings;
    uvm_processor_mask_t remaining_mappings, temp_mappings;
    uvm_processor_mask_t resident_processors;
    uvm_processor_id_t residency, id;

    uvm_va_range_t *va_range = block->va_range;
    uvm_va_space_t *va_space = va_range->va_space;

    uvm_va_block_region_t region = uvm_va_block_region(page_index, page_index + 1);

    uvm_va_block_region_authorized_processors(block, region, UVM_PROT_READ_WRITE_ATOMIC,
                                              &atomic_mappings);
    uvm_va_block_region_authorized_processors(block, region, UVM_PROT_READ_WRITE,
                                              &write_mappings);
    uvm_va_block_region_authorized_processors(block, region, UVM_PROT_READ_ONLY,
                                              &read_mappings);

    // Each access bit implies all accesses below it
    UVM_ASSERT(uvm_processor_mask_subset(&atomic_mappings, &write_mappings));
    UVM_ASSERT(uvm_processor_mask_subset(&write_mappings, &read_mappings));
    UVM_ASSERT(uvm_processor_mask_subset(&read_mappings, &block->mapped));

    uvm_va_block_page_resident_processors(block, page_index, &resident_processors);
    UVM_ASSERT(uvm_processor_mask_subset(&resident_processors, &block->resident));

    // Sanity check block_get_mapped_processors
    uvm_processor_mask_copy(&remaining_mappings, &read_mappings);
    for_each_id_in_mask(residency, &resident_processors) {
        block_get_mapped_processors(block, residency, page_index, &temp_mappings);
        UVM_ASSERT(uvm_processor_mask_subset(&temp_mappings, &remaining_mappings));
        uvm_processor_mask_andnot(&remaining_mappings, &remaining_mappings, &temp_mappings);
    }

    // Any remaining mappings point to non-resident locations, so they must be
    // UVM-Lite mappings.
    UVM_ASSERT(uvm_processor_mask_subset(&remaining_mappings, &va_range->uvm_lite_gpus));

    residency = uvm_processor_mask_find_first_id(&resident_processors);

    // If the page is not resident, there should be no valid mappings
    UVM_ASSERT_MSG(uvm_processor_mask_get_count(&resident_processors) > 0 ||
                   uvm_processor_mask_get_count(&read_mappings) == 0,
                   "Resident: 0x%lx - Mappings R: 0x%lx W: 0x%lx A: 0x%lx - SWA: 0x%lx - RD: 0x%lx\n",
                   *resident_processors.bitmap,
                   *read_mappings.bitmap, *write_mappings.bitmap, *atomic_mappings.bitmap,
                   *va_space->system_wide_atomics_enabled_processors.bitmap,
                   *block->read_duplicated_pages);

    // Test read_duplicated_pages mask
    UVM_ASSERT_MSG((uvm_processor_mask_get_count(&resident_processors) <= 1 &&
                     !test_bit(page_index, block->read_duplicated_pages)) ||
                   (uvm_processor_mask_get_count(&resident_processors) > 1 &&
                     test_bit(page_index, block->read_duplicated_pages)),
                   "Resident: 0x%lx - Mappings R: 0x%lx W: 0x%lx A: 0x%lx - SWA: 0x%lx - RD: 0x%lx\n",
                   *resident_processors.bitmap,
                   *read_mappings.bitmap, *write_mappings.bitmap, *atomic_mappings.bitmap,
                   *va_space->system_wide_atomics_enabled_processors.bitmap,
                   *block->read_duplicated_pages);

    // UVM-Lite checks. Since the range group is made non-migratable before the
    // actual migrations for that range group happen, we can only make those
    // checks which are valid on both migratable and non-migratable range
    // groups.
    uvm_processor_mask_and(&lite_read_mappings,
                           &read_mappings, &va_range->uvm_lite_gpus);
    uvm_processor_mask_and(&lite_atomic_mappings,
                           &atomic_mappings, &va_range->uvm_lite_gpus);

    // Any mapping from a UVM-Lite GPU must be atomic...
    UVM_ASSERT(uvm_processor_mask_equal(&lite_read_mappings, &lite_atomic_mappings));
    // ... and must have access to preferred_location
    UVM_ASSERT(uvm_processor_mask_subset(&lite_atomic_mappings,
                                         &va_space->accessible_from[va_range->preferred_location]));
    for_each_id_in_mask(id, &lite_atomic_mappings)
        UVM_ASSERT(uvm_processor_mask_test(&va_space->can_access[id], va_range->preferred_location));

    // Exclude uvm_lite_gpus from mappings' masks after UVM-Lite tests
    uvm_processor_mask_andnot(&read_mappings, &read_mappings, &va_range->uvm_lite_gpus);
    uvm_processor_mask_andnot(&write_mappings, &write_mappings, &va_range->uvm_lite_gpus);
    uvm_processor_mask_andnot(&atomic_mappings, &atomic_mappings, &va_range->uvm_lite_gpus);

    // atomic mappings from GPUs with disabled system-wide atomics are treated
    // as write mappings. Therefore, we remove them from the atomic mappings mask
    uvm_processor_mask_and(&atomic_mappings, &atomic_mappings, &va_space->system_wide_atomics_enabled_processors);

    if (!uvm_processor_mask_empty(&read_mappings)) {
        // Read-duplicate: if a page is resident in multiple locations, it
        // must be resident locally on each mapped processor.
        if (uvm_processor_mask_get_count(&resident_processors) > 1) {
            UVM_ASSERT_MSG(uvm_processor_mask_subset(&read_mappings, &resident_processors),
                           "Read-duplicate copies from remote processors\n"
                           "Resident: 0x%lx - Mappings R: 0x%lx W: 0x%lx A: 0x%lx - SWA: 0x%lx - RD: 0x%lx\n",
                           *resident_processors.bitmap,
                           *read_mappings.bitmap, *write_mappings.bitmap, *atomic_mappings.bitmap,
                           *va_space->system_wide_atomics_enabled_processors.bitmap,
                           *block->read_duplicated_pages);
        }
        else {
            // Processors with mappings must have access to the processor that
            // has the valid copy
            UVM_ASSERT_MSG(uvm_processor_mask_subset(&read_mappings, &va_space->accessible_from[residency]),
                           "Not all processors have access to %s\n",
                           "Resident: 0x%lx - Mappings R: 0x%lx W: 0x%lx A: 0x%lx -"
                           "Access: 0x%lx - Native Atomics: 0x%lx - SWA: 0x%lx\n",
                           uvm_processor_name(residency),
                           *resident_processors.bitmap,
                           *read_mappings.bitmap,
                           *write_mappings.bitmap,
                           *atomic_mappings.bitmap,
                           *va_space->accessible_from[residency].bitmap,
                           *va_space->has_native_atomics[residency].bitmap,
                           *va_space->system_wide_atomics_enabled_processors.bitmap);
            for_each_id_in_mask(id, &read_mappings)
                UVM_ASSERT(uvm_processor_mask_test(&va_space->can_access[id], residency));
        }
    }

    // If any processor has a writable mapping, there must only be one copy of
    // the page in the system
    if (!uvm_processor_mask_empty(&write_mappings)) {
        UVM_ASSERT_MSG(uvm_processor_mask_get_count(&resident_processors) == 1,
                       "Too many resident copies for pages with write_mappings\n"
                       "Resident: 0x%lx - Mappings R: 0x%lx W: 0x%lx A: 0x%lx - SWA: 0x%lx - RD: 0x%lx\n",
                       *resident_processors.bitmap,
                       *read_mappings.bitmap, *write_mappings.bitmap, *atomic_mappings.bitmap,
                       *va_space->system_wide_atomics_enabled_processors.bitmap,
                       *block->read_duplicated_pages);
    }

    if (!uvm_processor_mask_empty(&atomic_mappings)) {
        uvm_processor_mask_t native_atomics;

        uvm_processor_mask_and(&native_atomics, &atomic_mappings,
                               &va_space->has_native_atomics[residency]);

        if (uvm_processor_mask_empty(&native_atomics)) {
            uvm_processor_id_t atomic_id;

            // Only one processor outside of the native group can have atomics enabled
            UVM_ASSERT_MSG(uvm_processor_mask_get_count(&atomic_mappings) == 1,
                           "Too many atomics mappings to %s from processors with non-native atomics\n"
                           "Resident: 0x%lx - Mappings R: 0x%lx W: 0x%lx A: 0x%lx -"
                           "Access: 0x%lx - Native Atomics: 0x%lx - SWA: 0x%lx\n",
                           uvm_processor_name(residency),
                           *resident_processors.bitmap,
                           *read_mappings.bitmap, *write_mappings.bitmap, *atomic_mappings.bitmap,
                           *va_space->accessible_from[residency].bitmap,
                           *va_space->has_native_atomics[residency].bitmap,
                           *va_space->system_wide_atomics_enabled_processors.bitmap);

            atomic_id = uvm_processor_mask_find_first_id(&atomic_mappings);

            // Nobody else is allowed to write
            UVM_ASSERT_MSG(uvm_processor_mask_get_count(&write_mappings) == 1,
                           "Too many write mappings to %s from processors with non-native atomics\n"
                           "Resident: 0x%lx - Mappings R: 0x%lx W: 0x%lx A: 0x%lx -"
                           "Access: 0x%lx - Native Atomics: 0x%lx - SWA: 0x%lx\n",
                           uvm_processor_name(residency),
                           *resident_processors.bitmap,
                           *read_mappings.bitmap, *write_mappings.bitmap, *atomic_mappings.bitmap,
                           *va_space->accessible_from[residency].bitmap,
                           *va_space->has_native_atomics[residency].bitmap,
                           *va_space->system_wide_atomics_enabled_processors.bitmap);

        }
        else {
            uvm_processor_mask_t non_native_atomics;

            // One or more processors within the native group have atomics enabled.
            // All processors outside of that group may have write but not atomic
            // permissions.
            uvm_processor_mask_andnot(&non_native_atomics, &atomic_mappings,
                                      &va_space->has_native_atomics[residency]);
            UVM_ASSERT_MSG(uvm_processor_mask_empty(&non_native_atomics),
                           "atomic mappings to %s from processors native and non-native\n"
                           "Resident: 0x%lx - Mappings R: 0x%lx W: 0x%lx A: 0x%lx -"
                           "Access: 0x%lx - Native Atomics: 0x%lx - SWA: 0x%lx\n",
                           uvm_processor_name(residency),
                           *resident_processors.bitmap,
                           *read_mappings.bitmap, *write_mappings.bitmap, *atomic_mappings.bitmap,
                           *va_space->accessible_from[residency].bitmap,
                           *va_space->has_native_atomics[residency].bitmap,
                           *va_space->system_wide_atomics_enabled_processors.bitmap);
        }
    }

    return true;
}

static bool block_check_mappings_ptes(uvm_va_block_t *block, uvm_gpu_t *gpu)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_va_block_gpu_state_t *resident_gpu_state;
    uvm_pte_bits_gpu_t pte_bit;
    uvm_processor_id_t resident_id;
    uvm_prot_t prot;
    NvU32 big_page_size;
    size_t num_big_pages, big_page_index, chunk_index;
    uvm_va_block_region_t big_region, chunk_region;
    uvm_gpu_t *resident_gpu;
    uvm_gpu_chunk_t *chunk;

    // It's only safe to check the PTE mappings if we have page tables. See
    // uvm_va_block_get_gpu_va_space.
    if (!block_gpu_has_page_tables(block, gpu)) {
        UVM_ASSERT(!uvm_processor_mask_test(&block->mapped, gpu->id));
        return true;
    }

    big_page_size = uvm_va_block_gpu_big_page_size(block, gpu);
    num_big_pages = uvm_va_block_num_big_pages(block, big_page_size);

    if (block_gpu_supports_2m(block, gpu)) {
        if (gpu_state->page_table_range_big.table || gpu_state->page_table_range_4k.table) {
            // 2M blocks require the 2M entry to be allocated for the lower
            // ranges to also be allocated.
            UVM_ASSERT(gpu_state->page_table_range_2m.table);
        }
        else if (gpu_state->page_table_range_2m.table) {
            // If the 2M entry is present but the lower ones aren't, the PTE
            // must be 2M.
            UVM_ASSERT(gpu_state->pte_is_2m);
        }
    }
    else {
        UVM_ASSERT(!gpu_state->page_table_range_2m.table);
        if (num_big_pages == 0)
            UVM_ASSERT(!gpu_state->page_table_range_big.table);
    }

    // If we have the big table and it's in use then it must have been
    // initialized, even if it doesn't currently contain active PTEs.
    if ((!block_gpu_supports_2m(block, gpu) && gpu_state->page_table_range_big.table) ||
        (block_gpu_supports_2m(block, gpu) && !gpu_state->pte_is_2m && gpu_state->activated_big))
        UVM_ASSERT(gpu_state->initialized_big);

    if (gpu_state->pte_is_2m) {
        UVM_ASSERT(block_gpu_supports_2m(block, gpu));
        UVM_ASSERT(gpu_state->page_table_range_2m.table);
        UVM_ASSERT(bitmap_empty(gpu_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK));
        UVM_ASSERT(!gpu_state->force_4k_ptes);

        // GPU architectures which support 2M pages only support 64K as the big
        // page size. All of the 2M code assumes that
        // MAX_BIG_PAGES_PER_UVM_VA_BLOCK covers a 2M PTE exactly (bitmap_full,
        // bitmap_complement, etc).
        BUILD_BUG_ON((UVM_PAGE_SIZE_2M / UVM_PAGE_SIZE_64K) != MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

        prot = block_page_prot_gpu(block, gpu, 0);

        // All page permissions match
        for (pte_bit = 0; pte_bit < UVM_PTE_BITS_GPU_MAX; pte_bit++) {
            if (prot == UVM_PROT_NONE || pte_bit > get_gpu_pte_bit_index(prot))
                UVM_ASSERT(uvm_page_mask_empty(gpu_state->pte_bits[pte_bit]));
            else
                UVM_ASSERT(uvm_page_mask_full(gpu_state->pte_bits[pte_bit]));
        }

        if (prot != UVM_PROT_NONE) {
            resident_id = block_gpu_get_processor_to_map(block, gpu, 0);

            // block_check_resident_proximity verifies that no closer processor
            // has a resident page, so we don't need to check that all pages
            // have the same resident_id.

            // block_check_mappings_page verifies that all pages marked resident
            // are backed by populated memory.

            // The mapped processor should be fully resident and physically-
            // contiguous.
            UVM_ASSERT(uvm_page_mask_full(uvm_va_block_resident_mask_get(block, resident_id)));

            // TODO: Bug 1766172: Use 2M sysmem pages on x86
            UVM_ASSERT(resident_id != UVM_CPU_ID);
            resident_gpu_state = block->gpus[resident_id - 1];
            UVM_ASSERT(resident_gpu_state);
            UVM_ASSERT(uvm_gpu_chunk_get_size(resident_gpu_state->chunks[0]) == UVM_CHUNK_SIZE_2M);
        }
    }
    else if (!bitmap_empty(gpu_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK)) {
        UVM_ASSERT(gpu_state->page_table_range_big.table);
        UVM_ASSERT(!gpu_state->force_4k_ptes);
        UVM_ASSERT(num_big_pages > 0);
        UVM_ASSERT(gpu_state->initialized_big);

        for (big_page_index = 0; big_page_index < num_big_pages; big_page_index++) {
            big_region = uvm_va_block_big_page_region(block, big_page_index, big_page_size);

            if (!test_bit(big_page_index, gpu_state->big_ptes)) {
                // If there are valid mappings but this isn't a big PTE, the
                // mapping must be using the 4k PTEs.
                if (!uvm_page_mask_region_empty(gpu_state->pte_bits[UVM_PTE_BITS_GPU_READ], big_region))
                    UVM_ASSERT(gpu_state->page_table_range_4k.table);
                continue;
            }

            prot = block_page_prot_gpu(block, gpu, big_region.first);

            // All page permissions match
            for (pte_bit = 0; pte_bit < UVM_PTE_BITS_GPU_MAX; pte_bit++) {
                if (prot == UVM_PROT_NONE || pte_bit > get_gpu_pte_bit_index(prot))
                    UVM_ASSERT(uvm_page_mask_region_empty(gpu_state->pte_bits[pte_bit], big_region));
                else
                    UVM_ASSERT(uvm_page_mask_region_full(gpu_state->pte_bits[pte_bit], big_region));
            }

            if (prot != UVM_PROT_NONE) {
                resident_id = block_gpu_get_processor_to_map(block, gpu, big_region.first);

                // The mapped processor should be fully resident and physically-
                // contiguous. Exception: UVM-Lite GPUs always map the preferred
                // location even if the memory is resident elsewhere. Skip the
                // residency check but still verify contiguity.
                if (!uvm_processor_mask_test(&block->va_range->uvm_lite_gpus, gpu->id)) {
                    UVM_ASSERT(uvm_page_mask_region_full(uvm_va_block_resident_mask_get(block, resident_id),
                                                         big_region));
                }

                // TODO: Bug 1668859: Big pages can be used when mapping sysmem
                //       if PAGE_SIZE >= big_page_size and the GPU supports it
                //       (Pascal+).
                UVM_ASSERT(resident_id != UVM_CPU_ID);

                resident_gpu = uvm_gpu_get(resident_id);
                resident_gpu_state = block->gpus[resident_id - 1];
                UVM_ASSERT(resident_gpu_state);

                chunk_index = block_gpu_chunk_index(block, resident_gpu, big_region.first, NULL);
                chunk = resident_gpu_state->chunks[chunk_index];
                UVM_ASSERT(chunk);
                chunk_region = block_gpu_chunk_region(block, uvm_gpu_chunk_get_size(chunk), big_region.first);
                UVM_ASSERT(uvm_va_block_region_contains_region(chunk_region, big_region));
            }
        }
    }

    return true;
}

static bool block_check_mappings_swizzling(uvm_va_block_t *block, uvm_gpu_t *gpu)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    NvU32 big_page_size;
    size_t num_big_pages, page_index, big_page_index, chunk_index;
    uvm_va_block_region_t big_region;
    uvm_processor_id_t resident_id;
    uvm_gpu_t *resident_gpu;
    uvm_gpu_chunk_t *chunk;
    const unsigned long *map_mask = uvm_va_block_map_mask_get(block, gpu->id);

    if (!gpu->big_page.swizzling) {
        UVM_ASSERT(bitmap_empty(gpu_state->big_pages_swizzled, MAX_BIG_PAGES_PER_UVM_VA_BLOCK));
        return true;
    }

    big_page_size = uvm_va_block_gpu_big_page_size(block, gpu);
    num_big_pages = uvm_va_block_num_big_pages(block, big_page_size);

    if (num_big_pages == 0)
        UVM_ASSERT(bitmap_empty(gpu_state->big_pages_swizzled, MAX_BIG_PAGES_PER_UVM_VA_BLOCK));

    for (big_page_index = 0; big_page_index < num_big_pages; big_page_index++) {
        big_region = uvm_va_block_big_page_region(block, big_page_index, big_page_size);

        // If this GPU has its big page swizzled, then it must be populated
        // (though not necessarily mapped by anyone nor resident).
        if (test_bit(big_page_index, gpu_state->big_pages_swizzled)) {
            chunk_index = block_gpu_chunk_index(block, gpu, big_region.first, NULL);
            chunk = gpu_state->chunks[chunk_index];
            UVM_ASSERT(chunk);
            UVM_ASSERT(uvm_gpu_chunk_get_size(chunk) >= big_page_size);
        }

        // Now check the big pages which this GPU maps. These may point to peer
        // GPUs, so we have to check the resident location's big_pages_swizzled
        // mask.

        if (!test_bit(big_page_index, gpu_state->big_ptes)) {
            // If there are valid mappings but this isn't a big PTE, the
            // resident location must not be swizzled.
            for_each_va_block_page_in_mask(page_index, map_mask, big_region) {
                resident_id = block_gpu_get_processor_to_map(block, gpu, page_index);
                if (resident_id != UVM_CPU_ID) {
                    resident_gpu = uvm_gpu_get(resident_id);

                    // The resident GPU must swizzle if the mapping GPU does
                    UVM_ASSERT(resident_gpu->big_page.swizzling);

                    // And they must match big page sizes so we can use big page
                    // regions interchangeably. We enforce this at peer-enable
                    // time.
                    UVM_ASSERT(uvm_va_block_gpu_big_page_size(block, resident_gpu) == big_page_size);

                    UVM_ASSERT(!test_bit(big_page_index, block->gpus[resident_id - 1]->big_pages_swizzled));
                }
            }
        }
        else if (test_bit(big_region.first, map_mask)) {
            // If this big PTE is valid, the resident GPU must swizzle
            resident_id = block_gpu_get_processor_to_map(block, gpu, big_region.first);

            // GPUs which support swizzling can't map sysmem with big pages
            UVM_ASSERT(resident_id != UVM_CPU_ID);

            resident_gpu = uvm_gpu_get(resident_id);
            UVM_ASSERT(resident_gpu->big_page.swizzling);
            UVM_ASSERT(uvm_va_block_gpu_big_page_size(block, resident_gpu) == big_page_size);

            UVM_ASSERT(test_bit(big_page_index, block->gpus[resident_id - 1]->big_pages_swizzled));
        }
    }

    return true;
}

static bool block_check_mappings(uvm_va_block_t *block)
{
    size_t page_index;
    uvm_processor_id_t id;
    const unsigned long *resident_mask, *map_mask;

    // Verify the master masks, since block_check_mappings_page relies on them
    for (id = 0; id < UVM8_MAX_PROCESSORS; id++) {
        if (id != UVM_CPU_ID && !block->gpus[id - 1]) {
            UVM_ASSERT(!uvm_processor_mask_test(&block->resident, id));
            UVM_ASSERT(!uvm_processor_mask_test(&block->mapped, id));
            continue;
        }

        resident_mask = uvm_va_block_resident_mask_get(block, id);
        UVM_ASSERT(uvm_processor_mask_test(&block->resident, id) == !uvm_page_mask_empty(resident_mask));

        map_mask = uvm_va_block_map_mask_get(block, id);
        UVM_ASSERT(uvm_processor_mask_test(&block->mapped, id) == !uvm_page_mask_empty(map_mask));
    }

    // Check that every page has coherent mappings
    for (page_index = 0; page_index < uvm_va_block_num_cpu_pages(block); page_index++)
        block_check_mappings_page(block, page_index);

    for_each_gpu_id(id) {
        if (block->gpus[id - 1]) {
            // Check big and/or 2M PTE state
            block_check_mappings_ptes(block, uvm_gpu_get(id));

            block_check_mappings_swizzling(block, uvm_gpu_get(id));
        }
    }

    return true;
}

// See the comments on uvm_va_block_unmap
static void uvm_va_block_unmap_cpu(uvm_va_block_t *block, uvm_va_block_region_t region, const unsigned long *unmap_pages)
{
    uvm_va_range_t *va_range = block->va_range;
    uvm_pte_bits_cpu_t pte_bit;
    bool unmapped_something = false;
    uvm_va_block_region_t subregion;

    // Early-out if nothing in the region is mapped
    if (!block_has_valid_mapping_cpu(block, region))
        return;

    for_each_va_block_subregion_in_mask(subregion, unmap_pages, region) {
        if (!block_has_valid_mapping_cpu(block, subregion))
            continue;

        unmap_mapping_range(&va_range->va_space->mapping,
                            uvm_va_block_region_start(block, subregion),
                            uvm_va_block_region_size(subregion), 1);

        for (pte_bit = 0; pte_bit < UVM_PTE_BITS_CPU_MAX; pte_bit++)
            uvm_page_mask_region_clear(block->cpu.pte_bits[pte_bit], subregion);

        unmapped_something = true;
    }

    if (!unmapped_something)
        return;

    // Check whether the block has any more mappings
    if (uvm_page_mask_empty(block->cpu.pte_bits[UVM_PTE_BITS_CPU_READ])) {
        UVM_ASSERT(uvm_page_mask_empty(block->cpu.pte_bits[UVM_PTE_BITS_CPU_WRITE]));
        uvm_processor_mask_clear(&block->mapped, UVM_CPU_ID);
    }

    UVM_ASSERT(block_check_mappings(block));
}

// Given a mask of mapped pages, returns true if any of the pages in the mask
// are mapped remotely by the given GPU.
static bool block_has_remote_mapping_gpu(uvm_va_block_t *block,
                                         uvm_va_block_context_t *block_context,
                                         uvm_gpu_id_t gpu_id,
                                         const unsigned long *mapped_pages)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu_id - 1];
    uvm_va_range_t *va_range = block->va_range;

    if (!gpu_state)
        return false;

    // The caller must ensure that all pages of the input mask are really mapped
    UVM_ASSERT(uvm_page_mask_subset(mapped_pages, gpu_state->pte_bits[UVM_PTE_BITS_GPU_READ]));

    // UVM-Lite GPUs map the preferred location if it's accessible, regardless
    // of the resident location.
    if (uvm_processor_mask_test(&va_range->uvm_lite_gpus, gpu_id)) {
        if (uvm_page_mask_empty(mapped_pages))
            return false;
        return va_range->preferred_location != gpu_id;
    }

    // Remote pages are pages which are mapped but not resident locally
    return uvm_page_mask_andnot(block_context->scratch_page_mask, mapped_pages, gpu_state->resident);
}

// Writes pte_clear_val to the 4k PTEs covered by clear_page_mask. If
// clear_page_mask is NULL, all 4k PTEs in the {block, gpu} are written.
//
// If tlb_batch is provided, the 4k PTEs written are added to the batch. The
// caller is responsible for ending the TLB batch with the appropriate membar.
static void block_gpu_pte_clear_4k(uvm_va_block_t *block,
                                   uvm_gpu_t *gpu,
                                   const unsigned long *clear_page_mask,
                                   NvU64 pte_clear_val,
                                   uvm_pte_batch_t *pte_batch,
                                   uvm_tlb_batch_t *tlb_batch)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_page_tree_t *tree = &uvm_va_block_get_gpu_va_space(block, gpu)->page_tables;
    uvm_gpu_phys_address_t pte_addr;
    NvU32 pte_size = uvm_mmu_pte_size(tree, UVM_PAGE_SIZE_4K);
    uvm_va_block_region_t region = uvm_va_block_region_from_block(block);
    uvm_va_block_region_t subregion;
    size_t num_ptes, ptes_per_page = PAGE_SIZE / UVM_PAGE_SIZE_4K;

    for_each_va_block_subregion_in_mask(subregion, clear_page_mask, region) {
        num_ptes = uvm_va_block_region_num_pages(subregion) * ptes_per_page;

        pte_addr = uvm_page_table_range_entry_address(tree,
                                                      &gpu_state->page_table_range_4k,
                                                      subregion.first * ptes_per_page);

        uvm_pte_batch_clear_ptes(pte_batch, pte_addr, pte_clear_val, pte_size, num_ptes);

        if (tlb_batch) {
            uvm_tlb_batch_invalidate(tlb_batch,
                                     uvm_va_block_region_start(block, subregion),
                                     uvm_va_block_region_size(subregion),
                                     UVM_PAGE_SIZE_4K,
                                     UVM_MEMBAR_NONE);
        }
    }
}

// Writes the 4k PTEs covered by write_page_mask using memory from resident_id
// with new_prot permissions. new_prot must not be UVM_PROT_NONE: use
// block_gpu_pte_clear_4k instead.
//
// If write_page_mask is NULL, all 4k PTEs in the {block, gpu} are written.
//
// If tlb_batch is provided, the 4k PTEs written are added to the batch. The
// caller is responsible for ending the TLB batch with the appropriate membar.
static void block_gpu_pte_write_4k(uvm_va_block_t *block,
                                   uvm_gpu_t *gpu,
                                   uvm_processor_id_t resident_id,
                                   uvm_prot_t new_prot,
                                   const unsigned long *write_page_mask,
                                   uvm_pte_batch_t *pte_batch,
                                   uvm_tlb_batch_t *tlb_batch)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_page_tree_t *tree = &uvm_va_block_get_gpu_va_space(block, gpu)->page_tables;
    uvm_gpu_phys_address_t pte_addr, page_addr;
    NvU32 pte_size = uvm_mmu_pte_size(tree, UVM_PAGE_SIZE_4K);
    uvm_va_block_region_t region = uvm_va_block_region_from_block(block);
    size_t page_index, i, ptes_per_page = PAGE_SIZE / UVM_PAGE_SIZE_4K;
    NvU64 page_virt_addr, pte_val;
    bool is_vol;

    UVM_ASSERT(new_prot != UVM_PROT_NONE);
    UVM_ASSERT(resident_id != UVM8_MAX_PROCESSORS);

    for_each_va_block_page_in_mask(page_index, write_page_mask, region) {
        // Assume that this mapping will be used to write to the page
        if (new_prot > UVM_PROT_READ_ONLY && resident_id == UVM_CPU_ID)
            SetPageDirty(block->cpu.pages[page_index]);

        // Allow L2 to cache only local memory
        is_vol = (resident_id != gpu->id);

        // TODO: Bug 1766424: Detect when we're mapping contiguous regions to
        //       avoid re-computing the address each iteration.
        page_addr = block_phys_page_address(block, block_phys_page(resident_id, page_index), gpu);

        pte_addr = uvm_page_table_range_entry_address(tree,
                                                      &gpu_state->page_table_range_4k,
                                                      page_index * ptes_per_page);

        // Handle PAGE_SIZE > GPU PTE size
        for (i = 0; i < ptes_per_page; i++) {
            pte_val = tree->hal->make_pte(page_addr.aperture, page_addr.address, new_prot, is_vol, UVM_PAGE_SIZE_4K);
            uvm_pte_batch_write_pte(pte_batch, pte_addr, pte_val, pte_size);
            page_addr.address += UVM_PAGE_SIZE_4K;
            pte_addr.address += pte_size;
        }

        if (tlb_batch) {
            page_virt_addr = block->start + page_index * PAGE_SIZE;
            uvm_tlb_batch_invalidate(tlb_batch, page_virt_addr, PAGE_SIZE, UVM_PAGE_SIZE_4K, UVM_MEMBAR_NONE);
        }
    }
}

// Writes all 4k PTEs under the big PTE regions described by big_ptes_covered.
// This is used to initialize the 4k PTEs when splitting 2M and big PTEs. It
// only writes 4k PTEs, not big PTEs.
//
// For those 4k PTEs, new_pages_mask indicates which ones should inherit the
// mapping from the corresponding big page (0) and which ones should be written
// using memory from resident_id and new_prot (1). Unlike the other pte_write
// functions, new_prot may be UVM_PROT_NONE.
//
// If resident_id is UVM8_MAX_PROCESSORS, this function looks up the resident ID
// which should inherit the current permissions. new_prot must be UVM_PROT_NONE
// in this case.
//
// new_pages_mask must not be NULL.
//
// No TLB invalidates are required since we've set up the lower PTEs to never be
// cached by the GPU's MMU when covered by larger PTEs.
static void block_gpu_pte_big_split_write_4k(uvm_va_block_t *block,
                                             uvm_va_block_context_t *block_context,
                                             uvm_gpu_t *gpu,
                                             uvm_processor_id_t resident_id,
                                             uvm_prot_t new_prot,
                                             const unsigned long *big_ptes_covered,
                                             const unsigned long *new_pages_mask,
                                             uvm_pte_batch_t *pte_batch)
{
    uvm_va_block_region_t big_region;
    size_t big_page_index;
    uvm_processor_id_t curr_resident_id;
    uvm_prot_t curr_prot;
    NvU32 big_page_size = uvm_va_block_gpu_big_page_size(block, gpu);

    if (resident_id == UVM8_MAX_PROCESSORS)
        UVM_ASSERT(new_prot == UVM_PROT_NONE);

    for_each_set_bit(big_page_index, big_ptes_covered, MAX_BIG_PAGES_PER_UVM_VA_BLOCK) {
        big_region = uvm_va_block_big_page_region(block, big_page_index, big_page_size);

        curr_prot = block_page_prot_gpu(block, gpu, big_region.first);

        // The unmap path doesn't know the current residency ahead of time, so
        // we have to look it up.
        if (resident_id == UVM8_MAX_PROCESSORS) {
            curr_resident_id = block_gpu_get_processor_to_map(block, gpu, big_region.first);
        }
        else {
            // Check that we aren't changing the aperture of the existing
            // mappings. It could be legal in some cases (switching from {RO, A}
            // to {RO, B} for example) but we'd need to issue TLB membars.
            if (curr_prot != UVM_PROT_NONE)
                UVM_ASSERT(resident_id == block_gpu_get_processor_to_map(block, gpu, big_region.first));
            curr_resident_id = resident_id;
        }

        // pages in new_pages_mask under this big page get new_prot
        uvm_page_mask_zero(block_context->scratch_page_mask);
        uvm_page_mask_region_fill(block_context->scratch_page_mask, big_region);
        if (uvm_page_mask_and(block_context->scratch_page_mask, block_context->scratch_page_mask, new_pages_mask)) {
            if (new_prot == UVM_PROT_NONE) {
                block_gpu_pte_clear_4k(block, gpu, block_context->scratch_page_mask, 0, pte_batch, NULL);
            }
            else {
                block_gpu_pte_write_4k(block,
                                       gpu,
                                       curr_resident_id,
                                       new_prot,
                                       block_context->scratch_page_mask,
                                       pte_batch,
                                       NULL);
            }
        }

        // All other pages under this big page inherit curr_prot
        uvm_page_mask_zero(block_context->scratch_page_mask);
        uvm_page_mask_region_fill(block_context->scratch_page_mask, big_region);
        if (uvm_page_mask_andnot(block_context->scratch_page_mask, block_context->scratch_page_mask, new_pages_mask)) {
            if (curr_prot == UVM_PROT_NONE) {
                block_gpu_pte_clear_4k(block, gpu, block_context->scratch_page_mask, 0, pte_batch, NULL);
            }
            else {
                block_gpu_pte_write_4k(block,
                                       gpu,
                                       curr_resident_id,
                                       curr_prot,
                                       block_context->scratch_page_mask,
                                       pte_batch,
                                       NULL);
            }
        }
    }
}

// Writes pte_clear_val to the big PTEs in big_ptes_mask. If big_ptes_mask is
// NULL, all big PTEs in the {block, gpu} are cleared.
//
// If tlb_batch is provided, the big PTEs written are added to the batch. The
// caller is responsible for ending the TLB batch with the appropriate membar.
static void block_gpu_pte_clear_big(uvm_va_block_t *block,
                                    uvm_gpu_t *gpu,
                                    const unsigned long *big_ptes_mask,
                                    NvU64 pte_clear_val,
                                    uvm_pte_batch_t *pte_batch,
                                    uvm_tlb_batch_t *tlb_batch)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_gpu_va_space_t *gpu_va_space = uvm_va_block_get_gpu_va_space(block, gpu);
    NvU32 big_page_size = gpu_va_space->page_tables.big_page_size;
    uvm_gpu_phys_address_t pte_addr;
    NvU32 pte_size = uvm_mmu_pte_size(&gpu_va_space->page_tables, big_page_size);
    size_t big_page_index;
    DECLARE_BITMAP(big_ptes_to_clear, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    if (big_ptes_mask)
        bitmap_copy(big_ptes_to_clear, big_ptes_mask, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    else
        bitmap_set(big_ptes_to_clear, 0, uvm_va_block_num_big_pages(block, big_page_size));

    for_each_set_bit(big_page_index, big_ptes_to_clear, MAX_BIG_PAGES_PER_UVM_VA_BLOCK) {
        pte_addr = uvm_page_table_range_entry_address(&gpu_va_space->page_tables,
                                                      &gpu_state->page_table_range_big,
                                                      big_page_index);
        uvm_pte_batch_clear_ptes(pte_batch, pte_addr, pte_clear_val, pte_size, 1);

        if (tlb_batch) {
            uvm_tlb_batch_invalidate(tlb_batch,
                                     uvm_va_block_big_page_addr(block, big_page_index, big_page_size),
                                     big_page_size,
                                     big_page_size,
                                     UVM_MEMBAR_NONE);
        }
    }
}

// Writes the big PTEs in big_ptes_mask using memory from resident_id with
// new_prot permissions. new_prot must not be UVM_PROT_NONE: use
// block_gpu_pte_clear_big instead.
//
// Unlike block_gpu_pte_clear_big, big_ptes_mask must not be NULL.
//
// If tlb_batch is provided, the big PTEs written are added to the batch. The
// caller is responsible for ending the TLB batch with the appropriate membar.
static void block_gpu_pte_write_big(uvm_va_block_t *block,
                                    uvm_gpu_t *gpu,
                                    uvm_processor_id_t resident_id,
                                    uvm_prot_t new_prot,
                                    const unsigned long *big_ptes_mask,
                                    uvm_pte_batch_t *pte_batch,
                                    uvm_tlb_batch_t *tlb_batch)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_gpu_va_space_t *gpu_va_space = uvm_va_block_get_gpu_va_space(block, gpu);
    uvm_page_tree_t *tree = &gpu_va_space->page_tables;
    NvU32 big_page_size = tree->big_page_size;
    uvm_gpu_phys_address_t pte_addr, page_addr;
    uvm_va_block_region_t big_region;
    NvU32 pte_size = uvm_mmu_pte_size(tree, big_page_size);
    size_t big_page_index;
    NvU64 pte_val;
    bool is_vol;

    UVM_ASSERT(new_prot != UVM_PROT_NONE);
    UVM_ASSERT(resident_id != UVM8_MAX_PROCESSORS);
    UVM_ASSERT(big_ptes_mask);

    if (!bitmap_empty(big_ptes_mask, MAX_BIG_PAGES_PER_UVM_VA_BLOCK)) {
        UVM_ASSERT(uvm_va_block_num_big_pages(block, big_page_size) > 0);

        // TODO: Bug 1668859: Big pages can be used when mapping sysmem if
        //       PAGE_SIZE >= big_page_size and the GPU supports it (Pascal+).
        //       We'll need to dirty them.
        UVM_ASSERT(resident_id != UVM_CPU_ID);
    }

    for_each_set_bit(big_page_index, big_ptes_mask, MAX_BIG_PAGES_PER_UVM_VA_BLOCK) {
        big_region = uvm_va_block_big_page_region(block, big_page_index, big_page_size);

        // Allow L2 to cache only local memory
        is_vol = (resident_id != gpu->id);

        // TODO: Bug 1766424: Detect when we're mapping contiguous regions to
        //       avoid re-computing the address each iteration.
        page_addr = block_phys_page_address(block, block_phys_page(resident_id, big_region.first), gpu);

        pte_addr = uvm_page_table_range_entry_address(tree, &gpu_state->page_table_range_big, big_page_index);
        pte_val = tree->hal->make_pte(page_addr.aperture, page_addr.address, new_prot, is_vol, big_page_size);
        uvm_pte_batch_write_pte(pte_batch, pte_addr, pte_val, pte_size);

        if (tlb_batch) {
            uvm_tlb_batch_invalidate(tlb_batch,
                                     uvm_va_block_region_start(block, big_region),
                                     big_page_size,
                                     big_page_size,
                                     UVM_MEMBAR_NONE);
        }
    }
}

// Switches any mix of valid or invalid 4k PTEs under the big PTEs in
// big_ptes_to_merge to an unmapped big PTE. This also ends both pte_batch and
// tlb_batch in order to poison the now-unused 4k PTEs.
//
// The 4k PTEs are invalidated with the specified membar.
static void block_gpu_pte_merge_big_and_end(uvm_va_block_t *block,
                                            uvm_va_block_context_t *block_context,
                                            uvm_gpu_t *gpu,
                                            const unsigned long *big_ptes_to_merge,
                                            uvm_push_t *push,
                                            uvm_pte_batch_t *pte_batch,
                                            uvm_tlb_batch_t *tlb_batch,
                                            uvm_membar_t tlb_membar)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_page_tree_t *tree = &uvm_va_block_get_gpu_va_space(block, gpu)->page_tables;
    NvU32 big_page_size = tree->big_page_size;
    NvU64 unmapped_pte_val = tree->hal->unmapped_pte(big_page_size);
    size_t big_page_index;
    DECLARE_BITMAP(dummy_big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    UVM_ASSERT(!bitmap_empty(big_ptes_to_merge, MAX_BIG_PAGES_PER_UVM_VA_BLOCK));
    UVM_ASSERT(!bitmap_and(dummy_big_ptes, gpu_state->big_ptes, big_ptes_to_merge, MAX_BIG_PAGES_PER_UVM_VA_BLOCK));

    // We can be called with the 4k PTEs in two cases:
    // 1) 4k PTEs allocated. In this case the 4k PTEs are currently active.
    //
    // 2) 4k PTEs unallocated. In this case the GPU may not have invalid 4k PTEs
    //    active under the big PTE, depending on whether neighboring blocks
    //    caused the page tables to be allocated.
    //
    // In both cases we need to invalidate the 4k PTEs in case the GPU MMU has
    // them cached.

    // Each big PTE is currently invalid so the 4ks are active (or unallocated).
    // First make the big PTEs unmapped to disable future lookups of the 4ks
    // under it. We can't directly transition the entry from valid 4k PTEs to
    // valid big PTEs, because that could cause the GPU TLBs to cache the same
    // VA in different cache lines. That could cause memory ordering to not be
    // maintained.
    block_gpu_pte_clear_big(block, gpu, big_ptes_to_merge, unmapped_pte_val, pte_batch, tlb_batch);

    // Now invalidate the big PTEs we just wrote as well as all 4ks under them.
    // Subsequent MMU fills will stop at the now-unmapped big PTEs, so we only
    // need to invalidate the 4k PTEs without actually writing them.
    for_each_set_bit(big_page_index, big_ptes_to_merge, MAX_BIG_PAGES_PER_UVM_VA_BLOCK) {
        uvm_tlb_batch_invalidate(tlb_batch,
                                 uvm_va_block_big_page_addr(block, big_page_index, big_page_size),
                                 big_page_size,
                                 big_page_size | UVM_PAGE_SIZE_4K,
                                 UVM_MEMBAR_NONE);
    }

    // End the batches for the caller. We need to do this here in order to
    // poison the 4ks below.
    uvm_pte_batch_end(pte_batch);
    uvm_tlb_batch_end(tlb_batch, push, tlb_membar);

    // As a guard against bad PTE writes/TLB invalidates, fill the now-unused
    // PTEs with a pattern which will trigger fatal faults on access. We have to
    // do this after the TLB invalidate of the big PTEs, or the GPU might use
    // the new values.
    if (UVM_IS_DEBUG() && gpu_state->page_table_range_4k.table) {
        uvm_page_mask_init_from_big_ptes(block, gpu, block_context->scratch_page_mask, big_ptes_to_merge);
        uvm_pte_batch_begin(push, pte_batch);
        block_gpu_pte_clear_4k(block,
                               gpu,
                               block_context->scratch_page_mask,
                               tree->hal->poisoned_pte(UVM_PAGE_SIZE_4K),
                               pte_batch,
                               NULL);
        uvm_pte_batch_end(pte_batch);
    }
}

// Writes 0 (invalid) to the 2M PTE for this {block, gpu}.
//
// If tlb_batch is provided, the 2M PTE is added to the batch. The caller is
// responsible for ending the TLB batch with the appropriate membar.
static void block_gpu_pte_clear_2m(uvm_va_block_t *block,
                                   uvm_gpu_t *gpu,
                                   uvm_pte_batch_t *pte_batch,
                                   uvm_tlb_batch_t *tlb_batch)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_page_tree_t *tree = &uvm_va_block_get_gpu_va_space(block, gpu)->page_tables;
    uvm_gpu_phys_address_t pte_addr = uvm_page_table_range_entry_address(tree, &gpu_state->page_table_range_2m, 0);
    NvU32 pte_size = uvm_mmu_pte_size(tree, UVM_PAGE_SIZE_2M);

    // uvm_pte_batch_write_pte only writes the lower 8 bytes of the 16-byte PTE,
    // which would cause a problem when trying to make the entry invalid since
    // both halves must be 0. Using uvm_pte_batch_clear_ptes writes the entire
    // 16 bytes.
    uvm_pte_batch_clear_ptes(pte_batch, pte_addr, 0, pte_size, 1);

    if (tlb_batch)
        uvm_tlb_batch_invalidate(tlb_batch, block->start, UVM_PAGE_SIZE_2M, UVM_PAGE_SIZE_2M, UVM_MEMBAR_NONE);
}

// Writes the 2M PTE for {block, gpu} using memory from resident_id with
// new_prot permissions. new_prot must not be UVM_PROT_NONE: use
// block_gpu_pte_clear_2m instead.
//
// If tlb_batch is provided, the 2M PTE is added to the batch. The caller is
// responsible for ending the TLB batch with the appropriate membar.
static void block_gpu_pte_write_2m(uvm_va_block_t *block,
                                   uvm_gpu_t *gpu,
                                   uvm_processor_id_t resident_id,
                                   uvm_prot_t new_prot,
                                   uvm_pte_batch_t *pte_batch,
                                   uvm_tlb_batch_t *tlb_batch)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_page_tree_t *tree = &uvm_va_block_get_gpu_va_space(block, gpu)->page_tables;
    uvm_gpu_phys_address_t pte_addr = uvm_page_table_range_entry_address(tree, &gpu_state->page_table_range_2m, 0);
    uvm_gpu_phys_address_t page_addr;
    NvU32 pte_size = uvm_mmu_pte_size(tree, UVM_PAGE_SIZE_2M);
    NvU64 pte_val;
    bool is_vol;

    UVM_ASSERT(new_prot != UVM_PROT_NONE);
    UVM_ASSERT(resident_id != UVM8_MAX_PROCESSORS);

    // TODO: Bug 1766172: Use 2M sysmem pages on x86. Would need to dirty them.
    UVM_ASSERT(resident_id != UVM_CPU_ID);

    // Allow L2 to cache only local memory
    is_vol = (resident_id != gpu->id);

    page_addr = block_phys_page_address(block, block_phys_page(resident_id, 0), gpu);
    pte_val = tree->hal->make_pte(page_addr.aperture, page_addr.address, new_prot, is_vol, UVM_PAGE_SIZE_2M);
    uvm_pte_batch_write_pte(pte_batch, pte_addr, pte_val, pte_size);

    if (tlb_batch)
        uvm_tlb_batch_invalidate(tlb_batch, block->start, UVM_PAGE_SIZE_2M, UVM_PAGE_SIZE_2M, UVM_MEMBAR_NONE);
}

static bool block_gpu_needs_to_activate_table(uvm_va_block_t *block, uvm_gpu_t *gpu)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];

    if (!block_gpu_supports_2m(block, gpu))
        return false;

    if ((gpu_state->page_table_range_big.table && !gpu_state->activated_big) ||
        (gpu_state->page_table_range_4k.table  && !gpu_state->activated_4k))
        return true;

    return false;
}

// Only used if 2M PTEs are supported. Either transitions a 2M PTE to a PDE, or
// activates a newly-allocated page table (big or 4k) while the other is already
// active. The caller must have already written the new PTEs under the table
// with the appropriate membar.
static void block_gpu_write_pde(uvm_va_block_t *block, uvm_gpu_t *gpu, uvm_push_t *push, uvm_tlb_batch_t *tlb_batch)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_page_tree_t *tree = &uvm_va_block_get_gpu_va_space(block, gpu)->page_tables;

    if (!gpu_state->pte_is_2m)
        UVM_ASSERT(block_gpu_needs_to_activate_table(block, gpu));

    UVM_ASSERT(gpu_state->page_table_range_big.table || gpu_state->page_table_range_4k.table);

    // We always need a membar to order PDE/PTE writes with the TLB invalidate.
    // write_pde will do a MEMBAR_SYS by default.
    if (uvm_page_table_range_aperture(&gpu_state->page_table_range_2m) == UVM_APERTURE_VID)
        uvm_push_set_flag(push, UVM_PUSH_FLAG_CE_NEXT_MEMBAR_GPU);
    uvm_page_tree_write_pde(tree, &gpu_state->page_table_range_2m, push);

    gpu->host_hal->wait_for_idle(push);

    // Invalidate just the PDE
    uvm_tlb_batch_invalidate(tlb_batch, block->start, UVM_PAGE_SIZE_2M, UVM_PAGE_SIZE_2M, UVM_MEMBAR_NONE);

    if (gpu_state->page_table_range_big.table)
        gpu_state->activated_big = true;

    if (gpu_state->page_table_range_4k.table)
        gpu_state->activated_4k = true;
}

// Called to switch the 2M PTE (valid or invalid) to a PDE. The caller should
// have written all lower PTEs as appropriate into the given pte_batch already.
// This function ends the PTE batch, activates the 2M PDE, and does a TLB
// invalidate.
//
// The caller does not need to do any TLB invalidates since none of the lower
// PTEs could be cached.
static void block_gpu_pte_finish_split_2m(uvm_va_block_t *block,
                                          uvm_gpu_t *gpu,
                                          uvm_push_t *push,
                                          uvm_pte_batch_t *pte_batch,
                                          uvm_tlb_batch_t *tlb_batch,
                                          uvm_membar_t tlb_membar)
{
    uvm_page_tree_t *tree = &uvm_va_block_get_gpu_va_space(block, gpu)->page_tables;
    uvm_prot_t curr_prot = block_page_prot_gpu(block, gpu, 0);

    // Step 1: Make the 2M entry invalid. We can't directly transition from a
    //         valid 2M PTE to valid lower PTEs, because that could cause the
    //         GPU TLBs to cache the same VA in different cache lines. That
    //         could cause memory ordering to not be maintained.
    //
    //         If the 2M PTE is already invalid, no TLB invalidate is needed.

    if (curr_prot == UVM_PROT_NONE) {
        // If we aren't downgrading, then we don't need a membar.
        UVM_ASSERT(tlb_membar == UVM_MEMBAR_NONE);

        // End the batch, which pushes a membar to ensure that the caller's PTE
        // writes below 2M are observed before the PDE write we're about to do.
        uvm_pte_batch_end(pte_batch);
    }
    else {
        // The 64k and 4k PTEs can't possibly be cached since the 2M entry is
        // not yet a PDE, so we just need to invalidate this single 2M entry.
        uvm_tlb_batch_begin(tree, tlb_batch);
        block_gpu_pte_clear_2m(block, gpu, pte_batch, tlb_batch);

        // Make sure the PTE writes are observed before the TLB invalidate
        uvm_pte_batch_end(pte_batch);
        uvm_tlb_batch_end(tlb_batch, push, tlb_membar);
    }

    // Step 2: Switch the 2M entry from invalid to a PDE. This activates the
    //         smaller PTEs.
    uvm_tlb_batch_begin(tree, tlb_batch);
    block_gpu_write_pde(block, gpu, push, tlb_batch);
    uvm_tlb_batch_end(tlb_batch, push, UVM_MEMBAR_NONE);
}

// Switches any mix of valid or invalid 4k or 64k PTEs to an invalid 2M PTE.
// Any lower PTEs are invalidated with the specified membar.
static void block_gpu_pte_merge_2m(uvm_va_block_t *block,
                                   uvm_va_block_context_t *block_context,
                                   uvm_gpu_t *gpu,
                                   uvm_push_t *push,
                                   uvm_membar_t tlb_membar)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_page_tree_t *tree = &uvm_va_block_get_gpu_va_space(block, gpu)->page_tables;
    uvm_pte_batch_t *pte_batch = &block_context->mapping.pte_batch;
    uvm_tlb_batch_t *tlb_batch = &block_context->mapping.tlb_batch;
    NvU32 tlb_inval_sizes;

    UVM_ASSERT(!gpu_state->pte_is_2m);
    UVM_ASSERT(gpu_state->page_table_range_big.table || gpu_state->page_table_range_4k.table);

    // The 2M entry is currently a PDE, so first make it invalid. We can't
    // directly transition the entry from a valid PDE to a valid 2M PTE, because
    // that could cause the GPU TLBs to cache the same VA in different cache
    // lines. That could cause memory ordering to not be maintained.
    uvm_pte_batch_begin(push, pte_batch);
    block_gpu_pte_clear_2m(block, gpu, pte_batch, NULL);
    uvm_pte_batch_end(pte_batch);

    // Now invalidate both the 2M entry we just wrote as well as all lower-level
    // entries which could be cached. Subsequent MMU fills will stop at the now-
    // invalid 2M entry, so we only need to invalidate the lower PTEs without
    // actually writing them.
    tlb_inval_sizes = UVM_PAGE_SIZE_2M;
    if (gpu_state->page_table_range_big.table)
        tlb_inval_sizes |= UVM_PAGE_SIZE_64K;

    // Strictly-speaking we only need to invalidate those 4k ranges which are
    // not covered by a big pte. However, any such invalidate will require
    // enough 4k invalidates to force the TLB batching to invalidate everything
    // anyway, so just do the simpler thing.
    if (!bitmap_full(gpu_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK))
        tlb_inval_sizes |= UVM_PAGE_SIZE_4K;

    uvm_tlb_batch_begin(tree, tlb_batch);
    uvm_tlb_batch_invalidate(tlb_batch, block->start, UVM_PAGE_SIZE_2M, tlb_inval_sizes, UVM_MEMBAR_NONE);
    uvm_tlb_batch_end(tlb_batch, push, tlb_membar);

    // As a guard against bad PTE writes/TLB invalidates, fill the now-unused
    // PTEs with a pattern which will trigger fatal faults on access. We have to
    // do this after the TLB invalidate of the 2M entry, or the GPU might use
    // the new values.
    if (UVM_IS_DEBUG()) {
        uvm_pte_batch_begin(push, pte_batch);

        if (gpu_state->page_table_range_big.table) {
            block_gpu_pte_clear_big(block,
                                    gpu,
                                    NULL,
                                    tree->hal->poisoned_pte(UVM_PAGE_SIZE_64K),
                                    pte_batch,
                                    NULL);
        }

        if (gpu_state->page_table_range_4k.table) {
            block_gpu_pte_clear_4k(block,
                                   gpu,
                                   NULL,
                                   tree->hal->poisoned_pte(UVM_PAGE_SIZE_4K),
                                   pte_batch,
                                   NULL);
        }

        uvm_pte_batch_end(pte_batch);
    }
}

static uvm_membar_t block_pte_op_membar(block_pte_op_t pte_op, uvm_gpu_t *gpu, uvm_processor_id_t resident_id)
{
    // Permissions upgrades (MAP) don't need membars
    if (pte_op == BLOCK_PTE_OP_MAP)
        return UVM_MEMBAR_NONE;

    UVM_ASSERT(resident_id != UVM8_MAX_PROCESSORS);
    UVM_ASSERT(pte_op == BLOCK_PTE_OP_REVOKE);

    // Permissions downgrades always need a membar on TLB invalidate. If the
    // mapped memory was local, we only need a GPU-local membar.
    if (gpu->id == resident_id)
        return UVM_MEMBAR_GPU;

    // Otherwise, remote memory needs a sysmembar
    return UVM_MEMBAR_SYS;
}

// Write the 2M PTE for {block, gpu} to the memory on resident_id with new_prot
// permissions. If the 2M entry is currently a PDE, it is first merged into a
// PTE.
//
// new_prot must not be UVM_PROT_NONE: use block_gpu_unmap_to_2m instead.
//
// pte_op specifies whether this is a MAP or REVOKE operation, which determines
// the TLB membar required.
static void block_gpu_map_to_2m(uvm_va_block_t *block,
                                uvm_va_block_context_t *block_context,
                                uvm_gpu_t *gpu,
                                uvm_processor_id_t resident_id,
                                uvm_prot_t new_prot,
                                uvm_push_t *push,
                                block_pte_op_t pte_op)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_gpu_va_space_t *gpu_va_space = uvm_va_block_get_gpu_va_space(block, gpu);
    uvm_pte_batch_t *pte_batch = &block_context->mapping.pte_batch;
    uvm_tlb_batch_t *tlb_batch = &block_context->mapping.tlb_batch;
    uvm_membar_t tlb_membar;

    UVM_ASSERT(new_prot != UVM_PROT_NONE);

    // If we have a mix of big and 4k PTEs, we have to first merge them to an
    // invalid 2M PTE.
    if (!gpu_state->pte_is_2m) {
        block_gpu_pte_merge_2m(block, block_context, gpu, push, UVM_MEMBAR_NONE);

        gpu_state->pte_is_2m = true;
        bitmap_zero(gpu_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    }

    // Write the new permissions
    uvm_pte_batch_begin(push, pte_batch);
    uvm_tlb_batch_begin(&gpu_va_space->page_tables, tlb_batch);

    block_gpu_pte_write_2m(block, gpu, resident_id, new_prot, pte_batch, tlb_batch);

    uvm_pte_batch_end(pte_batch);

    tlb_membar = block_pte_op_membar(pte_op, gpu, resident_id);
    uvm_tlb_batch_end(tlb_batch, push, tlb_membar);
}

// Combination split + map operation, called when only part of a 2M PTE mapping
// is being changed. This splits an existing valid or invalid 2M PTE into the
// mix of big and 4k PTEs described by block_context->mapping.new_pte_state.
//
// The PTEs covering the pages in pages_to_write are written to the memory on
// resident_id with new_prot permissions. new_prot must not be UVM_PROT_NONE.
//
// The PTEs covering the pages not set in pages_to_write inherit the mapping of
// the current 2M PTE. If the current mapping is valid, it must target
// resident_id.
//
// pte_op specifies whether this is a MAP or REVOKE operation, which determines
// the TLB membar required.
static void block_gpu_map_split_2m(uvm_va_block_t *block,
                                   uvm_va_block_context_t *block_context,
                                   uvm_gpu_t *gpu,
                                   uvm_processor_id_t resident_id,
                                   const unsigned long *pages_to_write,
                                   uvm_prot_t new_prot,
                                   uvm_push_t *push,
                                   block_pte_op_t pte_op)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_page_tree_t *tree = &uvm_va_block_get_gpu_va_space(block, gpu)->page_tables;
    uvm_va_block_new_pte_state_t *new_pte_state = &block_context->mapping.new_pte_state;
    uvm_pte_batch_t *pte_batch = &block_context->mapping.pte_batch;
    uvm_tlb_batch_t *tlb_batch = &block_context->mapping.tlb_batch;
    uvm_prot_t curr_prot = block_page_prot_gpu(block, gpu, 0);
    uvm_membar_t tlb_membar;
    DECLARE_BITMAP(big_ptes_split, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    DECLARE_BITMAP(big_ptes_inherit, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    DECLARE_BITMAP(big_ptes_new_prot, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    UVM_ASSERT(gpu_state->pte_is_2m);

    if (!gpu_state->page_table_range_4k.table)
        UVM_ASSERT(bitmap_full(new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK));

    uvm_pte_batch_begin(push, pte_batch);

    // Since the 2M entry is active as a PTE, the GPU MMU can't fetch entries
    // from the lower levels. This means we don't need to issue a TLB invalidate
    // when writing those levels.

    // Cases to handle:
    // 1) Big PTEs which inherit curr_prot
    // 2) Big PTEs which get new_prot
    // 3) Big PTEs which are split to 4k
    //    a) 4k PTEs which inherit curr_prot under the split big PTEs
    //    b) 4k PTEs which get new_prot under the split big PTEs

    // Compute the big PTEs which will need to be split to 4k, if any.
    bitmap_complement(big_ptes_split, new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    if (gpu_state->page_table_range_big.table) {
        // Case 1: Write the big PTEs which will inherit the 2M permissions, if
        // any. These are the big PTEs which are unchanged (uncovered) by the
        // operation.
        bitmap_andnot(big_ptes_inherit,
                      new_pte_state->big_ptes,
                      new_pte_state->big_ptes_covered,
                      MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

        if (curr_prot == UVM_PROT_NONE) {
            block_gpu_pte_clear_big(block,
                                    gpu,
                                    big_ptes_inherit,
                                    tree->hal->unmapped_pte(UVM_PAGE_SIZE_64K),
                                    pte_batch,
                                    NULL);
        }
        else {
            block_gpu_pte_write_big(block, gpu, resident_id, curr_prot, big_ptes_inherit, pte_batch, NULL);
        }

        // Case 2: Write the new big PTEs
        bitmap_and(big_ptes_new_prot,
                   new_pte_state->big_ptes,
                   new_pte_state->big_ptes_covered,
                   MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
        block_gpu_pte_write_big(block, gpu, resident_id, new_prot, big_ptes_new_prot, pte_batch, NULL);

        // Case 3: Write the big PTEs which cover 4k PTEs
        block_gpu_pte_clear_big(block, gpu, big_ptes_split, 0, pte_batch, NULL);

        // We just wrote all possible big PTEs, so mark them as initialized
        gpu_state->initialized_big = true;
    }
    else {
        UVM_ASSERT(bitmap_empty(new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK));
    }

    // Cases 3a and 3b: Write all 4k PTEs under all now-split big PTEs
    block_gpu_pte_big_split_write_4k(block,
                                     block_context,
                                     gpu,
                                     resident_id,
                                     new_prot,
                                     big_ptes_split,
                                     pages_to_write,
                                     pte_batch);

    // Activate the 2M PDE. This ends the pte_batch and issues a single TLB
    // invalidate for the 2M entry.
    tlb_membar = block_pte_op_membar(pte_op, gpu, resident_id);
    block_gpu_pte_finish_split_2m(block, gpu, push, pte_batch, tlb_batch, tlb_membar);

    gpu_state->pte_is_2m = false;
    bitmap_copy(gpu_state->big_ptes, new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
}

// Split the existing 2M PTE into big and 4k PTEs. No permissions are changed.
//
// new_big_ptes specifies which PTEs should be big. NULL means all PTEs should
// be 4k.
static void block_gpu_split_2m(uvm_va_block_t *block,
                               uvm_va_block_context_t *block_context,
                               uvm_gpu_t *gpu,
                               const unsigned long *new_big_ptes,
                               uvm_push_t *push)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_page_tree_t *tree = &uvm_va_block_get_gpu_va_space(block, gpu)->page_tables;
    uvm_pte_batch_t *pte_batch = &block_context->mapping.pte_batch;
    uvm_tlb_batch_t *tlb_batch = &block_context->mapping.tlb_batch;
    uvm_prot_t curr_prot = block_page_prot_gpu(block, gpu, 0);
    DECLARE_BITMAP(new_big_ptes_local, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    DECLARE_BITMAP(big_ptes_split, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    NvU64 unmapped_pte_val;
    uvm_processor_id_t curr_residency;

    UVM_ASSERT(gpu_state->pte_is_2m);

    if (new_big_ptes)
        bitmap_copy(new_big_ptes_local, new_big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    else
        bitmap_zero(new_big_ptes_local, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    if (!bitmap_empty(new_big_ptes_local, MAX_BIG_PAGES_PER_UVM_VA_BLOCK))
        UVM_ASSERT(gpu_state->page_table_range_big.table);

    // We're splitting from 2M to big only, so we'll be writing all big PTEs
    if (gpu_state->page_table_range_big.table)
        gpu_state->initialized_big = true;

    // Cases to handle:
    // 1) Big PTEs which inherit curr_prot
    // 2) Big PTEs which are split to 4k
    //    a) 4k PTEs inherit curr_prot under the split big PTEs

    // big_ptes_split will cover the 4k regions
    bitmap_complement(big_ptes_split, new_big_ptes_local, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    uvm_page_mask_init_from_big_ptes(block, gpu, block_context->scratch_page_mask, big_ptes_split);

    uvm_pte_batch_begin(push, pte_batch);

    // Since the 2M entry is active as a PTE, the GPU MMU can't fetch entries
    // from the lower levels. This means we don't need to issue a TLB invalidate
    // when writing those levels.

    if (curr_prot == UVM_PROT_NONE) {
        unmapped_pte_val = tree->hal->unmapped_pte(tree->big_page_size);

        // Case 2a: Clear the 4k PTEs under big_ptes_split
        block_gpu_pte_clear_4k(block, gpu, block_context->scratch_page_mask, 0, pte_batch, NULL);

        // Case 1: Make the remaining big PTEs unmapped
        block_gpu_pte_clear_big(block, gpu, new_big_ptes_local, unmapped_pte_val, pte_batch, NULL);
    }
    else {
        curr_residency = block_gpu_get_processor_to_map(block, gpu, 0);

        // Case 2a: Write the new 4k PTEs under big_ptes_split
        block_gpu_pte_write_4k(block,
                               gpu,
                               curr_residency,
                               curr_prot,
                               block_context->scratch_page_mask,
                               pte_batch,
                               NULL);

        // Case 1: Write the new big PTEs
        block_gpu_pte_write_big(block, gpu, curr_residency, curr_prot, new_big_ptes_local, pte_batch, NULL);
    }

    // Case 2: Make big_ptes_split invalid to activate the 4k PTEs
    if (gpu_state->page_table_range_big.table)
        block_gpu_pte_clear_big(block, gpu, big_ptes_split, 0, pte_batch, NULL);

    // Activate the 2M PDE. This ends the pte_batch and issues a single TLB
    // invalidate for the 2M entry. No membar is necessary since we aren't
    // changing permissions.
    block_gpu_pte_finish_split_2m(block, gpu, push, pte_batch, tlb_batch, UVM_MEMBAR_NONE);

    gpu_state->pte_is_2m = false;
    bitmap_copy(gpu_state->big_ptes, new_big_ptes_local, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
}

// Split the big PTEs in big_ptes_to_split into 4k PTEs. No permissions are
// changed.
//
// big_ptes_to_split must not be NULL.
static void block_gpu_split_big(uvm_va_block_t *block,
                                uvm_va_block_context_t *block_context,
                                uvm_gpu_t *gpu,
                                const unsigned long *big_ptes_to_split,
                                uvm_push_t *push)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_page_tree_t *tree = &uvm_va_block_get_gpu_va_space(block, gpu)->page_tables;
    uvm_pte_batch_t *pte_batch = &block_context->mapping.pte_batch;
    uvm_tlb_batch_t *tlb_batch = &block_context->mapping.tlb_batch;
    NvU32 big_page_size = tree->big_page_size;
    uvm_va_block_region_t big_region;
    uvm_processor_id_t resident_id;
    size_t big_page_index;
    uvm_prot_t curr_prot;
    DECLARE_BITMAP(big_ptes_valid, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    UVM_ASSERT(!gpu_state->pte_is_2m);
    UVM_ASSERT(bitmap_subset(big_ptes_to_split, gpu_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK));
    UVM_ASSERT(!bitmap_empty(big_ptes_to_split, MAX_BIG_PAGES_PER_UVM_VA_BLOCK));

    uvm_pte_batch_begin(push, pte_batch);
    uvm_tlb_batch_begin(tree, tlb_batch);

    // Write all 4k PTEs under all big PTEs which are being split. We'll make
    // the big PTEs inactive below after flushing these writes. No TLB
    // invalidate is needed since the big PTE is active.
    bitmap_zero(big_ptes_valid, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    for_each_set_bit(big_page_index, big_ptes_to_split, MAX_BIG_PAGES_PER_UVM_VA_BLOCK) {
        big_region = uvm_va_block_big_page_region(block, big_page_index, big_page_size);
        curr_prot = block_page_prot_gpu(block, gpu, big_region.first);

        uvm_page_mask_zero(block_context->scratch_page_mask);
        uvm_page_mask_region_fill(block_context->scratch_page_mask, big_region);
        if (curr_prot == UVM_PROT_NONE) {
            block_gpu_pte_clear_4k(block, gpu, block_context->scratch_page_mask, 0, pte_batch, NULL);
        }
        else {
            __set_bit(big_page_index, big_ptes_valid);

            resident_id = block_gpu_get_processor_to_map(block, gpu, big_region.first);

            // We don't handle deswizzling here
            if (resident_id != UVM_CPU_ID)
                UVM_ASSERT(!test_bit(big_page_index, block->gpus[resident_id - 1]->big_pages_swizzled));

            block_gpu_pte_write_4k(block,
                                   gpu,
                                   resident_id,
                                   curr_prot,
                                   block_context->scratch_page_mask,
                                   pte_batch,
                                   NULL);
        }
    }

    // Unmap the big PTEs which are valid and are being split to 4k. We can't
    // directly transition from a valid big PTE to valid lower PTEs, because
    // that could cause the GPU TLBs to cache the same VA in different cache
    // lines. That could cause memory ordering to not be maintained.
    block_gpu_pte_clear_big(block, gpu, big_ptes_valid, tree->hal->unmapped_pte(big_page_size), pte_batch, tlb_batch);

    // End the batches. We have to commit the membars and TLB invalidates
    // before we finish splitting formerly-big PTEs. No membar is necessary
    // since we aren't changing permissions.
    uvm_pte_batch_end(pte_batch);
    uvm_tlb_batch_end(tlb_batch, push, UVM_MEMBAR_NONE);

    // Finish the split by switching the big PTEs from unmapped to invalid. This
    // causes the GPU MMU to start reading the 4k PTEs instead of stopping at
    // the unmapped big PTEs.
    uvm_pte_batch_begin(push, pte_batch);
    uvm_tlb_batch_begin(tree, tlb_batch);

    block_gpu_pte_clear_big(block, gpu, big_ptes_to_split, 0, pte_batch, tlb_batch);

    uvm_pte_batch_end(pte_batch);

    // Finally, activate the page tables if they're inactive
    if (block_gpu_needs_to_activate_table(block, gpu))
        block_gpu_write_pde(block, gpu, push, tlb_batch);

    uvm_tlb_batch_end(tlb_batch, push, UVM_MEMBAR_NONE);

    bitmap_andnot(gpu_state->big_ptes, gpu_state->big_ptes, big_ptes_to_split, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
}

// Changes permissions on some pre-existing mix of big and 4k PTEs into some
// other mix of big and 4k PTEs, as described by
// block_context->mapping.new_pte_state.
//
// The PTEs covering the pages in pages_to_write are written to the memory on
// resident_id with new_prot permissions. new_prot must not be UVM_PROT_NONE.
//
// pte_op specifies whether this is a MAP or REVOKE operation, which determines
// the TLB membar required.
static void block_gpu_map_big_and_4k(uvm_va_block_t *block,
                                     uvm_va_block_context_t *block_context,
                                     uvm_gpu_t *gpu,
                                     uvm_processor_id_t resident_id,
                                     const unsigned long *pages_to_write,
                                     uvm_prot_t new_prot,
                                     uvm_push_t *push,
                                     block_pte_op_t pte_op)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_page_tree_t *tree = &uvm_va_block_get_gpu_va_space(block, gpu)->page_tables;
    uvm_va_block_new_pte_state_t *new_pte_state = &block_context->mapping.new_pte_state;
    uvm_pte_batch_t *pte_batch = &block_context->mapping.pte_batch;
    uvm_tlb_batch_t *tlb_batch = &block_context->mapping.tlb_batch;
    DECLARE_BITMAP(big_ptes_split, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    DECLARE_BITMAP(big_ptes_before_or_after, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    DECLARE_BITMAP(big_ptes_merge, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    DECLARE_BITMAP(big_ptes_mask, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    uvm_va_block_region_t big_region;
    size_t big_page_index;
    NvU32 big_page_size = tree->big_page_size;
    uvm_membar_t tlb_membar = block_pte_op_membar(pte_op, gpu, resident_id);

    UVM_ASSERT(!gpu_state->pte_is_2m);

    uvm_pte_batch_begin(push, pte_batch);
    uvm_tlb_batch_begin(tree, tlb_batch);

    // All of these cases might be perfomed in the same call:
    // 1) Split currently-big PTEs to 4k
    //    a) Write new 4k PTEs which inherit curr_prot under the split big PTEs
    //    b) Write new 4k PTEs which get new_prot under the split big PTEs
    // 2) Merge currently-4k PTEs to big with new_prot
    // 3) Write currently-big PTEs which wholly get new_prot
    // 4) Write currently-4k PTEs which get new_prot
    // 5) Initialize big PTEs which are not covered by this operation

    // Cases 1a and 1b: Write all 4k PTEs under all currently-big PTEs which are
    // being split. We'll make the big PTEs inactive below after flushing these
    // writes. No TLB invalidate is needed since the big PTE is active.
    //
    // Mask computation: big_before && !big_after
    bitmap_andnot(big_ptes_split, gpu_state->big_ptes, new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    block_gpu_pte_big_split_write_4k(block,
                                     block_context,
                                     gpu,
                                     resident_id,
                                     new_prot,
                                     big_ptes_split,
                                     pages_to_write,
                                     pte_batch);

    // Case 4: Write the 4k PTEs which weren't covered by a big PTE before, and
    // remain uncovered after the operation.
    //
    // Mask computation: !big_before && !big_after
    bitmap_or(big_ptes_before_or_after, gpu_state->big_ptes, new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    uvm_page_mask_init_from_big_ptes(block, gpu, block_context->scratch_page_mask, big_ptes_before_or_after);
    if (uvm_page_mask_andnot(block_context->scratch_page_mask, pages_to_write, block_context->scratch_page_mask)) {
        block_gpu_pte_write_4k(block,
                               gpu,
                               resident_id,
                               new_prot,
                               block_context->scratch_page_mask,
                               pte_batch,
                               tlb_batch);
    }

    // Case 5: If the big page table is newly-allocated, make sure that all big
    // PTEs we aren't otherwise writing (that is, those which cover 4k PTEs) are
    // all initialized to invalid.
    //
    // The similar case of making newly-allocated big PTEs unmapped when no
    // lower 4k table is present is handled by having
    // block_gpu_compute_new_pte_state set new_pte_state->big_ptes
    // appropriately.
    if (gpu_state->page_table_range_big.table && !gpu_state->initialized_big) {
        // TODO: Bug 1766424: If we have the 4k page table already, we could
        //       attempt to merge all uncovered big PTE regions when first
        //       allocating the big table. That's probably not worth doing.
        UVM_ASSERT(gpu_state->page_table_range_4k.table);
        UVM_ASSERT(bitmap_empty(gpu_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK));
        bitmap_complement(big_ptes_mask, new_pte_state->big_ptes, uvm_va_block_num_big_pages(block, big_page_size));
        block_gpu_pte_clear_big(block, gpu, big_ptes_mask, 0, pte_batch, tlb_batch);
        gpu_state->initialized_big = true;
    }

    // Case 1 (step 1): Unmap the currently-big PTEs which are valid and are
    // being split to 4k. We can't directly transition from a valid big PTE to
    // valid lower PTEs, because that could cause the GPU TLBs to cache the same
    // VA in different cache lines. That could cause memory ordering to not be
    // maintained.
    bitmap_zero(big_ptes_mask, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    for_each_set_bit(big_page_index, big_ptes_split, MAX_BIG_PAGES_PER_UVM_VA_BLOCK) {
        big_region = uvm_va_block_big_page_region(block, big_page_index, big_page_size);
        if (test_bit(big_region.first, gpu_state->pte_bits[UVM_PTE_BITS_GPU_READ]))
            __set_bit(big_page_index, big_ptes_mask);
    }

    block_gpu_pte_clear_big(block, gpu, big_ptes_mask, tree->hal->unmapped_pte(big_page_size), pte_batch, tlb_batch);

    // Case 3: Write the currently-big PTEs which remain big PTEs, and are
    // wholly changing permissions.
    //
    // Mask computation: big_before && big_after && covered
    bitmap_and(big_ptes_mask, gpu_state->big_ptes, new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    if (bitmap_and(big_ptes_mask, big_ptes_mask, new_pte_state->big_ptes_covered, MAX_BIG_PAGES_PER_UVM_VA_BLOCK))
        block_gpu_pte_write_big(block, gpu, resident_id, new_prot, big_ptes_mask, pte_batch, tlb_batch);

    // Case 2 (step 1): Merge the new big PTEs and end the batches, now that
    // we've done all of the independent PTE writes we can. This also merges
    // newly-allocated uncovered big PTEs to unmapped (see
    // block_gpu_compute_new_pte_state).
    //
    // Mask computation: !big_before && big_after
    if (bitmap_andnot(big_ptes_merge, new_pte_state->big_ptes, gpu_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK)) {
        // This writes the newly-big PTEs to unmapped and ends the PTE and TLB
        // batches.
        block_gpu_pte_merge_big_and_end(block,
                                        block_context,
                                        gpu,
                                        big_ptes_merge,
                                        push,
                                        pte_batch,
                                        tlb_batch,
                                        tlb_membar);

        // Remove uncovered big PTEs. We needed to merge them to unmapped above,
        // but they shouldn't get new_prot below.
        bitmap_and(big_ptes_merge, big_ptes_merge, new_pte_state->big_ptes_covered, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    }
    else {
        // End the batches. We have to commit the membars and TLB invalidates
        // before we finish splitting formerly-big PTEs.
        uvm_pte_batch_end(pte_batch);
        uvm_tlb_batch_end(tlb_batch, push, tlb_membar);
    }

    if (!bitmap_empty(big_ptes_split, MAX_BIG_PAGES_PER_UVM_VA_BLOCK) ||
        !bitmap_empty(big_ptes_merge, MAX_BIG_PAGES_PER_UVM_VA_BLOCK) ||
        block_gpu_needs_to_activate_table(block, gpu)) {

        uvm_pte_batch_begin(push, pte_batch);
        uvm_tlb_batch_begin(tree, tlb_batch);

        // Case 1 (step 2): Finish splitting our big PTEs, if we have any, by
        // switching them from unmapped to invalid. This causes the GPU MMU to
        // start reading the 4k PTEs instead of stopping at the unmapped big
        // PTEs.
        block_gpu_pte_clear_big(block, gpu, big_ptes_split, 0, pte_batch, tlb_batch);

        // Case 2 (step 2): Finish merging our big PTEs, if we have any, by
        // switching them from unmapped to new_prot.
        block_gpu_pte_write_big(block, gpu, resident_id, new_prot, big_ptes_merge, pte_batch, tlb_batch);

        uvm_pte_batch_end(pte_batch);

        // Finally, activate the page tables if they're inactive
        if (block_gpu_needs_to_activate_table(block, gpu))
            block_gpu_write_pde(block, gpu, push, tlb_batch);

        uvm_tlb_batch_end(tlb_batch, push, UVM_MEMBAR_NONE);
    }

    // Update gpu_state
    bitmap_copy(gpu_state->big_ptes, new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
}

// Unmap all PTEs for {block, gpu}. If the 2M entry is currently a PDE, it is
// merged into a PTE.
static void block_gpu_unmap_to_2m(uvm_va_block_t *block,
                                  uvm_va_block_context_t *block_context,
                                  uvm_gpu_t *gpu,
                                  uvm_push_t *push,
                                  uvm_membar_t tlb_membar)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_gpu_va_space_t *gpu_va_space = uvm_va_block_get_gpu_va_space(block, gpu);
    uvm_pte_batch_t *pte_batch = &block_context->mapping.pte_batch;
    uvm_tlb_batch_t *tlb_batch = &block_context->mapping.tlb_batch;

    if (gpu_state->pte_is_2m) {
        // If we're already mapped as a valid 2M PTE, just write it to invalid
        uvm_pte_batch_begin(push, pte_batch);
        uvm_tlb_batch_begin(&gpu_va_space->page_tables, tlb_batch);

        block_gpu_pte_clear_2m(block, gpu, pte_batch, tlb_batch);

        uvm_pte_batch_end(pte_batch);
        uvm_tlb_batch_end(tlb_batch, push, tlb_membar);
    }
    else {
        // Otherwise we have a mix of big and 4K PTEs which need to be merged
        // into an invalid 2M PTE.
        block_gpu_pte_merge_2m(block, block_context, gpu, push, tlb_membar);

        gpu_state->pte_is_2m = true;
        bitmap_zero(gpu_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    }
}

// Combination split + unmap operation, called when only part of a valid 2M PTE
// mapping is being unmapped. The 2M PTE is split into a mix of valid and
// invalid big and/or 4k PTEs, as described by
// block_context->mapping.new_pte_state.
//
// The PTEs covering the pages in pages_to_unmap are cleared (unmapped).
//
// The PTEs covering the pages not set in pages_to_unmap inherit the mapping of
// the current 2M PTE.
static void block_gpu_unmap_split_2m(uvm_va_block_t *block,
                                     uvm_va_block_context_t *block_context,
                                     uvm_gpu_t *gpu,
                                     const unsigned long *pages_to_unmap,
                                     uvm_push_t *push,
                                     uvm_membar_t tlb_membar)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_page_tree_t *tree = &uvm_va_block_get_gpu_va_space(block, gpu)->page_tables;
    uvm_va_block_new_pte_state_t *new_pte_state = &block_context->mapping.new_pte_state;
    uvm_pte_batch_t *pte_batch = &block_context->mapping.pte_batch;
    uvm_tlb_batch_t *tlb_batch = &block_context->mapping.tlb_batch;
    uvm_prot_t curr_prot = block_page_prot_gpu(block, gpu, 0);
    uvm_processor_id_t resident_id;
    DECLARE_BITMAP(big_ptes_split, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    DECLARE_BITMAP(big_ptes_inherit, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    DECLARE_BITMAP(big_ptes_new_prot, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    UVM_ASSERT(gpu_state->pte_is_2m);

    resident_id = block_gpu_get_processor_to_map(block, gpu, 0);

    uvm_pte_batch_begin(push, pte_batch);

    // Since the 2M entry is active as a PTE, the GPU MMU can't fetch entries
    // from the lower levels. This means we don't need to issue a TLB invalidate
    // when writing those levels.

    // Cases to handle:
    // 1) Big PTEs which inherit curr_prot
    // 2) Big PTEs which get unmapped
    // 3) Big PTEs which are split to 4k
    //    a) 4k PTEs which inherit curr_prot under the split big PTEs
    //    b) 4k PTEs which get unmapped under the split big PTEs

    // Compute the big PTEs which will need to be split to 4k, if any.
    bitmap_complement(big_ptes_split, new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    if (gpu_state->page_table_range_big.table) {
        // Case 1: Write the big PTEs which will inherit the 2M permissions, if
        // any. These are the big PTEs which are unchanged (uncovered) by the
        // operation.
        bitmap_andnot(big_ptes_inherit,
                      new_pte_state->big_ptes,
                      new_pte_state->big_ptes_covered,
                      MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

        block_gpu_pte_write_big(block, gpu, resident_id, curr_prot, big_ptes_inherit, pte_batch, NULL);

        // Case 2: Clear the new big PTEs which get unmapped (those not covering
        // 4ks)
        bitmap_and(big_ptes_new_prot,
                   new_pte_state->big_ptes,
                   new_pte_state->big_ptes_covered,
                   MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

        block_gpu_pte_clear_big(block,
                                gpu,
                                big_ptes_new_prot,
                                tree->hal->unmapped_pte(UVM_PAGE_SIZE_64K),
                                pte_batch,
                                NULL);

        // Case 3: Write the big PTEs which cover 4k PTEs
        block_gpu_pte_clear_big(block, gpu, big_ptes_split, 0, pte_batch, NULL);

        // We just wrote all possible big PTEs, so mark them as initialized
        gpu_state->initialized_big = true;
    }
    else {
        UVM_ASSERT(bitmap_empty(new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK));
        UVM_ASSERT(bitmap_full(new_pte_state->big_ptes_covered, MAX_BIG_PAGES_PER_UVM_VA_BLOCK));
    }

    // Cases 3a and 3b: Write all 4k PTEs under all now-split big PTEs
    block_gpu_pte_big_split_write_4k(block,
                                     block_context,
                                     gpu,
                                     resident_id,
                                     UVM_PROT_NONE,
                                     big_ptes_split,
                                     pages_to_unmap,
                                     pte_batch);

    // And activate the 2M PDE. This ends the pte_batch and issues a single TLB
    // invalidate for the 2M entry.
    block_gpu_pte_finish_split_2m(block, gpu, push, pte_batch, tlb_batch, tlb_membar);

    gpu_state->pte_is_2m = false;
    bitmap_copy(gpu_state->big_ptes, new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
}

// Unmap some pre-existing mix of big and 4k PTEs into some other mix of big
// and 4k PTEs.
//
// The PTEs covering the pages in pages_to_unmap are cleared (unmapped).
static void block_gpu_unmap_big_and_4k(uvm_va_block_t *block,
                                       uvm_va_block_context_t *block_context,
                                       uvm_gpu_t *gpu,
                                       const unsigned long *pages_to_unmap,
                                       uvm_push_t *push,
                                       uvm_membar_t tlb_membar)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_page_tree_t *tree = &uvm_va_block_get_gpu_va_space(block, gpu)->page_tables;
    uvm_va_block_new_pte_state_t *new_pte_state = &block_context->mapping.new_pte_state;
    uvm_pte_batch_t *pte_batch = &block_context->mapping.pte_batch;
    uvm_tlb_batch_t *tlb_batch = &block_context->mapping.tlb_batch;
    DECLARE_BITMAP(big_ptes_split, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    DECLARE_BITMAP(big_ptes_before_or_after, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    DECLARE_BITMAP(big_ptes_mask, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    NvU32 big_page_size = tree->big_page_size;
    NvU64 unmapped_pte_val = tree->hal->unmapped_pte(big_page_size);

    UVM_ASSERT(!gpu_state->pte_is_2m);

    uvm_pte_batch_begin(push, pte_batch);
    uvm_tlb_batch_begin(tree, tlb_batch);

    // All of these cases might be perfomed in the same call:
    // 1) Split currently-big PTEs to 4k
    //    a) Write new 4k PTEs which inherit curr_prot under the split big PTEs
    //    b) Clear new 4k PTEs which get unmapped under the split big PTEs
    // 2) Merge currently-4k PTEs to unmapped big
    // 3) Clear currently-big PTEs which wholly get unmapped
    // 4) Clear currently-4k PTEs which get unmapped
    // 5) Initialize big PTEs which are not covered by this operation

    // Cases 1a and 1b: Write all 4k PTEs under all currently-big PTEs which are
    // being split. We'll make the big PTEs inactive below after flushing these
    // writes. No TLB invalidate is needed since the big PTE is active.
    //
    // Mask computation: big_before && !big_after
    bitmap_andnot(big_ptes_split, gpu_state->big_ptes, new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    block_gpu_pte_big_split_write_4k(block,
                                     block_context,
                                     gpu,
                                     UVM8_MAX_PROCESSORS,
                                     UVM_PROT_NONE,
                                     big_ptes_split,
                                     pages_to_unmap,
                                     pte_batch);

    // Case 4: Clear the 4k PTEs which weren't covered by a big PTE before, and
    // remain uncovered after the unmap.
    //
    // Mask computation: !big_before && !big_after
    bitmap_or(big_ptes_before_or_after, gpu_state->big_ptes, new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    uvm_page_mask_init_from_big_ptes(block, gpu, block_context->scratch_page_mask, big_ptes_before_or_after);
    if (uvm_page_mask_andnot(block_context->scratch_page_mask, pages_to_unmap, block_context->scratch_page_mask))
        block_gpu_pte_clear_4k(block, gpu, block_context->scratch_page_mask, 0, pte_batch, tlb_batch);

    // Case 5: If the big page table is newly-allocated, make sure that all big
    // PTEs we aren't otherwise writing (that is, those which cover 4k PTEs) are
    // all initialized to invalid.
    //
    // The similar case of making newly-allocated big PTEs unmapped when no
    // lower 4k table is present is handled by having
    // block_gpu_compute_new_pte_state set new_pte_state->big_ptes
    // appropriately.
    if (gpu_state->page_table_range_big.table && !gpu_state->initialized_big) {
        // TODO: Bug 1766424: If we have the 4k page table already, we could
        //       attempt to merge all uncovered big PTE regions when first
        //       allocating the big table. That's probably not worth doing.
        UVM_ASSERT(gpu_state->page_table_range_4k.table);
        UVM_ASSERT(bitmap_empty(gpu_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK));
        bitmap_complement(big_ptes_mask, new_pte_state->big_ptes, uvm_va_block_num_big_pages(block, big_page_size));
        block_gpu_pte_clear_big(block, gpu, big_ptes_mask, 0, pte_batch, tlb_batch);
        gpu_state->initialized_big = true;
    }

    // Case 3 and step 1 of case 1: Unmap both currently-big PTEs which are
    // getting wholly unmapped, and those currently-big PTEs which are being
    // split to 4k. We can't directly transition from a valid big PTE to valid
    // lower PTEs, because that could cause the GPU TLBs to cache the same VA in
    // different cache lines. That could cause memory ordering to not be
    // maintained.
    //
    // Mask computation: (big_before && big_after && covered) ||
    //                   (big_before && !big_after)
    bitmap_and(big_ptes_mask, gpu_state->big_ptes, new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    bitmap_and(big_ptes_mask, big_ptes_mask, new_pte_state->big_ptes_covered, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    bitmap_or(big_ptes_mask, big_ptes_mask, big_ptes_split, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    block_gpu_pte_clear_big(block, gpu, big_ptes_mask, unmapped_pte_val, pte_batch, tlb_batch);

    // Case 2: Merge the new big PTEs and end the batches, now that we've done
    // all of the independent PTE writes we can.
    //
    // Mask computation: !big_before && big_after
    if (bitmap_andnot(big_ptes_mask, new_pte_state->big_ptes, gpu_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK)) {
        // This writes the newly-big PTEs to unmapped and ends the PTE and TLB
        // batches.
        block_gpu_pte_merge_big_and_end(block,
                                        block_context,
                                        gpu,
                                        big_ptes_mask,
                                        push,
                                        pte_batch,
                                        tlb_batch,
                                        tlb_membar);
    }
    else {
        // End the batches. We have to commit the membars and TLB invalidates
        // before we finish splitting formerly-big PTEs.
        uvm_pte_batch_end(pte_batch);
        uvm_tlb_batch_end(tlb_batch, push, tlb_membar);
    }

    if (!bitmap_empty(big_ptes_split, MAX_BIG_PAGES_PER_UVM_VA_BLOCK) ||
        block_gpu_needs_to_activate_table(block, gpu)) {
        uvm_pte_batch_begin(push, pte_batch);
        uvm_tlb_batch_begin(tree, tlb_batch);

        // Case 1 (step 2): Finish splitting our big PTEs, if we have any, by
        // switching them from unmapped to invalid. This causes the GPU MMU to
        // start reading the 4k PTEs instead of stopping at the unmapped big
        // PTEs.
        block_gpu_pte_clear_big(block, gpu, big_ptes_split, 0, pte_batch, tlb_batch);

        uvm_pte_batch_end(pte_batch);

        // Finally, activate the page tables if they're inactive
        if (block_gpu_needs_to_activate_table(block, gpu))
            block_gpu_write_pde(block, gpu, push, tlb_batch);

        uvm_tlb_batch_end(tlb_batch, push, UVM_MEMBAR_NONE);
    }

    // Update gpu_state
    bitmap_copy(gpu_state->big_ptes, new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
}

// Helper for block_gpu_compute_new_pte_state. For GPUs which swizzle, all
// GPUs mapping the same memory must use the same PTE size. This function
// returns true if all currently-mapped GPUs (aside from mapping_gpu itself) can
// all be promoted to big PTE mappings.
static bool mapped_gpus_can_map_big(uvm_va_block_t *block,
                                    uvm_gpu_t *mapping_gpu,
                                    uvm_va_block_region_t big_page_region)
{
    uvm_processor_mask_t mapped_procs;
    uvm_gpu_t *other_gpu, *resident_gpu;
    uvm_va_block_gpu_state_t *gpu_state;
    uvm_pte_bits_gpu_t pte_bit;
    uvm_va_space_t *va_space = block->va_range->va_space;

    // GPUs without swizzling don't care what page size their peers use
    if (!mapping_gpu->big_page.swizzling)
        return true;

    resident_gpu = uvm_gpu_get(block_gpu_get_processor_to_map(block, mapping_gpu, big_page_region.first));
    UVM_ASSERT(uvm_processor_mask_test(&va_space->accessible_from[resident_gpu->id], mapping_gpu->id));

    // GPUs which don't swizzle can't have peer mappings to those which do.
    // We've also enforced that they all share the same big page size for a
    // given VA space, so we can use the big page regions interchangeably.
    UVM_ASSERT(resident_gpu->big_page.swizzling);

    uvm_processor_mask_and(&mapped_procs, &block->mapped, &va_space->accessible_from[resident_gpu->id]);

    // The caller checks mapping_gpu, since its gpu_state permissions mask
    // hasn't been updated yet.
    uvm_processor_mask_clear(&mapped_procs, mapping_gpu->id);

    // Since UVM-Lite GPUs always map the preferred location and remain mapped
    // even if the memory is resident on a non-UVM-Lite GPU, we ignore UVM-Lite
    // GPUs when mapping non-UVM-Lite GPUs, and vice-versa.
    if (uvm_processor_mask_test(&block->va_range->uvm_lite_gpus, mapping_gpu->id))
        uvm_processor_mask_and(&mapped_procs, &mapped_procs, &block->va_range->uvm_lite_gpus);
    else
        uvm_processor_mask_andnot(&mapped_procs, &mapped_procs, &block->va_range->uvm_lite_gpus);

    // If each peer GPU has matching permissions for this entire region, then
    // they can also map as a swizzled big page. Otherwise, all GPUs must demote
    // to 4k. Note that the GPUs don't have to match permissions with each
    // other.
    for_each_gpu_in_mask(other_gpu, &mapped_procs) {
        gpu_state = block->gpus[other_gpu->id - 1];

        UVM_ASSERT(other_gpu->big_page.swizzling);

        for (pte_bit = UVM_PTE_BITS_GPU_ATOMIC; pte_bit >= 0; pte_bit--) {
            // If the highest permissions has a full region, then we match
            if (uvm_page_mask_region_full(gpu_state->pte_bits[pte_bit], big_page_region)) {
                // Sanity check that all GPUs actually map the same memory
                UVM_ASSERT(block_check_mapping_residency_region(block,
                                                                other_gpu,
                                                                resident_gpu->id,
                                                                big_page_region,
                                                                gpu_state->pte_bits[pte_bit]));
                break;
            }

            // If some pages are set, then we don't match and we can't map big
            if (!uvm_page_mask_region_empty(gpu_state->pte_bits[pte_bit], big_page_region))
                return false;

            // Otherwise, try the next lower permissions. A fully-unmapped GPU
            // doesn't factor into the swizzling decisions, so we ignore those.
            if (pte_bit == 0)
                break;
        }
    }

    // All mapped peers can map as a big page
    return true;
}

// When PTE state is about to change (for example due to a map/unmap/revoke
// operation), this function decides how to split and merge the PTEs in response
// to that operation.
//
// The operation is described with the two page masks:
//
// - pages_changing indicates which pages will have their PTE mappings changed
//   on the GPU in some way as a result of the operation (for example, which
//   pages will actually have their mapping permissions upgraded).
//
// - page_mask_after indicates which pages on this GPU will have exactly the
//   same PTE attributes (permissions, residency) as pages_changing after the
//   operation is applied.
//
// PTEs are merged eagerly.
static void block_gpu_compute_new_pte_state(uvm_va_block_t *block,
                                            uvm_gpu_t *gpu,
                                            uvm_processor_id_t resident_id,
                                            const unsigned long *pages_changing,
                                            const unsigned long *page_mask_after,
                                            uvm_va_block_new_pte_state_t *new_pte_state)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_va_block_region_t big_region_all, big_page_region, region;
    NvU32 big_page_size;
    size_t page_index, big_page_index;
    DECLARE_BITMAP(big_ptes_not_covered, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    bool can_make_new_big_ptes, region_full;

    memset(new_pte_state, 0, sizeof(*new_pte_state));
    new_pte_state->needs_4k = true;

    // TODO: Bug 1676485: Force a specific page size for perf testing

    if (gpu_state->force_4k_ptes)
        return;

    UVM_ASSERT(uvm_page_mask_subset(pages_changing, page_mask_after));

    // TODO: Bug 1766172: Use 2M sysmem pages on x86
    if (block_gpu_supports_2m(block, gpu) && resident_id != UVM_CPU_ID) {
        // If all pages in the 2M mask have the same attributes after the
        // operation is applied, we can use a 2M PTE.
        if (uvm_page_mask_full(page_mask_after)) {
            new_pte_state->pte_is_2m = true;
            new_pte_state->needs_4k = false;
            return;
        }
    }

    // Find big PTEs with matching attributes

    // Can this block fit any big pages?
    big_page_size = uvm_va_block_gpu_big_page_size(block, gpu);
    big_region_all = uvm_va_block_big_page_region_all(block, big_page_size);
    if (big_region_all.first >= big_region_all.outer)
        return;

    new_pte_state->needs_4k = false;

    can_make_new_big_ptes = true;

    // TODO: Bug 1668859: Big pages can be used when mapping sysmem if PAGE_SIZE
    //       >= big_page_size and the GPU supports it (Pascal+).
    if (resident_id == UVM_CPU_ID)
        can_make_new_big_ptes = false;

    // We must not fail during teardown: unmap (resident_id ==
    // UVM8_MAX_PROCESSORS) with no splits required. That means we should avoid
    // allocating PTEs which are only needed for merges.
    //
    // This only matters if we're merging to big PTEs. If we're merging to 2M,
    // then we must already have the 2M level (since it has to be allocated
    // before the lower levels).
    //
    // If pte_is_2m already and we don't have a big table, we're splitting so we
    // have to allocate.
    if (resident_id == UVM8_MAX_PROCESSORS && !gpu_state->page_table_range_big.table && !gpu_state->pte_is_2m)
        can_make_new_big_ptes = false;

    for_each_va_block_page_in_mask(page_index, pages_changing, big_region_all) {
        big_page_index = uvm_va_block_big_page_index(block, page_index, big_page_size);
        big_page_region = uvm_va_block_big_page_region(block, big_page_index, big_page_size);

        __set_bit(big_page_index, new_pte_state->big_ptes_covered);

        region_full = uvm_page_mask_region_full(page_mask_after, big_page_region);
        if (region_full && resident_id == UVM8_MAX_PROCESSORS)
            __set_bit(big_page_index, new_pte_state->big_ptes_fully_unmapped);

        if (can_make_new_big_ptes && region_full) {
            if (gpu->big_page.swizzling) {
                // If we're fully unmapping, we don't care about the swizzle
                // format. Otherwise we have to check whether all mappings can
                // be promoted to a big PTE.
                if (resident_id == UVM8_MAX_PROCESSORS || mapped_gpus_can_map_big(block, gpu, big_page_region))
                    __set_bit(big_page_index, new_pte_state->big_ptes);
            }
            else {
                __set_bit(big_page_index, new_pte_state->big_ptes);
            }
        }

        if (!test_bit(big_page_index, new_pte_state->big_ptes))
            new_pte_state->needs_4k = true;

        // Skip to the end of the region
        page_index = big_page_region.outer - 1;
    }

    if (!new_pte_state->needs_4k) {
        // All big page regions in pages_changing will be big PTEs. Now check if
        // there are any unaligned pages outside of big_region_all which are
        // changing.
        region = uvm_va_block_region(0, big_region_all.first);
        if (!uvm_page_mask_region_empty(pages_changing, region)) {
            new_pte_state->needs_4k = true;
        }
        else {
            region = uvm_va_block_region(big_region_all.outer, uvm_va_block_num_cpu_pages(block));
            if (!uvm_page_mask_region_empty(pages_changing, region))
                new_pte_state->needs_4k = true;
        }
    }

    // Now add in the PTEs which should be big but weren't covered by this
    // operation.
    //
    // Note that we can't assume that a given page table range has been
    // initialized if it's present here, since it could have been allocated by a
    // thread which had to restart its operation due to allocation retry.
    if (gpu_state->pte_is_2m || (block_gpu_supports_2m(block, gpu) && !gpu_state->page_table_range_2m.table)) {
        // We're splitting a 2M PTE so all of the uncovered big PTE regions will
        // become big PTEs which inherit the 2M permissions. If we haven't
        // allocated the 2M table yet, it will start as a 2M PTE until the lower
        // levels are allocated, so it's the same split case regardless of
        // whether this operation will need to retry a later allocation.
        bitmap_complement(big_ptes_not_covered, new_pte_state->big_ptes_covered, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    }
    else if (!gpu_state->page_table_range_4k.table && !new_pte_state->needs_4k) {
        // If we don't have 4k PTEs and we won't be allocating them for this
        // operation, all of our PTEs need to be big.
        UVM_ASSERT(!bitmap_empty(new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK));
        bitmap_fill(big_ptes_not_covered, uvm_va_block_num_big_pages(block, big_page_size));
    }
    else {
        // Otherwise, add in all of the currently-big PTEs which are unchanging.
        // They won't be written, but they need to be carried into the new
        // gpu_state->big_ptes when it's updated.
        bitmap_andnot(big_ptes_not_covered,
                      gpu_state->big_ptes,
                      new_pte_state->big_ptes_covered,
                      MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    }

    bitmap_or(new_pte_state->big_ptes, new_pte_state->big_ptes, big_ptes_not_covered, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
}

// Wrapper around uvm_page_tree_get_ptes() and uvm_page_tree_alloc_table() that
// handles allocation retry. If the block lock has been unlocked and relocked as
// part of the allocation, NV_ERR_MORE_PROCESSING_REQUIRED is returned to signal
// to the caller that the operation likely needs to be restarted. If that
// happens, the pending tracker is added to the block's tracker.
static NV_STATUS block_alloc_pt_range_with_retry(uvm_va_block_t *va_block,
                                                 uvm_gpu_t *gpu,
                                                 NvU32 page_size,
                                                 uvm_page_table_range_t *page_table_range,
                                                 uvm_tracker_t *pending_tracker)
{
    uvm_va_block_gpu_state_t *gpu_state = va_block->gpus[gpu->id - 1];
    uvm_va_space_t *va_space = va_block->va_range->va_space;
    uvm_gpu_va_space_t *gpu_va_space = uvm_va_block_get_gpu_va_space(va_block, gpu);
    uvm_page_tree_t *page_tables = &gpu_va_space->page_tables;
    uvm_page_table_range_t local_range;
    NV_STATUS status;

    // Blocks may contain large PTEs without starting on a PTE boundary or
    // having an aligned size. Cover the PTEs of this size in the block's
    // interior so we match uvm_va_block_gpu_state_t::big_ptes.
    NvU64 start = UVM_ALIGN_UP(va_block->start, page_size);
    NvU64 size  = UVM_ALIGN_DOWN(va_block->end + 1, page_size) - start;

    // VA blocks which can use the 2MB level as either a PTE or a PDE need to
    // account for the PDE specially, so they must use uvm_page_tree_alloc_table
    // to allocate the lower levels.
    bool use_alloc_table = block_gpu_supports_2m(va_block, gpu) && page_size < UVM_PAGE_SIZE_2M;

    uvm_assert_rwsem_locked(&va_space->lock);
    UVM_ASSERT(page_table_range->table == NULL);

    if (va_block->page_table_allocation_retry_force_count > 0) {
        --va_block->page_table_allocation_retry_force_count;
        status = NV_ERR_NO_MEMORY;
    }
    else if (use_alloc_table) {
        // Pascal+: 4k/64k tables under a 2M entry
        UVM_ASSERT(gpu_state->page_table_range_2m.table);
        status = uvm_page_tree_alloc_table(page_tables,
                                           page_size,
                                           UVM_PMM_ALLOC_FLAGS_NONE,
                                           &gpu_state->page_table_range_2m,
                                           page_table_range);
    }
    else {
        // 4k/big tables on pre-Pascal, and the 2M entry on Pascal+
        status = uvm_page_tree_get_ptes(page_tables,
                                        page_size,
                                        start,
                                        size,
                                        UVM_PMM_ALLOC_FLAGS_NONE,
                                        page_table_range);
    }

    if (status == NV_OK)
        goto allocated;

    if (status != NV_ERR_NO_MEMORY)
        return status;

    // Before unlocking the block lock, any pending work on the block has to be
    // added to the block's tracker.
    if (pending_tracker) {
        status = uvm_tracker_add_tracker_safe(&va_block->tracker, pending_tracker);
        if (status != NV_OK)
            return status;
    }

    // Unlock the va block and retry with eviction enabled
    uvm_mutex_unlock(&va_block->lock);

    if (use_alloc_table) {
        // Although we don't hold the block lock here, it's safe to pass
        // gpu_state->page_table_range_2m to the page tree code because we know
        // that the 2m range has already been allocated, and that it can't go
        // away while we have the va_space lock held.
        status = uvm_page_tree_alloc_table(page_tables,
                                           page_size,
                                           UVM_PMM_ALLOC_FLAGS_EVICT,
                                           &gpu_state->page_table_range_2m,
                                           &local_range);
    }
    else {
        status = uvm_page_tree_get_ptes(page_tables,
                                        page_size,
                                        start,
                                        size,
                                        UVM_PMM_ALLOC_FLAGS_EVICT,
                                        &local_range);
    }

    uvm_mutex_lock(&va_block->lock);

    if (status != NV_OK)
        return status;

    status = NV_ERR_MORE_PROCESSING_REQUIRED;

    if (page_table_range->table) {
        // A different caller allocated the page tables in the meantime, release the
        // local copy.
        uvm_page_tree_put_ptes(page_tables, &local_range);
        return status;
    }

    *page_table_range = local_range;

allocated:
    // Mark the 2M PTE as active when we first allocate it, since we don't have
    // any PTEs below it yet.
    if (page_size == UVM_PAGE_SIZE_2M) {
        UVM_ASSERT(!gpu_state->pte_is_2m);
        gpu_state->pte_is_2m = true;
    }
    else if (page_size != UVM_PAGE_SIZE_4K) {
        // uvm_page_tree_get_ptes initializes big PTEs to invalid.
        // uvm_page_tree_alloc_table does not, so we'll have to do it later.
        if (use_alloc_table)
            UVM_ASSERT(!gpu_state->initialized_big);
        else
            gpu_state->initialized_big = true;
    }

    return status;
}

// Helper which allocates all page table ranges necessary for the given page
// sizes. See block_alloc_pt_range_with_retry.
static NV_STATUS block_alloc_ptes_with_retry(uvm_va_block_t *va_block,
                                             uvm_gpu_t *gpu,
                                             NvU32 page_sizes,
                                             uvm_tracker_t *pending_tracker)
{
    uvm_va_block_gpu_state_t *gpu_state = va_block->gpus[gpu->id - 1];
    uvm_gpu_va_space_t *gpu_va_space = uvm_va_block_get_gpu_va_space(va_block, gpu);
    uvm_page_table_range_t *range;
    NvU32 page_size;
    NV_STATUS status, final_status = NV_OK;

    UVM_ASSERT(gpu_state);

    // Blocks which can map 2M PTE/PDEs must always allocate the 2MB level first
    // in order to allocate the levels below.
    if (block_gpu_supports_2m(va_block, gpu))
        page_sizes |= UVM_PAGE_SIZE_2M;

    UVM_ASSERT((page_sizes & gpu_va_space->page_tables.hal->page_sizes()) == page_sizes);

    for_each_chunk_size_rev(page_size, page_sizes) {
        if (page_size == UVM_PAGE_SIZE_2M)
            range = &gpu_state->page_table_range_2m;
        else if (page_size == UVM_PAGE_SIZE_4K)
            range = &gpu_state->page_table_range_4k;
        else
            range = &gpu_state->page_table_range_big;

        if (range->table)
            continue;

        if (page_size == UVM_PAGE_SIZE_2M) {
            UVM_ASSERT(!gpu_state->pte_is_2m);
            UVM_ASSERT(!gpu_state->page_table_range_big.table);
            UVM_ASSERT(!gpu_state->page_table_range_4k.table);
        }
        else if (page_size != UVM_PAGE_SIZE_4K) {
            UVM_ASSERT(uvm_va_block_num_big_pages(va_block, uvm_va_block_gpu_big_page_size(va_block, gpu)) > 0);
            UVM_ASSERT(bitmap_empty(gpu_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK));
        }

        status = block_alloc_pt_range_with_retry(va_block, gpu, page_size, range, pending_tracker);

        // Keep going to allocate the remaining levels even if the allocation
        // requires a retry, since we'll likely still need them when we retry
        // anyway.
        if (status == NV_ERR_MORE_PROCESSING_REQUIRED)
            final_status = NV_ERR_MORE_PROCESSING_REQUIRED;
        else if (status != NV_OK)
            return status;
    }

    return final_status;
}

static NV_STATUS block_alloc_ptes_new_state(uvm_va_block_t *va_block,
                                            uvm_gpu_t *gpu,
                                            uvm_va_block_new_pte_state_t *new_pte_state,
                                            uvm_tracker_t *pending_tracker)
{
    NvU32 page_sizes = 0;

    if (new_pte_state->pte_is_2m) {
        page_sizes |= UVM_PAGE_SIZE_2M;
    }
    else {
        if (!bitmap_empty(new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK))
            page_sizes |= uvm_va_block_gpu_big_page_size(va_block, gpu);

        if (new_pte_state->needs_4k)
            page_sizes |= UVM_PAGE_SIZE_4K;
        else
            UVM_ASSERT(!bitmap_empty(new_pte_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK));
    }

    return block_alloc_ptes_with_retry(va_block, gpu, page_sizes, pending_tracker);
}

// Unmap the given big page from gpu in preparation for a swizzling change. See
// block_gpu_big_page_change_swizzling.
static NV_STATUS block_gpu_big_page_change_swizzling_unmap(uvm_va_block_t *block,
                                                           uvm_va_block_context_t *block_context,
                                                           uvm_gpu_t *gpu,
                                                           size_t big_page_index,
                                                           uvm_va_block_region_t big_page_region,
                                                           uvm_gpu_swizzle_op_t op,
                                                           uvm_tracker_t *tracker)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_page_tree_t *tree = &uvm_va_block_get_gpu_va_space(block, gpu)->page_tables;
    uvm_pte_batch_t *pte_batch = &block_context->mapping.pte_batch;
    uvm_tlb_batch_t *tlb_batch = &block_context->mapping.tlb_batch;
    NvU64 unmapped_pte_val = tree->hal->unmapped_pte(tree->big_page_size);
    DECLARE_BITMAP(big_page_single, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    uvm_push_t push;
    NV_STATUS status;

    bitmap_zero(big_page_single, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    __set_bit(big_page_index, big_page_single);

    status = uvm_push_begin_acquire(gpu->channel_manager,
                                    UVM_CHANNEL_TYPE_MEMOPS,
                                    tracker,
                                    &push,
                                    "Unmapping pages to %s [0x%llx, 0x%llx)",
                                    op == UVM_GPU_SWIZZLE_OP_SWIZZLE ? "swizzle" : "deswizzle",
                                    uvm_va_block_region_start(block, big_page_region),
                                    uvm_va_block_region_end(block, big_page_region) + 1);
    if (status != NV_OK)
        return status;

    uvm_pte_batch_begin(&push, pte_batch);
    uvm_tlb_batch_begin(tree, tlb_batch);

    if (op == UVM_GPU_SWIZZLE_OP_SWIZZLE) {
        // Write the newly-big PTE to unmapped and end the PTE and TLB batches
        block_gpu_pte_merge_big_and_end(block,
                                        block_context,
                                        gpu,
                                        big_page_single,
                                        &push,
                                        pte_batch,
                                        tlb_batch,
                                        UVM_MEMBAR_NONE);

        UVM_ASSERT(!test_bit(big_page_index, gpu_state->big_ptes));
        __set_bit(big_page_index, gpu_state->big_ptes);
    }
    else {
        // Write the already-big swizzled PTE to unmapped
        UVM_ASSERT(test_bit(big_page_index, gpu_state->big_ptes));

        block_gpu_pte_clear_big(block, gpu, big_page_single, unmapped_pte_val, pte_batch, tlb_batch);
        uvm_pte_batch_end(pte_batch);
        uvm_tlb_batch_end(tlb_batch, &push, UVM_MEMBAR_NONE);
    }

    uvm_push_end(&push);

    // This will cause the remaps to serialize, but changing the swizzling
    // format is not a performance-critical path.
    uvm_tracker_overwrite_with_push(tracker, &push);
    return NV_OK;
}

// Remap the given big page on gpu after completing a swizzling change. See
// block_gpu_big_page_change_swizzling.
static NV_STATUS block_gpu_big_page_change_swizzling_remap(uvm_va_block_t *block,
                                                           uvm_va_block_context_t *block_context,
                                                           uvm_gpu_t *gpu,
                                                           size_t big_page_index,
                                                           uvm_va_block_region_t big_page_region,
                                                           uvm_gpu_swizzle_op_t op,
                                                           uvm_tracker_t *tracker)
{
    uvm_page_tree_t *tree = &uvm_va_block_get_gpu_va_space(block, gpu)->page_tables;
    uvm_pte_batch_t *pte_batch = &block_context->mapping.pte_batch;
    uvm_tlb_batch_t *tlb_batch = &block_context->mapping.tlb_batch;
    uvm_processor_id_t resident_id;
    uvm_push_t push;
    uvm_prot_t curr_prot;
    DECLARE_BITMAP(big_page_single, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    NV_STATUS status;

    UVM_ASSERT(uvm_page_mask_region_full(uvm_va_block_map_mask_get(block, gpu->id), big_page_region));

    bitmap_zero(big_page_single, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    __set_bit(big_page_index, big_page_single);

    status = uvm_push_begin_acquire(gpu->channel_manager,
                                    UVM_CHANNEL_TYPE_MEMOPS,
                                    tracker,
                                    &push,
                                    "Remapping %s pages [0x%llx, 0x%llx)",
                                    op == UVM_GPU_SWIZZLE_OP_SWIZZLE ? "swizzled" : "unswizzled",
                                    uvm_va_block_region_start(block, big_page_region),
                                    uvm_va_block_region_end(block, big_page_region) + 1);
    if (status != NV_OK)
        return status;

    if (op == UVM_GPU_SWIZZLE_OP_SWIZZLE) {
        uvm_pte_batch_begin(&push, pte_batch);
        uvm_tlb_batch_begin(tree, tlb_batch);

        curr_prot = block_page_prot_gpu(block, gpu, big_page_region.first);
        resident_id = block_gpu_get_processor_to_map(block, gpu, big_page_region.first);
        block_gpu_pte_write_big(block, gpu, resident_id, curr_prot, big_page_single, pte_batch, tlb_batch);

        uvm_pte_batch_end(pte_batch);
        uvm_tlb_batch_end(tlb_batch, &push, UVM_MEMBAR_NONE);
    }
    else {
        // This will redundantly write the big PTE to unmapped first, but that
        // doesn't hurt anything.
        block_gpu_split_big(block, block_context, gpu, big_page_single, &push);
    }

    uvm_push_end(&push);

    // This will cause the remaps to serialize, but changing the swizzling
    // format is not a performance-critical path.
    uvm_tracker_overwrite_with_push(tracker, &push);
    return NV_OK;
}

// Swizzle or deswizzle big_page_index on resident_gpu. The sequence is:
// - Unmap all GPUs which point to the physical memory
// - Change the swizzling format
// - Remap all GPUs except for skip_remap_gpu (may be NULL)
//
// It's invalid in the programming model for GPUs to access this memory during
// the operation, but the unmap/change/remap sequence is used instead of remap-
// in-place so that if such a bad access occurs, it will fault instead of
// silently accessing incorrect data.
//
// tracker is an in/out parameter and must not be NULL. It is acquired before
// performing any of the above operations, and on return it contains all of the
// work pushed by this overall operation. That work is also added to the block
// tracker.
static NV_STATUS block_gpu_big_page_change_swizzling(uvm_va_block_t *block,
                                                     uvm_va_block_context_t *block_context,
                                                     uvm_gpu_t *resident_gpu,
                                                     uvm_gpu_t *skip_remap_gpu,
                                                     size_t big_page_index,
                                                     uvm_va_block_region_t big_page_region,
                                                     uvm_gpu_swizzle_op_t op,
                                                     uvm_tracker_t *tracker)
{
    uvm_va_block_gpu_state_t *resident_gpu_state = block->gpus[resident_gpu->id - 1];
    uvm_gpu_phys_address_t chunk_phys_addr;
    uvm_processor_mask_t mapped_gpus;
    uvm_gpu_t *mapped_gpu;
    NvU32 pte_alloc_size;
    NV_STATUS status;

    if (op == UVM_GPU_SWIZZLE_OP_SWIZZLE) {
        UVM_ASSERT(!test_bit(big_page_index, resident_gpu_state->big_pages_swizzled));
        pte_alloc_size = uvm_va_block_gpu_big_page_size(block, resident_gpu);
    }
    else {
        UVM_ASSERT(test_bit(big_page_index, resident_gpu_state->big_pages_swizzled));
        pte_alloc_size = UVM_PAGE_SIZE_4K;
    }

    // Note that this mask might be empty
    block_get_mapped_processors(block, resident_gpu->id, big_page_region.first, &mapped_gpus);

    // Step 1: Allocate PTEs up-front on all GPUs which will be changing
    //         formats. This needs to happen first to avoid allocation failures
    //         after we've started changing GPU state.
    for_each_gpu_in_mask(mapped_gpu, &mapped_gpus) {
        status = block_alloc_ptes_with_retry(block, mapped_gpu, pte_alloc_size, tracker);
        if (status != NV_OK)
            return status;
    }

    // We have to wait for prior work in the current operation, because we might
    // have pushed work for these GPUs earlier without adding it to the block
    // tracker under the assumption that we wouldn't push more work for that GPU
    // (for example, uvm_va_block_map_mask).
    //
    // Since all subsequent operations need to wait for both trackers, just
    // merge.
    status = uvm_tracker_add_tracker_safe(tracker, &block->tracker);
    if (status != NV_OK)
        return status;

    // Step 2: Unmap all PTEs in the big page region on all GPUs (4k or big).
    // Note that we're temporarily changing permissions without actually
    // modifying the pte_bits array. This is necessary because we'll need to
    // re-map using the old permissions below, but it means we must not return
    // until we're done (unless we hit a global error).
    for_each_gpu_in_mask(mapped_gpu, &mapped_gpus) {
        status = block_gpu_big_page_change_swizzling_unmap(block,
                                                           block_context,
                                                           mapped_gpu,
                                                           big_page_index,
                                                           big_page_region,
                                                           op,
                                                           tracker);
        if (status != NV_OK) {
            UVM_ASSERT(status == uvm_global_get_status());
            return status;
        }
    }

    // Step 3: Perform the actual swizzle via copy
    chunk_phys_addr = block_phys_page_address(block,
                                              block_phys_page(resident_gpu->id, big_page_region.first),
                                              resident_gpu);
    status = uvm_gpu_swizzle_phys(resident_gpu, chunk_phys_addr.address, op, tracker);
    if (status != NV_OK) {
        UVM_ASSERT(status == uvm_global_get_status());
        return status;
    }

    if (op == UVM_GPU_SWIZZLE_OP_SWIZZLE)
        __set_bit(big_page_index, resident_gpu_state->big_pages_swizzled);
    else
        __clear_bit(big_page_index, resident_gpu_state->big_pages_swizzled);

    // Step 4: Re-map the GPUs using the new PTE size but the same permissions
    for_each_gpu_in_mask(mapped_gpu, &mapped_gpus) {
        // If the caller is going to change permissions on its own, just leave
        // this GPU unmapped.
        if (mapped_gpu == skip_remap_gpu)
            continue;

        status = block_gpu_big_page_change_swizzling_remap(block,
                                                           block_context,
                                                           mapped_gpu,
                                                           big_page_index,
                                                           big_page_region,
                                                           op,
                                                           tracker);
        if (status != NV_OK) {
            UVM_ASSERT(status == uvm_global_get_status());
            return status;
        }
    }

    // Since we pushed work on some GPUs while mapping a different GPU, add the
    // swizzle operation to the block tracker so later map/unmaps which may be
    // part of this whole operation will wait for it.
    return uvm_tracker_add_tracker_safe(&block->tracker, tracker);
}

// Called prior to a mapping operation on mapping_gpu to swizzle those big pages
// which will be mapped as big pages, and deswizzle those big pages which will
// be mapped as 4k pages. The pages are described by
// block_context->mapping.new_pte_state.
//
// Note that although only mapping_gpu will have its permissions changed, this
// function will change the PTEs on other GPUs which map the pages being
// swizzled or deswizzled.
//
// tracker is an in/out parameter.
static NV_STATUS block_gpu_change_swizzling_map(uvm_va_block_t *block,
                                                uvm_va_block_context_t *block_context,
                                                uvm_gpu_t *resident_gpu,
                                                uvm_gpu_t *mapping_gpu,
                                                uvm_tracker_t *tracker)
{
    uvm_va_block_new_pte_state_t *new_pte_state = &block_context->mapping.new_pte_state;
    uvm_va_block_gpu_state_t *resident_gpu_state = block->gpus[resident_gpu->id - 1];
    NvU32 big_page_size = uvm_va_block_gpu_big_page_size(block, mapping_gpu);
    size_t big_page_index;
    NV_STATUS status;
    DECLARE_BITMAP(big_pages_to_swizzle, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    DECLARE_BITMAP(big_pages_to_deswizzle, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    UVM_ASSERT(mapping_gpu->big_page.swizzling);

    // Make sure each big PTE to map is swizzled. Unmapped PTEs which are
    // currently big might not be swizzled, so check swizzling regardless of the
    // current state.
    bitmap_and(big_pages_to_swizzle,
               new_pte_state->big_ptes_covered,
               new_pte_state->big_ptes,
               MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    bitmap_andnot(big_pages_to_swizzle,
                  big_pages_to_swizzle,
                  resident_gpu_state->big_pages_swizzled,
                  MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    for_each_set_bit(big_page_index, big_pages_to_swizzle, MAX_BIG_PAGES_PER_UVM_VA_BLOCK) {
        status = block_gpu_big_page_change_swizzling(block,
                                                     block_context,
                                                     resident_gpu,
                                                     mapping_gpu,
                                                     big_page_index,
                                                     uvm_va_block_big_page_region(block, big_page_index, big_page_size),
                                                     UVM_GPU_SWIZZLE_OP_SWIZZLE,
                                                     tracker);
        if (status != NV_OK)
            return status;
    }

    // Deswizzle pages which will have 4k mappings:
    // covered && !big_after && swizzled
    bitmap_andnot(big_pages_to_deswizzle,
                  new_pte_state->big_ptes_covered,
                  new_pte_state->big_ptes,
                  MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    bitmap_and(big_pages_to_deswizzle,
               big_pages_to_deswizzle,
               resident_gpu_state->big_pages_swizzled,
               MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    for_each_set_bit(big_page_index, big_pages_to_deswizzle, MAX_BIG_PAGES_PER_UVM_VA_BLOCK) {
        status = block_gpu_big_page_change_swizzling(block,
                                                     block_context,
                                                     resident_gpu,
                                                     mapping_gpu,
                                                     big_page_index,
                                                     uvm_va_block_big_page_region(block, big_page_index, big_page_size),
                                                     UVM_GPU_SWIZZLE_OP_DESWIZZLE,
                                                     tracker);
        if (status != NV_OK)
            return status;
    }

    return NV_OK;
}

// Called prior to an unmap operation on mapping_gpu to deswizzle those big
// pages which will be mapped as 4k pages. The pages are described by
// block_context->mapping.new_pte_state.
//
// Note that although only mapping_gpu will have its permissions changed, this
// function will change the PTEs on other GPUs which map the pages being
// swizzled or deswizzled.
//
// tracker is an in/out parameter.
static NV_STATUS block_gpu_change_swizzling_unmap(uvm_va_block_t *block,
                                                  uvm_va_block_context_t *block_context,
                                                  uvm_gpu_t *mapping_gpu,
                                                  uvm_tracker_t *tracker)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[mapping_gpu->id - 1];
    uvm_va_block_new_pte_state_t *new_pte_state = &block_context->mapping.new_pte_state;
    uvm_va_block_region_t big_page_region;
    uvm_gpu_t *resident_gpu;
    NvU32 big_page_size = uvm_va_block_gpu_big_page_size(block, mapping_gpu);
    size_t big_page_index;
    NV_STATUS status;
    DECLARE_BITMAP(big_pages_to_deswizzle, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    UVM_ASSERT(mapping_gpu->big_page.swizzling);

    // In general we avoid changing the swizzling format as much as possible on
    // unmap, because we want to avoid allocations on this and other GPUs during
    // teardown. When fully unmapping this GPU for example, we won't change the
    // swizzling state since this GPU can no longer access the data.
    //
    // In theory, if we're fully unmapping a big page region which was partially
    // mapped by this GPU before, we could swizzle since other GPUs could be
    // promoted to big PTEs. That optimization is not worthwhile given how rare
    // it is to have partial mappings on swizzled (Kepler) GPUs.
    //
    // The only time we're forced to change formats is when partially unmapping
    // a swizzled big page, in which case we must deswizzle.

    // A partially-unmapped swizzled big page is:
    // covered && !fully_unmapped && big_before
    bitmap_andnot(big_pages_to_deswizzle,
                  new_pte_state->big_ptes_covered,
                  new_pte_state->big_ptes_fully_unmapped,
                  MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    bitmap_and(big_pages_to_deswizzle, big_pages_to_deswizzle, gpu_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

    for_each_set_bit(big_page_index, big_pages_to_deswizzle, MAX_BIG_PAGES_PER_UVM_VA_BLOCK) {
        big_page_region = uvm_va_block_big_page_region(block, big_page_index, big_page_size);

        // Since this is currently a big PTE, it must have a single residency
        // and it must not be the CPU.
        resident_gpu = uvm_gpu_get(block_gpu_get_processor_to_map(block, mapping_gpu, big_page_region.first));

        status = block_gpu_big_page_change_swizzling(block,
                                                     block_context,
                                                     resident_gpu,
                                                     mapping_gpu,
                                                     big_page_index,
                                                     big_page_region,
                                                     UVM_GPU_SWIZZLE_OP_DESWIZZLE,
                                                     tracker);
        if (status != NV_OK)
            return status;
    }

    return NV_OK;
}

static NV_STATUS uvm_va_block_unmap_gpu(uvm_va_block_t *block,
                                        uvm_va_block_context_t *block_context,
                                        uvm_gpu_t *gpu,
                                        const unsigned long *unmap_page_mask,
                                        uvm_tracker_t *out_tracker)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_pte_bits_gpu_t pte_bit;
    uvm_push_t push;
    uvm_membar_t tlb_membar = UVM_MEMBAR_GPU;
    unsigned long *pages_to_unmap = block_context->mapping.page_mask;
    NV_STATUS status;
    uvm_va_block_new_pte_state_t *new_pte_state = &block_context->mapping.new_pte_state;
    bool mask_empty;

    // We have to check gpu_state before looking at any VA space state like our
    // gpu_va_space, because we could be on the eviction path where we don't
    // have a lock on that state. However, since remove_gpu_va_space walks each
    // block to unmap the GPU before destroying the gpu_va_space, we're
    // guaranteed that if this GPU has page tables, the gpu_va_space can't go
    // away while we're holding the block lock.
    if (!block_gpu_has_page_tables(block, gpu))
        return NV_OK;

    if (!uvm_page_mask_and(pages_to_unmap, unmap_page_mask, gpu_state->pte_bits[UVM_PTE_BITS_GPU_READ]))
        return NV_OK;

    // block_gpu_compute_new_pte_state needs a mask of pages which will have
    // matching attributes after the operation is performed. In the case of
    // unmap, those are the pages with unset bits.
    uvm_page_mask_andnot(block_context->scratch_page_mask, gpu_state->pte_bits[UVM_PTE_BITS_GPU_READ], pages_to_unmap);
    uvm_page_mask_complement(block_context->scratch_page_mask, block_context->scratch_page_mask);
    block_gpu_compute_new_pte_state(block,
                                    gpu,
                                    UVM8_MAX_PROCESSORS,
                                    pages_to_unmap,
                                    block_context->scratch_page_mask,
                                    new_pte_state);

    status = block_alloc_ptes_new_state(block, gpu, new_pte_state, out_tracker);
    if (status != NV_OK)
        return status;

    if (gpu->big_page.swizzling) {
        status = block_gpu_change_swizzling_unmap(block, block_context, gpu, out_tracker);
        if (status != NV_OK)
            return status;
    }

    // All PTE downgrades need a membar. If any of the unmapped PTEs pointed to
    // remote memory, we must use a sysmembar.
    if (block_has_remote_mapping_gpu(block, block_context, gpu->id, pages_to_unmap))
        tlb_membar = UVM_MEMBAR_SYS;

    status = uvm_push_begin_acquire(gpu->channel_manager,
                                    UVM_CHANNEL_TYPE_MEMOPS,
                                    &block->tracker,
                                    &push,
                                    "Unmapping pages in block [0x%llx, 0x%llx)",
                                    block->start,
                                    block->end + 1);
    if (status != NV_OK)
        return status;

    if (new_pte_state->pte_is_2m) {
        // We're either unmapping a whole valid 2M PTE, or we're unmapping all
        // remaining pages in a split 2M PTE.
        block_gpu_unmap_to_2m(block, block_context, gpu, &push, tlb_membar);
    }
    else if (gpu_state->pte_is_2m) {
        // The block is currently mapped as a valid 2M PTE and we're unmapping
        // some pages within the 2M, so we have to split it into the appropriate
        // mix of big and 4k PTEs.
        block_gpu_unmap_split_2m(block, block_context, gpu, pages_to_unmap, &push, tlb_membar);
    }
    else {
        // We're unmapping some pre-existing mix of big and 4K PTEs into some
        // other mix of big and 4K PTEs.
        block_gpu_unmap_big_and_4k(block, block_context, gpu, pages_to_unmap, &push, tlb_membar);
    }

    uvm_push_end(&push);

    // Clear block PTE state
    for (pte_bit = 0; pte_bit < UVM_PTE_BITS_GPU_MAX; pte_bit++) {
        mask_empty = !uvm_page_mask_andnot(gpu_state->pte_bits[pte_bit], gpu_state->pte_bits[pte_bit], pages_to_unmap);
        if (pte_bit == UVM_PTE_BITS_GPU_READ && mask_empty)
            uvm_processor_mask_clear(&block->mapped, gpu->id);
    }

    UVM_ASSERT(block_check_mappings(block));

    return uvm_tracker_add_push_safe(out_tracker, &push);
}

NV_STATUS uvm_va_block_unmap(uvm_va_block_t *va_block,
                             uvm_va_block_context_t *va_block_context,
                             uvm_processor_id_t id,
                             uvm_va_block_region_t region,
                             const unsigned long *unmap_page_mask,
                             uvm_tracker_t *out_tracker)
{
    uvm_va_range_t *va_range = va_block->va_range;
    unsigned long *region_page_mask = va_block_context->mapping.running_page_mask;

    UVM_ASSERT(va_range);
    UVM_ASSERT(va_range->type == UVM_VA_RANGE_TYPE_MANAGED);
    uvm_assert_mutex_locked(&va_block->lock);

    if (id == UVM_CPU_ID) {
       uvm_va_block_unmap_cpu(va_block, region, unmap_page_mask);
       return NV_OK;
    }

    uvm_page_mask_init_from_region(region_page_mask, region, unmap_page_mask);

    return uvm_va_block_unmap_gpu(va_block, va_block_context, uvm_gpu_get(id), region_page_mask, out_tracker);
}

// This function essentially works as a wrapper around vm_insert_page (hence
// the similar function prototype). This is needed since vm_insert_page
// doesn't take permissions as input, but uses vma->vm_page_prot instead.
// Since we may have multiple VA blocks under one VMA which need to map
// with different permissions, we have to manually change vma->vm_page_prot for
// each call to vm_insert_page. Multiple faults under one VMA in separate
// blocks can be serviced concurrently, so the VMA wrapper lock is used
// to protect access to vma->vm_page_prot.
static NV_STATUS uvm_cpu_insert_page(struct vm_area_struct *vma,
                                     NvU64 addr,
                                     struct page *page,
                                     uvm_prot_t new_prot)
{
    uvm_vma_wrapper_t *vma_wrapper;
    unsigned long target_flags;
    pgprot_t target_pgprot;
    int ret;

    UVM_ASSERT(vma);
    UVM_ASSERT(vma->vm_private_data);

    vma_wrapper = vma->vm_private_data;
    target_flags = vma->vm_flags;

    if (new_prot == UVM_PROT_READ_ONLY)
        target_flags &= ~VM_WRITE;

    target_pgprot = vm_get_page_prot(target_flags);

    // Take VMA wrapper lock to check vma->vm_page_prot
    uvm_down_read(&vma_wrapper->lock);

    // Take a write lock if we need to modify the VMA vm_page_prot
    // - vma->vm_page_prot creates writable PTEs but new prot is RO
    // - vma->vm_page_prot creates read-only PTEs but new_prot is RW
    if (pgprot_val(vma->vm_page_prot) != pgprot_val(target_pgprot)) {
        uvm_up_read(&vma_wrapper->lock);
        uvm_down_write(&vma_wrapper->lock);

        vma->vm_page_prot = target_pgprot;

        uvm_downgrade_write(&vma_wrapper->lock);
    }

    ret = vm_insert_page(vma, addr, page);
    uvm_up_read(&vma_wrapper->lock);
    if (ret) {
        UVM_ASSERT_MSG(ret == -ENOMEM, "ret: %d\n", ret);
        return errno_to_nv_status(ret);
    }

    return NV_OK;
}

// Helper for both block_map_cpu_page (upgrade) and block_revoke_cpu_write_page
// (downgrade). See those functions for details.
//
// This always performs an unmap first if page_index is already mapped
// (that is, curr_prot != UVM_PROT_NONE).
static NV_STATUS block_cpu_insert_page(uvm_va_block_t *block,
                                       size_t page_index,
                                       uvm_prot_t curr_prot,
                                       uvm_prot_t new_prot)
{
    uvm_va_range_t *va_range = block->va_range;
    struct vm_area_struct *vma;
    NV_STATUS status;
    NvU64 addr = block->start + page_index * PAGE_SIZE;

    UVM_ASSERT(va_range);
    UVM_ASSERT(va_range->type == UVM_VA_RANGE_TYPE_MANAGED);
    UVM_ASSERT(new_prot != UVM_PROT_NONE);
    UVM_ASSERT(new_prot < UVM_PROT_MAX);
    UVM_ASSERT(curr_prot == block_page_prot_cpu(block, page_index));

    // It's possible here that current->mm != vma->vm_mm. That can happen for
    // example due to access_process_vm (ptrace) or get_user_pages from another
    // driver. In those cases the caller has taken care of ref counting
    // vma->vm_mm for us, so we can safely operate on the vma, but we can't use
    // uvm_va_range_vma_current.
    vma = uvm_va_range_vma(va_range);
    uvm_assert_mmap_sem_locked(&vma->vm_mm->mmap_sem);
    uvm_assert_mutex_locked(&block->lock);
    UVM_ASSERT(block->cpu.pages[page_index]);

    // For the CPU, write implies atomic
    if (new_prot == UVM_PROT_READ_WRITE)
        new_prot = UVM_PROT_READ_WRITE_ATOMIC;

    // This unmap handles both the force_remap case of block_map_cpu_page and a
    // special case required by vm_insert_page. vm_insert_page returns -EBUSY
    // when there's already a mapping present at fault_addr, so we have to unmap
    // first anyway when upgrading from RO -> RW or downgrading from RW -> RO.
    if (curr_prot != UVM_PROT_NONE)
        unmap_mapping_range(&va_range->va_space->mapping, addr, PAGE_SIZE, 1);

    // Don't map the CPU until prior copies and GPU PTE updates finish,
    // otherwise we might not stay coherent.
    status = uvm_tracker_wait(&block->tracker);
    if (status != NV_OK)
        return status;

    status = uvm_cpu_insert_page(vma, addr, block->cpu.pages[page_index], new_prot);
    if (status != NV_OK)
        return status;

    // Update block mapping state
    if (curr_prot != new_prot) {
        // Transitioning from Invalid -> RO or Invalid -> RW
        if (curr_prot == UVM_PROT_NONE) {
            uvm_processor_mask_set(&block->mapped, UVM_CPU_ID);
            __set_bit(page_index, block->cpu.pte_bits[UVM_PTE_BITS_CPU_READ]);
        }

        // Transitioning from Invalid -> RW or RO -> RW
        if (new_prot == UVM_PROT_READ_WRITE_ATOMIC)
            __set_bit(page_index, block->cpu.pte_bits[UVM_PTE_BITS_CPU_WRITE]);

        // On RW -> RO transition, clear UVM_PTE_BITS_CPU_WRITE bit
        if (curr_prot == UVM_PROT_READ_WRITE_ATOMIC && new_prot == UVM_PROT_READ_ONLY)
            __clear_bit(page_index, block->cpu.pte_bits[UVM_PTE_BITS_CPU_WRITE]);
    }

    UVM_ASSERT(block_check_mappings_page(block, page_index));

    return NV_OK;
}

// Creates or upgrades a CPU mapping for the given page, updating the block's
// mapping and pte_bits bitmaps as appropriate. Upon sucessful return, the page
// will be mapped with at least new_prot permissions.
//
// This never downgrades mappings, so new_prot must not be UVM_PROT_NONE. Use
// uvm_va_block_unmap or block_revoke_cpu_write_page instead.
//
// If the existing mapping is >= new_prot already, the behavior depends on
// force_remap. If force_remap is false this is a no-op. If force_remap is true
// the existing CPU mapping is unmapped first and max(new_prot, curr_prot) is
// re-inserted. This handles cases in which the block tracking goes stale
// because the kernel unmapped the CPU behind our back.
//
// It is the caller's responsibility to:
//  - Revoke mappings from other processors as appropriate so the CPU can map
//    with new_prot permissions
//  - Guarantee that vm_insert_page is safe to use (vma->vm_mm has a reference
//    and mmap_sem is held in at least read mode)
//  - Ensure that the CPU page exists in block->cpu.pages
//  - Manage the block's residency bitmap
//  - Decide whether force_remap is appropriate
//  - Ensure that the block hasn't been killed (block->va_range is present)
static NV_STATUS block_map_cpu_page(uvm_va_block_t *block, size_t page_index, uvm_prot_t new_prot, bool force_remap)
{
    uvm_prot_t curr_prot = block_page_prot_cpu(block, page_index);

    // For the CPU, write implies atomic
    if (new_prot == UVM_PROT_READ_WRITE)
        new_prot = UVM_PROT_READ_WRITE_ATOMIC;

    if (!force_remap && new_prot <= curr_prot)
        return NV_OK;

    // In the case of force_remap where curr_prot == RW and new_prot == RO, we
    // might as well keep the mapping as RW.
    if (force_remap)
        new_prot = max(curr_prot, new_prot);

    // Check for existing VMA permissions. They could have been modified after
    // the initial mmap by mprotect.
    if (new_prot > uvm_va_range_logical_prot(block->va_range))
        return NV_ERR_INVALID_ACCESS_TYPE;

    if (block->va_range->preferred_location == UVM_CPU_ID) {
        // Add the page's range group range to the range group's migrated list.
        uvm_range_group_range_t *rgr = uvm_range_group_range_find(block->va_range->va_space,
                                                                  block->start + PAGE_SIZE * page_index);
        if (rgr != NULL) {
            uvm_spin_lock(&rgr->range_group->migrated_ranges_lock);
            if (list_empty(&rgr->range_group_migrated_list_node))
                list_move_tail(&rgr->range_group_migrated_list_node, &rgr->range_group->migrated_ranges);
            uvm_spin_unlock(&rgr->range_group->migrated_ranges_lock);
        }
    }

    return block_cpu_insert_page(block, page_index, curr_prot, new_prot);
}

// See uvm_va_block_revoke_prot for details.
static NV_STATUS block_revoke_cpu_write_page(uvm_va_block_t *block, size_t page_index)
{
    uvm_prot_t curr_prot = block_page_prot_cpu(block, page_index);

    if (curr_prot < UVM_PROT_READ_WRITE_ATOMIC)
        return NV_OK;

    // We can only remap as read-only if we hold the mmap_sem lock (assumed if
    // vma->vm_mm == current->mm). Otherwise we have to unmap the page entirely.
    if (uvm_va_range_vma_current(block->va_range))
        return block_cpu_insert_page(block, page_index, UVM_PROT_READ_WRITE_ATOMIC, UVM_PROT_READ_ONLY);

    uvm_va_block_unmap_cpu(block, uvm_va_block_region(page_index, page_index + 1), NULL);
    return NV_OK;
}

// See the comments on uvm_va_block_map
static NV_STATUS uvm_va_block_map_cpu(uvm_va_block_t *va_block,
                                      uvm_va_block_region_t region,
                                      const unsigned long *map_pages,
                                      uvm_prot_t new_prot)
{
    NV_STATUS status;
    size_t page_index;

    if (!map_pages)
        map_pages = va_block->cpu.resident;

    // TODO: Bug 1766424: Check if optimizing the unmap_mapping_range calls
    //       within block_map_cpu_page by doing them once here is helpful.

    // TODO: Bug 1766424: Could optimize this substantially for already-mapped
    //       regions by scanning the CPU bit vectors for pages without the
    //       intended permissions.

    for_each_va_block_page_in_mask(page_index, map_pages, region) {
        // Don't set force_remap because we'll take a CPU fault if we get this
        // wrong, and the CPU fault handler will take care of it.
        status = block_map_cpu_page(va_block, page_index, new_prot, false);
        if (status != NV_OK)
            return status;
    }

    return NV_OK;
}

// Maps the given pages on gpu which are resident on resident_id. map_page_mask
// is an in/out parameter: the pages which are mapped to resident_id are removed
// from the mask before returning.
static NV_STATUS uvm_va_block_map_gpu_to(uvm_va_block_t *va_block,
                                         uvm_va_block_context_t *block_context,
                                         uvm_gpu_t *gpu,
                                         uvm_processor_id_t resident_id,
                                         unsigned long *map_page_mask,
                                         uvm_prot_t new_prot,
                                         UvmEventMapRemoteCause cause,
                                         uvm_tracker_t *out_tracker)
{
    uvm_va_block_gpu_state_t *gpu_state = block_gpu_state_get_alloc(va_block, gpu);
    uvm_va_range_t *va_range = va_block->va_range;
    uvm_va_space_t *va_space = va_range->va_space;
    uvm_push_t push;
    NV_STATUS status;
    unsigned long *pages_to_map = block_context->mapping.page_mask;
    unsigned long *resident_mask = uvm_va_block_resident_mask_get(va_block, resident_id);
    uvm_pte_bits_gpu_t pte_bit, prot_pte_bit = get_gpu_pte_bit_index(new_prot);
    uvm_va_block_new_pte_state_t *new_pte_state = &block_context->mapping.new_pte_state;
    block_pte_op_t pte_op;

    UVM_ASSERT(uvm_processor_mask_test(&va_space->accessible_from[resident_id], gpu->id));
    if (uvm_processor_mask_test(&va_range->uvm_lite_gpus, gpu->id))
        UVM_ASSERT(resident_id == va_range->preferred_location);

    // The pages which will actually change are those in the input page mask
    // which don't already have the required permissions and are resident on the
    // target.
    uvm_page_mask_andnot(pages_to_map, map_page_mask, gpu_state->pte_bits[prot_pte_bit]);
    if (!uvm_page_mask_and(pages_to_map, pages_to_map, resident_mask)) {
        // Remove all pages resident on this processor from the input mask since
        // they already have the correct permissions.
        uvm_page_mask_andnot(map_page_mask, map_page_mask, resident_mask);
        return NV_OK;
    }

    UVM_ASSERT(block_check_mapping_residency(va_block, gpu, resident_id, pages_to_map));

    // For PTE merge/split computation, compute all resident pages which will
    // have exactly new_prot after performing the mapping.
    uvm_page_mask_or(block_context->scratch_page_mask, gpu_state->pte_bits[prot_pte_bit], pages_to_map);
    if (prot_pte_bit < UVM_PTE_BITS_GPU_ATOMIC) {
        uvm_page_mask_andnot(block_context->scratch_page_mask,
                             block_context->scratch_page_mask,
                             gpu_state->pte_bits[prot_pte_bit + 1]);
    }
    uvm_page_mask_and(block_context->scratch_page_mask, block_context->scratch_page_mask, resident_mask);

    block_gpu_compute_new_pte_state(va_block,
                                    gpu,
                                    resident_id,
                                    pages_to_map,
                                    block_context->scratch_page_mask,
                                    new_pte_state);

    status = block_alloc_ptes_new_state(va_block, gpu, new_pte_state, out_tracker);
    if (status != NV_OK)
        return status;

    if (gpu->big_page.swizzling && resident_id != UVM_CPU_ID) {
        status = block_gpu_change_swizzling_map(va_block, block_context, uvm_gpu_get(resident_id), gpu, out_tracker);
        if (status != NV_OK)
            return status;
    }

    status = uvm_push_begin_acquire(gpu->channel_manager,
                                    UVM_CHANNEL_TYPE_MEMOPS,
                                    &va_block->tracker,
                                    &push,
                                    "Mapping pages in block [0x%llx, 0x%llx) as %s",
                                    va_block->start,
                                    va_block->end + 1,
                                    uvm_prot_string(new_prot));
    if (status != NV_OK)
        return status;

    pte_op = BLOCK_PTE_OP_MAP;
    if (new_pte_state->pte_is_2m) {
        // We're either modifying permissions of a pre-existing 2M PTE, or all
        // permissions match so we can merge to a new 2M PTE.
        block_gpu_map_to_2m(va_block, block_context, gpu, resident_id, new_prot, &push, pte_op);
    }
    else if (gpu_state->pte_is_2m) {
        // Permissions on a subset of the existing 2M PTE are being upgraded, so
        // we have to split it into the appropriate mix of big and 4k PTEs.
        block_gpu_map_split_2m(va_block, block_context, gpu, resident_id, pages_to_map, new_prot, &push, pte_op);
    }
    else {
        // We're upgrading permissions on some pre-existing mix of big and 4K
        // PTEs into some other mix of big and 4K PTEs.
        block_gpu_map_big_and_4k(va_block, block_context, gpu, resident_id, pages_to_map, new_prot, &push, pte_op);
    }

    uvm_push_end(&push);

    // If we are mapping remotely, record the event
    if (va_space->tools.enabled && resident_id != gpu->id && cause != UvmEventMapRemoteCauseInvalid) {
        uvm_va_block_region_t subregion, region = uvm_va_block_region_from_block(va_block);
        for_each_va_block_subregion_in_mask(subregion, pages_to_map, region) {
            uvm_tools_record_map_remote(va_block,
                                        gpu->id,
                                        resident_id,
                                        uvm_va_block_region_start(va_block, subregion),
                                        uvm_va_block_region_size(subregion),
                                        cause);
        }
    }

    // Update GPU mapping state
    for (pte_bit = 0; pte_bit <= prot_pte_bit; pte_bit++)
        uvm_page_mask_or(gpu_state->pte_bits[pte_bit], gpu_state->pte_bits[pte_bit], pages_to_map);
    uvm_processor_mask_set(&va_block->mapped, gpu->id);

    // Remove all pages resident on this processor from the input mask, both
    // pages which were newly-mapped and pages which already had the correct
    // permissions.
    uvm_page_mask_andnot(map_page_mask, map_page_mask, resident_mask);

    UVM_ASSERT(block_check_mappings(va_block));

    return uvm_tracker_add_push_safe(out_tracker, &push);
}

static void map_gpu_get_allowed_destinations(uvm_va_block_t *block, uvm_gpu_t *gpu, uvm_processor_mask_t *allowed_mask)
{
    uvm_va_range_t *va_range = block->va_range;
    uvm_va_space_t *va_space = va_range->va_space;

    if (uvm_processor_mask_test(&va_range->uvm_lite_gpus, gpu->id)) {
        // UVM-Lite can only map resident pages on the preferred location
        uvm_processor_mask_zero(allowed_mask);
        uvm_processor_mask_set(allowed_mask, va_range->preferred_location);
    }
    else if ((va_range->read_duplication == UVM_READ_DUPLICATION_ENABLED &&
              uvm_va_space_can_read_duplicate(va_space, NULL)) ||
             va_range->preferred_location == gpu->id) {

        // When operating under read-duplication we should only map the local
        // GPU to cause fault-and-duplicate of remote pages.
        //
        // The same holds when this GPU is the preferred location: only create
        // local mappings to force remote pages to fault-and-migrate.
        uvm_processor_mask_zero(allowed_mask);
        uvm_processor_mask_set(allowed_mask, gpu->id);
    }
    else {
        // Common case: Just map wherever the memory happens to reside
        uvm_processor_mask_and(allowed_mask, &block->resident, &va_space->can_access[gpu->id]);
        return;
    }

    // Clamp to resident and accessible processors
    uvm_processor_mask_and(allowed_mask, allowed_mask, &block->resident);
    uvm_processor_mask_and(allowed_mask, allowed_mask, &va_space->can_access[gpu->id]);
}

NV_STATUS uvm_va_block_map(uvm_va_block_t *va_block,
                           uvm_va_block_context_t *va_block_context,
                           uvm_processor_id_t id,
                           uvm_va_block_region_t region,
                           const unsigned long *map_page_mask,
                           uvm_prot_t new_prot,
                           UvmEventMapRemoteCause cause,
                           uvm_tracker_t *out_tracker)
{
    uvm_va_range_t *va_range = va_block->va_range;
    uvm_va_space_t *va_space;
    uvm_gpu_t *gpu;
    uvm_processor_mask_t allowed_destinations;
    uvm_processor_id_t resident_id;
    unsigned long *running_page_mask = va_block_context->mapping.running_page_mask;
    NV_STATUS status;

    UVM_ASSERT(new_prot != UVM_PROT_NONE);
    UVM_ASSERT(new_prot < UVM_PROT_MAX);
    uvm_assert_mutex_locked(&va_block->lock);
    UVM_ASSERT(va_range);

    va_space = va_range->va_space;

    // Mapping is not supported on the eviction path that doesn't hold the VA
    // space lock.
    uvm_assert_rwsem_locked(&va_space->lock);

    if (id == UVM_CPU_ID) {
        if (uvm_va_range_vma_current(va_range))
            return uvm_va_block_map_cpu(va_block, region, map_page_mask, new_prot);
        return NV_OK;
    }

    gpu = uvm_gpu_get(id);

    // Although this GPU UUID is registered in the VA space, it might not have a
    // GPU VA space registered.
    if (!uvm_gpu_va_space_get(va_space, gpu))
        return NV_OK;

    uvm_page_mask_init_from_region(running_page_mask, region, map_page_mask);

    // Map per resident location so we can more easily detect physically-
    // contiguous mappings.
    map_gpu_get_allowed_destinations(va_block, gpu, &allowed_destinations);

    for_each_closest_id(resident_id, &allowed_destinations, gpu->id, va_space) {
        status = uvm_va_block_map_gpu_to(va_block,
                                         va_block_context,
                                         gpu,
                                         resident_id,
                                         running_page_mask,
                                         new_prot,
                                         cause,
                                         out_tracker);
        if (status != NV_OK)
            return status;

        // If we've mapped all requested pages, we're done
        if (uvm_page_mask_region_empty(running_page_mask, region))
            break;
    }

    return NV_OK;
}

static NV_STATUS uvm_va_block_revoke_cpu_write(uvm_va_block_t *va_block,
                                               uvm_va_block_context_t *block_context,
                                               uvm_va_block_region_t region,
                                               const unsigned long *revoke_page_mask)
{
    NvU64 page_addr;
    NV_STATUS status;
    size_t page_index;
    unsigned long *final_page_mask;

    // Return early if there are no CPU mappings present in the block
    if (!uvm_processor_mask_test(&va_block->mapped, UVM_CPU_ID))
        return NV_OK;

    if (revoke_page_mask) {
        final_page_mask = block_context->mapping.page_mask;
        uvm_page_mask_and(final_page_mask, va_block->cpu.pte_bits[UVM_PTE_BITS_CPU_WRITE], revoke_page_mask);
    }
    else {
        final_page_mask = va_block->cpu.pte_bits[UVM_PTE_BITS_CPU_WRITE];
    }

    // Iterate over the pages that need to be downgraded, only
    for_each_va_block_page_in_mask(page_index, final_page_mask, region) {
        uvm_perf_event_data_t event_data;
        page_addr = va_block->start + page_index * PAGE_SIZE;

        event_data = (uvm_perf_event_data_t)
            {
                .revocation =
                    {
                        .block = va_block,
                        .proc_id = UVM_CPU_ID,
                        .address = page_addr,
                        .bytes   = PAGE_SIZE,
                        .old_prot = UVM_PROT_READ_WRITE_ATOMIC,
                        .new_prot = UVM_PROT_READ_ONLY,
                    }
            };

        // TODO: Bug 1785853: Coalesce events on contiguous pages
        uvm_perf_event_notify(&va_block->va_range->va_space->perf_events, UVM_PERF_EVENT_REVOCATION, &event_data);
        status = block_revoke_cpu_write_page(va_block, page_index);
        if (status != NV_OK)
            return status;
    }

    return NV_OK;
}

static void block_revoke_prot_gpu_perf_notify(uvm_va_block_t *block,
                                              uvm_va_block_context_t *block_context,
                                              uvm_gpu_t *gpu,
                                              uvm_prot_t prot_revoked,
                                              const unsigned long *pages_revoked)
{
    uvm_va_space_t *va_space = block->va_range->va_space;
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[gpu->id - 1];
    uvm_va_block_region_t subregion, region = uvm_va_block_region_from_block(block);
    uvm_pte_bits_gpu_t pte_bit;
    uvm_perf_event_data_t event_data = (uvm_perf_event_data_t)
        {
            .revocation =
                {
                    .block = block,
                    .proc_id = gpu->id,
                    .new_prot = prot_revoked - 1,
                }
        };

    for (pte_bit = UVM_PTE_BITS_GPU_ATOMIC; pte_bit >= get_gpu_pte_bit_index(prot_revoked); pte_bit--) {
        if (!uvm_page_mask_and(block_context->scratch_page_mask, gpu_state->pte_bits[pte_bit], pages_revoked))
            continue;

        if (pte_bit == UVM_PTE_BITS_GPU_ATOMIC)
            event_data.revocation.old_prot = UVM_PROT_READ_WRITE_ATOMIC;
        else
            event_data.revocation.old_prot = UVM_PROT_READ_WRITE;

        for_each_va_block_subregion_in_mask(subregion, block_context->scratch_page_mask, region) {
            event_data.revocation.address = uvm_va_block_region_start(block, subregion);
            event_data.revocation.bytes   = uvm_va_block_region_size(subregion);
            uvm_perf_event_notify(&va_space->perf_events, UVM_PERF_EVENT_REVOCATION, &event_data);
        }
    }
}

// Revokes the given pages mapped by gpu which are resident on resident_id.
// revoke_page_mask is an in/out parameter: the pages which have the appropriate
// permissions and are mapped to resident_id are removed from the mask before
// returning.
static NV_STATUS uvm_va_block_revoke_prot_gpu_to(uvm_va_block_t *va_block,
                                                 uvm_va_block_context_t *block_context,
                                                 uvm_gpu_t *gpu,
                                                 uvm_processor_id_t resident_id,
                                                 unsigned long *revoke_page_mask,
                                                 uvm_prot_t prot_to_revoke,
                                                 uvm_tracker_t *out_tracker)
{
    uvm_va_block_gpu_state_t *gpu_state = va_block->gpus[gpu->id - 1];
    uvm_push_t push;
    NV_STATUS status;
    uvm_pte_bits_gpu_t pte_bit, search_pte_type = UVM_PTE_BITS_GPU_WRITE;
    uvm_prot_t new_prot = prot_to_revoke - 1;
    uvm_va_block_new_pte_state_t *new_pte_state = &block_context->mapping.new_pte_state;
    block_pte_op_t pte_op;
    unsigned long *resident_mask = uvm_va_block_resident_mask_get(va_block, resident_id);
    unsigned long *pages_to_revoke = block_context->mapping.page_mask;

    if (prot_to_revoke == UVM_PROT_READ_WRITE_ATOMIC)
        search_pte_type = UVM_PTE_BITS_GPU_ATOMIC;

    // The pages which will actually change are those in the input page mask
    // which have the required permissions and are resident on the target.
    uvm_page_mask_and(pages_to_revoke, revoke_page_mask, gpu_state->pte_bits[search_pte_type]);
    if (!uvm_page_mask_and(pages_to_revoke, pages_to_revoke, resident_mask)) {
        // Remove all pages resident on this processor from the input mask since
        // they already have the correct permissions.
        uvm_page_mask_andnot(revoke_page_mask, revoke_page_mask, resident_mask);
        return NV_OK;
    }

    UVM_ASSERT(block_check_mapping_residency(va_block, gpu, resident_id, pages_to_revoke));

    // For PTE merge/split computation, compute all resident pages which will
    // have exactly prot_to_revoke-1 after performing the revocation.
    uvm_page_mask_andnot(block_context->scratch_page_mask, gpu_state->pte_bits[search_pte_type], pages_to_revoke);
    uvm_page_mask_andnot(block_context->scratch_page_mask,
                         gpu_state->pte_bits[search_pte_type - 1],
                         block_context->scratch_page_mask);
    uvm_page_mask_and(block_context->scratch_page_mask, block_context->scratch_page_mask, resident_mask);

    block_gpu_compute_new_pte_state(va_block,
                                    gpu,
                                    resident_id,
                                    pages_to_revoke,
                                    block_context->scratch_page_mask,
                                    new_pte_state);

    status = block_alloc_ptes_new_state(va_block, gpu, new_pte_state, out_tracker);
    if (status != NV_OK)
        return status;

    if (gpu->big_page.swizzling && resident_id != UVM_CPU_ID) {
        status = block_gpu_change_swizzling_map(va_block, block_context, uvm_gpu_get(resident_id), gpu, out_tracker);
        if (status != NV_OK)
            return status;
    }

    status = uvm_push_begin_acquire(gpu->channel_manager,
                                    UVM_CHANNEL_TYPE_MEMOPS,
                                    &va_block->tracker,
                                    &push,
                                    "Revoking %s access privileges in block [0x%llx, 0x%llx) ",
                                    uvm_prot_string(prot_to_revoke),
                                    va_block->start,
                                    va_block->end + 1);
    if (status != NV_OK)
        return status;

    pte_op = BLOCK_PTE_OP_REVOKE;
    if (new_pte_state->pte_is_2m) {
        // We're either modifying permissions of a pre-existing 2M PTE, or all
        // permissions match so we can merge to a new 2M PTE.
        block_gpu_map_to_2m(va_block, block_context, gpu, resident_id, new_prot, &push, pte_op);
    }
    else if (gpu_state->pte_is_2m) {
        // Permissions on a subset of the existing 2M PTE are being downgraded,
        // so we have to split it into the appropriate mix of big and 4k PTEs.
        block_gpu_map_split_2m(va_block, block_context, gpu, resident_id, pages_to_revoke, new_prot, &push, pte_op);
    }
    else {
        // We're downgrading permissions on some pre-existing mix of big and 4K
        // PTEs into some other mix of big and 4K PTEs.
        block_gpu_map_big_and_4k(va_block, block_context, gpu, resident_id, pages_to_revoke, new_prot, &push, pte_op);
    }

    uvm_push_end(&push);

    block_revoke_prot_gpu_perf_notify(va_block, block_context, gpu, prot_to_revoke, pages_to_revoke);

    // Update GPU mapping state
    for (pte_bit = UVM_PTE_BITS_GPU_ATOMIC; pte_bit >= search_pte_type; pte_bit--)
        uvm_page_mask_andnot(gpu_state->pte_bits[pte_bit], gpu_state->pte_bits[pte_bit], pages_to_revoke);

    // Remove all pages resident on this processor from the input mask, both
    // pages which were revoked and pages which already had the correct
    // permissions.
    uvm_page_mask_andnot(revoke_page_mask, revoke_page_mask, resident_mask);

    UVM_ASSERT(block_check_mappings(va_block));

    return uvm_tracker_add_push_safe(out_tracker, &push);
}

NV_STATUS uvm_va_block_revoke_prot(uvm_va_block_t *va_block,
                                   uvm_va_block_context_t *va_block_context,
                                   uvm_processor_id_t id,
                                   uvm_va_block_region_t region,
                                   const unsigned long *revoke_page_mask,
                                   uvm_prot_t prot_to_revoke,
                                   uvm_tracker_t *out_tracker)
{
    uvm_gpu_t *gpu;
    uvm_processor_mask_t resident_procs;
    uvm_processor_id_t resident_id;
    unsigned long *running_page_mask = va_block_context->mapping.running_page_mask;
    NV_STATUS status;

    UVM_ASSERT(prot_to_revoke > UVM_PROT_READ_ONLY);
    UVM_ASSERT(prot_to_revoke < UVM_PROT_MAX);
    uvm_assert_mutex_locked(&va_block->lock);

    if (id == UVM_CPU_ID) {
        if (prot_to_revoke == UVM_PROT_READ_WRITE_ATOMIC)
            return NV_OK;
        return uvm_va_block_revoke_cpu_write(va_block, va_block_context, region, revoke_page_mask);
    }

    gpu = uvm_gpu_get(id);

    // UVM-Lite GPUs should never have access revoked
    UVM_ASSERT_MSG(!uvm_processor_mask_test(&va_block->va_range->uvm_lite_gpus, gpu->id), "GPU %s\n", gpu->name);

    // Return early if there are no mappings for the GPU present in the block
    if (!uvm_processor_mask_test(&va_block->mapped, gpu->id))
        return NV_OK;

    uvm_page_mask_init_from_region(running_page_mask, region, revoke_page_mask);

    // Revoke per resident location so we can more easily detect physically-
    // contiguous mappings.
    uvm_processor_mask_copy(&resident_procs, &va_block->resident);

    for_each_closest_id(resident_id, &resident_procs, gpu->id, va_block->va_range->va_space) {
        status = uvm_va_block_revoke_prot_gpu_to(va_block,
                                                 va_block_context,
                                                 gpu,
                                                 resident_id,
                                                 running_page_mask,
                                                 prot_to_revoke,
                                                 out_tracker);
        if (status != NV_OK)
            return status;

        // If we've revoked all requested pages, we're done
        if (uvm_page_mask_region_empty(running_page_mask, region))
            break;
    }

    return NV_OK;
}

NV_STATUS uvm_va_block_map_mask(uvm_va_block_t *va_block,
                                uvm_va_block_context_t *va_block_context,
                                const uvm_processor_mask_t *map_processor_mask,
                                uvm_va_block_region_t region,
                                const unsigned long *map_page_mask,
                                uvm_prot_t new_prot,
                                UvmEventMapRemoteCause cause)
{
    uvm_tracker_t local_tracker = UVM_TRACKER_INIT();
    NV_STATUS status = NV_OK;
    NV_STATUS tracker_status;
    uvm_processor_id_t id;

    for_each_id_in_mask(id, map_processor_mask) {
        status = uvm_va_block_map(va_block,
                                  va_block_context,
                                  id,
                                  region,
                                  map_page_mask,
                                  new_prot,
                                  cause,
                                  &local_tracker);
        if (status != NV_OK)
            break;
    }

    // Regardless of error, add the successfully-pushed mapping operations into
    // the block's tracker. Note that we can't overwrite the tracker because we
    // aren't guaranteed that the map actually pushed anything (in which case it
    // would've acquired the block tracker first).
    tracker_status = uvm_tracker_add_tracker_safe(&va_block->tracker, &local_tracker);
    uvm_tracker_deinit(&local_tracker);

    return status == NV_OK ? tracker_status : status;
}

NV_STATUS uvm_va_block_unmap_mask(uvm_va_block_t *va_block,
                                  uvm_va_block_context_t *va_block_context,
                                  const uvm_processor_mask_t *unmap_processor_mask,
                                  uvm_va_block_region_t region,
                                  const unsigned long *unmap_page_mask)
{
    uvm_tracker_t local_tracker = UVM_TRACKER_INIT();
    NV_STATUS status = NV_OK;
    NV_STATUS tracker_status;
    uvm_processor_id_t id;

    // Watch out, unmap_mask could change during iteration since it could be
    // va_block->mapped.
    for_each_id_in_mask(id, unmap_processor_mask) {
        // Errors could either be a system-fatal error (ECC) or an allocation
        // retry due to PTE splitting. In either case we should stop after
        // hitting the first one.
        status = uvm_va_block_unmap(va_block, va_block_context, id, region, unmap_page_mask, &local_tracker);
        if (status != NV_OK)
            break;
    }

    // See the comment in uvm_va_block_map_mask for adding to the tracker.
    tracker_status = uvm_tracker_add_tracker_safe(&va_block->tracker, &local_tracker);
    uvm_tracker_deinit(&local_tracker);

    return status == NV_OK ? tracker_status : status;
}

// Unmaps all GPU mappings under this block, frees the page tables, and frees
// all the GPU chunks. This simply drops the chunks on the floor, so the caller
// must take care of copying the data elsewhere if it needs to remain intact.
//
// This serializes on the block tracker since it must unmap page tables.
static void block_destroy_gpu_state(uvm_va_block_t *block, uvm_gpu_id_t id)
{
    uvm_va_block_gpu_state_t *gpu_state = block->gpus[id - 1];
    uvm_gpu_va_space_t *gpu_va_space;
    uvm_gpu_t *gpu;
    uvm_gpu_chunk_t *gpu_chunk;
    size_t i, num_chunks;

    if (!gpu_state)
        return;

    // Unmap PTEs and free page tables
    gpu = uvm_gpu_get(id);
    gpu_va_space = uvm_gpu_va_space_get(block->va_range->va_space, gpu);
    if (gpu_va_space)
        uvm_va_block_remove_gpu_va_space(block, gpu_va_space);

    UVM_ASSERT(!uvm_processor_mask_test(&block->mapped, id));

    // No processor should have this GPU mapped at this point
    UVM_ASSERT(block_check_processor_not_mapped(block, id));

    if (gpu_state->chunks) {
        uvm_page_mask_zero(gpu_state->resident);
        block_clear_resident_processor(block, id);

        num_chunks = block_num_gpu_chunks(block, gpu);
        for (i = 0; i < num_chunks; i++) {
            gpu_chunk = gpu_state->chunks[i];
            if (!gpu_chunk)
                continue;

            uvm_pmm_gpu_free(&gpu->pmm, gpu_chunk, &block->tracker);
        }
        uvm_kvfree(gpu_state->chunks);
    }
    else {
        UVM_ASSERT(!uvm_processor_mask_test(&block->resident, id));
    }

    kmem_cache_free(g_uvm_va_block_gpu_state_cache, gpu_state);
    block->gpus[id - 1] = NULL;
}

static void block_put_ptes_safe(uvm_page_tree_t *tree, uvm_page_table_range_t *range)
{
    if (range->table) {
        uvm_page_tree_put_ptes(tree, range);
        memset(range, 0, sizeof(*range));
    }
}

void uvm_va_block_remove_gpu_va_space(uvm_va_block_t *va_block, uvm_gpu_va_space_t *gpu_va_space)
{
    uvm_va_space_t *va_space = va_block->va_range->va_space;
    uvm_va_block_context_t *block_context = &va_space->va_block_context;
    uvm_pte_batch_t *pte_batch = &block_context->mapping.pte_batch;
    uvm_tlb_batch_t *tlb_batch = &block_context->mapping.tlb_batch;
    uvm_gpu_t *gpu = gpu_va_space->gpu;
    uvm_va_block_gpu_state_t *gpu_state = va_block->gpus[gpu->id - 1];
    uvm_va_block_region_t region = uvm_va_block_region_from_block(va_block);
    uvm_push_t push;
    NV_STATUS status;

    uvm_tracker_t local_tracker = UVM_TRACKER_INIT();

    if (!gpu_state)
        return;

    uvm_assert_rwsem_locked_write(&va_space->lock);
    uvm_assert_mutex_locked(&va_block->lock);

    // Unmapping the whole block won't cause a page table split, so this should
    // only fail if we have a system-fatal error.
    status = uvm_va_block_unmap(va_block, block_context, gpu->id, region, NULL, &local_tracker);
    if (status != NV_OK) {
        UVM_ASSERT(status == uvm_global_get_status());
        return; // Just leak
    }

    UVM_ASSERT(!uvm_processor_mask_test(&va_block->mapped, gpu->id));

    // Reset the page tables if other allocations could reuse them
    if (!block_gpu_supports_2m(va_block, gpu) &&
        !bitmap_empty(gpu_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK)) {

        status = uvm_push_begin_acquire(gpu->channel_manager,
                                        UVM_CHANNEL_TYPE_MEMOPS,
                                        &local_tracker,
                                        &push,
                                        "Resetting PTEs for block [0x%llx, 0x%llx)",
                                        va_block->start,
                                        va_block->end + 1);
        if (status != NV_OK) {
            UVM_ASSERT(status == uvm_global_get_status());
            return; // Just leak
        }

        uvm_pte_batch_begin(&push, pte_batch);
        uvm_tlb_batch_begin(&gpu_va_space->page_tables, tlb_batch);

        // When the big PTEs is active, the 4k PTEs under it are garbage. Make
        // them invalid so the page tree code can reuse them for other
        // allocations on this VA. These don't need TLB invalidates since the
        // big PTEs above them are active.
        if (gpu_state->page_table_range_4k.table) {
            uvm_page_mask_init_from_big_ptes(va_block, gpu, block_context->scratch_page_mask, gpu_state->big_ptes);
            block_gpu_pte_clear_4k(va_block, gpu, block_context->scratch_page_mask, 0, pte_batch, NULL);
        }

        // We unmapped all big PTEs above, which means they have the unmapped
        // pattern so the GPU MMU won't read 4k PTEs under them. Set them to
        // invalid to activate the 4ks below so new allocations using just those
        // 4k PTEs will work.
        block_gpu_pte_clear_big(va_block, gpu, gpu_state->big_ptes, 0, pte_batch, tlb_batch);

        uvm_pte_batch_end(pte_batch);
        uvm_tlb_batch_end(tlb_batch, &push, UVM_MEMBAR_NONE);

        uvm_push_end(&push);
        uvm_tracker_overwrite_with_push(&local_tracker, &push);
    }

    // The unmap must finish before we free the page tables
    status = uvm_tracker_wait_deinit(&local_tracker);
    if (status != NV_OK)
        return; // System-fatal error, just leak

    // Note that if the PTE is currently 2M with lower tables allocated but not
    // in use, calling put_ptes on those lower ranges will re-write the 2M entry
    // to be a PDE.
    block_put_ptes_safe(&gpu_va_space->page_tables, &gpu_state->page_table_range_4k);
    block_put_ptes_safe(&gpu_va_space->page_tables, &gpu_state->page_table_range_big);
    block_put_ptes_safe(&gpu_va_space->page_tables, &gpu_state->page_table_range_2m);

    gpu_state->pte_is_2m = false;
    bitmap_zero(gpu_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
}

void uvm_va_block_disable_peer(uvm_va_block_t *va_block, uvm_gpu_t *gpu0, uvm_gpu_t *gpu1)
{
    uvm_va_space_t *va_space = va_block->va_range->va_space;
    NV_STATUS status;
    uvm_tracker_t tracker = UVM_TRACKER_INIT();
    unsigned long *unmap_page_mask = va_space->va_block_context.caller_page_mask;
    const unsigned long *resident0;
    const unsigned long *resident1;

    uvm_assert_rwsem_locked_write(&va_space->lock);
    uvm_assert_mutex_locked(&va_block->lock);

    // If either of the GPUs doesn't have GPU state then nothing could be mapped
    // between them.
    if (!va_block->gpus[gpu0->id - 1] || !va_block->gpus[gpu1->id - 1])
        return;

    resident0 = uvm_va_block_resident_mask_get(va_block, gpu0->id);
    resident1 = uvm_va_block_resident_mask_get(va_block, gpu1->id);

    // Unmap all pages resident on GPU1, but not on GPU0, from GPU0
    if (uvm_page_mask_andnot(unmap_page_mask, resident1, resident0)) {
        status = uvm_va_block_unmap_gpu(va_block, &va_space->va_block_context, gpu0, unmap_page_mask, &tracker);
        if (status != NV_OK) {
            // Since all PTEs unmapped by this call have the same aperture, page
            // splits should never be required so any failure should be the
            // result of a system-fatal error.
            UVM_ASSERT_MSG(status == uvm_global_get_status(),
                           "Unmapping failed: %s, GPU %s\n",
                           nvstatusToString(status), gpu0->name);
        }
    }

    // Unmap all pages resident on GPU0, but not on GPU1, from GPU1
    if (uvm_page_mask_andnot(unmap_page_mask, resident0, resident1)) {
        status = uvm_va_block_unmap_gpu(va_block, &va_space->va_block_context, gpu1, unmap_page_mask, &tracker);
        if (status != NV_OK) {
            UVM_ASSERT_MSG(status == uvm_global_get_status(),
                           "Unmapping failed: %s, GPU %s\n",
                           nvstatusToString(status), gpu0->name);
        }
    }

    status = uvm_tracker_add_tracker_safe(&va_block->tracker, &tracker);
    if (status != NV_OK)
        UVM_ASSERT(status == uvm_global_get_status());

    status = uvm_tracker_wait_deinit(&tracker);
    if (status != NV_OK)
        UVM_ASSERT(status == uvm_global_get_status());
}

void uvm_va_block_unmap_preferred_location_uvm_lite(uvm_va_block_t *va_block, uvm_gpu_t *gpu)
{
    NV_STATUS status;
    uvm_va_range_t *va_range = va_block->va_range;
    uvm_va_space_t *va_space = va_range->va_space;
    uvm_va_block_region_t region = uvm_va_block_region_from_block(va_block);

    uvm_assert_rwsem_locked_write(&va_space->lock);
    uvm_assert_mutex_locked(&va_block->lock);
    UVM_ASSERT(uvm_processor_mask_test(&va_range->uvm_lite_gpus, gpu->id));

    // If the GPU doesn't have GPU state then nothing could be mapped.
    if (!va_block->gpus[gpu->id - 1])
        return;

    // In UVM-Lite mode, mappings to the preferred location are not tracked
    // directly, so just unmap the whole block.
    status = uvm_va_block_unmap(va_block, &va_space->va_block_context, gpu->id, region, NULL, &va_block->tracker);
    if (status != NV_OK) {
        // Unmapping the whole block should not cause page splits so any failure
        // should be the result of a system-fatal error.
        UVM_ASSERT_MSG(status == uvm_global_get_status(),
                       "Unmapping failed: %s, GPU %s\n",
                       nvstatusToString(status), gpu->name);
    }

    status = uvm_tracker_wait(&va_block->tracker);
    if (status != NV_OK) {
        UVM_ASSERT_MSG(status == uvm_global_get_status(),
                       "Unmapping failed: %s, GPU %s\n",
                       nvstatusToString(status), gpu->name);
    }
}

// Evict pages from the GPU by moving each resident region to the CPU
//
// Notably the caller needs to support allocation-retry as
// uvm_va_block_migrate_locked() requires that.
static NV_STATUS block_evict_pages_from_gpu(uvm_va_block_t *va_block, uvm_gpu_t *gpu)
{
    NV_STATUS status = NV_OK;
    unsigned long *resident = uvm_va_block_resident_mask_get(va_block, gpu->id);
    uvm_va_block_region_t region = uvm_va_block_region_from_block(va_block);
    uvm_va_block_region_t subregion;
    uvm_va_block_context_t *block_context = uvm_va_block_context_alloc();

    if (!block_context)
        return NV_ERR_NO_MEMORY;

    // Move all subregions resident on the GPU to the CPU
    for_each_va_block_subregion_in_mask(subregion, resident, region) {
        status = uvm_va_block_migrate_locked(va_block, NULL, block_context, subregion, UVM_CPU_ID, 0, NULL);
        if (status != NV_OK)
            break;
    }

    uvm_va_block_context_free(block_context);

    if (status == NV_OK)
        UVM_ASSERT(!uvm_processor_mask_test(&va_block->resident, gpu->id));

    return status;
}

// This handles allocation-retry internally and hence might unlock and relock
// block's lock.
static void block_unregister_gpu_locked(uvm_va_block_t *va_block, uvm_gpu_t *gpu)
{
    NV_STATUS status;
    uvm_va_block_gpu_state_t *gpu_state = va_block->gpus[gpu->id - 1];

    uvm_assert_mutex_locked(&va_block->lock);

    if (!gpu_state)
        return;

    // The mappings should've already been torn down by GPU VA space unregister
    UVM_ASSERT(!uvm_processor_mask_test(&va_block->mapped, gpu->id));
    UVM_ASSERT(uvm_page_mask_empty(gpu_state->pte_bits[UVM_PTE_BITS_GPU_READ]));
    UVM_ASSERT(!block_gpu_has_page_tables(va_block, gpu));

    // Use UVM_VA_BLOCK_RETRY_LOCKED() as the va block lock is already taken and
    // we don't rely on any state of the block across the call.
    status = UVM_VA_BLOCK_RETRY_LOCKED(va_block, NULL, block_evict_pages_from_gpu(va_block, gpu));
    if (status != NV_OK) {
        UVM_ERR_PRINT("Failed to evict GPU pages on GPU unregister: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        uvm_global_set_fatal_error(status);
    }

    // This function will copy the block's tracker into each chunk then free the
    // chunk to PMM. If we do this before waiting for the block tracker below
    // we'll populate PMM's free chunks with tracker entries, which gives us
    // better testing coverage of chunk synchronization on GPU unregister.
    block_destroy_gpu_state(va_block, gpu->id);

    // Any time a GPU is unregistered we need to make sure that there are no
    // pending (direct or indirect) tracker entries for that GPU left in the
    // block's tracker. The only way to ensure that is to wait for the whole
    // tracker.
    status = uvm_tracker_wait(&va_block->tracker);
    if (status != NV_OK)
        UVM_ASSERT(status == uvm_global_get_status());
}

void uvm_va_block_unregister_gpu(uvm_va_block_t *va_block, uvm_gpu_t *gpu)
{
    // Take the lock internally to not expose the caller to allocation-retry.
    uvm_mutex_lock(&va_block->lock);

    block_unregister_gpu_locked(va_block, gpu);

    uvm_mutex_unlock(&va_block->lock);
}

// Tears down everything within the block, but doesn't free the block itself.
// Note that when uvm_va_block_kill is called, this is called twice: once for
// the initial kill itself, then again when the block's ref count is eventually
// destroyed. block->va_range is used to track whether the block has already
// been killed.
static void block_kill(uvm_va_block_t *block)
{
    uvm_va_range_t *va_range = block->va_range;
    uvm_va_space_t *va_space;
    uvm_perf_event_data_t event_data;
    uvm_gpu_id_t id;
    NV_STATUS status;
    size_t i;
    uvm_va_block_region_t region = uvm_va_block_region_from_block(block);

    if (!va_range)
        return;

    UVM_ASSERT(va_range->type == UVM_VA_RANGE_TYPE_MANAGED);

    va_space = va_range->va_space;

    event_data.block_destroy.block = block;
    uvm_perf_event_notify(&va_space->perf_events, UVM_PERF_EVENT_BLOCK_DESTROY, &event_data);

    // Unmap all processors in parallel first. Unmapping the whole block won't
    // cause a page table split, so this should only fail if we have a system-
    // fatal error.
    if (!uvm_processor_mask_empty(&block->mapped)) {
        // We could only be killed with mapped GPU state by VA range free or VA
        // space teardown, so it's safe to use va_space->va_block_context
        // because both of those have the VA space lock held in write mode.
        uvm_assert_rwsem_locked_write(&va_space->lock);

        status = uvm_va_block_unmap_mask(block, &va_space->va_block_context, &block->mapped, region, NULL);
        UVM_ASSERT(status == uvm_global_get_status());
    }

    UVM_ASSERT(uvm_processor_mask_empty(&block->mapped));

    // Free the GPU page tables and chunks
    for_each_gpu_id(id)
        block_destroy_gpu_state(block, id);

    // Wait for the GPU PTE unmaps before freeing CPU memory
    uvm_tracker_wait_deinit(&block->tracker);

    // No processor should have the CPU mapped at this point
    UVM_ASSERT(block_check_processor_not_mapped(block, UVM_CPU_ID));

    // Free CPU pages
    if (block->cpu.pages) {
        for (i = 0; i < uvm_va_block_num_cpu_pages(block); i++) {
            if (block->cpu.pages[i]) {
                // be conservative.
                // Tell the OS we wrote to the page because we sometimes clear the dirty bit after writing to it.
                SetPageDirty(block->cpu.pages[i]);
                __free_page(block->cpu.pages[i]);
            }
            else {
                UVM_ASSERT(!test_bit(i, block->cpu.resident));
            }
        }

        // Clearing the resident bit isn't strictly necessary since this block
        // is getting destroyed, but it keeps state consistent for assertions.
        uvm_page_mask_zero(block->cpu.resident);
        block_clear_resident_processor(block, UVM_CPU_ID);

        uvm_kvfree(block->cpu.pages);
    }
    else {
        UVM_ASSERT(!uvm_processor_mask_test(&block->resident, UVM_CPU_ID));
    }

    block->va_range = NULL;
}

// Called when the block's ref count drops to 0
void uvm_va_block_destroy(struct kref *kref)
{
    uvm_va_block_t *block = container_of(kref, uvm_va_block_t, kref);

    // Nobody else should have a reference when freeing
    uvm_assert_mutex_unlocked(&block->lock);

    uvm_mutex_lock(&block->lock);
    block_kill(block);
    uvm_mutex_unlock(&block->lock);
    kmem_cache_free(g_uvm_va_block_cache, block);
}

void uvm_va_block_kill(uvm_va_block_t *va_block)
{
    uvm_mutex_lock(&va_block->lock);
    block_kill(va_block);
    uvm_mutex_unlock(&va_block->lock);

    // May call block_kill again
    uvm_va_block_release(va_block);
}

// Deswizzle the split point, if it's covered and populated by a big page on
// this gpu.
static NV_STATUS uvm_va_block_split_presplit_deswizzle_gpu(uvm_va_block_t *existing,
                                                           uvm_va_block_t *new,
                                                           uvm_gpu_t *gpu)
{
    uvm_va_block_gpu_state_t *existing_gpu_state = existing->gpus[gpu->id - 1];
    uvm_va_space_t *va_space = existing->va_range->va_space;
    uvm_va_block_context_t *block_context = &va_space->va_block_context;
    uvm_va_block_region_t big_page_region;
    NvU32 big_page_size = uvm_va_block_gpu_big_page_size(existing, gpu);
    size_t new_start_page_index = uvm_va_block_cpu_page_index(existing, new->start);
    size_t big_page_index;
    uvm_tracker_t tracker = UVM_TRACKER_INIT();
    NV_STATUS status = NV_OK;

    UVM_ASSERT(gpu->big_page.swizzling);

    big_page_index = uvm_va_block_big_page_index(existing, new_start_page_index, big_page_size);

    // If the split point is on a big page boundary, or if the split point
    // is not currently covered by a swizzled big page, we don't have to
    // deswizzle.
    if (IS_ALIGNED(new->start, big_page_size) ||
        big_page_index == MAX_BIG_PAGES_PER_UVM_VA_BLOCK ||
        !test_bit(big_page_index, existing_gpu_state->big_pages_swizzled))
        return NV_OK;

    big_page_region = uvm_va_block_big_page_region(existing, big_page_index, big_page_size);

    // If any part of the the swizzled big page is resident, we have to
    // deswizzle. Otherwise just clear the bit since we don't care about the
    // data.
    if (!uvm_page_mask_region_empty(existing_gpu_state->resident, big_page_region)) {
        status = block_gpu_big_page_change_swizzling(existing,
                                                     block_context,
                                                     gpu,
                                                     NULL,
                                                     big_page_index,
                                                     big_page_region,
                                                     UVM_GPU_SWIZZLE_OP_DESWIZZLE,
                                                     &tracker);
        if (status != NV_OK)
            goto out;
    }
    else {
        __clear_bit(big_page_index, existing_gpu_state->big_pages_swizzled);
    }

out:
    // block_gpu_big_page_change_swizzling added this work to existing's block
    // tracker.
    uvm_tracker_deinit(&tracker);
    return status;
}

static NV_STATUS uvm_va_block_split_presplit_ptes_gpu(uvm_va_block_t *existing, uvm_va_block_t *new, uvm_gpu_t *gpu)
{
    uvm_va_block_gpu_state_t *existing_gpu_state = existing->gpus[gpu->id - 1];
    uvm_va_space_t *va_space = existing->va_range->va_space;
    uvm_va_block_context_t *block_context = &va_space->va_block_context;
    NvU32 big_page_size = uvm_va_block_gpu_big_page_size(existing, gpu);
    NvU32 alloc_sizes;
    DECLARE_BITMAP(new_big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
    size_t new_start_page_index = uvm_va_block_cpu_page_index(existing, new->start);
    size_t big_page_index;
    uvm_push_t push;
    NV_STATUS status;

    // We only have to split to big PTEs if we're currently a 2M PTE
    if (existing_gpu_state->pte_is_2m) {
        // We can skip the split if the 2M PTE is invalid and we have no lower
        // PTEs.
        if (block_page_prot_gpu(existing, gpu, 0) == UVM_PROT_NONE &&
            !existing_gpu_state->page_table_range_big.table &&
            !existing_gpu_state->page_table_range_4k.table)
            return NV_OK;

        alloc_sizes = big_page_size;
        bitmap_fill(new_big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

        if (!IS_ALIGNED(new->start, big_page_size)) {
            alloc_sizes |= UVM_PAGE_SIZE_4K;

            big_page_index = uvm_va_block_big_page_index(existing, new_start_page_index, big_page_size);
            __clear_bit(big_page_index, new_big_ptes);
        }

        status = block_alloc_ptes_with_retry(existing, gpu, alloc_sizes, NULL);
        if (status != NV_OK)
            return status;

        status = uvm_push_begin_acquire(gpu->channel_manager,
                                        UVM_CHANNEL_TYPE_MEMOPS,
                                        &existing->tracker,
                                        &push,
                                        "Splitting 2M PTE, existing [0x%llx, 0x%llx) new [0x%llx, 0x%llx)",
                                        existing->start, existing->end + 1,
                                        new->start, new->end + 1);
        if (status != NV_OK)
            return status;

        block_gpu_split_2m(existing, block_context, gpu, new_big_ptes, &push);
    }
    else {
        big_page_index = uvm_va_block_big_page_index(existing, new_start_page_index, big_page_size);

        // If the split point is on a big page boundary, or if the split point
        // is not currently covered by a big PTE, we don't have to split
        // anything.
        if (IS_ALIGNED(new->start, big_page_size) ||
            big_page_index == MAX_BIG_PAGES_PER_UVM_VA_BLOCK ||
            !test_bit(big_page_index, existing_gpu_state->big_ptes))
            return NV_OK;

        status = block_alloc_ptes_with_retry(existing, gpu, UVM_PAGE_SIZE_4K, NULL);
        if (status != NV_OK)
            return status;

        bitmap_zero(new_big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
        __set_bit(big_page_index, new_big_ptes);

        status = uvm_push_begin_acquire(gpu->channel_manager,
                                        UVM_CHANNEL_TYPE_MEMOPS,
                                        &existing->tracker,
                                        &push,
                                        "Splitting big PTE, existing [0x%llx, 0x%llx) new [0x%llx, 0x%llx)",
                                        existing->start, existing->end + 1,
                                        new->start, new->end + 1);
        if (status != NV_OK)
            return status;

        block_gpu_split_big(existing, block_context, gpu, new_big_ptes, &push);
    }

    uvm_push_end(&push);

    // Adding this push to existing block tracker will cause all GPU PTE splits
    // to serialize on each other, but it's simpler than maintaining a separate
    // tracker and this path isn't performance-critical.
    return uvm_tracker_add_push_safe(&existing->tracker, &push);
}

static NV_STATUS uvm_va_block_split_presplit_ptes(uvm_va_block_t *existing, uvm_va_block_t *new)
{
    uvm_gpu_t *gpu;
    uvm_gpu_id_t id;
    NV_STATUS status;

    // Deswizzle the physical pages covering the split before splitting the PTEs
    for_each_gpu_id(id) {
        if (!existing->gpus[id - 1])
            continue;

        gpu = uvm_gpu_get(id);

        if (gpu->big_page.swizzling) {
            status = uvm_va_block_split_presplit_deswizzle_gpu(existing, new, gpu);
            if (status != NV_OK)
                return status;
        }
    }

    for_each_gpu_id(id) {
        if (!existing->gpus[id - 1])
            continue;

        gpu = uvm_gpu_get(id);

        if (block_gpu_has_page_tables(existing, gpu)) {
            status = uvm_va_block_split_presplit_ptes_gpu(existing, new, gpu);
            if (status != NV_OK)
                return status;
        }
    }

    return NV_OK;
}

typedef struct
{
    // Number of chunks contained by this VA block
    size_t num_chunks;

    // Index of the "interesting" chunk, either adjacent to or spanning the
    // split point depending on which block this is.
    size_t chunk_index;

    // Size of the chunk referenced by chunk_index
    uvm_chunk_size_t chunk_size;
} block_gpu_chunk_split_state_t;

static void block_gpu_chunk_get_split_state(block_gpu_chunk_split_state_t *state,
                                            NvU64 start,
                                            NvU64 end,
                                            size_t page_index,
                                            uvm_gpu_t *gpu)
{
    NvU64 size = end - start + 1;
    state->num_chunks = block_num_gpu_chunks_range(start, size, gpu);
    state->chunk_index = uvm_va_block_gpu_chunk_index_range(start, size, gpu, page_index, &state->chunk_size);
}

// Perform any chunk splitting and array growing required for this block split,
// but don't actually move chunk pointers anywhere.
static NV_STATUS block_presplit_gpu_chunks(uvm_va_block_t *existing, uvm_va_block_t *new, uvm_gpu_t *gpu)
{
    uvm_va_block_gpu_state_t *existing_gpu_state = existing->gpus[gpu->id - 1];
    uvm_gpu_chunk_t **temp_chunks;
    uvm_gpu_chunk_t *original_chunk, *curr_chunk;
    size_t split_page_index = uvm_va_block_cpu_page_index(existing, new->start);
    size_t last_index, num_subchunks;
    uvm_chunk_sizes_mask_t split_sizes;
    uvm_chunk_size_t subchunk_size;
    NV_STATUS status;
    block_gpu_chunk_split_state_t existing_before_state, existing_after_state, new_state;

    block_gpu_chunk_get_split_state(&existing_before_state, existing->start, existing->end,  split_page_index,     gpu);
    block_gpu_chunk_get_split_state(&existing_after_state,  existing->start, new->start - 1, split_page_index - 1, gpu);
    block_gpu_chunk_get_split_state(&new_state,             new->start,      new->end,       0,                    gpu);

    // Even though we're splitting existing, we could wind up requiring a larger
    // chunks array if we split a large chunk into many smaller ones.
    if (existing_after_state.num_chunks > existing_before_state.num_chunks) {
        temp_chunks = uvm_kvrealloc(existing_gpu_state->chunks,
                                    existing_after_state.num_chunks * sizeof(existing_gpu_state->chunks[0]));
        if (!temp_chunks)
            return NV_ERR_NO_MEMORY;
        existing_gpu_state->chunks = temp_chunks;
    }

    original_chunk = existing_gpu_state->chunks[existing_before_state.chunk_index];

    // If the chunk covering the split point is not populated, we're done. We've
    // already grown the array to cover any new chunks which may be populated
    // later.
    if (!original_chunk)
        return NV_OK;

    // Figure out the splits we need to perform. Remove all sizes >= the current
    // size, and all sizes < the target size. Note that the resulting mask will
    // be 0 if the sizes match (we're already splitting at a chunk boundary).
    UVM_ASSERT(uvm_gpu_chunk_get_size(original_chunk) == existing_before_state.chunk_size);
    UVM_ASSERT(existing_before_state.chunk_size >= new_state.chunk_size);
    split_sizes = uvm_mmu_user_chunk_sizes(gpu);
    split_sizes &= existing_before_state.chunk_size - 1;
    split_sizes &= ~(new_state.chunk_size - 1);

    // Keep splitting the chunk covering the split point until we hit the target
    // size.
    curr_chunk = original_chunk;
    for_each_chunk_size_rev(subchunk_size, split_sizes) {
        status = uvm_pmm_gpu_split_chunk(&gpu->pmm, curr_chunk, subchunk_size, NULL);
        if (status != NV_OK) {
            uvm_pmm_gpu_merge_chunk(&gpu->pmm, original_chunk);
            return status;
        }

        if (subchunk_size == new_state.chunk_size)
            break;

        // Compute the last subchunk index prior to the split point. Divide the
        // entire address space into units of subchunk_size, then mod by the
        // number of subchunks within the parent.
        last_index = (size_t)uvm_div_pow2_64(new->start - 1, subchunk_size);
        num_subchunks = (size_t)uvm_div_pow2_64(uvm_gpu_chunk_get_size(curr_chunk), subchunk_size);
        UVM_ASSERT(num_subchunks > 1);
        last_index &= num_subchunks - 1;

        uvm_pmm_gpu_get_subchunks(&gpu->pmm, curr_chunk, last_index, 1, &curr_chunk);
        UVM_ASSERT(uvm_gpu_chunk_get_size(curr_chunk) == subchunk_size);
    }

    // Note that existing's chunks array still has a pointer to original_chunk,
    // not to any newly-split subchunks. If a subsequent split failure occurs on
    // a later GPU we'll have to merge it back. Once we're past the preallocate
    // stage we'll remove it from the chunks array and move the new split chunks
    // in.

    return NV_OK;
}

// Pre-allocate everything which doesn't require retry on both existing and new
// which will be needed to handle a split. If this fails, existing must remain
// functionally unmodified.
static NV_STATUS uvm_va_block_split_preallocate_no_retry(uvm_va_block_t *existing, uvm_va_block_t *new)
{
    uvm_gpu_t *gpu;
    uvm_gpu_id_t id;
    NV_STATUS status;
    size_t split_page_index, chunk_index;
    uvm_va_block_gpu_state_t *existing_gpu_state;
    uvm_gpu_chunk_t *chunk;

    // Blocks don't have any CPU state to pre-allocate

    for_each_gpu_id(id) {
        if (!existing->gpus[id - 1])
            continue;

        gpu = uvm_gpu_get(id);

        status = block_presplit_gpu_chunks(existing, new, gpu);
        if (status != NV_OK)
            goto error;

        if (!block_gpu_state_get_alloc(new, gpu)) {
            status = NV_ERR_NO_MEMORY;
            goto error;
        }
    }

    if (existing->va_range->inject_split_error) {
        existing->va_range->inject_split_error = false;
        status = NV_ERR_NO_MEMORY;
        goto error;
    }

    return NV_OK;

error:
    // Merge back the chunks we split
    split_page_index = uvm_va_block_cpu_page_index(existing, new->start);

    for_each_gpu_id(id) {
        existing_gpu_state = existing->gpus[id - 1];
        if (!existing_gpu_state)
            continue;

        // If the chunk spanning the split point was split, merge it back
        gpu = uvm_gpu_get(id);
        chunk_index = block_gpu_chunk_index(existing, gpu, split_page_index, NULL);
        chunk = existing_gpu_state->chunks[chunk_index];
        if (chunk && uvm_gpu_chunk_get_state(chunk) == UVM_PMM_GPU_CHUNK_STATE_IS_SPLIT)
            uvm_pmm_gpu_merge_chunk(&gpu->pmm, chunk);

        // We could attempt to shrink the chunks array back down, but it doesn't
        // hurt much to have it larger than necessary, and we'd have to handle
        // the shrink call failing anyway on this error path.
    }

    return status;
}

// Re-calculate the block's top-level processor masks:
//   - block->mapped
//   - block->resident
//
// This is called on block split.
static void block_set_processor_masks(uvm_va_block_t *block)
{
    size_t num_pages = uvm_va_block_num_cpu_pages(block);
    uvm_va_block_region_t block_region = uvm_va_block_region(0, num_pages);
    uvm_va_block_gpu_state_t *gpu_state;
    uvm_gpu_id_t id;

    if (uvm_page_mask_region_empty(block->cpu.pte_bits[UVM_PTE_BITS_CPU_READ], block_region)) {
        UVM_ASSERT(uvm_page_mask_region_empty(block->cpu.pte_bits[UVM_PTE_BITS_CPU_WRITE], block_region));
        uvm_processor_mask_clear(&block->mapped, UVM_CPU_ID);
    }
    else {
        uvm_processor_mask_set(&block->mapped, UVM_CPU_ID);
    }

    if (uvm_page_mask_region_empty(block->cpu.resident, block_region)) {
        UVM_ASSERT(!uvm_processor_mask_test(&block->mapped, UVM_CPU_ID));
        block_clear_resident_processor(block, UVM_CPU_ID);
    }
    else {
        block_set_resident_processor(block, UVM_CPU_ID);
    }

    for_each_gpu_id(id) {
        gpu_state = block->gpus[id - 1];
        if (!gpu_state)
            continue;

        if (uvm_page_mask_region_empty(gpu_state->pte_bits[UVM_PTE_BITS_GPU_READ], block_region)) {
            UVM_ASSERT(uvm_page_mask_region_empty(gpu_state->pte_bits[UVM_PTE_BITS_GPU_WRITE], block_region));
            UVM_ASSERT(uvm_page_mask_region_empty(gpu_state->pte_bits[UVM_PTE_BITS_GPU_ATOMIC], block_region));
            uvm_processor_mask_clear(&block->mapped, id);
        }
        else {
            uvm_processor_mask_set(&block->mapped, id);
        }

        if (uvm_page_mask_region_empty(gpu_state->resident, block_region))
            block_clear_resident_processor(block, id);
        else
            block_set_resident_processor(block, id);
    }
}

// Split a PAGES_PER_UVM_VA_BLOCK sized bitmap into new and existing parts
// corresponding to a block split.
static void block_split_page_mask(unsigned long *existing_mask, size_t existing_pages,
                                  unsigned long *new_mask, size_t new_pages)
{
    UVM_ASSERT_MSG(existing_pages + new_pages <= PAGES_PER_UVM_VA_BLOCK, "existing %zu new %zu\n",
                   existing_pages, new_pages);

    // The new block is always in the upper region of existing, so shift the bit
    // vectors down.
    //
    // Note that bitmap_shift_right requires both dst and src to be the same
    // size. That's ok since we don't scale them by block size.
    bitmap_shift_right(new_mask, existing_mask, existing_pages, PAGES_PER_UVM_VA_BLOCK);
    uvm_page_mask_region_clear(existing_mask, uvm_va_block_region(existing_pages, existing_pages + new_pages));
}

// Split the CPU state within the existing block. existing's start is correct
// but its end has not yet been adjusted.
static void uvm_va_block_split_cpu(uvm_va_block_t *existing, uvm_va_block_t *new)
{
    struct page **temp_pages;
    size_t existing_pages, new_pages = uvm_va_block_num_cpu_pages(new);
    uvm_pte_bits_cpu_t pte_bit;

    UVM_ASSERT(existing->va_range->type == UVM_VA_RANGE_TYPE_MANAGED);
    UVM_ASSERT(existing->va_range->type == new->va_range->type);
    UVM_ASSERT(existing->start < new->start);
    UVM_ASSERT(existing->end == new->end);
    UVM_ASSERT(existing->cpu.pages);
    UVM_ASSERT(new->cpu.pages);

    // We don't have to unmap the CPU since its virtual -> physical mappings
    // don't change.

    UVM_ASSERT(PAGE_ALIGNED(new->start));
    UVM_ASSERT(PAGE_ALIGNED(existing->start));
    existing_pages = (new->start - existing->start) / PAGE_SIZE;

    // Move pages from the top of existing down into new
    memcpy(&new->cpu.pages[0],
           &existing->cpu.pages[existing_pages],
           uvm_va_block_num_cpu_pages(new) * sizeof(new->cpu.pages[0]));

    // Attempt to shrink existing's pages allocation. If the realloc fails, just
    // keep on using the old larger one.
    temp_pages = uvm_kvrealloc(existing->cpu.pages, existing_pages * sizeof(existing->cpu.pages[0]));
    if (temp_pages)
        existing->cpu.pages = temp_pages;

    block_split_page_mask(existing->cpu.resident, existing_pages, new->cpu.resident, new_pages);

    for (pte_bit = 0; pte_bit < UVM_PTE_BITS_CPU_MAX; pte_bit++)
        block_split_page_mask(existing->cpu.pte_bits[pte_bit], existing_pages, new->cpu.pte_bits[pte_bit], new_pages);
}

// Fill out the blocks' chunks arrays with the chunks split by
// block_presplit_gpu_chunks.
static void uvm_va_block_copy_split_gpu_chunks(uvm_va_block_t *existing, uvm_va_block_t *new, uvm_gpu_t *gpu)
{
    uvm_va_block_gpu_state_t *existing_gpu_state = existing->gpus[gpu->id - 1];
    uvm_va_block_gpu_state_t *new_gpu_state = new->gpus[gpu->id - 1];
    uvm_gpu_chunk_t **temp_chunks;
    uvm_gpu_chunk_t *original_chunk;
    block_gpu_chunk_split_state_t existing_before_state, existing_after_state, new_state;
    size_t num_subchunks, num_pre_chunks, num_post_chunks, num_split_chunks_existing, num_split_chunks_new;
    size_t split_page_index = uvm_va_block_cpu_page_index(existing, new->start);
    size_t i;

    block_gpu_chunk_get_split_state(&existing_before_state, existing->start, existing->end,  split_page_index,     gpu);
    block_gpu_chunk_get_split_state(&existing_after_state,  existing->start, new->start - 1, split_page_index - 1, gpu);
    block_gpu_chunk_get_split_state(&new_state,             new->start,      new->end,       0,                    gpu);

    // General case (B is original_chunk):
    //                                          split
    //                                            v
    //  existing (before) [------ A -----][------ B -----][------ C -----]
    //  existing (after)  [------ A -----][- B0 -]
    //  new                                       [- B1 -][------ C -----]
    //
    // Note that the logic below also handles the case of the split happening at
    // a chunk boundary. That case behaves as though there is no B0 chunk.

    // Number of chunks to the left and right of original_chunk (A and C above).
    // Either or both of these may be 0.
    num_pre_chunks  = existing_before_state.chunk_index;
    num_post_chunks = existing_before_state.num_chunks - num_pre_chunks - 1;

    // Number of subchunks under existing's portion of original_chunk (B0 above)
    num_split_chunks_existing = existing_after_state.num_chunks - num_pre_chunks;

    // Number of subchunks under new's portion of original_chunk (B1 above)
    num_split_chunks_new = new_state.num_chunks - num_post_chunks;

    UVM_ASSERT(num_pre_chunks + num_split_chunks_existing > 0);
    UVM_ASSERT(num_split_chunks_new > 0);

    // Copy post chunks from the end of existing into new (C above)
    memcpy(&new_gpu_state->chunks[num_split_chunks_new],
           &existing_gpu_state->chunks[existing_before_state.chunk_index + 1],
           num_post_chunks * sizeof(new_gpu_state->chunks[0]));

    // Save off the original split chunk since we may overwrite the array
    original_chunk = existing_gpu_state->chunks[existing_before_state.chunk_index];

    // Fill out the new pointers
    if (original_chunk) {
        // Note that if the split happened at a chunk boundary, original_chunk
        // will not be split. In that case, num_split_chunks_existing will be 0
        // and num_split_chunks_new will be 1, so the left copy will be skipped
        // and the right copy will pick up the chunk.

        // Copy left newly-split chunks into existing (B0 above). The array was
        // re-sized in block_presplit_gpu_chunks as necessary.
        num_subchunks = uvm_pmm_gpu_get_subchunks(&gpu->pmm,
                                                  original_chunk,
                                                  0, // start_index
                                                  num_split_chunks_existing,
                                                  &existing_gpu_state->chunks[existing_before_state.chunk_index]);
        UVM_ASSERT(num_subchunks == num_split_chunks_existing);

        // Copy right newly-split chunks into new (B1 above), overwriting the
        // pointer to the original chunk.
        num_subchunks = uvm_pmm_gpu_get_subchunks(&gpu->pmm,
                                                  original_chunk,
                                                  num_split_chunks_existing, // start_index
                                                  num_split_chunks_new,
                                                  &new_gpu_state->chunks[0]);
        UVM_ASSERT(num_subchunks == num_split_chunks_new);
    }
    else {
        // If the chunk wasn't already populated we don't need to copy pointers
        // anywhere, but we need to clear out stale pointers from existing's
        // array covering the new elements. new's chunks array was already zero-
        // initialized.
        memset(&existing_gpu_state->chunks[existing_before_state.chunk_index],
               0,
               num_split_chunks_existing * sizeof(existing_gpu_state->chunks[0]));
    }

    // Update the reverse map of all the chunks that are now under the new block
    for (i = 0; i < new_state.num_chunks; ++i) {
        if (new_gpu_state->chunks[i]) {
            UVM_ASSERT(new_gpu_state->chunks[i]->va_block == existing);
            new_gpu_state->chunks[i]->va_block = new;
        }
    }

    // Attempt to shrink existing's chunk allocation. If the realloc fails, just
    // keep on using the old larger one.
    if (existing_after_state.num_chunks < existing_before_state.num_chunks) {
        temp_chunks = uvm_kvrealloc(existing_gpu_state->chunks,
                                    existing_after_state.num_chunks * sizeof(existing_gpu_state->chunks[0]));
        if (temp_chunks)
            existing_gpu_state->chunks = temp_chunks;
    }
}

static void uvm_va_block_split_gpu(uvm_va_block_t *existing, uvm_va_block_t *new, uvm_gpu_id_t id)
{
    uvm_va_block_gpu_state_t *existing_gpu_state = existing->gpus[id - 1];
    uvm_va_block_gpu_state_t *new_gpu_state = new->gpus[id - 1];
    uvm_gpu_va_space_t *gpu_va_space;
    uvm_gpu_t *gpu;
    NvU32 big_page_size;
    size_t new_pages = uvm_va_block_num_cpu_pages(new);
    size_t existing_pages, existing_pages_4k, existing_pages_big, new_pages_big;
    uvm_pte_bits_gpu_t pte_bit;

    if (!existing_gpu_state)
        return;

    gpu = uvm_gpu_get(id);
    UVM_ASSERT(new_gpu_state);

    UVM_ASSERT(PAGE_ALIGNED(new->start));
    UVM_ASSERT(PAGE_ALIGNED(existing->start));
    existing_pages = (new->start - existing->start) / PAGE_SIZE;

    uvm_va_block_copy_split_gpu_chunks(existing, new, gpu);

    block_split_page_mask(existing_gpu_state->resident, existing_pages,
                          new_gpu_state->resident, new_pages);

    for (pte_bit = 0; pte_bit < UVM_PTE_BITS_GPU_MAX; pte_bit++) {
        block_split_page_mask(existing_gpu_state->pte_bits[pte_bit], existing_pages,
                              new_gpu_state->pte_bits[pte_bit], new_pages);
    }

    if (gpu->big_page.swizzling) {
        // Big page swizzling is a property of the physical memory, so pages
        // might be swizzled regardless of whether they have mappings.

        big_page_size = uvm_va_block_gpu_big_page_size(existing, gpu);

        // existing's end has not been adjusted yet
        existing_pages_big = range_num_big_pages(existing->start, new->start - 1, big_page_size);
        new_pages_big = uvm_va_block_num_big_pages(new, big_page_size);

        // See below comments on splitting the big table range
        bitmap_shift_right(new_gpu_state->big_pages_swizzled,
                           existing_gpu_state->big_pages_swizzled,
                           uvm_va_block_num_big_pages(existing, big_page_size) - new_pages_big,
                           MAX_BIG_PAGES_PER_UVM_VA_BLOCK);

        bitmap_clear(existing_gpu_state->big_pages_swizzled,
                     existing_pages_big,
                     MAX_BIG_PAGES_PER_UVM_VA_BLOCK - existing_pages_big);
    }
    else {
        UVM_ASSERT(bitmap_empty(existing_gpu_state->big_pages_swizzled, MAX_BIG_PAGES_PER_UVM_VA_BLOCK));
    }

    // Adjust page table ranges.
    gpu_va_space = uvm_gpu_va_space_get(existing->va_range->va_space, gpu);
    if (gpu_va_space) {
        if (existing_gpu_state->page_table_range_big.table) {
            big_page_size = uvm_va_block_gpu_big_page_size(existing, gpu);

            // existing's end has not been adjusted yet
            existing_pages_big = range_num_big_pages(existing->start, new->start - 1, big_page_size);

            // Take references on all big pages covered by new
            new_pages_big = uvm_va_block_num_big_pages(new, big_page_size);
            if (new_pages_big) {
                uvm_page_table_range_get_upper(&gpu_va_space->page_tables,
                                               &existing_gpu_state->page_table_range_big,
                                               &new_gpu_state->page_table_range_big,
                                               new_pages_big);

                // If the split point is within a big page region, we might have
                // a gap since neither existing nor new can use it anymore.
                // Get the top N bits from existing's mask to handle that.
                bitmap_shift_right(new_gpu_state->big_ptes,
                                   existing_gpu_state->big_ptes,
                                   uvm_va_block_num_big_pages(existing, big_page_size) - new_pages_big,
                                   MAX_BIG_PAGES_PER_UVM_VA_BLOCK);
            }

            // Drop existing's references on the big PTEs it no longer covers
            // now that new has references on them. Note that neither existing
            // nor new might have big PTEs after the split. In that case, this
            // shrink will free the entire old range.
            uvm_page_table_range_shrink(&gpu_va_space->page_tables,
                                        &existing_gpu_state->page_table_range_big,
                                        existing_pages_big);

            if (existing_pages_big == 0)
                memset(&existing_gpu_state->page_table_range_big, 0, sizeof(existing_gpu_state->page_table_range_big));

            bitmap_clear(existing_gpu_state->big_ptes,
                         existing_pages_big,
                         MAX_BIG_PAGES_PER_UVM_VA_BLOCK - existing_pages_big);

            new_gpu_state->initialized_big = existing_gpu_state->initialized_big;
        }

        if (existing_gpu_state->page_table_range_4k.table) {
            // Since existing and new share the same PDE we just need to bump
            // the ref-count on new's sub-range.
            uvm_page_table_range_get_upper(&gpu_va_space->page_tables,
                                           &existing_gpu_state->page_table_range_4k,
                                           &new_gpu_state->page_table_range_4k,
                                           uvm_va_block_size(new) / UVM_PAGE_SIZE_4K);

            // Drop existing's references on the PTEs it no longer covers now
            // that new has references on them.
            existing_pages_4k = existing_pages * (PAGE_SIZE / UVM_PAGE_SIZE_4K);
            uvm_page_table_range_shrink(&gpu_va_space->page_tables,
                                        &existing_gpu_state->page_table_range_4k,
                                        existing_pages_4k);
        }

        // We have to set this explicitly to handle the case of splitting an
        // invalid, active 2M PTE with no lower page tables allocated.
        if (existing_gpu_state->pte_is_2m) {
            UVM_ASSERT(!existing_gpu_state->page_table_range_big.table);
            UVM_ASSERT(!existing_gpu_state->page_table_range_4k.table);
            existing_gpu_state->pte_is_2m = false;
        }

        // existing can't possibly cover 2MB after a split, so drop any 2M PTE
        // references it has. We've taken the necessary references on the lower
        // tables above.
        block_put_ptes_safe(&gpu_va_space->page_tables, &existing_gpu_state->page_table_range_2m);
    }
}

NV_STATUS uvm_va_block_split(uvm_va_block_t *existing_va_block,
                             NvU64 new_end,
                             uvm_va_block_t **new_va_block,
                             uvm_va_range_t *new_va_range)
{
    uvm_va_space_t *va_space;
    uvm_va_block_t *new_block = NULL;
    uvm_gpu_id_t id;
    NV_STATUS status;

    va_space = new_va_range->va_space;
    UVM_ASSERT(existing_va_block->va_range);
    UVM_ASSERT(existing_va_block->va_range->va_space == va_space);

    // External range types can't be split
    UVM_ASSERT(existing_va_block->va_range->type == UVM_VA_RANGE_TYPE_MANAGED);
    UVM_ASSERT(new_va_range->type == UVM_VA_RANGE_TYPE_MANAGED);
    uvm_assert_rwsem_locked_write(&va_space->lock);

    UVM_ASSERT(new_end > existing_va_block->start);
    UVM_ASSERT(new_end < existing_va_block->end);
    UVM_ASSERT(PAGE_ALIGNED(new_end + 1));

    status = uvm_va_block_create(new_va_range, new_end + 1, existing_va_block->end, &new_block);
    if (status != NV_OK)
        return status;

    // We're protected from other splits and faults by the va_space lock being
    // held in write mode, but that doesn't stop the reverse mapping (eviction
    // path) from inspecting the existing block. Stop those threads by taking
    // the block lock. When a reverse mapping thread takes this lock after the
    // split has been performed, it will have to re-inspect state and may see
    // that it should use the newly-split block instead.
    uvm_mutex_lock(&existing_va_block->lock);

    for_each_gpu_id(id)
        UVM_ASSERT(block_check_chunks(existing_va_block, id));

    // As soon as we update existing's reverse mappings to point to the newly-
    // split block, the eviction path could try to operate on the new block.
    // Lock that out too until new is ready.
    //
    // Note that we usually shouldn't nest block locks, but it's ok here because
    // we just created new_block so no other thread could possibly take it out
    // of order with existing's lock.
    uvm_mutex_lock_no_tracking(&new_block->lock);

    // The split has to be transactional, meaning that if we fail, the existing
    // block must not be modified. Handle that by pre-allocating everything we
    // might need under both existing and new at the start so we only have a
    // single point of failure.

    // Since pre-allocation might require allocating new PTEs, we have to handle
    // allocation retry which might drop existing's block lock. The
    // preallocation is split into two steps for that: the first part which
    // allocates and splits PTEs can handle having the block lock dropped then
    // re-taken. It won't modify existing_va_block other than adding new PTE
    // allocations and splitting existing PTEs, which is always safe.
    status = UVM_VA_BLOCK_RETRY_LOCKED(existing_va_block,
                                       NULL,
                                       uvm_va_block_split_presplit_ptes(existing_va_block, new_block));
    if (status != NV_OK)
        goto out;

    // Pre-allocate, stage two. This modifies existing_va_block in ways which
    // violate many assumptions (such as changing chunk size), but it will put
    // things back into place on a failure without dropping the block lock.
    status = uvm_va_block_split_preallocate_no_retry(existing_va_block, new_block);
    if (status != NV_OK)
        goto out;

    // We'll potentially be freeing page tables, so we need to wait for any
    // outstanding work before we start
    status = uvm_tracker_wait(&existing_va_block->tracker);
    if (status != NV_OK)
        goto out;

    // Update existing's state only once we're past all failure points

    uvm_va_block_split_cpu(existing_va_block, new_block);

    for_each_gpu_id(id)
        uvm_va_block_split_gpu(existing_va_block, new_block, id);

    // Update the size of the existing block first so that
    // block_set_processor_masks can use block_{set,clear}_resident_processor
    // that relies on the size to be correct.
    existing_va_block->end = new_end;

    block_set_processor_masks(existing_va_block);
    block_set_processor_masks(new_block);

out:
    // Run checks on existing_va_block even on failure, since an error must
    // leave the block in a consistent state.
    for_each_gpu_id(id) {
        UVM_ASSERT(block_check_chunks(existing_va_block, id));
        if (status == NV_OK)
            UVM_ASSERT(block_check_chunks(new_block, id));
    }

    UVM_ASSERT(block_check_mappings(existing_va_block));
    if (status == NV_OK)
        UVM_ASSERT(block_check_mappings(new_block));

    uvm_mutex_unlock_no_tracking(&new_block->lock);
    uvm_mutex_unlock(&existing_va_block->lock);

    if (status != NV_OK)
        uvm_va_block_release(new_block);
    else if (new_va_block)
        *new_va_block = new_block;

    return status;
}

uvm_prot_t uvm_va_block_compute_new_permission_after_fault(uvm_va_block_t *va_block,
                                                           uvm_va_block_region_t region,
                                                           uvm_processor_id_t fault_processor_id,
                                                           uvm_processor_id_t new_residency,
                                                           uvm_fault_access_type_t access_type)
{
    uvm_va_range_t *va_range;
    uvm_va_space_t *va_space;
    uvm_prot_t logical_prot, new_prot;

    // TODO: Bug 1766432: Refactor into policies. Current policy is
    //       query_promote: upgrade access privileges to avoid future faults IF
    //       they don't trigger further revocations.
    va_range = va_block->va_range;
    UVM_ASSERT(va_range);
    va_space = va_range->va_space;
    UVM_ASSERT(va_space);

    new_prot = uvm_fault_access_type_to_prot(access_type);
    logical_prot = uvm_va_range_logical_prot(va_range);

    UVM_ASSERT(logical_prot >= new_prot);

    // TODO: Bug 1765189: this is currently relying on the fact that
    //       uvm_va_block_service_faults_locked is servicing only one page at a
    //       time. This could be changed in the future to optimize multiple
    //       faults on contiguous pages.
    if (((va_range->read_duplication == UVM_READ_DUPLICATION_DISABLED &&
          uvm_va_space_can_read_duplicate(va_space, NULL)) ||
         (va_range->read_duplication == UVM_READ_DUPLICATION_UNSET &&
          uvm_page_mask_region_weight(va_block->read_duplicated_pages, region) == 0)) &&
        logical_prot > UVM_PROT_READ_ONLY && new_prot == UVM_PROT_READ_ONLY) {
        uvm_processor_mask_t processors_with_atomic_mapping;
        uvm_processor_mask_t revoke_processors;

        uvm_va_block_region_authorized_processors(va_block, region, UVM_PROT_READ_WRITE_ATOMIC,
                                                  &processors_with_atomic_mapping);

        uvm_processor_mask_andnot(&revoke_processors,
                                  &processors_with_atomic_mapping, &va_space->has_native_atomics[new_residency]);

        // Only check if there are no faultable processors in the revoke processors mask
        uvm_processor_mask_and(&revoke_processors, &revoke_processors, &va_space->faultable_processors);

        if (uvm_processor_mask_empty(&revoke_processors))
            new_prot = UVM_PROT_READ_WRITE;
    }
    if (logical_prot == UVM_PROT_READ_WRITE_ATOMIC && new_prot == UVM_PROT_READ_WRITE) {
        if (uvm_processor_mask_test(&va_space->has_native_atomics[new_residency], fault_processor_id))
            new_prot = UVM_PROT_READ_WRITE_ATOMIC;
    }

    return new_prot;
}

NV_STATUS uvm_va_block_add_mappings_after_migration(uvm_va_block_t *va_block,
                                                    uvm_va_block_context_t *va_block_context,
                                                    uvm_processor_id_t new_residency,
                                                    uvm_processor_id_t processor_id,
                                                    uvm_va_block_region_t region,
                                                    const unsigned long *map_page_mask,
                                                    uvm_prot_t max_prot,
                                                    const uvm_processor_mask_t *thrashing_processors,
                                                    uvm_tracker_t *out_tracker)
{
    NV_STATUS status = NV_OK;
    uvm_processor_mask_t map_other_processors, map_uvm_lite_gpus;
    uvm_processor_id_t map_processor_id;
    uvm_va_range_t *va_range = va_block->va_range;
    uvm_va_space_t *va_space = va_range->va_space;
    uvm_prot_t new_map_prot = max_prot;
    const unsigned long *final_page_mask = map_page_mask;

    // Read duplication takes precedence over SetAccesedBy.
    //
    // Exclude ranges with read duplication set...
    if (va_range->read_duplication == UVM_READ_DUPLICATION_ENABLED && uvm_va_space_can_read_duplicate(va_space, NULL))
        return NV_OK;

    // ... and pages read-duplicated by performance heuristics
    if (va_range->read_duplication == UVM_READ_DUPLICATION_UNSET) {
        if (map_page_mask) {
            uvm_page_mask_andnot(va_block_context->mapping.filtered_page_mask,
                                 map_page_mask,
                                 va_block->read_duplicated_pages);
        }
        else {
            uvm_page_mask_complement(va_block_context->mapping.filtered_page_mask, va_block->read_duplicated_pages);
        }
        final_page_mask = va_block_context->mapping.filtered_page_mask;
    }

    // Add mappings for accessed_by processors and the given processor mask
    if (thrashing_processors)
        uvm_processor_mask_or(&map_other_processors, &va_range->accessed_by, thrashing_processors);
    else
        uvm_processor_mask_copy(&map_other_processors, &va_range->accessed_by);

    // Only processors that can access the new location must be considered
    uvm_processor_mask_and(&map_other_processors, &map_other_processors, &va_space->accessible_from[new_residency]);

    // Exclude caller processor as it must have already been mapped
    uvm_processor_mask_clear(&map_other_processors, processor_id);

    // Exclude preferred location so it won't get remote mappings
    if (va_range->preferred_location < UVM8_MAX_PROCESSORS && va_range->preferred_location != new_residency)
        uvm_processor_mask_clear(&map_other_processors, va_range->preferred_location);

    // Map the UVM-Lite GPUs if the new location is the preferred location. This
    // will only create mappings on first touch. After that they're persistent
    // so uvm_va_block_map will be a no-op.
    uvm_processor_mask_and(&map_uvm_lite_gpus, &map_other_processors, &va_range->uvm_lite_gpus);
    if (!uvm_processor_mask_empty(&map_uvm_lite_gpus) && new_residency == va_range->preferred_location) {
        for_each_id_in_mask(map_processor_id, &map_uvm_lite_gpus) {
            status = uvm_va_block_map(va_block,
                                      va_block_context,
                                      map_processor_id,
                                      region,
                                      final_page_mask,
                                      UVM_PROT_READ_WRITE_ATOMIC,
                                      UvmEventMapRemoteCauseCoherence,
                                      out_tracker);
            if (status != NV_OK)
                return status;
        }
    }

    uvm_processor_mask_andnot(&map_other_processors, &map_other_processors, &va_range->uvm_lite_gpus);

    // Handle atomic mappings separately
    if (max_prot == UVM_PROT_READ_WRITE_ATOMIC) {
        bool this_processor_has_native_atomics;

        this_processor_has_native_atomics = uvm_processor_mask_test(&va_space->has_native_atomics[new_residency],
                                                                    processor_id);

        if (this_processor_has_native_atomics) {
            uvm_processor_mask_t map_atomic_processors;

            // Compute processors with native atomics to the residency
            uvm_processor_mask_and(&map_atomic_processors,
                                   &map_other_processors, &va_space->has_native_atomics[new_residency]);

            // Filter out these mapped processors for the next steps
            uvm_processor_mask_andnot(&map_other_processors,
                                      &map_other_processors, &map_atomic_processors);

            for_each_id_in_mask(map_processor_id, &map_atomic_processors) {
                UvmEventMapRemoteCause cause;

                if (thrashing_processors && uvm_processor_mask_test(thrashing_processors, map_processor_id))
                    cause = UvmEventMapRemoteCauseThrashing;
                else
                    cause = UvmEventMapRemoteCausePolicy;

                status = uvm_va_block_map(va_block,
                                          va_block_context,
                                          map_processor_id,
                                          region,
                                          final_page_mask,
                                          UVM_PROT_READ_WRITE_ATOMIC,
                                          cause,
                                          out_tracker);
                if (status != NV_OK)
                    return status;
            }

            new_map_prot = UVM_PROT_READ_WRITE;
        }
        else {
            if (processor_id == UVM_CPU_ID)
                new_map_prot = UVM_PROT_READ_WRITE;
            else
                new_map_prot = UVM_PROT_READ_ONLY;
        }
    }

    // Map the rest of processors
    for_each_id_in_mask(map_processor_id, &map_other_processors) {
        UvmEventMapRemoteCause cause;
        uvm_prot_t final_map_prot;
        bool map_processor_has_enabled_system_wide_atomics =
            uvm_processor_mask_test(&va_space->system_wide_atomics_enabled_processors, map_processor_id);

        // Write mappings from processors with disabled system-wide atomics are treated like atomics
        if (new_map_prot == UVM_PROT_READ_WRITE && !map_processor_has_enabled_system_wide_atomics)
            final_map_prot = UVM_PROT_READ_WRITE_ATOMIC;
        else
            final_map_prot = new_map_prot;

        if (thrashing_processors && uvm_processor_mask_test(thrashing_processors, map_processor_id))
            cause = UvmEventMapRemoteCauseThrashing;
        else
            cause = UvmEventMapRemoteCausePolicy;

        status = uvm_va_block_map(va_block,
                                  va_block_context,
                                  map_processor_id,
                                  region,
                                  final_page_mask,
                                  final_map_prot,
                                  cause,
                                  out_tracker);
        if (status != NV_OK)
            return status;
    }

    return status;
}

// TODO: Bug 1750144: check logical permissions from HMM to know what's the
//       maximum allowed.
static uvm_prot_t block_page_compute_highest_permission(uvm_va_block_t *va_block,
                                                        uvm_processor_id_t processor_id,
                                                        size_t page_index)
{
    uvm_va_block_region_t region = uvm_va_block_region(page_index, page_index + 1);
    uvm_va_range_t *va_range = va_block->va_range;
    uvm_va_space_t *va_space = va_range->va_space;
    uvm_processor_mask_t resident_processors;
    size_t resident_processors_count;

    if (uvm_processor_mask_test(&va_range->uvm_lite_gpus, processor_id))
        return UVM_PROT_READ_WRITE_ATOMIC;

    uvm_va_block_page_resident_processors(va_block, page_index, &resident_processors);
    resident_processors_count = uvm_processor_mask_get_count(&resident_processors);

    if (resident_processors_count == 0) {
        return UVM_PROT_NONE;
    }
    else if (resident_processors_count > 1) {
        // If there are many copies, we can only map READ ONLY
        //
        // The block state doesn't track the mapping target (aperture) of each
        // individual PTE, just the permissions and where the data is resident.
        // If the data is resident in multiple places, then we have a problem
        // since we can't know where the PTE points. This means we won't know
        // what needs to be unmapped for cases like UvmUnregisterGpu and
        // UvmDisablePeerAccess.
        //
        // The simple way to solve this is to enforce that a read-duplication
        // mapping always points to local memory.
        if (uvm_processor_mask_test(&resident_processors, processor_id))
            return UVM_PROT_READ_ONLY;

        return UVM_PROT_NONE;
    }
    else {
        uvm_processor_id_t atomic_id;
        uvm_processor_id_t residency;
        uvm_processor_mask_t atomic_mappings;
        uvm_processor_mask_t write_mappings;

        // Search the id of the processor with the only resident copy
        residency = uvm_processor_mask_find_first_id(&resident_processors);
        UVM_ASSERT(residency < UVM8_MAX_PROCESSORS);

        // If we cannot map the processor with the resident copy, exit
        if (!uvm_processor_mask_test(&va_space->accessible_from[residency], processor_id))
            return UVM_PROT_NONE;

        uvm_va_block_region_authorized_processors(va_block, region, UVM_PROT_READ_WRITE_ATOMIC, &atomic_mappings);
        // Exclude the processor for which the mapping protections are being computed
        uvm_processor_mask_clear(&atomic_mappings, processor_id);

        // If there is any processor with atomic mapping, check if it has native atomics to the processor
        // with the resident copy. If it does not, we can only map READ ONLY
        atomic_id = uvm_processor_mask_find_first_id(&atomic_mappings);
        if (atomic_id < UVM8_MAX_PROCESSORS &&
            !uvm_processor_mask_test(&va_space->has_native_atomics[residency], atomic_id))
            return UVM_PROT_READ_ONLY;

        uvm_va_block_region_authorized_processors(va_block, region, UVM_PROT_READ_WRITE, &write_mappings);
        // Exclude the processor for which the mapping protections are being computed
        uvm_processor_mask_clear(&write_mappings, processor_id);

        // At this point, any processor with atomic mappings has native atomics support to the processor
        // with the resident copy. Therefore, if we also have native atomics to that processor, we can
        // map with ATOMIC privileges. Likewise, if there are no other processors with WRITE or ATOMIC
        // mappings, we can map with ATOMIC privileges.
        if (uvm_processor_mask_test(&va_space->has_native_atomics[residency], processor_id) ||
            uvm_processor_mask_empty(&write_mappings))
            return UVM_PROT_READ_WRITE_ATOMIC;

        return UVM_PROT_READ_WRITE;
    }
}

NV_STATUS uvm_va_block_add_mappings(uvm_va_block_t *va_block,
                                    uvm_va_block_context_t *va_block_context,
                                    uvm_processor_id_t processor_id,
                                    uvm_va_block_region_t region,
                                    const unsigned long *map_page_mask,
                                    uvm_tracker_t *out_tracker)
{
    NV_STATUS status = NV_OK;
    size_t page_index;
    uvm_range_group_range_iter_t iter;

    if (processor_id == UVM_CPU_ID) {
        if (uvm_va_range_vma_current(va_block->va_range) == NULL)
            return NV_OK;

        uvm_range_group_range_migratability_iter_first(va_block->va_range->va_space,
                                                       uvm_va_block_region_start(va_block, region),
                                                       uvm_va_block_region_end(va_block, region),
                                                       &iter);
    }

    for_each_va_block_page_in_mask(page_index, map_page_mask, region) {
        uvm_prot_t prot_to_map;

        // Read duplication takes precedence over SetAccesedBy. Exclude pages
        // read-duplicated by performance heuristics
        if (test_bit(page_index, va_block->read_duplicated_pages))
            continue;

        prot_to_map = block_page_compute_highest_permission(va_block, processor_id, page_index);

        if (prot_to_map == UVM_PROT_NONE)
            continue;

        if (processor_id == UVM_CPU_ID) {
            while (uvm_va_block_cpu_page_index(va_block, iter.end) < page_index) {
                uvm_range_group_range_migratability_iter_next(va_block->va_range->va_space,
                                                              &iter,
                                                              uvm_va_block_region_end(va_block, region));
            }
            if (iter.migratable)
                status = block_map_cpu_page(va_block, page_index, prot_to_map, false);
        }
        else {
            status = uvm_va_block_map(va_block,
                                      va_block_context,
                                      processor_id,
                                      uvm_va_block_region(page_index, page_index + 1),
                                      NULL,
                                      prot_to_map,
                                      UvmEventMapRemoteCausePolicy,
                                      out_tracker);
        }

        if (status != NV_OK)
            break;
    }

    return status;
}

static bool can_read_duplicate(uvm_va_block_t *va_block, uvm_va_block_region_t region,
                               uvm_fault_access_type_t access_type, const uvm_perf_thrashing_hint_t *thrashing_hint)
{
    return access_type >= UVM_FAULT_ACCESS_TYPE_READ &&
        ((va_block->va_range->read_duplication == UVM_READ_DUPLICATION_ENABLED &&
          uvm_va_space_can_read_duplicate(va_block->va_range->va_space, NULL)) ||
         (va_block->va_range->read_duplication != UVM_READ_DUPLICATION_DISABLED &&
          uvm_page_mask_region_full(va_block->read_duplicated_pages, region) &&
          thrashing_hint->type != UVM_PERF_THRASHING_HINT_TYPE_PIN));
}

uvm_processor_id_t uvm_va_block_select_residency_after_fault(uvm_va_block_t *va_block,
                                                             uvm_va_block_region_t region,
                                                             uvm_processor_id_t processor_id,
                                                             uvm_fault_access_type_t access_type,
                                                             const uvm_perf_thrashing_hint_t *thrashing_hint,
                                                             bool *read_duplicate)
{
    uvm_processor_id_t closest_resident_processor;
    uvm_va_range_t *va_range = va_block->va_range;
    uvm_va_space_t *va_space = va_range->va_space;

    if (is_uvm_fault_force_sysmem_set())
        return UVM_CPU_ID;

    // Read/prefetch faults on a VA range with read duplication enabled
    // always create a copy of the page on the faulting processor's memory
    *read_duplicate = can_read_duplicate(va_block, region, access_type, thrashing_hint);

    // CPU accesses will always make it the new resident processor
    if (processor_id == UVM_CPU_ID || *read_duplicate)
        return processor_id;

    *read_duplicate = false;

    // If the faulting GPU is the preferred location always migrate
    if (processor_id == va_range->preferred_location)
        return processor_id;

    if (thrashing_hint->type == UVM_PERF_THRASHING_HINT_TYPE_PIN) {
        UVM_ASSERT(uvm_processor_mask_test(&va_range->va_space->accessible_from[thrashing_hint->pin.residency],
                                           processor_id));
        return thrashing_hint->pin.residency;
    }

    // TODO: Bug 1765189: this is currently relying on the fact that
    //       uvm_va_block_service_faults_locked is servicing only one page at a
    //       time. This could be changed in the future to optimize multiple
    //       faults on contiguous pages.
    closest_resident_processor = uvm_va_block_page_get_closest_resident(va_block, region.first, processor_id);

    // If the block is not resident or is resident on a processor other than the preferred location,
    // we select the faulting GPU as the new residency, unless the faulting GPU has support for native
    // atomics to the current location and the fault was due to an atomic access. In the later case we
    // keep the current residency
    //
    // TODO: Bug id 1716025: UVM needs to implement performance heuristics to provide transparent data
    //                       transfer optimizations
    //
    // This is a short-term solution to exercise remote atomics over NVLINK when possible (not only when
    // preferred location is set to the remote GPU) as they are much faster than relying on page faults
    // and permission downgrades, which cause thrashing. In the future, the thrashing
    // detection/prevention heuristics will detect and handle this case.
    if (closest_resident_processor != UVM_MAX_PROCESSORS &&
        access_type == UVM_FAULT_ACCESS_TYPE_ATOMIC &&
        uvm_processor_mask_test(&va_space->has_native_atomics[closest_resident_processor], processor_id))
        return closest_resident_processor;

    if (closest_resident_processor == UVM_MAX_PROCESSORS ||
        closest_resident_processor != va_range->preferred_location)
        return processor_id;

    // If the faulting GPU can't access the current residency, we migrate the VA range
    if (!uvm_processor_mask_test(&va_range->va_space->accessible_from[va_range->preferred_location], processor_id))
        return processor_id;

    // If the faulting GPU can access the current residency, keep the VA range on its preferred location
    return va_range->preferred_location;
}

NV_STATUS uvm_va_block_service_faults_locked(uvm_processor_id_t processor_id, uvm_va_block_t *va_block,
                                             uvm_va_block_retry_t *block_retry,
                                             uvm_fault_service_block_context_t *service_context)
{
    NV_STATUS status = NV_OK;
    NV_STATUS tracker_status;
    uvm_processor_id_t new_residency;
    uvm_prot_t new_prot;
    uvm_tracker_t local_tracker = UVM_TRACKER_INIT();
    uvm_va_range_t *va_range = va_block->va_range;
    uvm_va_space_t *va_space = va_range->va_space;
    uvm_perf_prefetch_hint_t prefetch_hint = UVM_PERF_PREFETCH_HINT_NONE();
    uvm_processor_mask_t processors_involved_in_cpu_migration;

    uvm_assert_mutex_locked(&va_block->lock);
    UVM_ASSERT(va_range->type == UVM_VA_RANGE_TYPE_MANAGED);

    // GPU fault servicing must be done under the VA space read lock. GPU fault
    // servicing is required for RM to make forward progress, and we allow other
    // threads to call into RM while holding the VA space lock in read mode. If
    // we took the VA space lock in write mode on the GPU fault service path,
    // we could deadlock because the thread in RM which holds the VA space lock
    // for read wouldn't be able to complete until fault servicing completes.
    if (processor_id == UVM_CPU_ID)
        uvm_assert_rwsem_locked(&va_space->lock);
    else
        uvm_assert_rwsem_locked_read(&va_space->lock);

    // Performance heuristics policy: we only consider prefetching when faults
    // trigger migratons to a single processor.
    if (uvm_processor_mask_get_count(&service_context->resident_processors) == 1) {
        size_t page_index;
        uvm_va_block_region_t block_region = uvm_va_block_region_from_block(va_block);
        new_residency = uvm_processor_mask_find_first_id(&service_context->resident_processors);

        // Update prefetch tracking structure with the pages that will migrate
        // due to faults
        uvm_perf_prefetch_prenotify_fault_migrations(va_block, new_residency,
                                                     service_context->per_processor_masks[new_residency].new_residency,
                                                     service_context->fault_region);

        prefetch_hint = uvm_perf_prefetch_get_hint(va_block);

        // Obtain the prefetch hint and give a fake fault access type to the
        // prefetched pages
        if (prefetch_hint.residency != UVM8_MAX_PROCESSORS) {
            UVM_ASSERT(prefetch_hint.prefetch_pages_mask != NULL);

            for_each_va_block_page_in_mask(page_index, prefetch_hint.prefetch_pages_mask, block_region) {
                UVM_ASSERT(!test_bit(page_index, service_context->per_processor_masks[new_residency].new_residency));

                service_context->fault_access_type[page_index] = UVM_FAULT_ACCESS_TYPE_PREFETCH;

                if ((va_range->read_duplication == UVM_READ_DUPLICATION_ENABLED &&
                     uvm_va_space_can_read_duplicate(va_space, NULL)) ||
                    (va_range->read_duplication != UVM_READ_DUPLICATION_DISABLED &&
                     test_bit(page_index, va_block->read_duplicated_pages))) {
                    if (service_context->read_duplicate_count++ == 0)
                        uvm_page_mask_zero(service_context->read_duplicate_mask);

                    __set_bit(page_index, service_context->read_duplicate_mask);
                }
            }

            service_context->fault_region = block_region;
        }
    }

    service_context->read_mapping_count = 0;
    service_context->write_mapping_count = 0;
    service_context->atomic_mapping_count = 0;
    uvm_processor_mask_zero(&processors_involved_in_cpu_migration);

    // 1- Migrate pages and compute mapping protections
    for_each_id_in_mask(new_residency, &service_context->resident_processors) {
        unsigned long *did_migrate_mask = service_context->block_context.make_resident.pages_changed_residency;
        size_t page_index;

        // 1.1- Migrate pages

        // Reset masks before all of the make_resident calls
        uvm_page_mask_zero(did_migrate_mask);
        uvm_processor_mask_zero(&service_context->block_context.make_resident.all_involved_processors);

        if (service_context->read_duplicate_count == 0) {
            status = uvm_va_block_make_resident(va_block,
                                                block_retry,
                                                &service_context->block_context,
                                                new_residency,
                                                service_context->fault_region,
                                                service_context->per_processor_masks[new_residency].new_residency,
                                                UvmEventMigrationCauseCoherence);
        }
        else {
            bool do_migration;
            do_migration = uvm_page_mask_andnot(service_context->block_context.caller_page_mask,
                                                service_context->per_processor_masks[new_residency].new_residency,
                                                service_context->read_duplicate_mask);
            if (do_migration) {
                status = uvm_va_block_make_resident(va_block,
                                                    block_retry,
                                                    &service_context->block_context,
                                                    new_residency,
                                                    service_context->fault_region,
                                                    service_context->block_context.caller_page_mask,
                                                    UvmEventMigrationCauseCoherence);

                if (status != NV_OK)
                    goto done;
            }

            do_migration = uvm_page_mask_and(service_context->block_context.caller_page_mask,
                                             service_context->per_processor_masks[new_residency].new_residency,
                                             service_context->read_duplicate_mask);
            if (do_migration) {
                status = uvm_va_block_make_resident_read_duplicate(va_block,
                                                                   block_retry,
                                                                   &service_context->block_context,
                                                                   new_residency,
                                                                   service_context->fault_region,
                                                                   service_context->block_context.caller_page_mask,
                                                                   UvmEventMigrationCauseCoherence);
            }
        }
        if (status != NV_OK)
            goto done;

        if (prefetch_hint.residency != UVM8_MAX_PROCESSORS) {
            UVM_ASSERT(prefetch_hint.residency == new_residency);
            UVM_ASSERT(prefetch_hint.prefetch_pages_mask != NULL);

            // All prefetched pages in a block use the same read-duplicate policy
            if (service_context->read_duplicate_count == 0) {
                status = uvm_va_block_make_resident(va_block,
                                                    block_retry,
                                                    &service_context->block_context,
                                                    new_residency,
                                                    service_context->fault_region,
                                                    prefetch_hint.prefetch_pages_mask,
                                                    UvmEventMigrationCausePrefetch);
            }
            else {
                status = uvm_va_block_make_resident_read_duplicate(va_block,
                                                                   block_retry,
                                                                   &service_context->block_context,
                                                                   new_residency,
                                                                   service_context->fault_region,
                                                                   prefetch_hint.prefetch_pages_mask,
                                                                   UvmEventMigrationCausePrefetch);
            }
            if (status != NV_OK)
                goto done;

            uvm_page_mask_or(service_context->per_processor_masks[new_residency].new_residency,
                             service_context->per_processor_masks[new_residency].new_residency,
                             prefetch_hint.prefetch_pages_mask);
        }

        if (new_residency == UVM_CPU_ID) {
            // Save all the processors involved in migrations to the CPU for
            // an ECC check before establishing the CPU mappings.
            uvm_processor_mask_copy(&processors_involved_in_cpu_migration,
                                    &service_context->block_context.make_resident.all_involved_processors);
        }

        uvm_page_mask_andnot(service_context->did_not_migrate_mask,
                             service_context->per_processor_masks[new_residency].new_residency,
                             did_migrate_mask);

        // 1.2 - Compute mapping protections for the faulting processor on the new residency
        for_each_va_block_page_in_mask(page_index,
                                       service_context->per_processor_masks[new_residency].new_residency,
                                       service_context->fault_region) {
            uvm_va_block_region_t page_region = uvm_va_block_region(page_index, page_index + 1);
            new_prot = uvm_va_block_compute_new_permission_after_fault(va_block,
                                                                       page_region,
                                                                       processor_id,
                                                                       new_residency,
                                                                       service_context->fault_access_type[page_index]);

            if (new_prot == UVM_PROT_READ_ONLY) {
                if (service_context->read_mapping_count++ == 0)
                    uvm_page_mask_zero(service_context->read_mapping_mask);

                __set_bit(page_index, service_context->read_mapping_mask);
            }
            else if (new_prot == UVM_PROT_READ_WRITE) {
                if (service_context->write_mapping_count++ == 0)
                    uvm_page_mask_zero(service_context->write_mapping_mask);

                __set_bit(page_index, service_context->write_mapping_mask);
            }
            else if (new_prot == UVM_PROT_READ_WRITE_ATOMIC) {
                if (service_context->atomic_mapping_count++ == 0)
                    uvm_page_mask_zero(service_context->atomic_mapping_mask);

                __set_bit(page_index, service_context->atomic_mapping_mask);
            }
        }

        // 1.3- Revoke permissions
        //
        // NOTE: uvm_va_block_make_resident destroys mappings to old locations.
        //       Thus, we need to revoke only if residency did not change and we
        //       are mapping higher than READ ONLY.
        for (new_prot = UVM_PROT_READ_WRITE; new_prot <= UVM_PROT_READ_WRITE_ATOMIC; ++new_prot) {
            bool pages_need_revocation = false;

            if (new_prot == UVM_PROT_READ_WRITE && service_context->write_mapping_count > 0) {
                pages_need_revocation = uvm_page_mask_and(service_context->revocation_mask,
                                                          service_context->did_not_migrate_mask,
                                                          service_context->write_mapping_mask);
            }
            else if (new_prot == UVM_PROT_READ_WRITE_ATOMIC && service_context->atomic_mapping_count > 0) {
                pages_need_revocation = uvm_page_mask_and(service_context->revocation_mask,
                                                          service_context->did_not_migrate_mask,
                                                          service_context->atomic_mapping_mask);
            }
            else {
                continue;
            }

            if (pages_need_revocation) {
                uvm_processor_mask_t revoke_processors;
                uvm_prot_t revoke_prot;
                bool this_processor_has_enabled_atomics;
                uvm_processor_id_t revoke_id;

                uvm_processor_mask_and(&revoke_processors,
                                       &va_block->mapped, &va_space->faultable_processors);
                // Do not revoke the processor that took the fault
                uvm_processor_mask_clear(&revoke_processors, processor_id);

                this_processor_has_enabled_atomics = uvm_processor_mask_test(&va_space->system_wide_atomics_enabled_processors,
                                                                             processor_id);

                if (new_prot == UVM_PROT_READ_WRITE || !this_processor_has_enabled_atomics) {
                    // Exclude processors with native atomics on the resident copy
                    uvm_processor_mask_andnot(&revoke_processors,
                                              &revoke_processors, &va_space->has_native_atomics[new_residency]);

                    // Exclude processors with disabled system-wide atomics
                    uvm_processor_mask_and(&revoke_processors,
                                           &revoke_processors, &va_space->system_wide_atomics_enabled_processors);
                }
                else {
                    // new_prot == UVM_PROT_READ_WRITE_ATOMIC && this_processor_has_enabled_atomics
                    //
                    // If the processor taking the fault has native atomics on the new residency, do not revoke processors that
                    // have native atomics on that memory
                    bool this_processor_has_native_atomics = uvm_processor_mask_test(&va_space->has_native_atomics[new_residency],
                                                                                     processor_id);
                    if (this_processor_has_native_atomics) {
                        // Exclude processors with native atomics on the resident copy
                        uvm_processor_mask_andnot(&revoke_processors,
                                                  &revoke_processors, &va_space->has_native_atomics[new_residency]);
                    }
                }

                if (processor_id == UVM_CPU_ID) {
                    revoke_prot = UVM_PROT_READ_WRITE_ATOMIC;
                }
                else {
                    revoke_prot = (new_prot == UVM_PROT_READ_WRITE_ATOMIC)? UVM_PROT_READ_WRITE:
                                                                            UVM_PROT_READ_WRITE_ATOMIC;
                }

                // UVM-Lite processors must always have RWA mappings
                uvm_processor_mask_andnot(&revoke_processors,
                                          &revoke_processors, &va_block->va_range->uvm_lite_gpus);

                for_each_id_in_mask(revoke_id, &revoke_processors) {
                    // Downgrade other processors' mappings
                    status = uvm_va_block_revoke_prot(va_block,
                                                      &service_context->block_context,
                                                      revoke_id,
                                                      service_context->fault_region,
                                                      service_context->revocation_mask,
                                                      revoke_prot,
                                                      &local_tracker);
                    if (status != NV_OK)
                        goto done;
                }
            }
        }
    }

    // 2- Wait for revocations before creating/upgrading mappings
    status = uvm_tracker_add_tracker_safe(&va_block->tracker, &local_tracker);
    if (status != NV_OK)
        goto done;

    // Check for ECC errors on all GPUs involved in the migration if CPU is the
    // destination. Migrations in response to CPU faults are special because
    // they're on the only path (apart from tools) where CUDA is not involved
    // and wouldn't have a chance to do its own ECC checking.
    if (processor_id == UVM_CPU_ID) {
        uvm_processor_mask_zero(&service_context->cpu_fault_gpus_to_check_for_ecc);

        if (!uvm_processor_mask_empty(&processors_involved_in_cpu_migration)) {
            uvm_gpu_id_t gpu_id;

            // Before checking for ECC errors, make sure all of the GPU work
            // is finished. Creating mappings on the CPU would have to wait
            // for the tracker anyway so this shouldn't hurt performance.
            status = uvm_tracker_wait(&va_block->tracker);
            if (status != NV_OK)
                goto done;

            for_each_gpu_id_in_mask(gpu_id, &processors_involved_in_cpu_migration) {
                // We cannot call into RM here so use the no RM ECC check.
                status = uvm_gpu_check_ecc_error_no_rm(uvm_gpu_get(gpu_id));
                if (status == NV_WARN_MORE_PROCESSING_REQUIRED) {
                    // In case we need to call into RM to be sure whether
                    // there is an ECC error or not, signal that to the
                    // caller by adding the GPU to the mask.
                    //
                    // In that case the ECC error might be noticed only after
                    // the CPU mappings have been already created below,
                    // exposing different CPU threads to the possibly corrupt
                    // data, but this thread will fault eventually and that's
                    // considered to be an acceptable trade-off between
                    // performance and ECC error containment.
                    uvm_processor_mask_set(&service_context->cpu_fault_gpus_to_check_for_ecc, gpu_id);
                    status = NV_OK;
                }
                if (status != NV_OK)
                    goto done;
            }
        }
    }

    // 3- Map faulting processor with the necessary privileges
    for (new_prot = UVM_PROT_READ_ONLY; new_prot <= UVM_PROT_READ_WRITE_ATOMIC; ++new_prot) {
        unsigned long *map_mask;

        if (new_prot == UVM_PROT_READ_ONLY && service_context->read_mapping_count > 0)
            map_mask = service_context->read_mapping_mask;
        else if (new_prot == UVM_PROT_READ_WRITE && service_context->write_mapping_count > 0)
            map_mask = service_context->write_mapping_mask;
        else if (new_prot == UVM_PROT_READ_WRITE_ATOMIC && service_context->atomic_mapping_count > 0)
            map_mask = service_context->atomic_mapping_mask;
        else
            continue;

        if (processor_id == UVM_CPU_ID) {
            bool force_remap;
            size_t page_index;

            // TODO: Bug 1765432:
            //       The kernel can downgrade our CPU mappings at any time without
            //       notifying us, which means our PTE state could be stale. We handle
            //       this for now by always forcibly re-mapping the CPU PTE, but we
            //       could instead track if we get a bunch of unexpected faults, and
            //       only forcibly unmap once we've exceeded some threshold.
            //
            //       A CPU fault is unexpected if:
            //       curr_prot == RW || (!is_write && curr_prot == RO)
            //
            //       But, note that we could also get "unexpected" faults if multiple
            //       CPU threads fault on the same address simultaneously and race to
            //       create the mapping.
            force_remap = true;

            // Note: don't use uvm_va_block_map because that will check
            // uvm_va_range_vma_current to see if the current thread is allowed to call
            // vm_insert_page. current->mm might not be vma->vm_mm, but that's ok
            // because the kernel won't trigger a fault in vma->vm_mm without holding a
            // reference on it.
            for_each_va_block_page_in_mask(page_index, map_mask, service_context->fault_region) {
                status = block_map_cpu_page(va_block, page_index, new_prot, force_remap);
                if (status != NV_OK)
                    goto done;
            }
        }
        else {
            if (service_context->thrashing_pin_count == 0 || !va_space->tools.enabled) {
                status = uvm_va_block_map(va_block,
                                          &service_context->block_context,
                                          processor_id,
                                          service_context->fault_region,
                                          map_mask,
                                          new_prot,
                                          UvmEventMapRemoteCausePolicy,
                                          &local_tracker);

                if (status != NV_OK)
                    goto done;
            }
            else {
                size_t page_index;

                // Slow path if thrashing. Go page by page to determine the cause of potential remote mappings
                for_each_va_block_page_in_mask(page_index, map_mask, service_context->fault_region) {
                    UvmEventMapRemoteCause cause = UvmEventMapRemoteCausePolicy;

                    if (service_context->thrashing_pin_count > 0 &&
                        test_bit(page_index, service_context->thrashing_pin_mask)) {
                        cause = UvmEventMapRemoteCauseThrashing;
                    }

                    status = uvm_va_block_map(va_block,
                                              &service_context->block_context,
                                              processor_id,
                                              uvm_va_block_region(page_index, page_index + 1),
                                              map_mask,
                                              new_prot,
                                              cause,
                                              &local_tracker);

                    if (status != NV_OK)
                        goto done;
                }
            }
        }
    }

    // 4- If pages did migrate, map SetAccessedBy processors, except for UVM-Lite
    for_each_id_in_mask(new_residency, &service_context->resident_processors) {
        for (new_prot = UVM_PROT_READ_ONLY; new_prot <= UVM_PROT_READ_WRITE_ATOMIC; ++new_prot) {
            bool pages_need_mapping = false;

            if (new_prot == UVM_PROT_READ_ONLY && service_context->read_mapping_count > 0) {
                pages_need_mapping = uvm_page_mask_and(service_context->map_mask,
                                                       service_context->per_processor_masks[new_residency].new_residency,
                                                       service_context->read_mapping_mask);
            }
            else if (new_prot == UVM_PROT_READ_WRITE && service_context->write_mapping_count > 0) {
                pages_need_mapping = uvm_page_mask_and(service_context->map_mask,
                                                       service_context->per_processor_masks[new_residency].new_residency,
                                                       service_context->write_mapping_mask);
            }
            else if (new_prot == UVM_PROT_READ_WRITE_ATOMIC && service_context->atomic_mapping_count > 0) {
                pages_need_mapping = uvm_page_mask_and(service_context->map_mask,
                                                       service_context->per_processor_masks[new_residency].new_residency,
                                                       service_context->atomic_mapping_mask);
            }

            if (!pages_need_mapping)
                continue;

            if (service_context->thrashing_pin_count == 0) {
                // Fast path if no thrashing. Try to map all pages in a single call
                status = uvm_va_block_add_mappings_after_migration(va_block,
                                                                   &service_context->block_context,
                                                                   new_residency,
                                                                   processor_id,
                                                                   service_context->fault_region,
                                                                   service_context->map_mask,
                                                                   new_prot,
                                                                   NULL,
                                                                   &local_tracker);
                if (status != NV_OK)
                    goto done;
            }
            else {
                size_t page_index;

                // Slow path if thrashing. Go page by page
                for_each_va_block_page_in_mask(page_index, service_context->map_mask, service_context->fault_region) {
                    uvm_processor_mask_t *map_thrashing_processors = NULL;
                    NvU64 page_addr = va_block->start + page_index * PAGE_SIZE;

                    if (test_bit(page_index, service_context->thrashing_pin_mask))
                        map_thrashing_processors = uvm_perf_thrashing_get_thrashing_processors(va_block, page_addr);

                    status = uvm_va_block_add_mappings_after_migration(va_block,
                                                                       &service_context->block_context,
                                                                       new_residency,
                                                                       processor_id,
                                                                       uvm_va_block_region(page_index, page_index + 1),
                                                                       service_context->map_mask,
                                                                       new_prot,
                                                                       map_thrashing_processors,
                                                                       &local_tracker);
                    if (status != NV_OK)
                        goto done;
                }
            }
        }
    }

done:
    tracker_status = uvm_tracker_add_tracker_safe(&va_block->tracker, &local_tracker);

    uvm_tracker_deinit(&local_tracker);

    return status == NV_OK ? tracker_status : status;
}

static NV_STATUS block_cpu_fault_locked(uvm_va_block_t *va_block,
                                        NvU64 fault_addr,
                                        uvm_fault_access_type_t fault_access_type,
                                        uvm_fault_service_block_context_t *service_context)
{
    uvm_va_range_t *va_range = va_block->va_range;
    NV_STATUS status = NV_OK;
    uvm_perf_event_data_t event_data;
    uvm_va_block_region_t region;
    uvm_perf_thrashing_hint_t thrashing_hint;
    uvm_processor_id_t new_residency;
    bool read_duplicate;

    UVM_ASSERT(va_range);
    uvm_assert_rwsem_locked(&va_range->va_space->lock);
    UVM_ASSERT(va_range->type == UVM_VA_RANGE_TYPE_MANAGED);

    UVM_ASSERT(fault_addr >= va_block->start);
    UVM_ASSERT(fault_addr <= va_block->end);

    if (service_context->num_retries == 0) {
        // notify event to tools/performance heuristics
        event_data.fault.block = va_block;
        event_data.fault.space = va_range->va_space;
        event_data.fault.proc_id = UVM_CPU_ID;
        event_data.fault.cpu.fault_va = fault_addr;
        event_data.fault.cpu.is_write = fault_access_type < UVM_FAULT_ACCESS_TYPE_READ;

        uvm_perf_event_notify(&va_range->va_space->perf_events, UVM_PERF_EVENT_FAULT, &event_data);
    }

    // Check logical permissions
    status = uvm_va_range_check_logical_permissions(va_block->va_range,
                                                    UVM_CPU_ID,
                                                    fault_access_type,
                                                    uvm_range_group_address_migratable(va_range->va_space, fault_addr));
    if (status != NV_OK)
        return status;

    thrashing_hint = uvm_perf_thrashing_get_hint(va_block, fault_addr, UVM_CPU_ID);
    // Throttling is implemented by sleeping in the fault handler on the CPU
    if (thrashing_hint.type == UVM_PERF_THRASHING_HINT_TYPE_THROTTLE)
        return NV_WARN_MORE_PROCESSING_REQUIRED;

    // The masks need to be fully zeroed as the fault region may grow due to prefetching
    uvm_page_mask_zero(service_context->per_processor_masks[UVM_CPU_ID].new_residency);

    service_context->read_duplicate_count = 0;
    service_context->thrashing_pin_count = 0;

    region = uvm_va_block_region_from_start_size(va_block, fault_addr, PAGE_SIZE);
    if (thrashing_hint.type == UVM_PERF_THRASHING_HINT_TYPE_PIN) {
        uvm_page_mask_zero(service_context->thrashing_pin_mask);
        __set_bit(region.first, service_context->thrashing_pin_mask);
        service_context->thrashing_pin_count = 1;
    }

    // Compute new residency and update the masks
    new_residency = uvm_va_block_select_residency_after_fault(va_block,
                                                              region,
                                                              UVM_CPU_ID,
                                                              fault_access_type,
                                                              &thrashing_hint,
                                                              &read_duplicate);
    UVM_ASSERT(new_residency == UVM_CPU_ID);

    // Initialize the minimum necessary state in the fault service context
    uvm_processor_mask_zero(&service_context->resident_processors);

    // Set new residency and update the masks
    uvm_processor_mask_set(&service_context->resident_processors, new_residency);

    __set_bit(region.first, service_context->per_processor_masks[new_residency].new_residency);

    if (read_duplicate) {
        uvm_page_mask_zero(service_context->read_duplicate_mask);
        __set_bit(region.first, service_context->read_duplicate_mask);
        service_context->read_duplicate_count = 1;
    }

    service_context->fault_access_type[region.first] = fault_access_type;

    service_context->fault_region = region;

    status = uvm_va_block_service_faults_locked(UVM_CPU_ID, va_block, NULL, service_context);

    ++service_context->num_retries;

    return status;
}

NV_STATUS uvm_va_block_cpu_fault(uvm_va_block_t *va_block,
                                 NvU64 fault_addr,
                                 bool is_write,
                                 uvm_processor_mask_t *gpus_to_check_for_ecc)
{
    NV_STATUS status = NV_OK;
    uvm_fault_service_block_context_t *service_context = NULL;
    uvm_fault_access_type_t fault_access_type;

    if (is_write)
        fault_access_type = UVM_FAULT_ACCESS_TYPE_ATOMIC;
    else
        fault_access_type = UVM_FAULT_ACCESS_TYPE_READ;

    service_context = get_cpu_fault_service_context();
    if (!service_context) {
        status = NV_ERR_NO_MEMORY;
        goto done;
    }

    service_context->num_retries = 0;

    // We have to use vm_insert_page instead of handing the page to the kernel
    // and letting it insert the mapping, and we must do that while holding the
    // lock on this VA block. Otherwise there will be a window in which we think
    // we've mapped the page but the CPU mapping hasn't actually been created
    // yet. During that window a GPU fault event could arrive and claim
    // ownership of that VA, "unmapping" it. Then later the kernel would
    // eventually establish the mapping, and we'd end up with both CPU and GPU
    // thinking they each owned the page.
    //
    // This function must only be called when it's safe to call vm_insert_page.
    // That is, there's a reference held on the vma's vm_mm and vm_mm->mmap_sem
    // is held in at least read mode, but current->mm might not be vma->vm_mm.
    status = UVM_VA_BLOCK_LOCK_RETRY(va_block, NULL,
                                     block_cpu_fault_locked(va_block,
                                                            fault_addr,
                                                            fault_access_type,
                                                            service_context));

   uvm_processor_mask_copy(gpus_to_check_for_ecc, &service_context->cpu_fault_gpus_to_check_for_ecc);

done:
    if (service_context)
        put_cpu_fault_service_context(service_context);

    if (status != NV_OK)
        return status;

    return NV_OK;
}

NV_STATUS uvm_va_block_find(uvm_va_space_t *va_space, NvU64 addr, uvm_va_block_t **out_block)
{
    uvm_va_range_t *va_range;
    uvm_va_block_t *block;
    size_t index;

    va_range = uvm_va_range_find(va_space, addr);
    if (!va_range || va_range->type != UVM_VA_RANGE_TYPE_MANAGED)
        return NV_ERR_INVALID_ADDRESS;

    index = uvm_va_range_block_index(va_range, addr);
    block = uvm_va_range_block(va_range, index);
    if (!block)
        return NV_ERR_OBJECT_NOT_FOUND;

    *out_block = block;
    return NV_OK;
}

NV_STATUS uvm_va_block_find_create(uvm_va_space_t *va_space, NvU64 addr, uvm_va_block_t **out_block)
{
    uvm_va_range_t *va_range;
    NvU64 index;

    va_range = uvm_va_range_find(va_space, addr);
    if (!va_range || va_range->type != UVM_VA_RANGE_TYPE_MANAGED)
        return NV_ERR_INVALID_ADDRESS;

    index = uvm_va_range_block_index(va_range, addr);
    return uvm_va_range_block_create(va_range, index, out_block);
}

NV_STATUS uvm_va_block_write_from_cpu(uvm_va_block_t *va_block, NvU64 dst, const void *src, size_t size)
{
    NV_STATUS status;
    NvU32 page_index = uvm_va_block_cpu_page_index(va_block, dst);
    NvU64 page_offset = dst & (PAGE_SIZE - 1);
    uvm_processor_id_t proc = uvm_va_block_page_get_closest_resident(va_block, page_index, UVM_CPU_ID);
    uvm_va_block_region_t region = uvm_va_block_region(page_index, page_index + 1);
    uvm_va_block_context_t *block_context;

    uvm_gpu_t *gpu;
    uvm_gpu_address_t src_gpu_address;
    uvm_gpu_address_t dst_gpu_address;
    uvm_push_t push;

    uvm_assert_mutex_locked(&va_block->lock);
    UVM_ASSERT_MSG(UVM_ALIGN_DOWN(dst, PAGE_SIZE) == UVM_ALIGN_DOWN(dst + size - 1, PAGE_SIZE),
            "dst 0x%llx size 0x%zx\n", dst, size);

    if (proc == UVM8_MAX_PROCESSORS)
        proc = UVM_CPU_ID;

    block_context = uvm_va_block_context_alloc();
    if (!block_context)
        return NV_ERR_NO_MEMORY;

    // Use make_resident() in all cases to break read-duplication, but
    // block_retry can be NULL as if the page is not resident yet we will make
    // it resident on the CPU.
    // Notably we don't care about coherence with respect to atomics from other
    // processors.
    //
    // We use UvmEventMigrationCauseInvalid because make_resident is just populating
    // and no migration will be performed
    status = uvm_va_block_make_resident(va_block,
                                        NULL,
                                        block_context,
                                        proc,
                                        region,
                                        NULL,
                                        UvmEventMigrationCauseInvalid);

    uvm_va_block_context_free(block_context);

    if (status != NV_OK)
        return status;

    if (proc == UVM_CPU_ID) {
        char *mapped_page;
        struct page *page = va_block->cpu.pages[page_index];

        status = uvm_tracker_wait(&va_block->tracker);
        if (status != NV_OK)
            return status;

        mapped_page = (char *)kmap(page);
        memcpy(mapped_page + page_offset, src, size);
        kunmap(page);

        return NV_OK;
    }

    gpu = uvm_gpu_get(proc);
    dst_gpu_address = block_phys_page_copy_address(va_block, block_phys_page(proc, page_index), gpu);
    dst_gpu_address.address += page_offset;
    src_gpu_address = uvm_gpu_address_physical(UVM_APERTURE_SYS, virt_to_phys((void *)src));

    status = uvm_push_begin_acquire(gpu->channel_manager, UVM_CHANNEL_TYPE_CPU_TO_GPU, &va_block->tracker, &push,
            "Direct write to [0x%llx, 0x%llx)", dst, (NvU64)(dst + size));
    if (status != NV_OK)
        return status;

    gpu->ce_hal->memcopy(&push, dst_gpu_address, src_gpu_address, size);
    return uvm_push_end_and_wait(&push);
}

NV_STATUS uvm_va_block_read_to_cpu(uvm_va_block_t *va_block, void *dst, NvU64 src, size_t size)
{
    NV_STATUS status;
    NvU64 page_index = uvm_va_block_cpu_page_index(va_block, src);
    NvU64 page_offset = src & (PAGE_SIZE - 1);
    uvm_processor_id_t proc = uvm_va_block_page_get_closest_resident(va_block, page_index, UVM_CPU_ID);

    uvm_gpu_t *gpu;
    uvm_gpu_address_t src_gpu_address;
    uvm_gpu_address_t dst_gpu_address;
    uvm_push_t push;

    uvm_assert_mutex_locked(&va_block->lock);
    UVM_ASSERT_MSG(UVM_ALIGN_DOWN(src, PAGE_SIZE) == UVM_ALIGN_DOWN(src + size - 1, PAGE_SIZE),
            "src 0x%llx size 0x%zx\n", src, size);

    if (proc == UVM8_MAX_PROCESSORS) {
        memset(dst, 0, size);
        return NV_OK;
    }

    if (proc == UVM_CPU_ID) {
        char *mapped_page;
        struct page *page = va_block->cpu.pages[page_index];

        status = uvm_tracker_wait(&va_block->tracker);
        if (status != NV_OK)
            return status;

        mapped_page = (char *)kmap(page);
        memcpy(dst, mapped_page + page_offset, size);
        kunmap(page);

        return NV_OK;
    }

    gpu = uvm_gpu_get(proc);
    src_gpu_address = block_phys_page_copy_address(va_block, block_phys_page(proc, page_index), gpu);
    src_gpu_address.address += page_offset;
    dst_gpu_address = uvm_gpu_address_physical(UVM_APERTURE_SYS, virt_to_phys(dst));

    status = uvm_push_begin_acquire(gpu->channel_manager, UVM_CHANNEL_TYPE_GPU_TO_CPU, &va_block->tracker, &push,
            "Direct read from [0x%llx, 0x%llx)", src, (NvU64)(src + size));
    if (status != NV_OK)
        return status;

    gpu->ce_hal->memcopy(&push, dst_gpu_address, src_gpu_address, size);

    return uvm_push_end_and_wait(&push);
}

// Deferred work item reestablishing any accessed by mappings that might be
// missing, for example after eviction.
static void block_deferred_accessed_by(void *args)
{
    NV_STATUS status;
    uvm_va_block_t *va_block = (uvm_va_block_t*)args;
    uvm_va_range_t *va_range;
    uvm_va_space_t *va_space = NULL;
    uvm_gpu_id_t gpu_id;

    uvm_mutex_lock(&va_block->lock);

    va_range = va_block->va_range;
    if (va_range)
        va_space = va_range->va_space;

    uvm_mutex_unlock(&va_block->lock);

    if (!va_range) {
        // Block has been killed in the meantime
        goto done;
    }

    UVM_ASSERT(va_space);

    // The block wasn't dead when we checked above and that's enough to
    // guarantee that the VA space is still around, because
    // uvm_va_space_destroy() flushes the associated nv_kthread_q, and that
    // flush waits for this function call to finish.
    uvm_va_space_down_read(va_space);

    // Now that we have the VA space lock held, check whether the block is still
    // alive.
    uvm_mutex_lock(&va_block->lock);

    va_range = va_block->va_range;

    uvm_mutex_unlock(&va_block->lock);

    if (va_range) {
        // We don't have a reference on the mm so can only reestablish GPU mappings.
        for_each_gpu_id_in_mask(gpu_id, &va_range->accessed_by) {
            status = uvm_va_block_set_accessed_by(va_block, gpu_id);
            if (status != NV_OK) {
                UVM_ERR_PRINT("Deferred set accessed by for block [0x%llx, 0x%llx] failed %s, GPU %s\n",
                        va_block->start, va_block->end,
                        nvstatusToString(status),
                        uvm_gpu_get(gpu_id)->name);
                break;
            }
        }
    }

    uvm_va_space_up_read(va_space);
done:
    uvm_va_block_release(va_block);
}

NV_STATUS uvm_va_block_evict_chunks(uvm_va_block_t *va_block,
                                    uvm_gpu_t *gpu,
                                    uvm_gpu_chunk_t *root_chunk,
                                    uvm_tracker_t *tracker)
{
    NV_STATUS status = NV_OK;
    NvU32 i;
    uvm_va_block_gpu_state_t *gpu_state;
    uvm_va_block_region_t chunk_region;
    size_t num_gpu_chunks = block_num_gpu_chunks(va_block, gpu);
    size_t chunks_to_evict = 0;
    uvm_va_block_context_t *block_context;
    unsigned long *pages_to_evict;

    uvm_assert_mutex_locked(&va_block->lock);

    // The block might have been killed in the meantime
    if (!va_block->va_range)
        return NV_OK;

    gpu_state = va_block->gpus[gpu->id - 1];
    if (!gpu_state)
        return NV_OK;

    if (va_block->inject_eviction_error) {
        va_block->inject_eviction_error = false;
        return NV_ERR_NO_MEMORY;
    }

    block_context = uvm_va_block_context_alloc();
    if (!block_context)
        return NV_ERR_NO_MEMORY;

    pages_to_evict = block_context->caller_page_mask;
    uvm_page_mask_zero(pages_to_evict);
    chunk_region.outer = 0;

    // Find all chunks that are subchunks of the root chunk
    for (i = 0; i < num_gpu_chunks; ++i) {
        uvm_chunk_size_t chunk_size;
        size_t chunk_index = block_gpu_chunk_index(va_block, gpu, chunk_region.outer, &chunk_size);
        UVM_ASSERT(chunk_index == i);
        chunk_region.first = chunk_region.outer;
        chunk_region.outer = chunk_region.first + chunk_size / PAGE_SIZE;

        if (!gpu_state->chunks[i])
            continue;
        if (!uvm_gpu_chunk_same_root(gpu_state->chunks[i], root_chunk))
            continue;

        uvm_page_mask_region_fill(pages_to_evict, chunk_region);
        ++chunks_to_evict;
    }

    if (chunks_to_evict == 0)
        goto out;

    // Only move pages resident on the GPU
    uvm_page_mask_and(pages_to_evict, pages_to_evict, uvm_va_block_resident_mask_get(va_block, gpu->id));

    // TODO: Bug 1765193: make_resident() breaks read-duplication, but it's not
    // necessary to do so for eviction. Add a version that unmaps only the
    // processors that have mappings to the pages being evicted.
    status = uvm_va_block_make_resident(va_block,
                                        NULL,
                                        block_context,
                                        UVM_CPU_ID,
                                        uvm_va_block_region_from_block(va_block),
                                        pages_to_evict,
                                        UvmEventMigrationCauseEviction);
    if (status != NV_OK)
        goto out;

    // VA space lock may not be held and hence we cannot reestablish any
    // mappings here and need to defer it to a work queue.
    // CPU mappings cannot be established from a work queue without holding a
    // reference on the mm (which we don't and can't) so only check for GPUs.
    //
    // Reading the accessed_by mask without the VA space lock is safe because
    // adding a new processor to the mask triggers going over all the VA blocks
    // in the range and locking them. And we hold one of the VA block's locks.
    //
    // If uvm_va_range_set_accessed_by() hasn't called uvm_va_block_set_accessed_by()
    // for this block yet then it will take care of adding the mapping after we
    // are done. If it already did then we are guaranteed to see the new
    // GPU in the accessed_by mask as we locked the block's lock that the thread
    // calling uvm_va_range_set_accessed_by() unlocked after updating the mask.
    //
    // If a GPU gets removed from the mask then we might not notice and schedule
    // the work item anyway, but that's benign as deferred_accessed_by()
    // reexamines the mask.
    if (uvm_processor_mask_get_gpu_count(&va_block->va_range->accessed_by) > 0) {
        // Always retain the VA block first so that it's safe for the deferred
        // callback to release it immediately after it runs.
        uvm_va_block_retain(va_block);

        if (!nv_kthread_q_schedule_q_item(&g_uvm_global.global_q,
                                          &va_block->accessed_by_mappings_q_item)) {
            // And release it if no new callback was scheduled
            uvm_va_block_release(va_block);
        }
    }

    status = uvm_tracker_add_tracker_safe(tracker, &va_block->tracker);
    if (status != NV_OK)
        goto out;

    for (i = 0; i < num_gpu_chunks; ++i) {
        if (!gpu_state->chunks[i])
            continue;
        if (!uvm_gpu_chunk_same_root(gpu_state->chunks[i], root_chunk))
            continue;

        uvm_pmm_gpu_mark_chunk_evicted(&gpu->pmm, gpu_state->chunks[i]);
        gpu_state->chunks[i] = NULL;
    }

out:
    uvm_va_block_context_free(block_context);
    return status;
}

static NV_STATUS block_gpu_force_4k_ptes(uvm_va_block_t *block, uvm_va_block_context_t *block_context, uvm_gpu_t *gpu)
{
    uvm_va_block_gpu_state_t *gpu_state = block_gpu_state_get_alloc(block, gpu);
    uvm_push_t push;
    NV_STATUS status;

    // We don't currently have a use case to force PTEs to 4k on GPUs with
    // swizzling (Keplers). Don't bother implementing that until we have a need.
    UVM_ASSERT(!gpu->big_page.swizzling);

    // Force all pages to be 4K and prevent future upgrades during cancel
    gpu_state->force_4k_ptes = true;

    // If we have no page tables we're done. For fault cancel we need to make
    // sure that fatal faults are on different 4k PTEs than non-fatal faults,
    // and we need to service all non-fatal faults before issuing the cancel. So
    // either all faults are fatal and we have no PTEs (we're PROT_NONE), or
    // we'll allocate PTEs later when we service the non-fatal faults. Those
    // PTEs will be 4k since force_4k_ptes is set.
    if (!block_gpu_has_page_tables(block, gpu))
        return NV_OK;

    // Are we 4k already?
    if (!gpu_state->pte_is_2m && bitmap_empty(gpu_state->big_ptes, MAX_BIG_PAGES_PER_UVM_VA_BLOCK))
        return NV_OK;

    status = block_alloc_ptes_with_retry(block, gpu, UVM_PAGE_SIZE_4K, NULL);
    if (status != NV_OK)
        return status;

    status = uvm_push_begin_acquire(gpu->channel_manager,
                                    UVM_CHANNEL_TYPE_MEMOPS,
                                    &block->tracker,
                                    &push,
                                    "Forcing 4k PTEs on block [0x%llx, 0x%llx)",
                                    block->start,
                                    block->end + 1);
    if (status != NV_OK)
        return status;

    if (gpu_state->pte_is_2m)
        block_gpu_split_2m(block, block_context, gpu, NULL, &push);
    else
        block_gpu_split_big(block, block_context, gpu, gpu_state->big_ptes, &push);

    uvm_push_end(&push);

    UVM_ASSERT(block_check_mappings(block));

    return uvm_tracker_add_push_safe(&block->tracker, &push);
}

NV_STATUS uvm_va_block_set_cancel(uvm_va_block_t *va_block, uvm_gpu_t *gpu)
{
    uvm_va_block_context_t *block_context;
    NV_STATUS status;

    uvm_assert_mutex_locked(&va_block->lock);

    block_context = uvm_va_block_context_alloc();
    if (!block_context)
        return NV_ERR_NO_MEMORY;

    status = block_gpu_force_4k_ptes(va_block, block_context, gpu);

    uvm_va_block_context_free(block_context);
    return status;
}

NV_STATUS uvm8_test_va_block_inject_error(UVM_TEST_VA_BLOCK_INJECT_ERROR_PARAMS *params, struct file *filp)
{
    uvm_va_space_t *va_space = uvm_va_space_get(filp);
    uvm_va_block_t *va_block;
    NV_STATUS status = NV_OK;

    uvm_va_space_down_read(va_space);

    status = uvm_va_block_find_create(va_space, params->lookup_address, &va_block);
    if (status != NV_OK)
        goto out;

    uvm_mutex_lock(&va_block->lock);

    if (params->page_table_allocation_retry_force_count)
        va_block->page_table_allocation_retry_force_count = params->page_table_allocation_retry_force_count;

    if (params->user_pages_allocation_retry_force_count)
        va_block->user_pages_allocation_retry_force_count = params->user_pages_allocation_retry_force_count;

    va_block->inject_eviction_error = params->eviction_error;

    uvm_mutex_unlock(&va_block->lock);

out:
    uvm_va_space_up_read(va_space);
    return status;
}

NV_STATUS uvm8_test_change_pte_mapping(UVM_TEST_CHANGE_PTE_MAPPING_PARAMS *params, struct file *filp)
{
    uvm_va_space_t *va_space = uvm_va_space_get(filp);
    uvm_va_block_t *block;
    NV_STATUS status = NV_OK;
    uvm_prot_t curr_prot, new_prot;
    uvm_gpu_t *gpu = NULL;
    uvm_processor_id_t id;
    uvm_tracker_t local_tracker;
    uvm_va_block_region_t region;
    uvm_va_block_context_t *block_context = NULL;

    if (!PAGE_ALIGNED(params->va))
        return NV_ERR_INVALID_ADDRESS;

    switch (params->mapping) {
        case UVM_TEST_PTE_MAPPING_INVALID:
            new_prot = UVM_PROT_NONE;
            break;
        case UVM_TEST_PTE_MAPPING_READ_ONLY:
            new_prot = UVM_PROT_READ_ONLY;
            break;
        case UVM_TEST_PTE_MAPPING_READ_WRITE:
            new_prot = UVM_PROT_READ_WRITE;
            break;
        case UVM_TEST_PTE_MAPPING_READ_WRITE_ATOMIC:
            new_prot = UVM_PROT_READ_WRITE_ATOMIC;
            break;
        default:
            return NV_ERR_INVALID_ARGUMENT;
    }

    // mmap_sem isn't needed for invalidating CPU mappings, but it will be
    // needed for inserting them.
    uvm_down_read_mmap_sem(&current->mm->mmap_sem);
    uvm_va_space_down_read(va_space);

    if (uvm_uuid_is_cpu(&params->uuid)) {
        id = UVM_CPU_ID;
    }
    else {
        gpu = uvm_va_space_get_gpu_by_uuid_with_gpu_va_space(va_space, &params->uuid);
        if (!gpu) {
            status = NV_ERR_INVALID_DEVICE;
            goto out;
        }

        // Check if the GPU can access the VA
        if (!uvm_gpu_can_address(gpu, params->va)) {
            status = NV_ERR_OUT_OF_RANGE;
            goto out;
        }

        id = gpu->id;
    }

    status = uvm_va_block_find_create(va_space, params->va, &block);
    if (status != NV_OK)
        goto out;

    // Creating new CPU mappings requires that we hold a reference on
    // vma->vm_mm, but we can only do that if current->mm == vma->vm_mm.
    if (id == UVM_CPU_ID && !uvm_va_range_vma_current(block->va_range)) {
        status = NV_ERR_INVALID_STATE;
        goto out;
    }

    uvm_mutex_lock(&block->lock);

    region = uvm_va_block_region_from_start_size(block, params->va, PAGE_SIZE);
    curr_prot = block_page_prot(block, id, region.first);

    if (new_prot == curr_prot) {
        status = NV_OK;
        goto out_block;
    }

    // TODO: Bug 1766124: Upgrades might require revoking other processors'
    //       access privileges. We just fail for now. Only downgrades are
    //       supported.
    if (new_prot > curr_prot) {
        status = NV_ERR_INVALID_OPERATION;
        goto out_block;
    }

    block_context = uvm_va_block_context_alloc();
    if (!block_context) {
        status = NV_ERR_NO_MEMORY;
        goto out_block;
    }

    if (new_prot == UVM_PROT_NONE) {
        status = uvm_va_block_unmap(block, block_context, id, region, NULL, &block->tracker);
    }
    else {
        UVM_ASSERT(block_is_page_resident_anywhere(block, region.first));

        if (id == UVM_CPU_ID) {
            status = block_revoke_cpu_write_page(block, region.first);
        }
        else {
            status = uvm_va_block_revoke_prot(block,
                                              block_context,
                                              id,
                                              region,
                                              NULL,
                                              new_prot + 1,
                                              &block->tracker);
        }
    }

out_block:
    if (status == NV_OK)
        status = uvm_tracker_init_from(&local_tracker, &block->tracker);

    uvm_mutex_unlock(&block->lock);

    if (status == NV_OK)
        status = uvm_tracker_wait_deinit(&local_tracker);

out:
    uvm_va_space_up_read(va_space);
    uvm_up_read_mmap_sem(&current->mm->mmap_sem);

    uvm_va_block_context_free(block_context);

    return status;
}

NV_STATUS uvm8_test_va_block_info(UVM_TEST_VA_BLOCK_INFO_PARAMS *params, struct file *filp)
{
    uvm_va_space_t *va_space = uvm_va_space_get(filp);
    uvm_va_block_t *va_block;
    NV_STATUS status = NV_OK;

    BUILD_BUG_ON(UVM_TEST_VA_BLOCK_SIZE != UVM_VA_BLOCK_SIZE);

    uvm_down_read_mmap_sem(&current->mm->mmap_sem);
    uvm_va_space_down_read(va_space);

    status = uvm_va_block_find(va_space, params->lookup_address, &va_block);
    if (status != NV_OK)
        goto out;

    if (!uvm_va_range_vma_current(va_block->va_range)) {
        // We aren't in the same mm as the one which owns the vma
        status = NV_ERR_INVALID_STATE;
        goto out;
    }

    params->va_block_start = va_block->start;
    params->va_block_end   = va_block->end;

out:
    uvm_va_space_up_read(va_space);
    uvm_up_read_mmap_sem(&current->mm->mmap_sem);
    return status;
}

NV_STATUS uvm8_test_va_residency_info(UVM_TEST_VA_RESIDENCY_INFO_PARAMS *params, struct file *filp)
{
    NV_STATUS status = NV_OK;
    uvm_va_space_t *va_space = uvm_va_space_get(filp);
    uvm_va_block_t *block = NULL;
    NvU32 count = 0;
    uvm_processor_mask_t resident_on_mask;
    uvm_processor_id_t id;
    size_t page_index;

    uvm_down_read_mmap_sem(&current->mm->mmap_sem);
    uvm_va_space_down_read(va_space);

    status = uvm_va_block_find(va_space, params->lookup_address, &block);
    if (status != NV_OK)
        goto out;

    if (!uvm_va_range_vma_current(block->va_range)) {
        // We aren't in the same mm as the one which owns the vma
        status = NV_ERR_INVALID_STATE;
        goto out;
    }

    uvm_mutex_lock(&block->lock);

    page_index = uvm_va_block_cpu_page_index(block, params->lookup_address);
    uvm_va_block_page_resident_processors(block, page_index, &resident_on_mask);

    for_each_id_in_mask(id, &resident_on_mask) {
        block_phys_page_t block_page = block_phys_page(id, page_index);
        uvm_processor_uuid_from_id(&params->resident_on[count], id);
        params->resident_physical_size[count] = block_phys_page_size(block, block_page);
        if (id == UVM_CPU_ID)
            params->resident_physical_address[count] = page_to_phys(block->cpu.pages[id]);
        else
            params->resident_physical_address[count] = block_phys_page_address(block, block_page, uvm_gpu_get(id)).address;
        ++count;
    }
    params->resident_on_count = count;

    count = 0;
    for_each_id_in_mask(id, &block->mapped) {
        NvU32 page_size = uvm_va_block_page_size_processor(block, id, page_index);
        if (page_size == 0)
            continue;

        uvm_processor_uuid_from_id(&params->mapped_on[count], id);
        params->page_size[count] = page_size;
        ++count;
    }
    params->mapped_on_count = count;

    count = 0;
    for (id = 0; id < UVM8_MAX_PROCESSORS; ++id) {
        if (!block_processor_page_is_populated(block, id, page_index))
            continue;

        uvm_processor_uuid_from_id(&params->populated_on[count], id);
        ++count;
    }
    params->populated_on_count = count;

out:
    if (block)
        uvm_mutex_unlock(&block->lock);
    uvm_va_space_up_read(va_space);
    uvm_up_read_mmap_sem(&current->mm->mmap_sem);
    return status;
}

static void block_mark_region_cpu_dirty(uvm_va_block_t *va_block, uvm_va_block_region_t region)
{
    size_t page_index;
    uvm_assert_mutex_locked(&va_block->lock);

    for_each_va_block_page_in_mask(page_index, va_block->cpu.resident, region)
        SetPageDirty(va_block->cpu.pages[page_index]);
}

void uvm_va_block_mark_cpu_dirty(uvm_va_block_t *va_block)
{
    block_mark_region_cpu_dirty(va_block, uvm_va_block_region_from_block(va_block));
}

