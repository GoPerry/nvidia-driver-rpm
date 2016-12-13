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

#ifndef __UVM8_HAL_PASCAL_FAULT_BUFFER_H__
#define __UVM8_HAL_PASCAL_FAULT_BUFFER_H__

// When NV_CHIP_TPCARB_NUM_TEX_PORTS is defined to 2, TPCn has TPCCSn, PEn, TEXp, and TEXq, where p=2*n and q=p+1.
// When NV_CHIP_TPCARB_NUM_TEX_PORTS is not defined or is defined to 1, TPCn has TPCCSn, PEn, TEXn.
//
// NV_PFAULT_CLIENT_GPC_LTP_UTLB_n and NV_PFAULT_CLIENT_GPC_RGG_UTLB enums can be ignored. These will never be reported in a
// fault message, and should never be used in an invalidate.
//
// There is 1 LTP uTLB per TPC. There are up to 5 TPCs per GPC. In Pascal 2 TEX/L1 units per TPC
// (NV_CHIP_TPCARB_NUM_TEX_PORTS = 2)
typedef enum {
    UVM_PASCAL_GPC_UTLB_ID_LTP0 = 0,
    UVM_PASCAL_GPC_UTLB_ID_LTP1 = 1,
    UVM_PASCAL_GPC_UTLB_ID_LTP2 = 2,
    UVM_PASCAL_GPC_UTLB_ID_LTP3 = 3,
    UVM_PASCAL_GPC_UTLB_ID_LTP4 = 4,
    UVM_PASCAL_GPC_UTLB_ID_RGG = 5,

    UVM_PASCAL_GPC_UTLB_COUNT,
} uvm_pascal_gpc_utlb_id_t;

#endif
