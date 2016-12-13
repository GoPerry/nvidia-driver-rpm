/*******************************************************************************
    Copyright (c) 2014 NVIDIA Corporation

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

#include "nvstatus.h"

#include "uvmtypes.h"
#include "uvm_page_migration.h"
#include "uvm_common.h"
#include "uvm_linux.h"
#include "uvm_lite.h"
#include "uvm_lite_prefetch.h"

//
// uvm_lite_prefetch.c
// This file contains code that is specific to the UVM-Lite mode page
// prefetching.
//

static struct kmem_cache *g_uvmLitePrefetchRegionAccessCache __read_mostly = NULL;

// Preferred region length
static const NvLength g_uvmLitePrefetchRegionOrder    = 8; // 2^8 pages -> 256 pages
// Smallest allocation size to enable prefetching
static const NvLength g_uvmLitePrefetchMinCommitRecordOrder = 2; // 2^2 pages -> 4 pages

typedef short UvmAccessTreeCounter;

typedef struct UvmAccessTreeLeaf_tag
{
    NvU8 fault:1;
    NvU8 prefetch:1;
    NvU8 accessed:1;            // prefetch must be 1
}UvmAccessTreeLeaf;

typedef struct UvmAccessTreeNode_tag
{
    union
    {
        UvmAccessTreeCounter count;
        UvmAccessTreeLeaf leaf;
    };
}UvmAccessTreeNode;

typedef struct UvmRegionAccess_tag
{
    UvmPrefetchRegionCounters counters;

    UvmAccessTreeCounter pages;
    UvmAccessTreeNode *nodes;
}UvmRegionAccess;

typedef struct UvmRegionPrefetchHint_tag
{
    UvmAccessTreeCounter level;
    UvmAccessTreeCounter entryId;
}UvmRegionPrefetchHint;

#define UVM_PREFETCH_MIN_THRESHOLD              50
#define UVM_PREFETCH_MAX_THRESHOLD             100
#define UVM_PREFETCH_DEFAULT_INITIAL_THRESHOLD  75

#define UVM_PREFETCH_ADAPTIVE_DEFAULT_EPOCH 100

#define UVM_PREFETCH_ADAPTIVE_DEFAULT_INC_THRESHOLD 30
#define UVM_PREFETCH_ADAPTIVE_DEFAULT_DEC_THRESHOLD 10

static unsigned
g_uvmLitePrefetchInitialThreshold = UVM_PREFETCH_DEFAULT_INITIAL_THRESHOLD;
static unsigned
g_uvmLitePrefetchAdaptiveEpoch = UVM_PREFETCH_ADAPTIVE_DEFAULT_EPOCH; // # of faults
static unsigned
g_uvmLitePrefetchAdaptiveSparsityIncThreshold =
    UVM_PREFETCH_ADAPTIVE_DEFAULT_INC_THRESHOLD;
static unsigned
g_uvmLitePrefetchAdaptiveSparsityDecThreshold =
    UVM_PREFETCH_ADAPTIVE_DEFAULT_DEC_THRESHOLD;

static const unsigned g_uvmLitePrefetchAdaptiveThresholdStep = 5;

// Module parameters to tune page prefetching in UVM-Lite. Use signed instead of
// unsigned int variables for modules to avoid compilation errors in old
// versions of the kernel.

// Provide statistics for the prefetcher when counters are reset. This is mainly
// the number of prefetched pages and the accuracy of the prefetcher (number of
// prefetched pages that are accessed later). Accesses to prefetched pages must
// be notified through uvmlite_prefetch_log_minor_fault.
int uvm_prefetch_stats = 0;
module_param(uvm_prefetch_stats, int, S_IRUGO);

// Threshold used to control how aggressive predictions are. The threshold sets
// the percentage of children pages in any node of the prefetching tree that
// need to be accessed to prefetch the remaining pages in that node
static int uvm_prefetch_threshold =
    UVM_PREFETCH_DEFAULT_INITIAL_THRESHOLD;
module_param(uvm_prefetch_threshold, int, S_IRUGO);

// Enable automatic changes to the threshold to adapt to access patterns.
static int uvm_prefetch_adaptive = 0;
module_param(uvm_prefetch_adaptive, int, S_IRUGO);

// How often to update the threshold (in number of major page faults)
static int uvm_prefetch_epoch =
    UVM_PREFETCH_ADAPTIVE_DEFAULT_EPOCH;
module_param(uvm_prefetch_epoch, int, S_IRUGO);

// Lower bound of the sparsity level to trigger a threshold increment. The
// prefetcher keeps track of subregions within each region to detect if it is
// being accessed sparsely. When the sparsity ratio is greater than this value,
// the threshold is incremented.
static int uvm_prefetch_sparsity_inc =
    UVM_PREFETCH_ADAPTIVE_DEFAULT_INC_THRESHOLD;
module_param(uvm_prefetch_sparsity_inc, int, S_IRUGO);

// Upper bound of the sparsity level to trigger a threshold decrement. The
// prefetcher keeps track of subregions within each region to detect if it is
// being accessed sparsely. When the sparsity ratio is lower than this value,
// the threshold is decremented.
static int uvm_prefetch_sparsity_dec =
    UVM_PREFETCH_ADAPTIVE_DEFAULT_DEC_THRESHOLD;
module_param(uvm_prefetch_sparsity_dec, int, S_IRUGO);

// Computes the number of nodes needed to track the given number of pages
static inline NvLength uvm_prefetch_tree_elems(NvLength pages)
{
    return 2 * pages - 1;
}

static UvmRegionPrefetchHint
uvmlite_region_log_major_fault(UvmRegionAccess * region, UvmAccessTreeCounter entryId,
                               unsigned threshold)
{
    UvmRegionPrefetchHint hint;

    UvmAccessTreeCounter level;
    UvmAccessTreeCounter off;
    UvmAccessTreeCounter elemsLevel;
    NvLength children;
    NvLength count;

    UVM_PANIC_ON(region->nodes[entryId].leaf.fault ||
                 region->nodes[entryId].leaf.prefetch ||
                 region->nodes[entryId].leaf.accessed);

    hint.level = -1;

    region->nodes[entryId].leaf.fault = 1;

    level = 1;
    children = 2;
    off = region->pages;
    elemsLevel = region->pages / 2;
    entryId /= 2;
    while (elemsLevel > 0)
    {
        // Update the node
        count = (NvLength)++ region->nodes[off + entryId].count;

        if (count != children && count * 100 > children * threshold)
        {
            hint.level = level;
            hint.entryId = entryId;
        }

        ++level;
        children *= 2;
        off += elemsLevel;
        elemsLevel /= 2;
        entryId /= 2;
    }

    return hint;
}

static void
uvmlite_region_log_minor_fault(UvmRegionAccess * region, UvmAccessTreeCounter entryId)
{
    // Many threads can set this bit (but NOT concurrently) if they fault on the
    // same page. This is safe
    region->nodes[entryId].leaf.accessed = 1;
}

static inline void
uvmlite_region_ack_prefetch(UvmRegionAccess * region, UvmAccessTreeCounter entryId)
{
    UvmAccessTreeCounter off;
    UvmAccessTreeCounter elemsLevel;

    UVM_PANIC_ON(region->nodes[entryId].leaf.prefetch);

    region->nodes[entryId].leaf.prefetch = 1;

    off = region->pages;
    elemsLevel = region->pages / 2;
    entryId /= 2;
    while (elemsLevel > 0)
    {
        // Update the node
        ++region->nodes[off + entryId].count;

        off += elemsLevel;
        elemsLevel /= 2;
        entryId /= 2;
    }
}

static void uvmlite_destroy_access_region(UvmRegionAccess * region)
{
    if (region)
    {
        if (region->nodes)
        {
            vfree(region->nodes);
        }
        kmem_cache_free(g_uvmLitePrefetchRegionAccessCache, region);
    }
}

static UvmRegionAccess *uvmlite_create_access_region(UvmCommitRecord *pRecord,
                                                     NvLength regionLength)
{
    UvmRegionAccess *region;
    UvmAccessTreeCounter pages;
    NvLength bytes;

    pages = (UvmAccessTreeCounter) (regionLength >> PAGE_SHIFT);
    // Ensure that can pages be represented with a UvmAccessTreeCounter value
    UVM_PANIC_ON((NvLength)pages != (regionLength >> PAGE_SHIFT));

    bytes = uvm_prefetch_tree_elems(pages) * sizeof(UvmAccessTreeNode);

    region = (UvmRegionAccess*) kmem_cache_zalloc(g_uvmLitePrefetchRegionAccessCache,
                                                  NV_UVM_GFP_FLAGS);
    if (region == NULL)
        goto fail;

    region->pages = pages;

    region->nodes = vmalloc(bytes);
    if (!region->nodes)
    {
        UVM_ERR_PRINT("vmalloc(%llu) failed.\n", (unsigned long long)bytes);
        goto fail;
    }
    // Zero counters of all nodes
    memset(region->nodes, 0, bytes);

    UVM_DBG_PRINT_RL("Created access region %p with %u pages\n",
                     region, (unsigned)pages);

    return region;

fail:
    uvmlite_destroy_access_region(region);

    return NULL;
}

NV_STATUS
uvmlite_init_prefetch_info(UvmPrefetchInfo *pPrefetchInfo, UvmCommitRecord *pRecord)
{
    NV_STATUS rmStatus;
    UvmRegionAccess *region;
    NvLength i;
    NvLength regionLength = 1 << (PAGE_SHIFT + g_uvmLitePrefetchRegionOrder);
    NvLength bytes;

    if (!uvm_prefetch)
        return NV_OK;

    UVM_PANIC_ON(pPrefetchInfo == NULL);

    if (pRecord->length <
            (NvLength)(1 << (PAGE_SHIFT + g_uvmLitePrefetchMinCommitRecordOrder)))
    {
        // Do not create prefetch information for commit records smaller than
        // 2 ^ g_uvmLitePrefetchMinCommitRecordOrder pages
        pPrefetchInfo->regions = 0;
        return NV_OK;
    }

    pPrefetchInfo->threshold = g_uvmLitePrefetchInitialThreshold;

    pPrefetchInfo->regions = ((pRecord->length + regionLength - 1) / regionLength);

    bytes = pPrefetchInfo->regions * sizeof(UvmRegionAccess *);
    // Create the array to store pointers to regions
    pPrefetchInfo->regionPtrs = vmalloc(bytes);
    if (!pPrefetchInfo->regionPtrs)
    {
        UVM_ERR_PRINT("vmalloc(%llu) failed.\n", (unsigned long long) bytes);
        rmStatus = NV_ERR_NO_MEMORY;
        goto fail;
    }
    // Zero pointer table
    memset(pPrefetchInfo->regionPtrs, 0, bytes);
    // Create the region and initialize the region pointer array
    for (i = 0; i < pPrefetchInfo->regions; ++i)
    {
        if (i == pPrefetchInfo->regions - 1)
        {
            regionLength = pRecord->length - i * regionLength;
            // Handle non-power of two region sizes (for the last region)
            regionLength = roundup_pow_of_two(regionLength);
        }

        region = uvmlite_create_access_region(pRecord, regionLength);
        if (!region)
        {
            rmStatus = NV_ERR_NO_MEMORY;
            goto fail;
        }

        pPrefetchInfo->regionPtrs[i] = region;
    }

    UVM_DBG_PRINT_RL("Created prefetch pPrefetchInfo %p with %llu regions\n",
                     pPrefetchInfo, pPrefetchInfo->regions);

    return NV_OK;

fail:
    uvmlite_destroy_prefetch_info(pPrefetchInfo);

    return rmStatus;
}

#define UVM_REGION_PAGES() (1 << g_uvmLitePrefetchRegionOrder)
#define UVM_PAGE_REGION_ID(p) ((p) / UVM_REGION_PAGES())
#define UVM_PAGE_LOCAL_ID(p) (UvmAccessTreeCounter)((p) - (UVM_REGION_PAGES() * UVM_PAGE_REGION_ID(p)))

void uvmlite_reset_prefetch_info(UvmPrefetchInfo * pPrefetchInfo, UvmCommitRecord *pRecord)
{
    UvmRegionAccess *region;
    NvLength elems;
    NvLength i;
    UvmAccessTreeCounter j;
    NvLength nprefetch = 0;
    NvLength hits = 0;

    NvLength localPrefetches;
    NvLength localHits;

    if (!uvm_prefetch)
        return;

    UVM_PANIC_ON(pPrefetchInfo == NULL);

    // Print stats if enabled
    if (uvm_prefetch_stats)
        UVM_DBG_PRINT("== PREFETCH STATS for %p\n",
                      (void *)pRecord->baseAddress);

    if (uvm_prefetch_adaptive)
    {
        pPrefetchInfo->faultRegions = 0;
        pPrefetchInfo->counters.faults = 0;
        pPrefetchInfo->counters.nprefetch = 0;
    }
    // Reset the information in all the regions
    for (i = 0; i < pPrefetchInfo->regions; ++i)
    {
        region = pPrefetchInfo->regionPtrs[i];
        // Reset counters to compute the adaptive threshold
        if (uvm_prefetch_adaptive)
        {
            region->counters.faults = 0;
            region->counters.nprefetch = 0;
        }
        // Compute and print per-region stats
        if (uvm_prefetch_stats)
        {
            localPrefetches = 0;
            localHits = 0;
            for (j = 0; j < region->pages; ++j)
            {
                if (region->nodes[j].leaf.prefetch)
                    ++localPrefetches;
                if (region->nodes[j].leaf.accessed)
                    ++localHits;
            }
            UVM_DBG_PRINT("- Region %llu: %llu nprefetch, %llu hits\n",
                          (unsigned long long)i,
                          (unsigned long long)localPrefetches,
                          (unsigned long long)localHits);
            nprefetch += localPrefetches;
            hits += localHits;
        }
        elems = uvm_prefetch_tree_elems(region->pages);
        // Reset all counters
        memset(region->nodes, 0, elems * sizeof(UvmAccessTreeNode));
    }
    // Print global stats if enabled
    if (uvm_prefetch_stats)
        UVM_DBG_PRINT("- Global: %llu nprefetch, %llu hits\n",
                      (unsigned long long)nprefetch, (unsigned long long)hits);
}

void uvmlite_destroy_prefetch_info(UvmPrefetchInfo * pPrefetchInfo)
{
    if (!uvm_prefetch)
        return;

    UVM_PANIC_ON(!pPrefetchInfo);
    if (pPrefetchInfo->regions > 0)
    {
        NvLength i;
        UvmRegionAccess *region;

        for (i = 0; i < pPrefetchInfo->regions; ++i)
        {
            region = pPrefetchInfo->regionPtrs[i];
            if (region != NULL)
                uvmlite_destroy_access_region(region);
        }

        vfree(pPrefetchInfo->regionPtrs);
    }
    UVM_DBG_PRINT_RL("Destroyed prefetch info %p\n", pPrefetchInfo);
}

#define UVM_SPARSITY_RATIO(p) (((p)->faultRegions * 100) / (p)->counters.faults)


NvBool
uvmlite_prefetch_log_major_fault(UvmPrefetchInfo * pPrefetchInfo,
                                 UvmCommitRecord * pRecord,
                                 unsigned long pageIndex,
                                 UvmPrefetchHint * hint)
{
    NvBool retValue;
    UvmRegionPrefetchHint prefetchHint;
    UvmRegionAccess *region;
    NvLength children;
    NvLength regionId;

    if (!uvm_prefetch)
        return NV_FALSE;

    retValue = NV_FALSE;

    if (pPrefetchInfo->regions == 0)
        return retValue;

    regionId = UVM_PAGE_REGION_ID(pageIndex);
    UVM_PANIC_ON(regionId >= pPrefetchInfo->regions);

    UVM_DBG_PRINT_RL
        ("Logging major fault in page index %lu, region %llu, local_id %llu\n",
         pageIndex, (unsigned long long)regionId,
         (unsigned long long) UVM_PAGE_LOCAL_ID(pageIndex));
    region = pPrefetchInfo->regionPtrs[regionId];

    if (uvm_prefetch_adaptive)
    {
        // Update counters
        if (region->counters.faults == 0)
        {
            ++pPrefetchInfo->faultRegions;
        }
        ++pPrefetchInfo->counters.faults;
        ++region->counters.faults;

        // Check if we have to tune the threshold
        if (pPrefetchInfo->counters.faults % g_uvmLitePrefetchAdaptiveEpoch == 0)
        {
            unsigned sparsityRatio = UVM_SPARSITY_RATIO(pPrefetchInfo);
            if (sparsityRatio > g_uvmLitePrefetchAdaptiveSparsityIncThreshold)
            {

                pPrefetchInfo->threshold =
                    min(pPrefetchInfo->threshold +
                            g_uvmLitePrefetchAdaptiveThresholdStep,
                        (unsigned)UVM_PREFETCH_MAX_THRESHOLD);
            }
            else if (sparsityRatio <
                     g_uvmLitePrefetchAdaptiveSparsityDecThreshold)
            {
                if (pPrefetchInfo->threshold >
                    UVM_PREFETCH_MIN_THRESHOLD + g_uvmLitePrefetchAdaptiveThresholdStep)
                {
                    pPrefetchInfo->threshold -= g_uvmLitePrefetchAdaptiveThresholdStep;
                }
                else
                {
                    pPrefetchInfo->threshold = UVM_PREFETCH_MIN_THRESHOLD;
                }
            }
        }
    }

    prefetchHint =
        uvmlite_region_log_major_fault(region, UVM_PAGE_LOCAL_ID(pageIndex),
                                       pPrefetchInfo->threshold);
    if (prefetchHint.level != -1)
    {
        NvLength localEntry;
        NvLength maxPages;
        // Notify we are making a prediction
        retValue = NV_TRUE;
        children = ((NvLength) 1) << prefetchHint.level;
        localEntry =
            ((NvLength) UVM_PAGE_LOCAL_ID(pageIndex)) & ~(children - 1);
        // Compute index
        hint->baseEntry = localEntry + regionId * UVM_REGION_PAGES();
        // Compute number of pages to be transferred
        maxPages = pRecord->length >> PAGE_SHIFT;
        if (hint->baseEntry + children > maxPages)
            hint->count = maxPages - hint->baseEntry;
        else
            hint->count = children;
    }

    return retValue;
}

void
uvmlite_prefetch_log_minor_fault(UvmPrefetchInfo * pPrefetchInfo,
                                 unsigned long pageIndex)
{
    UvmRegionAccess *region;
    NvLength regionId;

    if (!uvm_prefetch)
        return;

    if (pPrefetchInfo->regions == 0)
        return;

    regionId = UVM_PAGE_REGION_ID(pageIndex);
    UVM_PANIC_ON(regionId >= pPrefetchInfo->regions);

    UVM_DBG_PRINT_RL
        ("Logging minor fault in page index %lu, region %llu, local_id %llu\n",
         pageIndex, (unsigned long long )regionId,
         (unsigned long long)UVM_PAGE_LOCAL_ID(pageIndex));
    region = pPrefetchInfo->regionPtrs[regionId];
    uvmlite_region_log_minor_fault(region, UVM_PAGE_LOCAL_ID(pageIndex));
}

void
uvmlite_prefetch_page_ack(UvmPrefetchInfo * pPrefetchInfo, unsigned long pageIndex)
{
    UvmRegionAccess *region;
    NvLength regionId;

    if (!uvm_prefetch)
        return;

    if (pPrefetchInfo->regions == 0)
        return;

    regionId = UVM_PAGE_REGION_ID(pageIndex);
    UVM_PANIC_ON(regionId >= pPrefetchInfo->regions);

    region = pPrefetchInfo->regionPtrs[regionId];

    uvmlite_region_ack_prefetch(region, UVM_PAGE_LOCAL_ID(pageIndex));

    if (uvm_prefetch_adaptive)
    {
        ++pPrefetchInfo->counters.nprefetch;
        ++region->counters.nprefetch;
    }
}

NV_STATUS uvmlite_prefetch_init(void)
{
    NV_STATUS rmStatus;

    if (!uvm_prefetch)
        return NV_OK;

    rmStatus = NV_ERR_NO_MEMORY;
    g_uvmLitePrefetchRegionAccessCache = NV_KMEM_CACHE_CREATE("UvmRegionAccess",
                                                              UvmRegionAccess);
    if (!g_uvmLitePrefetchRegionAccessCache)
        goto fail;

    // Clamp to valid value boundaries
    if (uvm_prefetch_threshold < UVM_PREFETCH_MIN_THRESHOLD)
        g_uvmLitePrefetchInitialThreshold = UVM_PREFETCH_MIN_THRESHOLD;
    else if (uvm_prefetch_threshold > UVM_PREFETCH_MAX_THRESHOLD)
        g_uvmLitePrefetchInitialThreshold = UVM_PREFETCH_MAX_THRESHOLD;
    else
        g_uvmLitePrefetchInitialThreshold = (unsigned)uvm_prefetch_threshold;

    if (uvm_prefetch_epoch > 0)
        g_uvmLitePrefetchAdaptiveEpoch = (unsigned)uvm_prefetch_epoch;

    if (uvm_prefetch_sparsity_dec <= 100)
        g_uvmLitePrefetchAdaptiveSparsityDecThreshold =
            (unsigned)uvm_prefetch_sparsity_dec;

    if (uvm_prefetch_sparsity_inc <= 100)
        g_uvmLitePrefetchAdaptiveSparsityIncThreshold =
            (unsigned)uvm_prefetch_sparsity_inc;

    // If user-provided values are not valid, use the default ones
    if (uvm_prefetch_sparsity_dec > uvm_prefetch_sparsity_inc)
    {
        g_uvmLitePrefetchAdaptiveSparsityDecThreshold =
            UVM_PREFETCH_ADAPTIVE_DEFAULT_DEC_THRESHOLD;
        g_uvmLitePrefetchAdaptiveSparsityIncThreshold =
            UVM_PREFETCH_ADAPTIVE_DEFAULT_INC_THRESHOLD;
    }

    UVM_DBG_PRINT("UVM Lite prefetching support enabled\n");
    UVM_DBG_PRINT("Initial prefetch threshold: %u\n",
                  g_uvmLitePrefetchInitialThreshold);
    if (uvm_prefetch_adaptive)
    {
        UVM_DBG_PRINT("Prefetch threshold step size: %u\n",
                      g_uvmLitePrefetchAdaptiveThresholdStep);
        UVM_DBG_PRINT("Prefetch epoch length: %u\n",
                      g_uvmLitePrefetchAdaptiveEpoch);
        UVM_DBG_PRINT("Sparsity ratio inc threshold: %u\n",
                      g_uvmLitePrefetchAdaptiveSparsityDecThreshold);
        UVM_DBG_PRINT("Sparsity ratio dec threshold: %u\n",
                      g_uvmLitePrefetchAdaptiveSparsityIncThreshold);
    }

    return NV_OK;
fail:
    kmem_cache_destroy_safe(&g_uvmLitePrefetchRegionAccessCache);

    return rmStatus;
}

void uvmlite_prefetch_exit(void)
{
    if (!uvm_prefetch)
        return;

    UVM_DBG_PRINT("Destroyed caches\n");

    kmem_cache_destroy(g_uvmLitePrefetchRegionAccessCache);
}
