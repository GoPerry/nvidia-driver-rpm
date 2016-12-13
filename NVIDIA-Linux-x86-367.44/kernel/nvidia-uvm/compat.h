#ifndef __COMPAT_H__
#define __COMPAT_H__

 /***************************************************************************\
|*                                                                           *|
|*      Copyright (c) NVIDIA Corporation.  All rights reserved.              *|
|*                                                                           *|
|*   THE SOFTWARE AND INFORMATION CONTAINED HEREIN IS PROPRIETARY AND        *|
|*   CONFIDENTIAL TO NVIDIA CORPORATION. THIS SOFTWARE IS FOR INTERNAL USE   *|
|*   ONLY AND ANY REPRODUCTION OR DISCLOSURE TO ANY PARTY OUTSIDE OF NVIDIA  *|
|*   IS STRICTLY PROHIBITED.                                                 *|
|*                                                                           *|
 \***************************************************************************/

#include "nvtypes.h"
#include "nvmisc.h"
#include "nvstatus.h"

#if defined(NVMEM_MACOSX_BUILD)
#include "nvMmuWalkCompat.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if defined(NVMEM_WINDOWS_BUILD) || defined(NVMEM_MODS_BUILD)

/*!
 * NVMEM (drivers/tegra/platform/drivers/memory)
 */

// debug translations
#define SHR_ASSERT                         NV_ASSERT
#define shrBool                            NvBool
#define SHR_PRINTF                         PRINTF
#define SHR_LEVEL_ERRORS                   HI
#define SHR_LEVEL_INFO                     LO
#define SHR_MEMORY_TAG                     NVMEM_MEMORY_TAG
#define g_shrDebugFlags                    g_nvMemDebugFlags
#define SHR_DRIVER_NAME                    "NVMEM "

#if defined(DBG)
#define SHR_DEBUG
#endif

// function translations and/or renaming
#define shrFreeMemory                      nvmOsFreeMem
#define shrAllocMem                        nvmOsAllocMem
#define shrAllocMemEx                      nvmOsAllocMem
#define shrFreeMem                         nvmOsFreeMem
#define shrAllocPages                      nvmOsAllocPages
#define shrFreePages                       nvmOsFreePages
#define shrMemSet                          nvmOsMemSet
#define shrMemCopy                         nvmOsMemCopy
#define shrMapSystemMemory                 nvmOsMapSystemMemory
#define shrUnmapSystemMemory               nvmOsUnmapSystemMemory
#define shrGetCurrentProcess               nvmOsGetCurrentProcess
#define shrUnmapIOUserSpace                nvmOsUnmapIOUserSpace
#define shrMapIOUserSpace                  nvmOsMapIOUserSpace
#define shrGetCacheMode                    nvmOsGetCacheMode
#define shrAllocContiguousSystemPages      nvmOsAllocContiguousSystemPages
#define shrMemGetPhysAddr(a, b, c)         nvmMemGetPhysAddr(a, c)
#define shrExecuteIoctlIrp                 nvmOsExecuteIoctlIrp
#define shrExecuteAcpiMethod               nvmOsExecuteAcpiMethod
#define shrMemGetCpuCacheAttrib            nvmMemGetCpuCacheAttrib
#define shrMemSetCpuCacheAttrib            nvmMemSetCpuCacheAttrib
#define shrMemGetFlag                      nvmMemGetFlag
#define shrMemSetFlag                      nvmMemSetFlag
#define shrMemSetAddress                   nvmMemSetAddress
#define shrMemGetMemData                   nvmMemGetMemData
#define shrMemSetMemData                   nvmMemSetMemData
#define shrMemGetAddressSpace(a, b)        nvmMemGetAddressSpace(a)
#define shrMemGetContiguity(a, b)          nvmMemGetContiguity(a)
#define shrMemSetContiguity(a, b, c)       nvmMemSetContiguity(a, c)
#define shrMemGetPte(a, b, c)              ((a)->PteArray[(c)])
#define shrMemSetPte(a, b, c, d)           do {(a)->PteArray[(c)] = (d); } while (0)
#define shrMemGetPteArray(a, b)            ((a)->PteArray)
#define shrConstructObjEHeap               nvmConstructObjEHeap 

#elif defined(NVMEM_MACOSX_BUILD)

/*!
 * Mac KMD (drivers/OpenGL/macosx/GLKernel)
 */

