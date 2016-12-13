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

#ifndef _NVIDIA_MMU_MGMT_H_
#define _NVIDIA_MMU_MGMT_H_

#include "uvmtypes.h"

typedef struct UvmMmuOps_s UvmMmuOps;

typedef struct UvmMemOps_s UvmMemOps;

NV_STATUS NvUvmMemOpsInit (unsigned fifoClass, UvmMemOps *memOps);

typedef enum
{
    UvmTlbInvalidateReplayType_none,
    UvmTlbInvalidateReplayType_start,
    UvmTlbInvalidateReplayType_start_ack_all,
    UvmTlbInvalidateReplayType_cancel_targeted,
    UvmTlbInvalidateReplayType_cancel_global,
    UvmTlbInvalidateReplayType_unsupported,
} UvmTlbInvalidateReplayType;

typedef enum
{
    UvmTlbInvalidateAckType_none,
    UvmTlbInvalidateAckType_globally,
    UvmTlbInvalidateAckType_intranode,
} UvmTlbInvalidateAckType;

typedef enum
{
    UvmTlbInvalidateLevel_all,
    UvmTlbInvalidateLevel_pte,
    UvmTlbInvalidateLevel_pl0,
    UvmTlbInvalidateLevel_pl1,
    UvmTlbInvalidateLevel_pl2,
    UvmTlbInvalidateLevel_pl3,
} UvmTlbInvalidateLevel;

typedef enum
{
    UvmTlbInvalidateMemBar_none,
    UvmTlbInvalidateMemBar_sys,
    UvmTlbInvalidateMemBar_local,
}UvmTlbInvalidateMembarType;

typedef enum
{
    UvmTlbInvalidatePdbAperture_vidmem,
    UvmTlbInvalidatePdbAperture_sysmemCoh,
    UvmTlbInvalidatePdbAperture_sysmemNcoh,
} UvmTlbInvalidatePdbAperture;

typedef enum
{
    UvmTlbInvalidateTargetVA_All,
    UvmTlbInvalidateTargetVA_Targeted,
} UvmTlbInvalidateTargetVAMode;

typedef enum
{
    UvmFaultApperture_vidmem = 0,
    UvmFaultApperture_sysmemCoh = 2,
    UvmFaultApperture_sysmemNcoh = 3,
} UvmFaultApperture;

/*******************************************************************************
    NvUvmMmuTlbInvalidate

    Push a TLB invalidate MEM_OP

    Arguments:
         pbPut: (INPUT / OUPTUT)
            Pointer to the base pointer of where in the PB methods should be
            written to.

        pbEnd: (INPUT)
            Address of the end (largest address) of the push buffer.

        targetPdb: (INPUT)
            PDB of the channels address space that needs invalidation

        gpcId: (INPUT)
            GPCID which associated to the uTLB that faulted. This inforamation is
            returned by the GPU in the fault packet.

        clientId: (INPUT)
            clientID which is also associated with the uTLB that faulted. This information
            is returned in the fault buffer packet.

        replay type: (INPUT)

        tlbAckType: (INPUT)

        invalidateLevel: (INPUT)

        sysMemBar: (INPUT)


     Returns:
        Number of bytes written to the passed in push buffer. Returns 0 if there
        was not enough room in the push buffer.
*/

typedef struct 
{
    NvU64 targetedVa;
    UvmTlbInvalidateLevel invalidatelevel;
    UvmTlbInvalidateTargetVAMode targetVAMode;
} UvmTlbInvalidateVaParams;

typedef struct
{
    UvmTlbInvalidateReplayType replayType;
    NvU32 gpcId;
    NvU32 clientId;
    UvmTlbInvalidateAckType tlbAckType;
    UvmTlbInvalidateVaParams invalidateParams;
    UvmTlbInvalidateMembarType membarType;
    NvBool disableGpcInvalidate;
} UvmTlbInvalidateMemOpsParams;

typedef NvLength (*NvUvmMmuTlbInvalidate_t)(unsigned **pbPut, unsigned *pbEnd,
                                            NvU64 targetPdb, 
                                            UvmTlbInvalidatePdbAperture targetPdbAperture,
                                            UvmTlbInvalidateMemOpsParams *pMemOpsParams);

/*******************************************************************************
    NvUvmMmuMembar

    Push a MEMBAR MEM_OP

    Arguments:
         pbPut: (INPUT / OUPTUT)
            Pointer to the base pointer of where in the PB methods should be
            written to.

        pbEnd: (INPUT)
            Address of the end (largest address) of the push buffer.

        sysMemBar: (INPUT)


     Returns:
        Number of bytes written to the passed in push buffer. Returns 0 if there
        was not enough room in the push buffer.
*/
typedef NvLength (*NvUvmMmuMembar_t)(unsigned **pbPut, unsigned *pbEnd,
                                     UvmTlbInvalidateMembarType membarType);

/*******************************************************************************
    NvUvmHostWfi

    Push a HOST WFI

    Arguments:
         pbPut: (INPUT / OUPTUT)
            Pointer to the base pointer of where in the PB methods should be
            written to.

        pbEnd: (INPUT)
            Address of the end (largest address) of the push buffer.

     Returns:
        Number of bytes written to the passed in push buffer. Returns 0 if there
        was not enough room in the push buffer.
*/
typedef NvLength (*NvUvmHostWfi_t)(unsigned **pbPut, unsigned *pbEnd);

/*******************************************************************************
    NvUvmFaultCancelSwMethod_t

    Push methods to do targetted gpu fault cancel

    Arguments:
         pbPut: (INPUT / OUPTUT)
            Pointer to the base pointer of where in the PB methods should be
            written to.

        pbEnd: (INPUT)
            Address of the end (largest address) of the push buffer.

        gpcId: (INPUT)
            gpc ID needed for targetted cancel

        clientId: (INPUT)
            client id causing the invalid access

        instancePointer: (INPUT)
            instance pointer associated with the GR context

        aperture: (INPUT)
            pdb aperture for current context

     Returns:
        Number of bytes written to the passed in push buffer. Returns 0 if there
        was not enough room in the push buffer.
*/
typedef NvLength (*NvUvmFaultCancelSwMethod_t)(unsigned **pPbPut, unsigned *pbEnd,
    unsigned gpcId, unsigned clientId, NvU64 instancePointer, UvmFaultApperture aperture);

struct UvmMemOps_s
{
    NvUvmMmuTlbInvalidate_t    tlbInvalidate;
    NvUvmMmuMembar_t           membar;
    NvUvmHostWfi_t             hostwfi;
    NvUvmFaultCancelSwMethod_t faultCancelSwMethod;
};


#endif //  _NVIDIA_MMU_MGMT_H_
