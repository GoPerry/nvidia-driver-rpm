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

#include "uvmtypes.h"
#ifdef __linux__
#include "uvm_common.h"
#endif
#include "nvgputypes.h"
#include "uvm_full_fault_buffer.h"
#include "uvm_full_fault_buffer_pascal.h"
#include "nvmisc.h"
#include "clb069.h"
#include "clb069sw.h"
#include "uvm_pascal_fault_ref.h"

NV_STATUS uvmfull_get_fault_val(UvmFaultType fType, NvU32 *regField)
{
    if (!regField)
        return NV_ERR_INVALID_ARGUMENT;

    switch (fType)
    {
        case uvmFault_invalidPde:
            *regField = NV_PFAULT_FAULT_TYPE_PDE;
            return NV_OK;
        case uvmFault_invalidPdeSize:
            *regField = NV_PFAULT_FAULT_TYPE_PDE_SIZE;
            return NV_OK;
        case uvmFault_invalidPte:
            *regField = NV_PFAULT_FAULT_TYPE_PTE;
            return NV_OK;
        case uvmFault_limitViolation:
            *regField = NV_PFAULT_FAULT_TYPE_VA_LIMIT_VIOLATION;
            return NV_OK;
        case uvmFault_unboundInstBlock:
            *regField = NV_PFAULT_FAULT_TYPE_UNBOUND_INST_BLOCK;
            return NV_OK;
        case uvmFault_privViolation:
            *regField = NV_PFAULT_FAULT_TYPE_PRIV_VIOLATION;
            return NV_OK;
        case uvmFault_pitchMaskViolation:
            *regField = NV_PFAULT_FAULT_TYPE_PITCH_MASK_VIOLATION;
            return NV_OK;
        case uvmFault_write:
            *regField = NV_PFAULT_FAULT_TYPE_RO_VIOLATION;
            return NV_OK;
        case uvmFault_workCreation:
            *regField = NV_PFAULT_FAULT_TYPE_WORK_CREATION;
            return NV_OK;
        case uvmFault_unsupportedAperture:
            *regField = NV_PFAULT_FAULT_TYPE_UNSUPPORTED_APERTURE;
            return NV_OK;
        case uvmFault_compressionFailure:
            *regField = NV_PFAULT_FAULT_TYPE_COMPRESSION_FAILURE;
            return NV_OK;
        case uvmFault_unsupportedKind:
            *regField = NV_PFAULT_FAULT_TYPE_UNSUPPORTED_KIND;
            return NV_OK;
        case uvmFault_regionViolation:
            *regField = NV_PFAULT_FAULT_TYPE_REGION_VIOLATION;
            return NV_OK;
        case uvmFault_poison:
            *regField = NV_PFAULT_FAULT_TYPE_POISONED;
            return NV_OK;
        case uvmFault_atomic:
            *regField = NV_PFAULT_FAULT_TYPE_ATOMIC_VIOLATION;
            return NV_OK;
        default:
            return NV_ERR_INVALID_ARGUMENT;
    }
}

NV_STATUS uvmfull_get_fault_type(NvU32 fault, UvmFaultType *fType)
{
    if (!fType)
        return NV_ERR_INVALID_ARGUMENT;

    switch (fault)
    {
        case NV_PFAULT_FAULT_TYPE_PDE:
            *fType = uvmFault_invalidPde;
            return NV_OK;
        case NV_PFAULT_FAULT_TYPE_PDE_SIZE:
            *fType = uvmFault_invalidPdeSize;
            return NV_OK;
        case NV_PFAULT_FAULT_TYPE_PTE:
            *fType = uvmFault_invalidPte;
            return NV_OK;
        case NV_PFAULT_FAULT_TYPE_VA_LIMIT_VIOLATION:
            *fType = uvmFault_limitViolation;
            return NV_OK;
        case NV_PFAULT_FAULT_TYPE_UNBOUND_INST_BLOCK:
            *fType = uvmFault_unboundInstBlock;
            return NV_OK;
        case NV_PFAULT_FAULT_TYPE_PRIV_VIOLATION:
            *fType = uvmFault_privViolation;
            return NV_OK;
        case NV_PFAULT_FAULT_TYPE_PITCH_MASK_VIOLATION:
            *fType = uvmFault_pitchMaskViolation;
            return NV_OK;
        case NV_PFAULT_FAULT_TYPE_RO_VIOLATION:
            *fType = uvmFault_write;
            return NV_OK;
        case NV_PFAULT_FAULT_TYPE_WORK_CREATION:
            *fType = uvmFault_workCreation;
            return NV_OK;
        case NV_PFAULT_FAULT_TYPE_UNSUPPORTED_APERTURE:
            *fType = uvmFault_unsupportedAperture;
            return NV_OK;
        case NV_PFAULT_FAULT_TYPE_COMPRESSION_FAILURE:
            *fType = uvmFault_compressionFailure;
            return NV_OK;
        case NV_PFAULT_FAULT_TYPE_UNSUPPORTED_KIND:
            *fType = uvmFault_unsupportedKind;
            return NV_OK;
        case NV_PFAULT_FAULT_TYPE_REGION_VIOLATION:
            *fType = uvmFault_regionViolation;
            return NV_OK;
        case NV_PFAULT_FAULT_TYPE_POISONED:
            *fType = uvmFault_poison;
            return NV_OK;
        case NV_PFAULT_FAULT_TYPE_ATOMIC_VIOLATION:
            *fType = uvmFault_atomic;
            return NV_OK;
        default:
            return NV_ERR_INVALID_ARGUMENT;
    }
}