// status translations
#define STATUS_TYPE                        NV_STATUS
#define STATUS_OK                          NV_OK
#define STATUS_ERROR                       NV_ERR_GENERIC
#define STATUS_ERROR_INVALID_ARGUMENT      NV_ERR_INVALID_ARGUMENT
#define STATUS_ERROR_INSERT_DUPLICATE_NAME NV_ERR_IN_USE
#define STATUS_ERROR_OBJECT_NOT_FOUND      NV_ERR_OBJECT_NOT_FOUND
#define STATUS_ERROR_NO_FREE_MEMORY        NV_ERR_INSUFFICIENT_RESOURCES
#define STATUS_ERROR_INVALID_OFFSET        NV_ERR_INVALID_OFFSET
#define STATUS_ERROR_OPERATING_SYSTEM      NV_ERR_OPERATING_SYSTEM
#define STATUS_ERROR_INVALID_STATE         NV_ERRR_INVALID_STATE

// debug translations
#define SHR_ASSERT                         assert
#define shrBool                            bool
#define SHR_PRINTF(X)                      SHR_PRINTF_IMPL X
#define SHR_LEVEL_ERRORS                   3
#define SHR_LEVEL_INFO                     20

#define SHR_PRINTF_IMPL(LEVEL, FMT, ...) do {                                   \
    NV_TRACE(TR_GLK_MMU_WALK, LEVEL,                                            \
        (FMT, ## __VA_ARGS__));                                                 \
} while (0)

// function translations and/or renaming
#define shrAllocMem                        nvShrMemAlloc
#define shrFreeMem                         nvShrMemFree
#define shrFreeMemory                      nvShrMemFree
#define shrMemSet                          memset
#define shrMemCopy                         memcpy

// assert translations
#define ASSERT_AND_GOTO(COND, LABEL)                                            \
    if (!(COND))                                                                \
    {                                                                           \
        assert(COND);                                                           \
        goto LABEL;                                                             \
    }

#define ASSERT_OR_RETURN(COND, RET) do {                                        \
    if (!(COND))                                                                \
    {                                                                           \
        assert(COND);                                                           \
        return (RET);                                                           \
    }                                                                           \
} while(0)

#define ASSERT_OR_RETURN_VOID(COND) do {                                        \
    if (!(COND))                                                                \
    {                                                                           \
        assert(COND);                                                           \
        return;                                                                 \
    }                                                                           \
} while(0)

#elif defined(NVPEP_WINDOWS_BUILD) || defined(NVPEP_MODS_BUILD)

/*!
 * PEP2 (drivers/tegra/platform/drivers/pep)
 */

// debug translations
#define SHR_ASSERT                         NV_ASSERT
#define shrBool                            NvBool
#define SHR_PRINTF                         PRINTF
#define SHR_LEVEL_ERRORS                   HI
#define SHR_LEVEL_INFO                     LO
#define g_shrDebugFlags                    g_nvPepDebugFlags
#define SHR_DRIVER_NAME                    "NVPEP "

#if defined(DBG)
#define SHR_DEBUG
#endif

// function translations and/or renaming
#define shrFreeMemory                      osFreeMem

#elif defined(SRT_BUILD)

/*!
 * SRT - Standalone RM Tests (drivers/resman/tests)
 */
#include <stdlib.h>
#include <string.h>
#include "nvos.h"

// debug translations
#define SHR_ASSERT                         NV_ASSERT
#define shrBool                            NvBool
#define SHR_PRINTF                         PRINTF
#define SHR_LEVEL_ERRORS                   HI
#define SHR_LEVEL_INFO                     LO
#define SHR_MEMORY_TAG                     'SRT'
#define SHR_DRIVER_NAME                    "SRT "

#ifndef NV_SIZEOF32
#define NV_SIZEOF32(x) ((NvU32)(sizeof(x)))
#endif

#if defined(DEBUG)
#define SHR_DEBUG
#endif

// TODO: Generalize.
typedef struct
{
    NvBool bAllocFail;       //<! Enable artificial shrAllocMem failure.
    NvU32  allocFailCounter; //<! Countdown to artificial shrAllocMem failure.
} SHR_TEST_SETTINGS;

extern SHR_TEST_SETTINGS g_shrTestSettings;

NV_STATUS srtMemAlloc(void **ppMem, NvU32 size);
void srtMemFree(void *pMem);

// function translations and/or renaming
#define shrFreeMemory                      srtMemFree
#define shrAllocMem                        srtMemAlloc
#define shrFreeMem                         srtMemFree
#define shrMemSet                          memset
#define shrMemCopy                         memcpy

#elif defined(NV_MODS) && !defined(NVRM) && !defined(NV_RMAPI_TEGRA)

