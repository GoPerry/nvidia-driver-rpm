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

#ifndef _UVM_MMU_MGMT_PASCAL_H_
#define _UVM_MMU_MGMT_PASCAL_H_
#include "uvm_mmu_mgmt.h"

NvLength NvUvmMmuTlbInvalidateC06F(unsigned **pbPut, unsigned *pbEnd,
                                  NvU64 targetPdb,
                                  UvmTlbInvalidatePdbAperture targetPdbAperture, 
                                  UvmTlbInvalidateMemOpsParams *pMemOpsParams);

NvLength NvUvmMmuMembarC06F(unsigned **pbPut, unsigned *pbEnd,
                            UvmTlbInvalidateMembarType membarType);

NvLength NvUvmHostWfiC06F(unsigned **pbPut, unsigned *pbEnd);

NvLength NvUvmFaultCancelSwMethodC06F(unsigned **pPbPut, unsigned *pbEnd, 
    unsigned gpcId, unsigned clientId, NvU64 instancePointer, UvmFaultApperture aperture);

#endif //_UVM_MMU_MGMT_PASCAL_H_

