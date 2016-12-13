/* _NVRM_COPYRIGHT_BEGIN_
 *
 * Copyright 2014-2015 by NVIDIA Corporation.  All rights reserved.  All
 * information contained herein is proprietary and confidential to NVIDIA
 * Corporation.  Any use, reproduction, or disclosure without the written
 * permission of NVIDIA Corporation is prohibited.
 *
 * _NVRM_COPYRIGHT_END_
 */
#ifndef _NV_FIELD_DESC_H_
#define _NV_FIELD_DESC_H_

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @file field_desc.h
 *
 * @brief Simple runtime DRF-macro framework.
 *
 * Allows HAL definitions at the register/field level to maximize common code.
 * Two primitive versions are provided - 32-bit and 64-bit.
 */
#include "nvtypes.h"
#include "nvmisc.h"
#include "compat.h"

#if defined(SRT_BUILD)
#include "shrdebug.h"
#elif (defined(NVRM) || (defined UVM_WIN)) && !defined(NVWATCH) && !defined(NV_LDDM_KMODE) && !defined(NV_UVM_ENABLE)
#include "nvrm.h"
#include "class.h"
#include "objos.h"
#endif

// Forward declarations.
typedef struct NV_FIELD_DESC32     NV_FIELD_DESC32;
typedef struct NV_FIELD_DESC64     NV_FIELD_DESC64;
typedef struct NV_FIELD_ENUM       NV_FIELD_ENUM;
typedef struct NV_FIELD_ENUM_ENTRY NV_FIELD_ENUM_ENTRY;
typedef struct NV_FIELD_BOOL       NV_FIELD_BOOL;

/*!
 * Descriptor for fields <= 32-bits in length.
 * A field is assumed to fit within a 4-byte aligned region.
 */
struct NV_FIELD_DESC32
{
    /*!
     * Positive bitmask of the field, e.g. 0x6ULL for a field 2:1.
     */
    NvU32 maskPos;

    /*!
     * Negative bitmask of the field, e.g. ~0x6ULL for a field 2:1.
     */
    NvU32 maskNeg;

    /*!
     * Bit shift, e.g. 4 for a field 31:4.
     */
    NvU16 shift;

    /*!
     * Offset into the memory in bytes.
     * This is needed for regions greater than 4 bytes.
     */
    NvU16 offset;
};

#define NV_FIELD_DESC64_MAX_DISCONTIG_REGIONS 2

/*!
 * Descriptor for fields <= 64-bits in length.
 * A field is assumed to fit within an 8-byte aligned region of memory.
 */
struct NV_FIELD_DESC64
{
    NvU32 regionCount;

    struct
    {
        /*!
         * Positive bitmask of the field, e.g. 0x6ULL for a field 2:1.
         */
        NvU64 maskPos;

        /*!
         *  Width of field, e.g. 0x2 for a field 2:1.
         */
        NvU32 width;

        /*!
         * Bit shift, e.g. 4 for a field 31:4.
         */
        NvU32 shift;

        /*!
         * Offset into the memory in bytes.
         * This is needed for regions greater than 8 bytes.
         */
        NvU32 offset;
    } regions[NV_FIELD_DESC64_MAX_DISCONTIG_REGIONS];
};

/*!
 * Enumeration field value.
 */
struct NV_FIELD_ENUM_ENTRY
{
    NvU8 bValid; //!< Indicates the value is valid (for checking).
    NvU8 value;  //!< Raw (encoded) value.
};

/*!
 * Enumeration field descriptor.
 */
struct NV_FIELD_ENUM
{
    NV_FIELD_DESC32            desc;
    NvU8                       numEntries;
    const NV_FIELD_ENUM_ENTRY *entries;
};

/*!
 * Boolean field descriptor.
 */
struct NV_FIELD_BOOL
{
    NV_FIELD_DESC32 desc;
    NvBool          bInvert;
};

// TODO: Move to nvmisc.h.
#ifndef DRF_OFFSET
#define DRF_OFFSET(drf)     (((0?drf) / 32) * 4)
#endif

#ifndef DRF_OFFSET64
#define DRF_OFFSET64(drf)   (((0?drf) / 64) * 8)
#endif

// Utility macros to define field formats using HW manuals.
#define INIT_FIELD_DESC32(pField, drf)                      \
    do {                                                    \
        (pField)->maskPos = DRF_SHIFTMASK(drf);             \
        (pField)->maskNeg = ~DRF_SHIFTMASK(drf);            \
        (pField)->shift   = DRF_SHIFT(drf);                 \
        (pField)->offset  = DRF_OFFSET(drf);                \
    } while (0)

#define INIT_FIELD_DESC64(pField, drf)                          \
    do {                                                        \
        (pField)->regionCount = 1;                              \
        (pField)->regions[0].maskPos = DRF_SHIFTMASK64(drf);    \
        (pField)->regions[0].width   = (1?drf) - (0?drf) + 1;   \
        (pField)->regions[0].shift   = DRF_SHIFT64(drf);        \
        (pField)->regions[0].offset  = DRF_OFFSET64(drf);       \
    } while (0)


