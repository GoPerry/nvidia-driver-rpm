/* _NVRM_COPYRIGHT_BEGIN_
 *
 * Copyright 2014-2015 by NVIDIA Corporation.  All rights reserved.  All
 * information contained herein is proprietary and confidential to NVIDIA
 * Corporation.  Any use, reproduction, or disclosure without the written
 * permission of NVIDIA Corporation is prohibited.
 *
 * _NVRM_COPYRIGHT_END_
 */
#ifndef _NV_MMU_FMT_H_
#define _NV_MMU_FMT_H_

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @file mmu_fmt.h
 *
 * @brief Defines an abstraction over general MMU HW formats.
 *
 * The main goal is to leverage common page table management
 * code across a variety MMU HW formats.
 */
#include "nvtypes.h"
#include "nvmisc.h"
#include "../compat.h"

#if defined(SRT_BUILD)
#include "shrdebug.h"
#elif defined(NVRM) && !defined(NVWATCH) && !defined(NV_UVM_ENABLE)
#include "rmbase.h"
#include "osCore.h"
// These are only needed for the RM_ASSERT macros and osGetIP; eventually
// we probably just want nvassert.h and nvport.h.
#endif

// Forward declarations.
typedef struct MMU_FMT_LEVEL  MMU_FMT_LEVEL;

/*!
 * Generic MMU page directory/table level format description.
 *
 * Since the terminology of page directories and tables varies,
 * the following describes the interpretation assumed here.
 *
 * Each level of virtual address translation is described by a range of
 * virtual address bits.
 * These bits index into a contiguous range of physical memory referred to
 * generally as a "page level."
 * Page level memory is interpretted as an array of entries, with each entry
 * describing the next step of virtual to physical translation.
 *
 * Each entry in a given level may be interpretted as either a PDE or PTE.
 * 1. A PDE (page directory entry) points to one or more "sub-levels" that
 *    continue the VA translation recursively.
 * 2. A PTE (page table entry) is the base case, pointing to a physical page.
 *
 * The decision to treat an entry as a PDE or PTE may be static for a level.
 * Levels that only contain PDEs are referred to as page directories.
 * Levels that only contain PTEs are referred to as page tables.
 *
 * However, some formats have levels that may contain a mix of PDEs and PTEs,
 * with the intpretation based on a "cutoff" bit in each entry (e.g. PTE valid bit).
 * Such levels are referred to as "polymorphic page levels" since they can be
 * viewed as both a page directory and a page table.
 */
struct MMU_FMT_LEVEL
{
    /*!
     * First virtual address bit that this page level covers.
     */
    NvU8 virtAddrBitLo;

    /*!
     * Last virtual address bit that this page level covers.
     */
    NvU8 virtAddrBitHi;

    /*!
     * Size in bytes of each entry within a level instance.
     */
    NvU8 entrySize;

    /*!
     * Indicates if this level can contain PTEs.
     */
    NvBool bPageTable;

    /*!
     * Number of sub-levels pointed to by PDEs in this level in
     * range [0, MMU_FMT_MAX_SUB_LEVELS].
     * 0 indicates this level cannot contain PDEs.
     */
    NvU8 numSubLevels;

    /*!
     * Array of sub-level formats of length numSubLevels.
     */
    const MMU_FMT_LEVEL *subLevels;
};

/*!
 * Maximum number of pointers to sub-levels within a page directory entry.
 *
 * Standard page directory entries (PDEs) point to a single sub-level,
 * either the next page directory level in the topology or a leaf page table.
 *
 * However, some formats contain PDEs that point to more than one sub-level.
 * These sub-levels are translated by HW in parallel to support multiple
 * page sizes at a higher granularity (e.g. for migration between
 * 4K system memory pages and big video memory pages for GPU MMU).
 *
 * The current supported formats have a maximum of 2 parallel sub-levels,
 * often referred to as "dual PDE" or "dual page table" support.
 *
 * Example for Fermi GPU HW:
 *      Sub-level 0 corresponds to big page table pointer.
 *      Sub-level 1 corresponds to small page table pointer.
 *
 * This number is very unlikely to change, but it is defined to
 * simplify SW handling, encouraging loops over "dual copy-paste."
 */
#define MMU_FMT_MAX_SUB_LEVELS 2

/*!
 * Get bitmask of page sizes supported under a given MMU level.
 *
 * Example: For the root level this returns all the page sizes
 *          supported by the MMU format.
 *
 * @returns Bitmask of page sizes (sufficient since page sizes are power of 2).
 */
NvU64 mmuFmtAllPageSizes(const MMU_FMT_LEVEL *pLevel);

/*!
 * Get bitmask of the VA coverages for each level, starting at a given level.
 * This is a superset of mmuFmtAllPageSizes, but includes page directory coverage bits.
 *
 * Example: For the root level this provides a summary of the VA breakdown.
 *          Each bit corresponds to the shift of a level in the format and
 *          the number bits set is equal to the total number of levels
 *          (including parallel sub-levels).
 *
 * @returns Bitmask of level VA coverages.
 */
NvU64 mmuFmtAllLevelCoverages(const MMU_FMT_LEVEL *pLevel);

/*!
 * Find a level with the given page shift.
 *
 * @param[in]  pLevel    Level format to start search.
 * @param[in]  pageShift log2(pageSize).
 *
 * @returns The level if found or NULL otherwise.
 */
const MMU_FMT_LEVEL *mmuFmtFindLevelWithPageShift(
                        const MMU_FMT_LEVEL *pLevel,
                        const NvU64          pageShift);