NV_STATUS uvmfull_get_access_type(NvU32 a, UvmAccessType *aType)
{
    if (!aType)
        return NV_ERR_INVALID_ARGUMENT;
    switch (a)
    {
        case NV_PFAULT_ACCESS_TYPE_READ:
            *aType = UvmAccessType_read;
            return NV_OK;
        case NV_PFAULT_ACCESS_TYPE_WRITE:
            *aType = UvmAccessType_write;
            return NV_OK;
        case NV_PFAULT_ACCESS_TYPE_ATOMIC:
            *aType = UvmAccessType_atomic;
            return NV_OK;
        case NV_PFAULT_ACCESS_TYPE_PREFETCH:
            *aType = UvmAccessType_prefetch;
            return NV_OK;
        default:
            return NV_ERR_INVALID_ARGUMENT;
    }
}

NV_STATUS uvmfull_get_access_type_val(UvmAccessType aType, NvU32 *val)
{
    if (!val)
        return NV_ERR_INVALID_ARGUMENT;

    switch (aType)
    {
        case UvmAccessType_read:
            *val = NV_PFAULT_ACCESS_TYPE_READ;
            return NV_OK;
        case UvmAccessType_write:
            *val = NV_PFAULT_ACCESS_TYPE_WRITE;
            return NV_OK;
        case UvmAccessType_atomic:
            *val = NV_PFAULT_ACCESS_TYPE_ATOMIC;
            return NV_OK;
        case UvmAccessType_prefetch:
            *val = NV_PFAULT_ACCESS_TYPE_PREFETCH;
            return NV_OK;
        default:
            return NV_ERR_INVALID_ARGUMENT;
    }
    return NV_ERR_INVALID_ARGUMENT;
}

NV_STATUS uvmfull_get_replay_type_val(UvmReplayType replay, NvU32 *val)
{
    if (!val)
        return NV_ERR_INVALID_ARGUMENT;

    switch (replay)
    {
        case UvmReplayType_none:
            *val = NV_PFB_PRI_MMU_INVALIDATE_REPLAY_NONE;
            break;
        case UvmReplayType_start:
            *val = NV_PFB_PRI_MMU_INVALIDATE_REPLAY_START;
            break;
        case UvmReplayType_start_ack_all:
            *val = NV_PFB_PRI_MMU_INVALIDATE_REPLAY_START_ACK_ALL;
            break;
        case UvmReplayType_cancel_targeted:
            *val = NV_PFB_PRI_MMU_INVALIDATE_REPLAY_CANCEL_TARGETED;
            break;
        case UvmReplayType_cancel_global:
            *val = NV_PFB_PRI_MMU_INVALIDATE_REPLAY_CANCEL_GLOBAL;
            break;
        default:
            return NV_ERR_INVALID_ARGUMENT;
    }
    return NV_OK;
}

