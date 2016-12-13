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

//
// uvm_full_fault_buffer.h
//
// This file contains structures and function declarations to read or update
// the fault buffer/related registers and read/mask/unmask the fault intr.
//
#ifndef _UVM_FULL_FAULT_BUFFER_H_
#define _UVM_FULL_FAULT_BUFFER_H_

#include "uvmtypes.h"

#define MAXWELL_FAULT_BUFFER_A (0xb069)
#define MEM_RD32(a) (*(const volatile NvU32 *)(a)) 
#define MEM_WR32(a, d) do { *(volatile NvU32 *)(a) = (d); } while (0)
typedef enum
{
    UvmReplayType_none,
    UvmReplayType_start,
    UvmReplayType_start_ack_all,
    UvmReplayType_cancel_targeted,
    UvmReplayType_cancel_global
} UvmReplayType;

typedef enum
{
    UvmAccessType_read = 0,
    UvmAccessType_write = 1,
    UvmAccessType_atomic = 2,
    UvmAccessType_prefetch = 3
} UvmAccessType;

typedef enum
{
    uvmFault_invalidPde,
    uvmFault_invalidPdeSize,
    uvmFault_invalidPte,
    uvmFault_limitViolation,
    uvmFault_unboundInstBlock,
    uvmFault_privViolation,
    uvmFault_pitchMaskViolation,
    uvmFault_write,
    uvmFault_workCreation,
    uvmFault_unsupportedAperture,
    uvmFault_compressionFailure,
    uvmFault_unsupportedKind,
    uvmFault_regionViolation,
    uvmFault_poison,
    uvmFault_atomic,
} UvmFaultType;

typedef enum
{
    uvmFaultInstLoc_invalid,
    uvmFaultInstLoc_vidmem,
    uvmFaultInstLoc_sysmem_coh,
    uvmFaultInstLoc_sysmem_ncoh,
} UvmFaultInstLoc;

typedef struct
{
    NvU64 uvmFaultInstance;
    UvmFaultInstLoc uvmFaultInstLoc;
    NvU64 uvmFaultAddress;
    NvU64 uvmFaultTimestamp;
    UvmFaultType uvmFaultType;
    UvmAccessType uvmFaultAccessType;
    NvU32 uvmFaultClientId;
    NvU32 uvmFaultMmuClientType;
    NvU32 uvmFaultGpcId;
    NvBool uvmFaultEntryValid;
} UvmFaultBufferEntry;

typedef enum
{
    uvmPrefetchThrottle_allow_all,
    uvmPrefetchThrottle_1_in_Npower1,
    uvmPrefetchThrottle_1_in_Npower2,
    uvmPrefetchThrottle_allow_none,
} UvmPrefetchThrottleRate;

typedef struct
{
    volatile NvU32 *pFaultBufferGet;        //!< cpu mapped gpu bar0 get pointer
    volatile NvU32 *pFaultBufferPut;        //!< cpu mapped gpu bar0 put pointer
    volatile NvU32 *pFaultBufferInfo;       //!< cpu mapped gpu bar0 fault info ptr
} UvmFaultBufferRegisters;

/******************************************************************************
    NvUvmSetFaultBufferEntryValid_t
        Sets the given fault buffer entry to valid/invalid state

     Arguments:
         faultBufferAddress: (INPUT)
            Base address of the fault buffer.

        offset: (INPUT)
            Offset into the fault buffer for the curent fault.

        bValid: (INPUT)
            Set fault buffer entry to valid/invalid state
 */
typedef void (*NvUvmSetFaultBufferEntryValid_t)(NvU64 faultBufferAddress,
    NvU32 offset, NvBool bValid);

/******************************************************************************
    NvUvmIsFaultBufferEntryValid_t
        Checks if a given fault buffer entry is valid

     Arguments:
        faultBufferAddress: (INPUT)
            Base address of the fault buffer.

        offset: (INPUT)
            Offset into the fault buffer for the curent fault.

     Returns:
        NV_TRUE, if the entry is valid, else NV_FALSE
 */
typedef NvBool (*NvUvmIsFaultBufferEntryValid_t)(NvU64 faultBufferAddress,
    NvU32 offset);

/******************************************************************************
    NvUvmParseFaultBufferEntry_t
        Parses the fault buffer entry at a given offset and fills
        UvmFaultBufferEntry structure passed as input

     Arguments:
         faultBufferAddress: (INPUT)
            Base address of the fault buffer.

         offset: (INPUT)
            Offset into the fault buffer for the curent fault.

         bufferData: (OUTPUT)
            Pointer to the fault buffer data structure, where the information
            of the fault is filled by this API.

    Returns:
        NV_OK: if buffer data is filled corectly
        NV_ERR_INVALID_ARGUMENT: if a invalid data entry is read in the
        fault buffer
 */

typedef NV_STATUS (*NvUvmParseFaultBufferEntry_t)(NvU64 faultBufferAddress,
    NvU32 offset, UvmFaultBufferEntry *bufferData);

