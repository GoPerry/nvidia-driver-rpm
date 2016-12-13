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

#ifndef __nvreplay_h__
#define __nvreplay_h__

#define NV_PFB_PRI_MMU_INVALIDATE                                   0x00100CBC /* RW-4R */

#define NV_PFB_PRI_MMU_INVALIDATE_ALL_VA                                   0:0 /* RWXVF */
#define NV_PFB_PRI_MMU_INVALIDATE_ALL_VA_FALSE                      0x00000000 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_ALL_VA_TRUE                       0x00000001 /* RW--V */

#define NV_PFB_PRI_MMU_INVALIDATE_ALL_PDB                                  1:1 /* RWXVF */
#define NV_PFB_PRI_MMU_INVALIDATE_ALL_PDB_FALSE                     0x00000000 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_ALL_PDB_TRUE                      0x00000001 /* RW--V */

#define NV_PFB_PRI_MMU_INVALIDATE_HUBTLB_ONLY                              2:2 /* RWXVF */
#define NV_PFB_PRI_MMU_INVALIDATE_HUBTLB_ONLY_FALSE                 0x00000000 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_HUBTLB_ONLY_TRUE                  0x00000001 /* RW--V */

#define NV_PFB_PRI_MMU_INVALIDATE_REPLAY                                   5:3 /* RWXVF */
#define NV_PFB_PRI_MMU_INVALIDATE_REPLAY_NONE                       0x00000000 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_REPLAY_START                      0x00000001 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_REPLAY_START_ACK_ALL              0x00000002 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_REPLAY_CANCEL_TARGETED            0x00000003 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_REPLAY_CANCEL_GLOBAL              0x00000004 /* RW--V */
// TODO bug 1226729 - remove this transitional alias once RM has moved to using CANCEL_GLOBAL
#define NV_PFB_PRI_MMU_INVALIDATE_REPLAY_CANCEL                     0x00000004 /*       */

#define NV_PFB_PRI_MMU_INVALIDATE_SYS_MEMBAR                              6:6 /* RWXVF */
#define NV_PFB_PRI_MMU_INVALIDATE_SYS_MEMBAR_FALSE                 0x00000000 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_SYS_MEMBAR_TRUE                  0x00000001 /* RW--V */

#define NV_PFB_PRI_MMU_INVALIDATE_ACK                                     8:7 /* RWXVF */
#define NV_PFB_PRI_MMU_INVALIDATE_ACK_NONE_REQUIRED                0x00000000 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_ACK_INTRANODE                    0x00000001 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_ACK_GLOBALLY                     0x00000002 /* RW--V */

#define NV_PFB_PRI_MMU_INVALIDATE_CANCEL_CLIENT_ID                       14:9 /* RWXVF */

#define NV_PFB_PRI_MMU_INVALIDATE_CANCEL_GPC_ID                         19:15 /* RWXVF */

#define NV_PFB_PRI_MMU_INVALIDATE_CANCEL_CLIENT_TYPE                    20:20 /* RWXVF */
#define NV_PFB_PRI_MMU_INVALIDATE_CANCEL_CLIENT_TYPE_GPC           0x00000000 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_CANCEL_CLIENT_TYPE_HUB           0x00000001 /* RW--V */

#define NV_PFB_PRI_MMU_INVALIDATE_CACHE_LEVEL                           26:24 /* RWXVF */
#define NV_PFB_PRI_MMU_INVALIDATE_CACHE_LEVEL_ALL                  0x00000000 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_CACHE_LEVEL_PTE_ONLY             0x00000001 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_CACHE_LEVEL_UP_TO_PDE0           0x00000002 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_CACHE_LEVEL_UP_TO_PDE1           0x00000003 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_CACHE_LEVEL_UP_TO_PDE2           0x00000004 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_CACHE_LEVEL_UP_TO_PDE3           0x00000005 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_CACHE_LEVEL_UP_TO_PDE4           0x00000006 /* RW--V */
#define NV_PFB_PRI_MMU_INVALIDATE_CACHE_LEVEL_UP_TO_PDE5           0x00000007 /* RW--V */