/*
    @note:
        BEGIN_DISCONTIG_FIELD_DESC64(pField)
            DRF_DISCONTIG_FIELD_DESC64(pField, NV_MMU_VER2_PTE_COMPTAGLINE)
            DRF_DISCONTIG_FIELD_DESC64(pField, NV_MMU_VER2_PTE_ADDRESS_VID)
        END_FIELD_DESC64_DISCONTIGUOUS(pField)

*/
#define BEGIN_DISCONTIG_FIELD_DESC64(pField)                         \
    pField->regionCount = 0;

#define DRF_DISCONTIG_FIELD_DESC64(pField, drf)                                      \
    do {                                                                             \
        SHR_DBG_ASSERT(pField->regionCount < NV_FIELD_DESC64_MAX_DISCONTIG_REGIONS); \
        (pField)->regions[pField->regionCount].maskPos = DRF_SHIFTMASK64(drf);       \
        (pField)->regions[pField->regionCount].width   = (1?drf) - (0?drf) + 1;      \
        (pField)->regions[pField->regionCount].shift   = DRF_SHIFT64(drf);           \
        (pField)->regions[pField->regionCount].offset  = DRF_OFFSET64(drf);          \
        pField->regionCount ++;                                                      \
    } while(0);

#define END_FIELD_DESC64_DISCONTIGUOUS(pField)          


#define INIT_FIELD_ENUM(pEnum, drf, count, pEntries)        \
    do {                                                    \
        INIT_FIELD_DESC32(&(pEnum)->desc, drf);             \
        (pEnum)->numEntries = count;                        \
        (pEnum)->entries    = pEntries;                     \
    } while(0)