/******************************************************************************
    NvUvmSetReplayParamsReg_t
        Replay/Cancel fault using bar0 register mapping

     Arguments:
        Bar0ReplayPtr: (INPUT)
            CPU ptr to the bar0 replay register

         GPC ID: (INPUT)
            Graphics processing cluster id for fault replay

         Client ID: (INPUT)
            Client ID for a given client type

         Client Type: (INPUT)
            Whether it is a GPC/HUB client

         Flags: (INPUT)
            Flag to select replay/cancel fault

     Returns:
            NV_OK: if the operation succeeds
            NV_ERR_INVALID_ARGUMENT: if any of the input arguments are invalid
*/
typedef NV_STATUS (*NvUvmSetReplayParamsReg_t)(volatile NvU32*gpuBar0ReplayPtr,
    NvU32 gpcId,
        NvU32 clientId, NvU32 clientType, UvmReplayType replayType,
        NvBool bIsSysmem, NvU32 flags);

/******************************************************************************
    NvUvmGetFaultPacketSize_t
        This function returns the fault buffer size for its parent gpu
*/
typedef NvU32 (*NvUvmGetFaultPacketSize_t)(void);

/******************************************************************************
    NvUvmIsFaultInterruptPending_t
        Check if fault intr is pending

    Arguments:
        intrReg: (INPUT)
            Bar0 mapping to intr register.
Returns:
    NV_TRUE: if intr is pending
    NV_FALSE: otherwise
*/
typedef NvBool (*NvUvmIsFaultInterruptPending_t)(NvU32* intrReg);

/******************************************************************************
    NvUvmSetFaultIntrBit_t
        This function is used to both enable and disable the gpu fault intr.
        Setting 1 in the PMC_INTR_EN_SET register enables the fault intr
        Setting 1 in the PMC_INTR_EN_CLEAR register disables the fault intr

    Arguments:
        intrReg: (INPUT)
            Bar0 mapping to PMC_INTR_EN_SET register.
*/
typedef void (*NvUvmSetFaultIntrBit_t)(NvU32* intrReg);

/******************************************************************************
    NvUvmWriteFaultBufferPacket_t
        This function is needed for fake fault injection

    Arguments:
        pEntry: (INPUT)
            Fault buffer entry address which needs to be written to

        data: (INPUT)
            Data to be filled in the fault buffer entry. Size of data is equal 
            to the fault buffer entry size

    Returns:
        NV_OK: if the operation succeeds
        NV_ERR_INVALID_ARGUMENT: if any input argument in data is invalid
*/
typedef NV_STATUS (*NvUvmWriteFaultBufferPacket_t)(UvmFaultBufferEntry *pEntry,
        NvU8 *data);

/******************************************************************************
    NvUvmControlPrefetch_t
        This function controls the rate of prefetch accesses showing up in the
        fault buffer.

    Arguments:
        prefetchCtrlReg: (INPUT)
            The register offset to control the prefetch rate.

        throttleRate: (INPUT)
            The rate at which the prefetches need to be throttled into the
            fault buffer. Allow 1 prefetch in n^throttleRate.
            n is a chip specific constant. Generally 16.
*/
typedef void (*NvUvmControlPrefetch_t)(volatile NvU32* prefetchCtrlReg,
                                       UvmPrefetchThrottleRate throttleRate);
/******************************************************************************
    NvUvmTestFaultBufferOverflow_t
        This function will test if the fault overflow bit is set in the fault buffer

    Arguments:
        faultBufferInfoReg: (INPUT)
            Fault Buffer operations structure.

*/
typedef NvBool (*NvUvmTestFaultBufferOverflow_t)(UvmFaultBufferRegisters gpuBar0faultBuffer);


/******************************************************************************
    NvUvmClearFaultBufferOverflow_t
        This function resets the fault buffer overflow bit in the fault buffer.

    Arguments:
        faultBufferInfoReg: (INPUT)
            The Fault buffer operations structure.

*/
typedef void (*NvUvmClearFaultBufferOverflow_t)(UvmFaultBufferRegisters gpuBar0faultBuffer);



//
// These function pointers are initialized during gpu initialization
//
typedef struct
{
    NvUvmParseFaultBufferEntry_t        parseFaultBufferEntry;
    NvUvmSetFaultBufferEntryValid_t     setFaultBufferEntryValid;
    NvUvmIsFaultBufferEntryValid_t      isFaultBufferEntryValid;
    NvUvmSetReplayParamsReg_t           setReplayParamsReg;
    NvUvmGetFaultPacketSize_t           getFaultPacketSize;
    NvUvmWriteFaultBufferPacket_t       writeFaultBufferPacket;
    NvUvmIsFaultInterruptPending_t      isFaultIntrPending;
    NvUvmSetFaultIntrBit_t              setFaultIntrBit;
    NvUvmControlPrefetch_t              controlPrefetch;
    NvUvmTestFaultBufferOverflow_t      testFaultBufferOverflow;
    NvUvmClearFaultBufferOverflow_t     clearFaultBufferOverflow;
} UvmFaultBufferOps;

/******************************************************************************
    uvmfull_fault_buffer_init
        Initialze fault buffer management related function pointers for a 
        fault class

    Arguments:
        faultBufferClass: (INPUT)
            fault class for the current gpu

        faultBufferOps: (INPUT/OUTPUT)
            function pointers to be initialized
    Returns:
        NV_OK: if the operation succeeds
        NV_ERR_NOT_SUPPORTED: if the input class is invalid/not supported

*/
NV_STATUS uvmfull_fault_buffer_init(NvU32 faultBufferClass,
        UvmFaultBufferOps *faultBufferOps);

#endif // _UVM_FULL_FAULT_BUFFER_H_
