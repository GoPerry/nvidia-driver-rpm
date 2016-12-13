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

#include "uvmtypes.h"
#include "nvgputypes.h"
#include "uvm_mmu_mgmt.h"
#include "uvm_page_migration_pascal.h"
#include "uvm_mmu_mgmt_pascal.h"

#include "nvmisc.h"
#include "clc06f.h"
#include "cla06fsubch.h"
#include "clc0b5.h"
#include "clc076.h"

#define NV_METHOD(SubCh, Method, Num)                                          \
    (REF_DEF(NVC06F_DMA_INCR_OPCODE,      _VALUE)  |                           \
     REF_NUM(NVC06F_DMA_INCR_COUNT,       Num)     |                           \
     REF_NUM(NVC06F_DMA_INCR_SUBCHANNEL,  SubCh)   |                           \
     REF_NUM(NVC06F_DMA_INCR_ADDRESS,     (Method) >> 2) )

#define PUSH_PAIR(SubCh, Method, Data)                                         \
    do {                                                                       \
        **pPbPut = (NV_METHOD((SubCh),(Method),1));                             \
        (*pPbPut)++;                                                            \
        **pPbPut = ((Data));                                                    \
        (*pPbPut)++;                                                            \
    } while (0)

// Note: this hal is only used when pushing a standalone membar (no- invalidate)
static void _memOpSetupMembar(UvmTlbInvalidateMembarType membarType,
                            NvU32 *memOpC)
{
    switch(membarType)
    {
        case UvmTlbInvalidateMemBar_sys:
            FLD_SET_DRF_DEF(C06F, _MEM_OP_C, _MEMBAR_TYPE, _SYS_MEMBAR, *memOpC);
            break;
        case UvmTlbInvalidateMemBar_local:
            FLD_SET_DRF_DEF(C06F, _MEM_OP_C, _MEMBAR_TYPE, _MEMBAR, *memOpC);
            break;
        default:
            break;
     }
}

static void _memOpSetupPdb(NvU64 targetPdb, UvmTlbInvalidatePdbAperture pdbAperture, NvU32 *memOpC, NvU32 *memOpD)
{
    if (targetPdb == 0)
    {
        // invalidate all PDB's
        FLD_SET_DRF_DEF(C06F, _MEM_OP_C, _TLB_INVALIDATE_PDB, _ALL, *memOpC);
        return;
    }
    else
    {
        // setup the PDB
        *memOpC = FLD_SET_DRF_NUM(C06F, _MEM_OP_C, _TLB_INVALIDATE_PDB_ADDR_LO,
                                 targetPdb, *memOpC);
        *memOpD = FLD_SET_DRF_NUM(C06F, _MEM_OP_D, _TLB_INVALIDATE_PDB_ADDR_HI,
                                  targetPdb >> 20, *memOpD);
        FLD_SET_DRF_DEF(C06F, _MEM_OP_C, _TLB_INVALIDATE_PDB, _ONE, *memOpC);

        switch(pdbAperture)
        {
            default:
            case UvmTlbInvalidatePdbAperture_vidmem:
                FLD_SET_DRF_DEF(C06F, _MEM_OP_C,
                               _TLB_INVALIDATE_PDB_APERTURE,
                               _VID_MEM, *memOpC);
                break;
            case UvmTlbInvalidatePdbAperture_sysmemCoh:
                FLD_SET_DRF_DEF(C06F, _MEM_OP_C,
                               _TLB_INVALIDATE_PDB_APERTURE,
                               _SYS_MEM_COHERENT, *memOpC);
                break;
            case UvmTlbInvalidatePdbAperture_sysmemNcoh:
                FLD_SET_DRF_DEF(C06F, _MEM_OP_C,
                               _TLB_INVALIDATE_PDB_APERTURE,
                               _SYS_MEM_NONCOHERENT, *memOpC);
                break;
        }
    }
}

static void _memOpSetupTlbInvalidateGpc(NvU32 *memOpC, NvBool disableGpcInvalidate)
{
    if (disableGpcInvalidate)
        FLD_SET_DRF_DEF(C06F, _MEM_OP_C, _TLB_INVALIDATE_GPC, _DISABLE, *memOpC);
    else
        FLD_SET_DRF_DEF(C06F, _MEM_OP_C, _TLB_INVALIDATE_GPC, _ENABLE, *memOpC);
}

