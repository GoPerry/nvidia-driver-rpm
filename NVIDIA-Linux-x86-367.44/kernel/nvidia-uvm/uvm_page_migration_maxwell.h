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

#ifndef _NVIDIA_PAGE_MIGRATION_MAXWELL_H_
#define _NVIDIA_PAGE_MIGRATION_MAXWELL_H_

//
// B06F and B0B5 are respectively the copy engine and FIFO classes belonging to
// NVIDIA's "MAXWELL" GPU architecture. This interface implements a hardware
// abstraction layer for "MAXWELL".
//

#define NVB06F_NOP_MAX_SIZE                         ((1 << 13) - 1)

void NvUvmChannelWriteGpEntryB06F
    (unsigned long long *gpFifoEntries, unsigned index, unsigned long long bufferBase,
     NvLength bufferLength);

NvLength NvUvmCopyEngineLaunchDmaB0B5
    (unsigned **pbPut, unsigned *pbEnd, NvUPtr source, unsigned srcFlags,
     NvUPtr destination, unsigned dstFlags, NvLength size,
     unsigned launchFlags);

NvLength NvUvmCopyEngineInsertSemaphoreAcquireB06F
    (unsigned **pbPut, unsigned *pbEnd, UvmGpuPointer semaphoreGpuPointer,
     unsigned payload);


NvLength NvUvmCopyEngineInsertSemaphoreReleaseB0B5
    (unsigned **pbPut, unsigned *pbEnd, UvmGpuPointer semaphoreGpuPointer,
     unsigned payload);

NvLength NvUvmCopyEngineMemSetB0B5
    (unsigned **pbPut, unsigned *pbEnd, NvUPtr base,
     NvLength size, unsigned payload, unsigned flags);

NvLength NvUvmCopyEngineInsertSemaphoreAcquireGreaterEqualToB06F
    (unsigned **pPbPut, unsigned *pbEnd, UvmGpuPointer semaphoreGpuPointer,
     unsigned payload);

NvLength NvUvmInsertNopB06F
    (unsigned **pPbPut, unsigned *pbEnd, unsigned dwords);

#endif // _NVIDIA_PAGE_MIGRATION_MAXWELL_H_
