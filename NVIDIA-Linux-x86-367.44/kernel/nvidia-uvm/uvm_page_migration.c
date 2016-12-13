/*******************************************************************************
    Copyright (c) 2013 NVIDIA Corporation

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
#include "uvm_page_migration.h"
#include "uvm_page_migration_next.h"
#include "uvm_page_migration_kepler.h"
#include "uvm_page_migration_maxwell.h"
#include "uvm_page_migration_pascal.h"

#if defined(__linux__)
#include "uvm_linux.h"             // For NV_UVM_FENCE()
#endif
//
// Instead of including cla06f.h and cla0b5.h, copy the (immutable) two items
// that are required here. This is in order to avoid a conflict between the
// Linux kernel's definition of "BIT", and the same definition from NVIDIA's
// nvmisc.h.
//
#define KEPLER_DMA_COPY_A              (0x0000A0B5)
#define KEPLER_CHANNEL_GPFIFO_A        (0x0000A06F)
#define KEPLER_CHANNEL_GPFIFO_B        (0x0000A16F)
#define KEPLER_CHANNEL_GPFIFO_C        (0x0000A26F)
#define MAXWELL_DMA_COPY_A             (0x0000B0B5)
#define MAXWELL_CHANNEL_GPFIFO_A       (0x0000B06F)
#define PASCAL_DMA_COPY_A              (0x0000C0B5)
#define PASCAL_DMA_COPY_B              (0x0000C1B5)
#define PASCAL_CHANNEL_GPFIFO_A        (0x0000C06F)

void NvUvmChannelWriteGpPut(volatile unsigned *gpPut, unsigned index)
{
    NV_UVM_FENCE();
    *gpPut = index;
}

void NvUvmChannelQueueWork_Legacy(volatile unsigned *gpPut, unsigned index,
                                volatile unsigned *trigger, unsigned submitToken)
{
    NvUvmChannelWriteGpPut(gpPut, index);
}

static NV_STATUS _UvmCeHalInit(unsigned ceClass, UvmCopyOps *copyOps)
{
    // setup CE HALs
    switch (ceClass)
    {
        case KEPLER_DMA_COPY_A: 
            copyOps->launchDma = NvUvmCopyEngineLaunchDmaA0B5;
            copyOps->memset = NvUvmCopyEngineMemSetA0B5;
            copyOps->semaphoreRelease =
                NvUvmCopyEngineInsertSemaphoreReleaseA0B5;
            break;
        case MAXWELL_DMA_COPY_A: 
            copyOps->launchDma = NvUvmCopyEngineLaunchDmaB0B5;
            copyOps->memset = NvUvmCopyEngineMemSetB0B5;  
            copyOps->semaphoreRelease =
                NvUvmCopyEngineInsertSemaphoreReleaseB0B5;       
            break;
        case PASCAL_DMA_COPY_A:
        case PASCAL_DMA_COPY_B:
            copyOps->launchDma = NvUvmCopyEngineLaunchDmaC0B5;
            copyOps->memset = NvUvmCopyEngineMemSetC0B5;
            copyOps->semaphoreRelease =
                NvUvmCopyEngineInsertSemaphoreReleaseC0B5;
            break;
        default:
            return NV_ERR_NOT_SUPPORTED;
    }
    return NV_OK;
}

static NV_STATUS _UvmFifoHalInit(unsigned fifoClass, UvmCopyOps *copyOps)
{
    // setup FIFO HALs
    switch (fifoClass)
    {
        case KEPLER_CHANNEL_GPFIFO_A:
        case KEPLER_CHANNEL_GPFIFO_B:
        case KEPLER_CHANNEL_GPFIFO_C:
            copyOps->writeGpEntry = NvUvmChannelWriteGpEntryA06F;
            copyOps->semaphoreAcquire =
                       NvUvmCopyEngineInsertSemaphoreAcquireA06F;
            copyOps->semaphoreAcquire_GEQ =
                       NvUvmCopyEngineInsertSemaphoreAcquireGreaterEqualToA06F;
            copyOps->insertNop = NvUvmInsertNopA06F;
            copyOps->queueWork = NvUvmChannelQueueWork_Legacy;
            break;
        case MAXWELL_CHANNEL_GPFIFO_A:
            copyOps->writeGpEntry = NvUvmChannelWriteGpEntryB06F;
            copyOps->semaphoreAcquire =
                        NvUvmCopyEngineInsertSemaphoreAcquireB06F;
            copyOps->semaphoreAcquire_GEQ =
                       NvUvmCopyEngineInsertSemaphoreAcquireGreaterEqualToB06F;
            copyOps->insertNop = NvUvmInsertNopB06F;
            copyOps->queueWork = NvUvmChannelQueueWork_Legacy;
            break;
        case PASCAL_CHANNEL_GPFIFO_A:
            copyOps->writeGpEntry = NvUvmChannelWriteGpEntryC06F;
            copyOps->semaphoreAcquire =
                       NvUvmCopyEngineInsertSemaphoreAcquireC06F;
            copyOps->semaphoreAcquire_GEQ =
                       NvUvmCopyEngineInsertSemaphoreAcquireGreaterEqualToC06F;
            copyOps->insertNop = NvUvmInsertNopC06F;
            copyOps->queueWork = NvUvmChannelQueueWork_Legacy;
            break;
        default:
            return NV_ERR_NOT_SUPPORTED;
    }
    return NV_OK;
}

NV_STATUS NvUvmHalInit
    (unsigned ceClass, unsigned fifoClass, UvmCopyOps *copyOps)
{
    if (NV_OK == _UvmCeHalInit(ceClass, copyOps) &&
        NV_OK == _UvmFifoHalInit(fifoClass, copyOps))
        return NV_OK;
    else
        return NvUvmHalInit_Next(ceClass, fifoClass, copyOps);
}