/*!
 * MODS (diag/mods)
 */
#include <assert.h>
#include <stdio.h>

// debug translations
#define SHR_ASSERT                         assert
#define SHR_PRINTF                         printf
#define SHR_LEVEL_ERRORS                   stdout
#define SHR_LEVEL_INFO                     stdout

#if defined(DEBUG)
#define SHR_DEBUG
#endif

// TODO: Move this with compat.h/shrdebug.h cleanup.
#ifndef ASSERT_OR_GOTO
#define ASSERT_OR_GOTO(c, l)           \
    do                                  \
    {                                   \
        if (!(c))                       \
        {                               \
            SHR_ASSERT(0);              \
            goto l;                     \
        }                               \
    } while (0)
#endif

#ifndef ASSERT_OR_RETURN
#define ASSERT_OR_RETURN(c, r)          \
    do                                  \
    {                                   \
        if (!(c))                       \
        {                               \
            SHR_ASSERT(0);              \
            return (r);                 \
        }                               \
    } while (0)
#endif

#elif defined(NVWATCH)

#include <assert.h>

#if defined(NV_MAC_KEXT)
#ifndef NULL
#define NULL ((void*)0)
#endif
#else
#include <stdlib.h>
#endif

#define SHR_ASSERT assert

#ifndef ASSERT_AND_RETURN
#define ASSERT_AND_RETURN(c, r)         \
    do                                  \
    {                                   \
        if (!(c))                       \
        {                               \
            return (r);                 \
        }                               \
    } while (0)
#endif

#ifndef ASSERT_OR_RETURN
#define ASSERT_OR_RETURN ASSERT_AND_RETURN
#endif

#elif defined(NV_LDDM_KMODE)

// TODO (KMD) - implement dependencies as needed.

#elif defined(NV_RMAPI_TEGRA)

/*!
 * Tegra rmapi (drivers/unix/rmapi_tegra)
 */
#include <stdlib.h>
#include "nvos.h"

// debug translations
#define SHR_ASSERT                         NV_ASSERT
#define SHR_PRINTF                         PRINTF
#define SHR_LEVEL_ERRORS                   HI
#define SHR_LEVEL_INFO                     LO
#define SHR_DRIVER_NAME                    "NVRM_TEGRA"

#if defined(DEBUG)
#define SHR_DEBUG
#endif

// function translations
#define shrFreeMemory                      free

#elif defined(NV_UVM_ENABLE)
/*!
 * NV_UVM_ENABLE is a linux only define
 * UVM-Linux-Kernel (drivers/mm/kernel/linux)
 */