#define NV_PFB_PRI_MMU_INVALIDATE_TRIGGER                                31:31 /* -WEVF */
#define NV_PFB_PRI_MMU_INVALIDATE_TRIGGER_FALSE                     0x00000000 /* -WE-V */
#define NV_PFB_PRI_MMU_INVALIDATE_TRIGGER_TRUE                      0x00000001 /* -W--T */



#define NV_PFAULT_FAULT_TYPE_PDE                  0x00000000 /*       */
#define NV_PFAULT_FAULT_TYPE_PDE_SIZE             0x00000001 /*       */
#define NV_PFAULT_FAULT_TYPE_PTE                  0x00000002 /*       */
#define NV_PFAULT_FAULT_TYPE_VA_LIMIT_VIOLATION   0x00000003 /*       */
#define NV_PFAULT_FAULT_TYPE_UNBOUND_INST_BLOCK   0x00000004 /*       */
#define NV_PFAULT_FAULT_TYPE_PRIV_VIOLATION       0x00000005 /*       */
#define NV_PFAULT_FAULT_TYPE_RO_VIOLATION         0x00000006 /*       */
#define NV_PFAULT_FAULT_TYPE_PITCH_MASK_VIOLATION 0x00000008 /*       */
#define NV_PFAULT_FAULT_TYPE_WORK_CREATION        0x00000009 /*       */
#define NV_PFAULT_FAULT_TYPE_UNSUPPORTED_APERTURE 0x0000000a /*       */
#define NV_PFAULT_FAULT_TYPE_COMPRESSION_FAILURE  0x0000000b /*       */
#define NV_PFAULT_FAULT_TYPE_UNSUPPORTED_KIND     0x0000000c /*       */
#define NV_PFAULT_FAULT_TYPE_REGION_VIOLATION     0x0000000d /*       */
#define NV_PFAULT_FAULT_TYPE_POISONED             0x0000000e /*       */
#define NV_PFAULT_FAULT_TYPE_ATOMIC_VIOLATION     0x0000000f /*       */

#define NV_PFAULT_ACCESS_TYPE_READ       0x00000000 /*       */
#define NV_PFAULT_ACCESS_TYPE_WRITE      0x00000001 /*       */
#define NV_PFAULT_ACCESS_TYPE_ATOMIC     0x00000002 /*       */
#define NV_PFAULT_ACCESS_TYPE_PREFETCH   0x00000003 /*       */

#define NV_PFAULT_MMU_CLIENT_TYPE_GPC    0x00000000 /*       */
#define NV_PFAULT_MMU_CLIENT_TYPE_HUB    0x00000001 /*       */

#define NV_PMC_INTR_REPLAYABLE_FAULT                            9:9 /* R--VF */
#define NV_PMC_INTR_REPLAYABLE_FAULT_NOT_PENDING         0x00000000 /* R---V */
#define NV_PMC_INTR_REPLAYABLE_FAULT_PENDING             0x00000001 /* R---V */

#define NV_PFB_PRI_MMU_PAGE_FAULT_CTRL_PRF_FILTER                          1:0 /* RWEVF */
#define NV_PFB_PRI_MMU_PAGE_FAULT_CTRL_PRF_FILTER_SEND_ALL          0x00000000 /* RWE-V */
#define NV_PFB_PRI_MMU_PAGE_FAULT_CTRL_PRF_FILTER_SEND_NONE         0x00000003 /* RW--V */

#define NV_PFIFO_REPLAYABLE_FAULT_BUFFER_INFO_OVERFLOW                    0:0 /* RWIVF */
#define NV_PFIFO_REPLAYABLE_FAULT_BUFFER_INFO_OVERFLOW_FALSE              0x0 /* R-I-V */
#define NV_PFIFO_REPLAYABLE_FAULT_BUFFER_INFO_OVERFLOW_TRUE               0x1 /* R---V */
#define NV_PFIFO_REPLAYABLE_FAULT_BUFFER_INFO_OVERFLOW_CLR                0x1 /* -W--V */

#endif //__nvreplay_h__