static void _memOpSetupTlbTargetVA(UvmTlbInvalidateVaParams invalidateParams,
                                   NvU32 *memOpA, NvU32 *memOpB, NvU32 *memOpD)
{
    if (invalidateParams.targetVAMode == UvmTlbInvalidateTargetVA_All)
        *memOpD = FLD_SET_DRF(C06F, _MEM_OP_D, _OPERATION, _MMU_TLB_INVALIDATE, *memOpD);
    else if (invalidateParams.targetVAMode == UvmTlbInvalidateTargetVA_Targeted)
    {
        *memOpA = FLD_SET_DRF_NUM(C06F, _MEM_OP_A, _TLB_INVALIDATE_TARGET_ADDR_LO,
                               invalidateParams.targetedVa >> 12, *memOpA);
        *memOpB = FLD_SET_DRF_NUM(C06F, _MEM_OP_B, _TLB_INVALIDATE_TARGET_ADDR_HI,
                               (invalidateParams.targetedVa >> 32), *memOpB);
        *memOpD = FLD_SET_DRF(C06F, _MEM_OP_D, _OPERATION, _MMU_TLB_INVALIDATE_TARGETED, *memOpD);
    }
}
static void _memOpSetupTlbLevel(UvmTlbInvalidateLevel tlbLevel, NvU32 *memOpC)
{
    switch(tlbLevel)
    {
        case UvmTlbInvalidateLevel_all :
          *memOpC = FLD_SET_DRF(C06F, _MEM_OP_C, _TLB_INVALIDATE_PAGE_TABLE_LEVEL,
                _ALL, *memOpC);
            break;
        case UvmTlbInvalidateLevel_pte:
            *memOpC = FLD_SET_DRF(C06F, _MEM_OP_C, _TLB_INVALIDATE_PAGE_TABLE_LEVEL,
                _PTE_ONLY, *memOpC);
            break;
        case UvmTlbInvalidateLevel_pl0:
            *memOpC = FLD_SET_DRF(C06F, _MEM_OP_C, _TLB_INVALIDATE_PAGE_TABLE_LEVEL,
                _UP_TO_PDE0, *memOpC);
            break;
        case UvmTlbInvalidateLevel_pl1:
            *memOpC = FLD_SET_DRF(C06F, _MEM_OP_C, _TLB_INVALIDATE_PAGE_TABLE_LEVEL,
                _UP_TO_PDE1, *memOpC);
            break;
        case UvmTlbInvalidateLevel_pl2:
            *memOpC = FLD_SET_DRF(C06F, _MEM_OP_C, _TLB_INVALIDATE_PAGE_TABLE_LEVEL,
                _UP_TO_PDE2, *memOpC);
            break;
        case UvmTlbInvalidateLevel_pl3:
            *memOpC = FLD_SET_DRF(C06F, _MEM_OP_C, _TLB_INVALIDATE_PAGE_TABLE_LEVEL,
                _UP_TO_PDE3, *memOpC);
            break;
    }
}

