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

#include "uvm_full_fault_buffer.h"
#include "uvm_full_fault_buffer_pascal.h"

NV_STATUS uvmfull_fault_buffer_init(NvU32 faultBufferClass,
        UvmFaultBufferOps *faultBufferOps)
{
    // setup fault buffer class
    switch (faultBufferClass)
    {
        case MAXWELL_FAULT_BUFFER_A:
            faultBufferOps->parseFaultBufferEntry =
                    uvmfull_parse_fault_buffer_hal_b069;
            faultBufferOps->setFaultBufferEntryValid =
                    uvmfull_set_faultbuffer_entry_valid_hal_b069;
            faultBufferOps->isFaultBufferEntryValid =
                    uvmfull_is_faultbuffer_entry_valid_hal_b069;
            faultBufferOps->setReplayParamsReg =
                    uvmfull_set_reg_replay_params_hal_b069;
            faultBufferOps->getFaultPacketSize =
                    uvmfull_get_fault_packet_size_b069;
            faultBufferOps->writeFaultBufferPacket =
                    uvmfull_write_fault_buffer_packet_b069;
            faultBufferOps->isFaultIntrPending =
                    uvmfull_is_faultbuffer_interrupt_pending_b069;
            faultBufferOps->setFaultIntrBit =
                    uvmfull_set_hi_fault_interrupt_bit_b069;
            faultBufferOps->controlPrefetch =
                    uvmfull_control_prefetch_b069;
            faultBufferOps->testFaultBufferOverflow =
                    uvmfull_test_faultbuffer_overflow_hal_b069;
            faultBufferOps->clearFaultBufferOverflow =
                    uvmfull_clear_faultbuffer_overflow_hal_b069;
            break;
        default:
            return NV_ERR_NOT_SUPPORTED;
    }
    return NV_OK;
}