void uvmfull_set_faultbuffer_entry_valid_hal_b069(NvU64 faultBufferAddress,
        NvU32 index, NvBool bValid)
{
    NVB069_FAULT_BUFFER_ENTRY *bufferAddress =
            (NVB069_FAULT_BUFFER_ENTRY *) faultBufferAddress;
    NvU32 *faultEntry = (NvU32 *) &bufferAddress[index];
    if (bValid)
        FLD_SET_DRF_DEF_MW(B069, _FAULT_BUF_ENTRY, _VALID, _TRUE, faultEntry);
    else
        FLD_SET_DRF_DEF_MW(B069, _FAULT_BUF_ENTRY, _VALID, _FALSE, faultEntry);
    return;
}

NvBool uvmfull_is_faultbuffer_entry_valid_hal_b069(NvU64 faultBufferAddress,
        NvU32 index)
{
    NvBool faultBufferEntryValid;
    NVB069_FAULT_BUFFER_ENTRY *bufferAddress =
            (NVB069_FAULT_BUFFER_ENTRY *) faultBufferAddress;
    NvU32 *faultEntry = (NvU32 *) &bufferAddress[index];
    faultBufferEntryValid = DRF_VAL_MW(B069, _FAULT_BUF_ENTRY, _VALID,
            faultEntry);
    return faultBufferEntryValid;
}

NV_STATUS uvmfull_parse_fault_buffer_hal_b069(NvU64 faultBufferAddress,
    NvU32 index, UvmFaultBufferEntry *bufferData)
{
    NVB069_FAULT_BUFFER_ENTRY *bufferAddress =
            (NVB069_FAULT_BUFFER_ENTRY *) faultBufferAddress;
    NvU32 *faultEntry = (NvU32 *) &bufferAddress[index];
    NvU64 addrHi, addrLo;
    NvU64 timestampLo, timestampHi;
    NvU32 tmp;
    NV_STATUS status = NV_OK;

    addrHi = DRF_VAL_MW(B069, _FAULT_BUF_ENTRY, _INST_HI, faultEntry);
    addrLo = DRF_VAL_MW(B069, _FAULT_BUF_ENTRY, _INST_LO, faultEntry);
    bufferData->uvmFaultInstance = (addrLo << 12) + (addrHi << 32);

    tmp = DRF_VAL_MW(B069, _FAULT_BUF_ENTRY, _INST_APERTURE, faultEntry);
    bufferData->uvmFaultInstLoc =
    tmp == NVB069_FAULT_BUF_ENTRY_INST_APERTURE_VID_MEM ?
            uvmFaultInstLoc_vidmem :
        tmp == NVB069_FAULT_BUF_ENTRY_INST_APERTURE_SYS_MEM_COHERENT ?
                uvmFaultInstLoc_sysmem_coh :
            tmp == NVB069_FAULT_BUF_ENTRY_INST_APERTURE_SYS_MEM_NONCOHERENT ?
                    uvmFaultInstLoc_sysmem_ncoh : uvmFaultInstLoc_invalid;

    addrHi = DRF_VAL_MW(B069, _FAULT_BUF_ENTRY, _ADDR_HI, faultEntry);
    addrLo = DRF_VAL_MW(B069, _FAULT_BUF_ENTRY, _ADDR_LO, faultEntry);
    bufferData->uvmFaultAddress = addrLo + (addrHi << 32);

    timestampLo = DRF_VAL_MW(B069, _FAULT_BUF_ENTRY, _TIMESTAMP_LO, faultEntry); 
    timestampHi = DRF_VAL_MW(B069, _FAULT_BUF_ENTRY, _TIMESTAMP_HI, faultEntry);
    bufferData->uvmFaultTimestamp = timestampLo + (timestampHi << 32);

    tmp = DRF_VAL_MW(B069, _FAULT_BUF_ENTRY, _FAULT_TYPE, faultEntry);
    status = uvmfull_get_fault_type(tmp, &bufferData->uvmFaultType);
    if (status != NV_OK)
        return status;

    tmp = DRF_VAL_MW(B069, _FAULT_BUF_ENTRY, _ACCESS_TYPE, faultEntry);
    status = uvmfull_get_access_type(tmp, &bufferData->uvmFaultAccessType);
    if (status != NV_OK)
        return status;

    bufferData->uvmFaultClientId = DRF_VAL_MW(B069, _FAULT_BUF_ENTRY, _CLIENT,
            faultEntry);
    bufferData->uvmFaultMmuClientType = DRF_VAL_MW(B069, _FAULT_BUF_ENTRY,
            _MMU_CLIENT_TYPE, faultEntry);
    bufferData->uvmFaultGpcId = DRF_VAL_MW(B069, _FAULT_BUF_ENTRY, _GPC_ID,
            faultEntry);
    bufferData->uvmFaultEntryValid = DRF_VAL_MW(B069, _FAULT_BUF_ENTRY, _VALID,
            faultEntry);
    return status;
}