static void _memOpSetupReplayType(UvmTlbInvalidateReplayType replay, NvU32 gpcId, NvU32 clientId, NvU32 *memOpC, NvU32 *memOpA)
{
    switch (replay)
    {

        default:
        case UvmTlbInvalidateReplayType_none:
            FLD_SET_DRF_DEF(C06F, _MEM_OP_C, _TLB_INVALIDATE_REPLAY,
                _NONE, *memOpC);
            break;

        case UvmTlbInvalidateReplayType_start:
            FLD_SET_DRF_DEF(C06F, _MEM_OP_C, _TLB_INVALIDATE_REPLAY,
                _START, *memOpC);
            break;

        case UvmTlbInvalidateReplayType_cancel_targeted:
            FLD_SET_DRF_DEF(C06F, _MEM_OP_C, _TLB_INVALIDATE_REPLAY,
                _CANCEL_TARGETED, *memOpC);
            *memOpA = FLD_SET_DRF_NUM(C06F, _MEM_OP_A,
                                     _TLB_INVALIDATE_CANCEL_TARGET_GPC_ID,
                                     gpcId, *memOpA);
            *memOpA = FLD_SET_DRF_NUM(C06F, _MEM_OP_A,
                                    _TLB_INVALIDATE_CANCEL_TARGET_CLIENT_UNIT_ID,
                                     clientId, *memOpA);
            break;
        case UvmTlbInvalidateReplayType_cancel_global:
            FLD_SET_DRF_DEF(C06F, _MEM_OP_C, _TLB_INVALIDATE_REPLAY,
                _CANCEL_GLOBAL, *memOpC);
            break;

        case UvmTlbInvalidateReplayType_start_ack_all:
            FLD_SET_DRF_DEF(C06F, _MEM_OP_C, _TLB_INVALIDATE_REPLAY,
                _START_ACK_ALL, *memOpC);
            break;
    }
}

static void _memOpSetupInvalidateAckType(UvmTlbInvalidateAckType tlbAckType,
                                         NvU32 *memOpC)
{
    switch (tlbAckType)
    {
        default:
        case UvmTlbInvalidateAckType_none:
            FLD_SET_DRF_DEF(C06F, _MEM_OP_C, _TLB_INVALIDATE_ACK_TYPE,
                _NONE, *memOpC);
            break;

        case UvmTlbInvalidateAckType_globally:
            FLD_SET_DRF_DEF(C06F, _MEM_OP_C, _TLB_INVALIDATE_ACK_TYPE,
                _GLOBALLY, *memOpC);
            break;

        case UvmTlbInvalidateAckType_intranode:
            FLD_SET_DRF_DEF(C06F, _MEM_OP_C, _TLB_INVALIDATE_ACK_TYPE,
                _INTRANODE, *memOpC);
            break;
    }
}

NvLength NvUvmMmuTlbInvalidateC06F(
    unsigned **pPbPut, unsigned *pPbEnd,
    NvU64 targetPdb,
    UvmTlbInvalidatePdbAperture targetPdbAperture,
    UvmTlbInvalidateMemOpsParams *pMemOpsParams
)
{
    NvU32 memOpA = 0;
    NvU32 memOpB = 0;
    NvU32 memOpC = 0;
    NvU32 memOpD = 0;
    NvLength methodSize = 0;

    methodSize = 4 * 2 * sizeof(unsigned);
    if (((NvU64)*pPbPut + methodSize > (NvU64)pPbEnd) || (!pMemOpsParams))
        return 0;

    if ((pMemOpsParams->membarType != UvmTlbInvalidateMemBar_none &&
         pMemOpsParams->tlbAckType != UvmTlbInvalidateAckType_globally) ||
        (pMemOpsParams->membarType == UvmTlbInvalidateMemBar_none &&
         pMemOpsParams->tlbAckType == UvmTlbInvalidateAckType_globally))
        return 0;

    _memOpSetupPdb(targetPdb, targetPdbAperture, &memOpC, &memOpD);

    _memOpSetupTlbInvalidateGpc(&memOpC, pMemOpsParams->disableGpcInvalidate);

    _memOpSetupReplayType(pMemOpsParams->replayType, pMemOpsParams->gpcId,
                          pMemOpsParams->clientId, &memOpC, &memOpA);

    _memOpSetupInvalidateAckType(pMemOpsParams->tlbAckType, &memOpC);

    _memOpSetupTlbTargetVA(pMemOpsParams->invalidateParams,
                               &memOpA, &memOpB, &memOpD);

    _memOpSetupTlbLevel(pMemOpsParams->invalidateParams.invalidatelevel,
                        &memOpC);

    PUSH_PAIR(NVA06F_SUBCHANNEL_COPY_ENGINE, NVC06F_MEM_OP_A, memOpA);
    PUSH_PAIR(NVA06F_SUBCHANNEL_COPY_ENGINE, NVC06F_MEM_OP_B, memOpB);
    PUSH_PAIR(NVA06F_SUBCHANNEL_COPY_ENGINE, NVC06F_MEM_OP_C, memOpC);
    PUSH_PAIR(NVA06F_SUBCHANNEL_COPY_ENGINE, NVC06F_MEM_OP_D, memOpD);

    // Follow up with a standalone hub membar
    if (pMemOpsParams->membarType != UvmTlbInvalidateMemBar_none)
        methodSize += NvUvmMmuMembarC06F(pPbPut, pPbEnd,
                                            pMemOpsParams->membarType);

    return methodSize;
}