#define INIT_FIELD_BOOL(pBool, drf)                         \
    do {                                                    \
        INIT_FIELD_DESC32(&(pBool)->desc, drf);             \
        (pBool)->bInvert = (NvBool)!(drf##_TRUE);           \
    } while(0)

static NV_FORCEINLINE void
nvFieldEnumEntryInit(NV_FIELD_ENUM_ENTRY *pEntry, const NvU8 value)
{
    pEntry->bValid = NV_TRUE;
    pEntry->value  = value;
}

/*!
 * Test whether a 32-bit field descriptor is valid.
 */
static NV_FORCEINLINE NvBool
nvFieldIsValid32(const NV_FIELD_DESC32 *pField)
{
    if (0 != pField->maskPos)
    {
        SHR_DBG_ASSERT(pField->maskPos == ~pField->maskNeg);
        SHR_DBG_ASSERT(0 != (NVBIT64(pField->shift) & pField->maskPos));
        SHR_DBG_ASSERT(0 == (pField->offset & 0x3));
        return NV_TRUE;
    }
    return NV_FALSE;
}

/*!
 * Test whether a 64-bit field descriptor is valid.
 */
static NV_FORCEINLINE NvBool
nvFieldIsValid64(const NV_FIELD_DESC64 *pField)
{
    NvU32 i;
    NvU32 aggregate_mask = 0;
    for (i = 0; i < pField->regionCount; i++) {
        // Forbid empty registers
        if (pField->regions[i].maskPos == 0)
            return NV_FALSE;

        // Ensure that fields don't overlap
        SHR_DBG_ASSERT((pField->regions[i].maskPos & aggregate_mask) == 0);
        aggregate_mask |= pField->regions[i].maskPos;

        // Ensure that shift is bottom bit of maskPos
        SHR_DBG_ASSERT(0 != (NVBIT64(pField->regions[i].shift) & pField->regions[i].maskPos));

        // Ensure offset is quad-word aligned
        SHR_DBG_ASSERT(0 == (pField->regions[i].offset & 0x7));
    }
    return NV_TRUE;
}

/*!
 * Set a 32-bit field based on its descriptor.
 *
 * @param[in]     pField Field format.
 * @param[in]     value  Value to set within the entry.
 * @param[in,out] pMem   Existing memory to update of at least length (pField->offset + 4).
 */
static NV_FORCEINLINE void
nvFieldSet32
(
    const NV_FIELD_DESC32 *pField,
    const NvU32            value,
    NvU8                  *pMem
)
{
          NvU32 *pValue  = (NvU32*)(pMem + pField->offset);
    const NvU32  shifted = value << pField->shift;

    SHR_DBG_ASSERT(nvFieldIsValid32(pField));
    SHR_DBG_ASSERT((shifted >> pField->shift) == value);
    SHR_DBG_ASSERT((shifted & pField->maskPos) == shifted);

    *pValue = (*pValue & pField->maskNeg) | shifted;
}

/*!
 * Set a 64-bit field based on its descriptor.
 *
 * @param[in]     pField Field format.
 * @param[in]     value  Value to set within the entry.
 * @param[in,out] pMem   Existing memory to update of at least length (pField->offset + 8).
 */
static NV_FORCEINLINE void
nvFieldSet64
(
    const NV_FIELD_DESC64 *pField,
    NvU64                  value,
    NvU8                  *pMem
)
{
    NvU32 i;
    SHR_DBG_ASSERT(nvFieldIsValid64(pField));

    for (i = 0; i < pField->regionCount; i++)
    {
        // Compute location and mask
              NvU64 *pValue  = (NvU64*)(pMem + pField->regions[i].offset);
        const NvU64  shifted = value << pField->regions[i].shift;

        // Store the portion of the value that fits in this field
        *pValue = (*pValue & ~pField->regions[i].maskPos) |
            (shifted & pField->regions[i].maskPos);

        // Shift off the bits we just stored
        value >>= pField->regions[i].width;
    }

    // Ensure value doesn't overflow fiel
    SHR_DBG_ASSERT(value == 0);
}

/*!
 * Encode and set an enum value based on its descriptor.
 *
 * @param[in]     pEnum  Enum format.
 * @param[in]     value  Un-encoded value to set within the entry.
 * @param[in,out] pMem   Existing memory to update of at least length (pEnum->desc.offset + 4).
 */
static NV_FORCEINLINE void
nvFieldSetEnum
(
    const NV_FIELD_ENUM *pEnum,
    const NvU32          value,
    NvU8                *pMem
)
{
    SHR_DBG_ASSERT(value < pEnum->numEntries);
    SHR_DBG_ASSERT(pEnum->entries[value].bValid);
    nvFieldSet32(&pEnum->desc, pEnum->entries[value].value, pMem);
}

/*!
 * Set an boolean field based on its descriptor.
 *
 * @param[in]     pField Boolean field descriptor.
 * @param[in]     value  Truth value.
 * @param[in,out] pMem   Existing memory to update of at least length (pField->desc.offset + 4).
 */
static NV_FORCEINLINE void
nvFieldSetBool
(
    const NV_FIELD_BOOL *pField,
    const NvBool         value,
    NvU8                *pMem
)
{
    nvFieldSet32(&pField->desc, value ^ pField->bInvert, pMem);
}

/*!
 * Get the value of a 32-bit field based on its descriptor.
 *
 * @param[in]     pField Field format.
 * @param[in]     pMem   Memory of at least length (pField->offset + 4).
 *
 * @returns the extracted value.
 */
static NV_FORCEINLINE NvU32
nvFieldGet32
(
    const NV_FIELD_DESC32 *pField,
    const NvU8            *pMem
)
{
    SHR_DBG_ASSERT(nvFieldIsValid32(pField));
    return (*(const NvU32*)(pMem + pField->offset) & pField->maskPos) >> pField->shift;
}

/*!
 * Get the value of a 64-bit field based on its descriptor.
 *
 * @param[in]     pField Field format.
 * @param[in]     pMem   Memory of at least length (pField->offset + 8).
 *
 * @returns the extracted value.
 */
static NV_FORCEINLINE NvU64
nvFieldGet64
(
    const NV_FIELD_DESC64 *pField,
    const NvU8            *pMem
)
{
    NvU32 i, shift = 0;
    NvU64 value = 0;
    SHR_DBG_ASSERT(nvFieldIsValid64(pField));
    for (i = 0; i < pField->regionCount; i++)
    {
        NvU64 region_value = (*(const NvU64*)(pMem + pField->regions[i].offset) &
            pField->regions[i].maskPos) >> pField->regions[i].shift;

        value |= region_value << shift;

        shift += pField->regions[i].width;
    }
    return value;
}

/*!
 * Get and decode an enum value based on its descriptor.
 *
 * @param[in]     pEnum  Enum format.
 * @param[in]     pMem   Memory of at least length (pEnum->desc.offset + 4).
 */
static NV_FORCEINLINE NvU32
nvFieldGetEnum
(
    const NV_FIELD_ENUM *pEnum,
    const NvU8          *pMem
)
{
    const NvU32 encoded = nvFieldGet32(&pEnum->desc, pMem);
    NvU32       decoded;
    for (decoded = 0; decoded < pEnum->numEntries; ++decoded)
    {
        if (pEnum->entries[decoded].bValid &&
            (pEnum->entries[decoded].value == encoded))
        {
            return decoded;
        }
    }
    SHR_DBG_ASSERT(0);
    return 0;
}

/*!
 * Get an boolean field based on its descriptor.
 *
 * @param[in]     pField Boolean field descriptor.
 * @param[in]     pMem   Memory of at least length (pField->desc.offset + 4).
 */
static NV_FORCEINLINE NvBool
nvFieldGetBool
(
    const NV_FIELD_BOOL *pField,
    const NvU8          *pMem
)
{
    const NvU32 value = nvFieldGet32(&pField->desc, pMem);
    SHR_DBG_ASSERT(value <= 1);
    return (NvBool)(value ^ pField->bInvert);
}

#ifdef __cplusplus
}
#endif

#endif
