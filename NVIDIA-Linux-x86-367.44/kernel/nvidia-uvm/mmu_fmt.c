/* _NVRM_COPYRIGHT_BEGIN_
 *
 * Copyright 2014-2014 by NVIDIA Corporation.  All rights reserved.  All
 * information contained herein is proprietary and confidential to NVIDIA
 * Corporation.  Any use, reproduction, or disclosure without the written
 * permission of NVIDIA Corporation is prohibited.
 *
 * _NVRM_COPYRIGHT_END_
 */
#include "mmu/mmu_fmt.h"

NvU64
mmuFmtAllPageSizes(const MMU_FMT_LEVEL *pLevel)
{
    NvU32 i;
    NvU64 mask = 0;
    if (pLevel->bPageTable)
    {
        mask |= mmuFmtLevelPageSize(pLevel);
    }
    for (i = 0; i < pLevel->numSubLevels; ++i)
    {
        mask |= mmuFmtAllPageSizes(pLevel->subLevels + i);
    }
    return mask;
}

NvU64
mmuFmtAllLevelCoverages(const MMU_FMT_LEVEL *pLevel)
{
    NvU32 i;
    NvU64 mask = mmuFmtLevelPageSize(pLevel);
    for (i = 0; i < pLevel->numSubLevels; ++i)
    {
        mask |= mmuFmtAllLevelCoverages(pLevel->subLevels + i);
    }
    return mask;
}

const MMU_FMT_LEVEL *
mmuFmtFindLevelWithPageShift
(
    const MMU_FMT_LEVEL *pLevel,
    const NvU64          pageShift
)
{
    NvU32 i;
    if (pLevel->virtAddrBitLo == pageShift)
    {
        return pLevel;
    }
    for (i = 0; i < pLevel->numSubLevels; ++i)
    {
        const MMU_FMT_LEVEL *pRes =
            mmuFmtFindLevelWithPageShift(pLevel->subLevels + i, pageShift);
        if (NULL != pRes)
        {
            return pRes;
        }
    }
    return NULL;
}

const MMU_FMT_LEVEL *
mmuFmtFindLevelParent
(
    const MMU_FMT_LEVEL *pRoot,
    const MMU_FMT_LEVEL *pLevel,
    NvU32               *pSubLevel
)
{
    NvU32 i;
    for (i = 0; i < pRoot->numSubLevels; ++i)
    {
        const MMU_FMT_LEVEL *pRes;
        if ((pRoot->subLevels + i) == pLevel)
        {
            if (NULL != pSubLevel)
            {
                *pSubLevel = i;
            }
            pRes = pRoot;
        }
        else
        {
            pRes = mmuFmtFindLevelParent(pRoot->subLevels + i, pLevel, pSubLevel);
        }
        if (NULL != pRes)
        {
            return pRes;
        }
    }
    return NULL;
}

const MMU_FMT_LEVEL *
mmuFmtGetNextLevel
(
    const MMU_FMT_LEVEL *pLevelFmt,
    const MMU_FMT_LEVEL *pTargetFmt
)
{
    if (pLevelFmt != pTargetFmt)
    {
        NvU32 subLevel = 0;
        if (1 == pLevelFmt->numSubLevels)
        {
            return pLevelFmt->subLevels;
        }
        for (subLevel = 0; subLevel < pLevelFmt->numSubLevels; ++subLevel)
        {
            const MMU_FMT_LEVEL *pSubLevelFmt = pLevelFmt->subLevels + subLevel;
            if (pSubLevelFmt == pTargetFmt)
            {
                return pSubLevelFmt;
            }
        }
    }
    return NULL;
}