#include "uvm_common.h"
#define SHR_ERR_WHEN_NOT(cond)                                      \
    do {                                                            \
        if (unlikely(!(cond)))                                      \
            UVM_PRINT_FUNC(pr_err, "Failed: %s\n", #cond);          \
    } while(0)

#define SHR_ASSERT              SHR_ERR_WHEN_NOT
#define SHR_PRINTF              UVM_ERR_PRINT
#define SHR_LEVEL_ERRORS
#define SHR_LEVEL_INFO

#if defined(DEBUG)
#define SHR_DEBUG
#endif

// TODO: Move this with compat.h/shrdebug.h cleanup.
#ifndef ASSERT_OR_GOTO
#define ASSERT_OR_GOTO(c, l)           \
    do                                  \
    {                                   \
        if (!(c))                       \
        {                               \
            SHR_ASSERT(0);              \
            goto l;                     \
        }                               \
    } while (0)
#endif

#ifndef ASSERT_OR_RETURN
#define ASSERT_OR_RETURN(c, r)          \
    do                                  \
    {                                   \
        if (!(c))                       \
        {                               \
            SHR_ASSERT(0);              \
            return (r);                 \
        }                               \
    } while (0)
#endif // NV_UVM_ENABLE
#else

/*!
 * Resource Manager (drivers/resman)
 */

// debug translations
#define SHR_ASSERT                         RM_ASSERT
#define SHR_PRINTF                         DBG_PRINTF
#define SHR_LEVEL_ERRORS                   DBG_MODULE_GLOBAL, DBG_LEVEL_ERRORS
#define SHR_LEVEL_INFO                     DBG_MODULE_GLOBAL, DBG_LEVEL_INFO
#define SHR_MEMORY_TAG                     NV_MEMORY_TAG

#if defined(DEBUG) || defined(QA_BUILD)
#define SHR_DEBUG
#endif  //defined(Debug)||defined(QA_BUILD)

// function translations and/or renaming
#define shrBool                            BOOL
#define shrFreeMemory                      osFreeMem
#if !defined(MACOS) || !defined(KERNEL)
#define shrAllocMem                        osAllocMemInternal
#define shrAllocMemEx                      osAllocMemExInternal
#define shrFreeMem                         osFreeMemInternal
#define shrAllocPages                      osAllocPagesInternal
#define shrFreePages                       osFreePagesInternal
#else
#define shrAllocMem                        osAllocMem
#define shrAllocMemEx                      osAllocMemEx
#define shrFreeMem                         osFreeMem
#define shrAllocPages                      osAllocPages
#define shrFreePages                       osFreePages
#endif
#define shrMemSet                          osMemSet
#define shrMemCopy                         osMemCopy
#define shrMapSystemMemory                 osMapSystemMemory
#define shrUnmapSystemMemory               osUnmapSystemMemory
#define shrGetCurrentProcess               osGetCurrentProcess
#define shrUnmapIOUserSpace                osUnmapIOUserSpace
#define shrMapIOUserSpace                  osMapIOUserSpace
#define shrGetCacheMode                    osGetCacheMode
#define shrAllocContiguousSystemPages      osAllocContiguousSystemPages
#define shrMemGetPhysAddr(a, b, c)         memGetPhysAddr(a, b, c)
#define shrExecuteIoctlIrp                 osExecuteIoctlIrp
#define shrExecuteAcpiMethod               osExecuteAcpiMethod
#define shrMemGetCpuCacheAttrib            memGetCpuCacheAttrib
#define shrMemSetCpuCacheAttrib            memSetCpuCacheAttrib
#define shrMemGetFlag                      memGetFlag
#define shrMemSetFlag                      memSetFlag
#define shrMemSetAddress                   memSetAddress
#define shrMemGetMemData                   memGetMemData
#define shrMemSetMemData                   memSetMemData
#define shrMemGetAddressSpace(a, b)        memGetAddressSpace(a, b)
#define shrMemSetContiguity(a, b, c)       memSetContiguity(a, b, c)
#define shrMemGetContiguity(a, b)          memGetContiguity(a, b)
#define shrMemSetContiguity(a, b, c)       memSetContiguity(a, b, c)
#define shrMemGetPte(a, b, c)              memGetPte(a, b, c)
#define shrMemSetPte(a, b, c, d)           memSetPte(a, b, c, d)
#define shrMemGetPteArray(a, b)            memGetPteArray(a, b)
#define shrConstructObjEHeap               constructObjEHeap

// TODO: Move this with compat.h/shrdebug.h cleanup.
#ifndef ASSERT_AND_GOTO
#define ASSERT_AND_GOTO(c, l)           \
    do                                  \
    {                                   \
        if (!(c))                       \
        {                               \
            SHR_ASSERT(0);              \
            goto l;                     \
        }                               \
    } while (0)
#endif

#endif  //defined(NEMEM_WINDOWS_BUILD) || defined(NVMEM_MODS_BUILD)

#ifndef ASSERT_OK_OR_RETURN
#define ASSERT_OK_OR_RETURN(CALL)                                               \
    do                                                                          \
    {                                                                           \
        NV_STATUS status = (CALL);                                              \
        if (NV_OK != status)                                                    \
        {                                                                       \
            SHR_PRINTF((SHR_LEVEL_ERRORS,                                       \
                "%s: Error 0x%08x returned from " #CALL ".\n",                  \
                __FUNCTION__, status));                                         \
            SHR_ASSERT(0);                                                      \
            return status;                                                      \
        }                                                                       \
    } while(0)
#endif

// Renaming AND to OR for clarity (assert true is the passing case).
#ifndef ASSERT_OR_GOTO
#define ASSERT_OR_GOTO ASSERT_AND_GOTO
#endif

#ifndef ASSERT_OR_RETURN
#define ASSERT_OR_RETURN ASSERT_AND_RETURN
#endif

#ifndef ASSERT_OR_RETURN_VOID
#define ASSERT_OR_RETURN_VOID ASSERT_AND_RETURN_VOID
#endif

// Debug-only assert for expensive (or critical path) checks.
#ifdef SHR_DEBUG
#define SHR_DBG_ASSERT(x) SHR_ASSERT(x)
#else
#define SHR_DBG_ASSERT(x)
#endif

static NV_FORCEINLINE NvU64
nvHighBitIdx64(NvU64 val)
{
    NvU64 count = 0;
    while (val >>= 1)
    {
        count++;
    }
    return count;
}

#ifdef __cplusplus
}
#endif

#endif //__COMPAT_H__