/*!
 * Find the parent level of a given level.
 *
 * @param[in]  pRoot     Root level format.
 * @param[in]  pLevel    Child level format.
 * @param[out] pSubLevel Returns the sub-level of the child within the parent if found.
 *                       Can be NULL if not needed.
 *
 * @returns Parent level if found or NULL otherwise.
 */
const MMU_FMT_LEVEL *mmuFmtFindLevelParent(
                        const MMU_FMT_LEVEL *pRoot,
                        const MMU_FMT_LEVEL *pLevel,
                        NvU32               *pSubLevel);

/*!
 * Get the next sub-level format in a search for a particular level.
 *
 * @returns Next level if found or NULL otherwise.
 */
const MMU_FMT_LEVEL *mmuFmtGetNextLevel(
                        const MMU_FMT_LEVEL *pLevelFmt,
                        const MMU_FMT_LEVEL *pTargetFmt);

/*!
 * Bitmask of VA covered by a given level.
 * e.g. for the root level this is the maximum VAS limit.
 */
static NV_FORCEINLINE NvU64
mmuFmtLevelVirtAddrMask(const MMU_FMT_LEVEL *pLevel)
{
    return NVBIT64(pLevel->virtAddrBitHi + 1) - 1;
}

/*!
 * Bitmask of VA covered by a single entry within a level.
 * e.g. (page size - 1) for PTEs within this level.
 */
static NV_FORCEINLINE NvU64
mmuFmtEntryVirtAddrMask(const MMU_FMT_LEVEL *pLevel)
{
    return NVBIT64(pLevel->virtAddrBitLo) - 1;
}

/*!
 * Bitmask of VA that contains the entry index of a level.
 */
static NV_FORCEINLINE NvU64
mmuFmtEntryIndexVirtAddrMask(const MMU_FMT_LEVEL *pLevel)
{
    return mmuFmtLevelVirtAddrMask(pLevel) & ~mmuFmtEntryVirtAddrMask(pLevel);
}

/*!
 * Extract the entry index of a level from a virtual address.
 */
static NV_FORCEINLINE NvU32
mmuFmtVirtAddrToEntryIndex(const MMU_FMT_LEVEL *pLevel, const NvU64 virtAddr)
{
    return (NvU32)((virtAddr & mmuFmtEntryIndexVirtAddrMask(pLevel)) >> pLevel->virtAddrBitLo);
}

/*!
 * Truncate a virtual address to the base of a level.
 */
static NV_FORCEINLINE NvU64
mmuFmtLevelVirtAddrLo(const MMU_FMT_LEVEL *pLevel, const NvU64 virtAddr)
{
    return virtAddr & ~mmuFmtLevelVirtAddrMask(pLevel);
}

/*!
 * Round a virtual address up to the limit covered by a level.
 */
static NV_FORCEINLINE NvU64
mmuFmtLevelVirtAddrHi(const MMU_FMT_LEVEL *pLevel, const NvU64 virtAddr)
{
    return mmuFmtLevelVirtAddrLo(pLevel, virtAddr) + mmuFmtLevelVirtAddrMask(pLevel);
}

/*!
 * Get the virtual address base of an entry index from the base virtual
 * address of its level.
 */
static NV_FORCEINLINE NvU64
mmuFmtEntryIndexVirtAddrLo(const MMU_FMT_LEVEL *pLevel, const NvU64 vaLevelBase,
                           const NvU32 entryIndex)
{
    SHR_DBG_ASSERT(0 == (vaLevelBase & mmuFmtLevelVirtAddrMask(pLevel)));
    return vaLevelBase + ((NvU64)entryIndex << pLevel->virtAddrBitLo);
}

/*!
 * Get the virtual address limit of an entry index from the base virtual
 * address of its level.
 */
static NV_FORCEINLINE NvU64
mmuFmtEntryIndexVirtAddrHi(const MMU_FMT_LEVEL *pLevel, const NvU64 vaLevelBase,
                           const NvU32 entryIndex)
{
    return mmuFmtEntryIndexVirtAddrLo(pLevel, vaLevelBase, entryIndex) +
                mmuFmtEntryVirtAddrMask(pLevel);
}

/*!
 * Get the page size for PTEs within a given MMU level.
 */
static NV_FORCEINLINE NvU64
mmuFmtLevelPageSize(const MMU_FMT_LEVEL *pLevel)
{
    return mmuFmtEntryVirtAddrMask(pLevel) + 1;
}

/*!
 * Extract the page offset of a virtual address based on a given MMU level.
 */
static NV_FORCEINLINE NvU64
mmuFmtVirtAddrPageOffset(const MMU_FMT_LEVEL *pLevel, const NvU64 virtAddr)
{
    return virtAddr & mmuFmtEntryVirtAddrMask(pLevel);
}

/*!
 * Calculate the maximum number of entries contained by a given MMU level.
 */
static NV_FORCEINLINE NvU32
mmuFmtLevelEntryCount(const MMU_FMT_LEVEL *pLevel)
{
    return NVBIT32(pLevel->virtAddrBitHi - pLevel->virtAddrBitLo + 1);
}

/*!
 * Calculate the maximum size in bytes of a given MMU level.
 */
static NV_FORCEINLINE NvU32
mmuFmtLevelSize(const MMU_FMT_LEVEL *pLevel)
{
    return mmuFmtLevelEntryCount(pLevel) * pLevel->entrySize;
}

#ifdef __cplusplus
}
#endif

#endif