NV_STATUS uvmfull_write_fault_buffer_packet_b069(
        UvmFaultBufferEntry *pFaultBuffer, NvU8 *pFaultBufferb096)
{
    NvU32 tmp;
    NV_STATUS status = NV_OK;
    NvU32 *pFb = (NvU32 *)pFaultBufferb096;

    status = uvmfull_get_fault_val(pFaultBuffer->uvmFaultType, &tmp);
    if (NV_OK != status)
        return status;
    FLD_SET_DRF_NUM_MW(B069, _FAULT_BUF_ENTRY, _FAULT_TYPE, tmp, pFb);

    status = uvmfull_get_access_type_val(pFaultBuffer->uvmFaultAccessType,
            &tmp);
    if (NV_OK != status)
        return status;
    FLD_SET_DRF_NUM_MW(B069, _FAULT_BUF_ENTRY, _ACCESS_TYPE, tmp, pFb);

    FLD_SET_DRF_NUM_MW(B069, _FAULT_BUF_ENTRY, _INST_HI,
            (pFaultBuffer->uvmFaultInstance >> 32), pFb);
    FLD_SET_DRF_NUM_MW(B069, _FAULT_BUF_ENTRY, _INST_LO,
            ((pFaultBuffer->uvmFaultInstance >> 12) & 0xFFFFFFFF), pFb);
    FLD_SET_DRF_NUM_MW(B069, _FAULT_BUF_ENTRY, _ADDR_HI,
            (pFaultBuffer->uvmFaultAddress >> 32), pFb);
    FLD_SET_DRF_NUM_MW(B069, _FAULT_BUF_ENTRY, _ADDR_LO,
            (pFaultBuffer->uvmFaultAddress & 0xFFFFFFFF), pFb);

    FLD_SET_DRF_NUM_MW(B069, _FAULT_BUF_ENTRY, _CLIENT,
            (pFaultBuffer->uvmFaultClientId), pFb);
    FLD_SET_DRF_NUM_MW(B069, _FAULT_BUF_ENTRY, _MMU_CLIENT_TYPE,
            (pFaultBuffer->uvmFaultMmuClientType), pFb);
    FLD_SET_DRF_NUM_MW(B069, _FAULT_BUF_ENTRY, _GPC_ID,
            (pFaultBuffer->uvmFaultGpcId), pFb);

    FLD_SET_DRF_NUM_MW(B069, _FAULT_BUF_ENTRY, _VALID,
            !!pFaultBuffer->uvmFaultEntryValid, pFb);

    return NV_OK;
}

NvU32 uvmfull_get_fault_packet_size_b069()
{
    return NVB069_FAULT_BUF_SIZE;
}

NV_STATUS uvmfull_set_reg_replay_params_hal_b069(volatile NvU32 *gpuBar0ReplayPtr,
        NvU32 gpcId, NvU32 clientId, NvU32 clientType, UvmReplayType replayType,
        NvBool bIsSysmem, NvU32 flags)
{
    NvU32 invalidateMmuRegVal = 0;
    NvU32 replay = 0;


    invalidateMmuRegVal = FLD_SET_DRF(_PFB, _PRI_MMU_INVALIDATE, _ALL_VA, _TRUE,
            invalidateMmuRegVal);
    invalidateMmuRegVal = FLD_SET_DRF(_PFB, _PRI_MMU_INVALIDATE, _ALL_PDB,
            _TRUE, invalidateMmuRegVal);
    if (bIsSysmem)
        invalidateMmuRegVal = FLD_SET_DRF(_PFB, _PRI_MMU_INVALIDATE,
                _SYS_MEMBAR, _TRUE, invalidateMmuRegVal);
    else
        invalidateMmuRegVal = FLD_SET_DRF(_PFB, _PRI_MMU_INVALIDATE,
                _SYS_MEMBAR, _FALSE, invalidateMmuRegVal);
    invalidateMmuRegVal = FLD_SET_DRF_NUM(_PFB, _PRI_MMU_INVALIDATE,
            _CANCEL_CLIENT_ID, clientId, invalidateMmuRegVal);
    invalidateMmuRegVal = FLD_SET_DRF_NUM(_PFB, _PRI_MMU_INVALIDATE,
            _CANCEL_GPC_ID, gpcId, invalidateMmuRegVal);
    invalidateMmuRegVal = FLD_SET_DRF(_PFB, _PRI_MMU_INVALIDATE,
            _CANCEL_CLIENT_TYPE, _GPC, invalidateMmuRegVal);
    invalidateMmuRegVal = FLD_SET_DRF(_PFB, _PRI_MMU_INVALIDATE, _TRIGGER,
            _TRUE, invalidateMmuRegVal);

    if (NV_OK != uvmfull_get_replay_type_val(replayType, &replay))
        return NV_ERR_INVALID_ARGUMENT;

    invalidateMmuRegVal = FLD_SET_DRF_NUM(_PFB, _PRI_MMU_INVALIDATE, _REPLAY,
            replay, invalidateMmuRegVal);

    MEM_WR32(gpuBar0ReplayPtr, invalidateMmuRegVal);

    return NV_OK;
}

