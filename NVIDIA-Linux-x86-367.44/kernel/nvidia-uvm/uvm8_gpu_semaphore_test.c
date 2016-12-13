/*******************************************************************************
    Copyright (c) 2015 NVIDIA Corporation

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

#include "uvm8_global.h"
#include "uvm8_gpu_semaphore.h"
#include "uvm8_test.h"
#include "uvm8_va_space.h"
#include "uvm8_kvmalloc.h"

static NV_STATUS add_and_test(uvm_gpu_tracking_semaphore_t *tracking_sem, NvU32 increment_by)
{
    NvU64 new_value;
    NvU64 completed = uvm_gpu_tracking_semaphore_update_completed_value(tracking_sem);
    new_value = completed + increment_by;
    tracking_sem->queued_value = new_value;

    TEST_CHECK_RET(uvm_gpu_tracking_semaphore_update_completed_value(tracking_sem) == completed);
    TEST_CHECK_RET(uvm_gpu_tracking_semaphore_is_value_completed(tracking_sem, 0));
    if (completed > 0)
        TEST_CHECK_RET(uvm_gpu_tracking_semaphore_is_value_completed(tracking_sem, completed - 1));
    TEST_CHECK_RET(uvm_gpu_tracking_semaphore_is_value_completed(tracking_sem, completed));
    TEST_CHECK_RET(!uvm_gpu_tracking_semaphore_is_value_completed(tracking_sem, completed + 1));
    TEST_CHECK_RET(!uvm_gpu_tracking_semaphore_is_value_completed(tracking_sem, new_value));
    TEST_CHECK_RET(!uvm_gpu_tracking_semaphore_is_completed(tracking_sem));

    uvm_gpu_semaphore_set_payload(&tracking_sem->semaphore, (NvU32)new_value);
    TEST_CHECK_RET(uvm_gpu_tracking_semaphore_update_completed_value(tracking_sem) == new_value);
    TEST_CHECK_RET(uvm_gpu_tracking_semaphore_is_value_completed(tracking_sem, completed));
    TEST_CHECK_RET(uvm_gpu_tracking_semaphore_is_value_completed(tracking_sem, new_value));
    TEST_CHECK_RET(uvm_gpu_tracking_semaphore_is_value_completed(tracking_sem, new_value - 1));
    TEST_CHECK_RET(!uvm_gpu_tracking_semaphore_is_value_completed(tracking_sem, new_value + 1));
    TEST_CHECK_RET(uvm_gpu_tracking_semaphore_is_completed(tracking_sem));

    return NV_OK;
}

NV_STATUS test_tracking(uvm_va_space_t *va_space)
{
    NV_STATUS status;
    uvm_gpu_tracking_semaphore_t tracking_sem;
    int i;
    uvm_gpu_t *gpu = uvm_processor_mask_find_first_gpu(&va_space->registered_gpus);

    if (gpu == NULL)
        return NV_ERR_INVALID_STATE;

    status = uvm_gpu_tracking_semaphore_alloc(gpu->semaphore_pool, &tracking_sem);
    if (status != NV_OK)
        return status;

    status = add_and_test(&tracking_sem, 1);
    if (status != NV_OK)
        goto done;

    for (i = 0; i < 100; ++i) {
        status = add_and_test(&tracking_sem, UINT_MAX - 1);
        if (status != NV_OK)
            goto done;
    }

done:
    uvm_gpu_tracking_semaphore_free(&tracking_sem);
    return status;
}

#define NUM_SEMAPHORES_PER_GPU 4096

static NV_STATUS test_alloc(uvm_va_space_t *va_space)
{
    NV_STATUS status = NV_OK;
    uvm_gpu_t *gpu;
    uvm_gpu_semaphore_t *semaphores;
    int i;
    NvU32 semaphore_count;
    NvU32 gpu_count = uvm_processor_mask_get_gpu_count(&va_space->registered_gpus);
    NvU32 current_semaphore = 0;

    if (gpu_count == 0)
        return NV_ERR_INVALID_STATE;

    semaphore_count = gpu_count * NUM_SEMAPHORES_PER_GPU;

    semaphores = uvm_kvmalloc_zero(semaphore_count * sizeof(*semaphores));
    if (semaphores == NULL)
        return NV_ERR_NO_MEMORY;

    for (i = 0; i < NUM_SEMAPHORES_PER_GPU; ++i) {
        for_each_va_space_gpu(gpu, va_space) {
            status = uvm_gpu_semaphore_alloc(gpu->semaphore_pool, &semaphores[current_semaphore++]);
            if (status != NV_OK)
                goto done;
        }
    }

    for (i = 0; i < current_semaphore; ++i) {
        for_each_va_space_gpu(gpu, va_space) {
            NvU64 gpu_va = uvm_gpu_semaphore_get_gpu_va(&semaphores[i], gpu);
            if (gpu_va == 0) {
                status = NV_ERR_INVALID_STATE;
                goto done;
            }
            uvm_gpu_semaphore_set_payload(&semaphores[i], 1);
            if (uvm_gpu_semaphore_get_payload(&semaphores[i]) != 1) {
                status = NV_ERR_INVALID_STATE;
                goto done;
            }
        }
    }

done:
    for (i = 0; i < current_semaphore; ++i)
        uvm_gpu_semaphore_free(&semaphores[i]);

    uvm_kvfree(semaphores);

    return status;
}


NV_STATUS uvm8_test_gpu_semaphore_sanity(UVM_TEST_GPU_SEMAPHORE_SANITY_PARAMS *params, struct file *filp)
{
    NV_STATUS status;
    uvm_va_space_t *va_space = uvm_va_space_get(filp);

    uvm_mutex_lock(&g_uvm_global.global_lock);
    uvm_va_space_down_read_rm(va_space);

    status = test_alloc(va_space);
    if (status != NV_OK)
        goto done;

    status = test_tracking(va_space);
    if (status != NV_OK)
        goto done;

done:
    uvm_va_space_up_read_rm(va_space);
    uvm_mutex_unlock(&g_uvm_global.global_lock);

    return status;
}