NvLength NvUvmMmuMembarC06F(unsigned **pPbPut, unsigned *pPbEnd,
                            UvmTlbInvalidateMembarType membarType)
{
    NvU32 memOpA = 0;
    NvU32 memOpB = 0;
    NvU32 memOpC = 0;
    NvU32 memOpD = 0;
    NvLength methodSize = 0;

    methodSize = 4 * 2 * sizeof(unsigned);
    if (((NvU64)*pPbPut + methodSize > (NvU64)pPbEnd))
        return 0;

    if (membarType == UvmTlbInvalidateMemBar_none)
        return 0;

    _memOpSetupMembar(membarType, &memOpC);
    FLD_SET_DRF_DEF(C06F, _MEM_OP_D, _OPERATION, _MEMBAR, memOpD);

    PUSH_PAIR(NVA06F_SUBCHANNEL_COPY_ENGINE, NVC06F_MEM_OP_A, memOpA);
    PUSH_PAIR(NVA06F_SUBCHANNEL_COPY_ENGINE, NVC06F_MEM_OP_B, memOpB);
    PUSH_PAIR(NVA06F_SUBCHANNEL_COPY_ENGINE, NVC06F_MEM_OP_C, memOpC);
    PUSH_PAIR(NVA06F_SUBCHANNEL_COPY_ENGINE, NVC06F_MEM_OP_D, memOpD);

    return methodSize;
}

NvLength NvUvmHostWfiC06F(unsigned **pPbPut, unsigned *pPbEnd)
{
    NvLength methodSize = 1 * 2 * sizeof(unsigned);
    if (((NvU64)*pPbPut + methodSize > (NvU64)pPbEnd))
        return 0;

    PUSH_PAIR(NVA06F_SUBCHANNEL_COPY_ENGINE, NVC06F_WFI, NVC06F_WFI_SCOPE_ALL);

    return methodSize;
}

NvLength NvUvmFaultCancelSwMethodC06F(unsigned **pPbPut, unsigned *pbEnd, 
    unsigned gpcId, unsigned clientId, NvU64 instancePointer, UvmFaultApperture aperture)
{
    NvLength methodSize = 0;
    unsigned dataA = (NvU32)instancePointer;
    unsigned dataB = (NvU32)(instancePointer >>32);
    unsigned dataC = 0;

    //
    // The amount of space needed to push the methods is:
    // 2 methods per push * 4 pushes * 32 bits per method
    //
    methodSize = 2 * 4 * sizeof(unsigned);

    if ((NvU64)*pPbPut + methodSize > (NvU64)pbEnd)
        return 0;

    // instance pointer is 4K aligned. Using lower 12 bits to store other information
    dataA = FLD_SET_DRF_NUM(C076, _FAULT_CANCEL_A, _INST_APERTURE, aperture, dataA);

    dataC = FLD_SET_DRF_NUM(C076, _FAULT_CANCEL_C, _CLIENT_ID, clientId, dataC);
    dataC = FLD_SET_DRF_NUM(C076, _FAULT_CANCEL_C, _GPC_ID, gpcId, dataC);

    PUSH_PAIR(UVM_SW_OBJ_SUBCHANNEL, NVC076_SET_OBJECT, GP100_UVM_SW);
    PUSH_PAIR(UVM_SW_OBJ_SUBCHANNEL, NVC076_FAULT_CANCEL_A, dataA);
    PUSH_PAIR(UVM_SW_OBJ_SUBCHANNEL, NVC076_FAULT_CANCEL_B, dataB);
    PUSH_PAIR(UVM_SW_OBJ_SUBCHANNEL, NVC076_FAULT_CANCEL_C, dataC);

    return methodSize;
}