NvBool uvmfull_is_faultbuffer_interrupt_pending_b069(NvU32 *gpuBar0Fault)
{
    NvU32 *temp_int_enable;
    NvU32 pending_intr;
    if (!gpuBar0Fault)
        return NV_FALSE;

    temp_int_enable = (NvU32 *)((NvU64)gpuBar0Fault + 0x40);

    pending_intr = ((*(volatile NvU32*)temp_int_enable & *(volatile NvU32*)gpuBar0Fault));

    return FLD_TEST_DRF(_PMC, _INTR_REPLAYABLE, _FAULT, _PENDING, pending_intr);
}

void uvmfull_set_hi_fault_interrupt_bit_b069(NvU32 *reg)
{
    NvU32 tmp = 0;

    FLD_SET_DRF_DEF(_PMC, _INTR_REPLAYABLE, _FAULT, _PENDING, tmp);
    MEM_WR32(reg, tmp);
}

void uvmfull_control_prefetch_b069(volatile NvU32 *gpuBar0PrefetchCtrlReg,
                                    UvmPrefetchThrottleRate throttleRate)
{
    NvU32 prefetchCtrl = 0;
    NvU32 rate = 0;
    switch (throttleRate)
    {
    case uvmPrefetchThrottle_allow_all:
        rate = NV_PFB_PRI_MMU_PAGE_FAULT_CTRL_PRF_FILTER_SEND_ALL;
        break;
    case uvmPrefetchThrottle_1_in_Npower1:
        rate = 1;
        break;
    case uvmPrefetchThrottle_1_in_Npower2:
        rate = 2;
        break;
    case uvmPrefetchThrottle_allow_none:
        rate = NV_PFB_PRI_MMU_PAGE_FAULT_CTRL_PRF_FILTER_SEND_NONE;
        break;
    default:
        return;
    }

    prefetchCtrl = (*gpuBar0PrefetchCtrlReg);
    prefetchCtrl = FLD_SET_DRF_NUM(_PFB, _PRI_MMU_PAGE_FAULT_CTRL, _PRF_FILTER,
                                    rate, prefetchCtrl);
    MEM_WR32(gpuBar0PrefetchCtrlReg, prefetchCtrl);
}

NvBool uvmfull_test_faultbuffer_overflow_hal_b069(UvmFaultBufferRegisters gpuBar0faultBuffer)
{
    NvU32 faultBufferInfo = MEM_RD32(gpuBar0faultBuffer.pFaultBufferInfo);
    return FLD_TEST_DRF(_PFIFO, _REPLAYABLE_FAULT_BUFFER_INFO, _OVERFLOW,_TRUE,
            faultBufferInfo);
}
void uvmfull_clear_faultbuffer_overflow_hal_b069(UvmFaultBufferRegisters gpuBar0faultBuffer)
{
    NvU32 faultBufferInfo = MEM_RD32(gpuBar0faultBuffer.pFaultBufferInfo);
    FLD_SET_DRF_DEF(_PFIFO, _REPLAYABLE_FAULT_BUFFER_INFO, _OVERFLOW, _CLR,
            faultBufferInfo);
    MEM_WR32(gpuBar0faultBuffer.pFaultBufferInfo, faultBufferInfo);
}
