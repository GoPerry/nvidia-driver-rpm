/*******************************************************************************
    Copyright (c) 2016 NVIDIA Corporation

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

#ifndef __UVM8_NEXT_DECL_H__
#define __UVM8_NEXT_DECL_H__

#include "uvm_common.h"
#include "uvm8_forward_decl.h"

#if (UVM_IS_NEXT())

NV_STATUS uvm_next_add_gpu(uvm_gpu_t *gpu);
void uvm_next_remove_gpu(uvm_gpu_t *gpu);
NV_STATUS uvm_next_service_fault_batch(uvm_gpu_t *gpu, uvm_fault_service_batch_context_t *batch_context);
bool uvm_next_is_hmm_compatible(void);
NV_STATUS uvm_next_cancel_faults_precise(uvm_gpu_t *gpu);
bool uvm_hal_fault_buffer_class_supports_next_faults(NvU32 fault_buffer_class);
void uvm_hal_print_next_fault_entry_fields(uvm_fault_buffer_entry_t *entry);
NV_STATUS uvm_gpu_init_next_faults(uvm_gpu_t *gpu);
void uvm_gpu_deinit_next_faults(uvm_gpu_t *gpu);
void uvm_hal_fault_entry_init_next_fields(uvm_fault_buffer_entry_t *entry);

#else

static NV_STATUS uvm_next_add_gpu(uvm_gpu_t *gpu)
{
    return NV_OK;
}

static NV_STATUS uvm_next_remove_gpu(uvm_gpu_t *gpu)
{
    return NV_OK;
}

static NV_STATUS uvm_next_service_fault_batch(uvm_gpu_t *gpu, uvm_fault_service_batch_context_t *batch_context)
{
    return NV_ERR_NOT_SUPPORTED;
}

static bool uvm_next_is_hmm_compatible(void)
{
    return true;
}

static NV_STATUS uvm_next_cancel_faults_precise(uvm_gpu_t *gpu)
{
    return NV_ERR_NOT_SUPPORTED;
}

static void uvm_hal_print_next_fault_entry_fields(uvm_fault_buffer_entry_t *entry)
{
}

static bool uvm_hal_fault_buffer_class_supports_next_faults(NvU32 fault_buffer_class)
{
    return false;
}

static NV_STATUS uvm_gpu_init_next_faults(uvm_gpu_t *gpu)
{
    return NV_OK;
}

static void uvm_gpu_deinit_next_faults(uvm_gpu_t *gpu)
{
}

static void uvm_hal_fault_entry_init_next_fields(uvm_fault_buffer_entry_t *entry)
{
}

#endif

#endif // __UVM8_NEXT_DECL_H__
