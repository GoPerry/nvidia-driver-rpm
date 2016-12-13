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

#ifndef _NVIDIA_FAULT_BUFFER_PASCAL_H_
#define _NVIDIA_FAULT_BUFFER_PASCAL_H

NV_STATUS uvmfull_parse_fault_buffer_hal_b069(NvU64 faultBufferAddress,
    NvU32 index, UvmFaultBufferEntry *bufferData);

void uvmfull_set_faultbuffer_entry_valid_hal_b069(NvU64 faultBufferAddress,
    NvU32 index, NvBool bInvalid);

NvBool uvmfull_is_faultbuffer_entry_valid_hal_b069(NvU64 faultBufferAddress,
    NvU32 index);

NV_STATUS uvmfull_set_reg_replay_params_hal_b069(volatile NvU32 *gpuBar0ReplayPtr,
    NvU32 gpcId, NvU32 clientId, NvU32 clientType, UvmReplayType replayType,
    NvBool bIsSysmem, NvU32 flags);

NV_STATUS uvmfull_write_fault_buffer_packet_b069(
    UvmFaultBufferEntry *pFaultBuffer, NvU8 *pFaultBufferb096);
NvU32 uvmfull_get_fault_packet_size_b069(void);
void uvmfull_set_hi_fault_interrupt_bit_b069(NvU32 *gpuBar0FaultIntrReg);
void uvmfull_set_lo_fault_interrupt_bit_b069(NvU32 *gpuBar0FaultIntrReg);
NvBool uvmfull_is_faultbuffer_interrupt_pending_b069(NvU32 *gpuBar0Fault);
void uvmfull_control_prefetch_b069(volatile NvU32 *gpuBar0PrefetchCtrlReg,
                                    UvmPrefetchThrottleRate throttleRate);
NvBool uvmfull_test_faultbuffer_overflow_hal_b069(UvmFaultBufferRegisters gpuBar0faultBufferInfo);
void uvmfull_clear_faultbuffer_overflow_hal_b069(UvmFaultBufferRegisters gpuBar0faultBufferInfo);

#endif
