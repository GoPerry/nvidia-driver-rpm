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

// Excerpt of //sw/dev/gpu_drv/chips_a/drivers/common/inc/hwref/pascal/gp100/dev_fault.h

#ifndef __dev_fault_h__
#define __dev_fault_h__

#define NV_PFAULT_FAULT_TYPE                             4:0 /*       */
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
#define NV_PFAULT_CLIENT                       14:8 /*       */
#define NV_PFAULT_CLIENT_GPC_L1_0        0x00000000 /*       */
#define NV_PFAULT_CLIENT_GPC_T1_0        0x00000001 /*       */
#define NV_PFAULT_CLIENT_GPC_PE_0        0x00000002 /*       */
#define NV_PFAULT_CLIENT_GPC_L1_1        0x00000003 /*       */
#define NV_PFAULT_CLIENT_GPC_T1_1        0x00000004 /*       */
#define NV_PFAULT_CLIENT_GPC_PE_1        0x00000005 /*       */
#define NV_PFAULT_CLIENT_GPC_L1_2        0x00000006 /*       */
#define NV_PFAULT_CLIENT_GPC_T1_2        0x00000007 /*       */
#define NV_PFAULT_CLIENT_GPC_PE_2        0x00000008 /*       */
#define NV_PFAULT_CLIENT_GPC_L1_3        0x00000009 /*       */
#define NV_PFAULT_CLIENT_GPC_T1_3        0x0000000A /*       */
#define NV_PFAULT_CLIENT_GPC_PE_3        0x0000000B /*       */
#define NV_PFAULT_CLIENT_GPC_RAST        0x0000000C /*       */
#define NV_PFAULT_CLIENT_GPC_GCC         0x0000000D /*       */
#define NV_PFAULT_CLIENT_GPC_GPCCS       0x0000000E /*       */
#define NV_PFAULT_CLIENT_GPC_PROP_0      0x0000000F /*       */
#define NV_PFAULT_CLIENT_GPC_PROP_1      0x00000010 /*       */
#define NV_PFAULT_CLIENT_GPC_PROP_2      0x00000011 /*       */
#define NV_PFAULT_CLIENT_GPC_PROP_3      0x00000012 /*       */
#define NV_PFAULT_CLIENT_GPC_L1_4        0x00000014 /*       */
#define NV_PFAULT_CLIENT_GPC_T1_4        0x00000015 /*       */
#define NV_PFAULT_CLIENT_GPC_PE_4        0x00000016 /*       */
#define NV_PFAULT_CLIENT_GPC_L1_5        0x00000017 /*       */
#define NV_PFAULT_CLIENT_GPC_T1_5        0x00000018 /*       */
#define NV_PFAULT_CLIENT_GPC_PE_5        0x00000019 /*       */
#define NV_PFAULT_CLIENT_GPC_L1_6        0x0000001A /*       */
#define NV_PFAULT_CLIENT_GPC_T1_6        0x0000001B /*       */
#define NV_PFAULT_CLIENT_GPC_PE_6        0x0000001C /*       */
#define NV_PFAULT_CLIENT_GPC_L1_7        0x0000001D /*       */
#define NV_PFAULT_CLIENT_GPC_T1_7        0x0000001E /*       */
#define NV_PFAULT_CLIENT_GPC_PE_7        0x0000001F /*       */
#define NV_PFAULT_CLIENT_GPC_L1_8        0x00000020 /*       */
#define NV_PFAULT_CLIENT_GPC_T1_8        0x00000021 /*       */
#define NV_PFAULT_CLIENT_GPC_PE_8        0x00000022 /*       */
#define NV_PFAULT_CLIENT_GPC_L1_9        0x00000023 /*       */
#define NV_PFAULT_CLIENT_GPC_T1_9        0x00000024 /*       */
#define NV_PFAULT_CLIENT_GPC_PE_9        0x00000025 /*       */
#define NV_PFAULT_CLIENT_GPC_L1_10       0x00000026 /*       */
#define NV_PFAULT_CLIENT_GPC_T1_10       0x00000027 /*       */
#define NV_PFAULT_CLIENT_GPC_PE_10       0x00000028 /*       */
#define NV_PFAULT_CLIENT_GPC_L1_11       0x00000029 /*       */
#define NV_PFAULT_CLIENT_GPC_T1_11       0x0000002A /*       */
#define NV_PFAULT_CLIENT_GPC_PE_11       0x0000002B /*       */
#define NV_PFAULT_CLIENT_GPC_TPCCS_0     0x00000030 /*       */
#define NV_PFAULT_CLIENT_GPC_TPCCS_1     0x00000031 /*       */
#define NV_PFAULT_CLIENT_GPC_TPCCS_2     0x00000032 /*       */
#define NV_PFAULT_CLIENT_GPC_TPCCS_3     0x00000033 /*       */
#define NV_PFAULT_CLIENT_GPC_TPCCS_4     0x00000034 /*       */
#define NV_PFAULT_CLIENT_GPC_TPCCS_5     0x00000035 /*       */
#define NV_PFAULT_CLIENT_GPC_TPCCS_6     0x00000036 /*       */
#define NV_PFAULT_CLIENT_GPC_TPCCS_7     0x00000037 /*       */
#define NV_PFAULT_CLIENT_GPC_TPCCS_8     0x00000038 /*       */
#define NV_PFAULT_CLIENT_GPC_TPCCS_9     0x00000039 /*       */
#define NV_PFAULT_CLIENT_GPC_TPCCS_10    0x0000003A /*       */
#define NV_PFAULT_CLIENT_GPC_TPCCS_11    0x0000003B /*       */
#define NV_PFAULT_CLIENT_GPC_GPM         0x00000013 /*       */
#define NV_PFAULT_CLIENT_GPC_LTP_UTLB_0  0x00000014 /*       */
#define NV_PFAULT_CLIENT_GPC_LTP_UTLB_1  0x00000015 /*       */
#define NV_PFAULT_CLIENT_GPC_LTP_UTLB_2  0x00000016 /*       */
#define NV_PFAULT_CLIENT_GPC_LTP_UTLB_3  0x00000017 /*       */
#define NV_PFAULT_CLIENT_GPC_RGG_UTLB    0x00000018 /*       */
#define NV_PFAULT_CLIENT_HUB_CE0         0x00000001 /*       */
#define NV_PFAULT_CLIENT_HUB_CE1         0x00000002 /*       */
#define NV_PFAULT_CLIENT_HUB_DNISO       0x00000003 /*       */
#define NV_PFAULT_CLIENT_HUB_FE          0x00000004 /*       */
#define NV_PFAULT_CLIENT_HUB_FECS        0x00000005 /*       */
#define NV_PFAULT_CLIENT_HUB_HOST        0x00000006 /*       */
#define NV_PFAULT_CLIENT_HUB_HOST_CPU    0x00000007 /*       */
#define NV_PFAULT_CLIENT_HUB_HOST_CPU_NB 0x00000008 /*       */
#define NV_PFAULT_CLIENT_HUB_ISO         0x00000009 /*       */
#define NV_PFAULT_CLIENT_HUB_MMU         0x0000000A /*       */
#define NV_PFAULT_CLIENT_HUB_NVDEC       0x0000000B /*       */
#define NV_PFAULT_CLIENT_HUB_NVENC1      0x0000000D /*       */
#define NV_PFAULT_CLIENT_HUB_NVENC2      0x00000033 /*       */
#define NV_PFAULT_CLIENT_HUB_NISO        0x0000000E /*       */
#define NV_PFAULT_CLIENT_HUB_P2P         0x0000000F /*       */
#define NV_PFAULT_CLIENT_HUB_PD          0x00000010 /*       */
#define NV_PFAULT_CLIENT_HUB_PERF        0x00000011 /*       */
#define NV_PFAULT_CLIENT_HUB_PMU         0x00000012 /*       */
#define NV_PFAULT_CLIENT_HUB_RASTERTWOD  0x00000013 /*       */
#define NV_PFAULT_CLIENT_HUB_SCC         0x00000014 /*       */
#define NV_PFAULT_CLIENT_HUB_SCC_NB      0x00000015 /*       */
#define NV_PFAULT_CLIENT_HUB_SEC         0x00000016 /*       */
#define NV_PFAULT_CLIENT_HUB_SSYNC       0x00000017 /*       */
#define NV_PFAULT_CLIENT_HUB_VIP         0x00000000 /*       */
#define NV_PFAULT_CLIENT_HUB_GRCOPY      0x00000018 /*       */
#define NV_PFAULT_CLIENT_HUB_CE2         0x00000018 /*       */
#define NV_PFAULT_CLIENT_HUB_XV          0x00000019 /*       */
#define NV_PFAULT_CLIENT_HUB_MMU_NB      0x0000001A /*       */
#define NV_PFAULT_CLIENT_HUB_NVENC       0x0000001B /*       */
#define NV_PFAULT_CLIENT_HUB_NVENC0      0x0000001B /*       */
#define NV_PFAULT_CLIENT_HUB_DFALCON     0x0000001C /*       */
#define NV_PFAULT_CLIENT_HUB_SKED        0x0000001D /*       */
#define NV_PFAULT_CLIENT_HUB_AFALCON     0x0000001E /*       */
#define NV_PFAULT_CLIENT_HUB_HSCE0       0x00000020 /*       */
#define NV_PFAULT_CLIENT_HUB_HSCE1       0x00000021 /*       */
#define NV_PFAULT_CLIENT_HUB_HSCE2       0x00000022 /*       */
#define NV_PFAULT_CLIENT_HUB_HSCE3       0x00000023 /*       */
#define NV_PFAULT_CLIENT_HUB_HSCE4       0x00000024 /*       */
#define NV_PFAULT_CLIENT_HUB_HSCE5       0x00000025 /*       */
#define NV_PFAULT_CLIENT_HUB_HSCE6       0x00000026 /*       */
#define NV_PFAULT_CLIENT_HUB_HSCE7       0x00000027 /*       */
#define NV_PFAULT_CLIENT_HUB_HSCE8       0x00000028 /*       */
#define NV_PFAULT_CLIENT_HUB_HSCE9       0x00000029 /*       */
#define NV_PFAULT_CLIENT_HUB_HSHUB       0x0000002A /*       */
#define NV_PFAULT_CLIENT_HUB_PTP_X0      0x0000002B /*       */
#define NV_PFAULT_CLIENT_HUB_PTP_X1      0x0000002C /*       */
#define NV_PFAULT_CLIENT_HUB_PTP_X2      0x0000002D /*       */
#define NV_PFAULT_CLIENT_HUB_PTP_X3      0x0000002E /*       */
#define NV_PFAULT_CLIENT_HUB_PTP_X4      0x0000002F /*       */
#define NV_PFAULT_CLIENT_HUB_PTP_X5      0x00000030 /*       */
#define NV_PFAULT_CLIENT_HUB_PTP_X6      0x00000031 /*       */
#define NV_PFAULT_CLIENT_HUB_PTP_X7      0x00000032 /*       */
#define NV_PFAULT_CLIENT_HUB_VPR_SCRUBBER0 0x00000034 /*       */
#define NV_PFAULT_CLIENT_HUB_VPR_SCRUBBER1 0x00000035 /*       */
#define NV_PFAULT_CLIENT_HUB_DONT_CARE   0x0000001F /*       */
#define NV_PFAULT_ACCESS_TYPE                 18:16 /*       */
#define NV_PFAULT_ACCESS_TYPE_READ       0x00000000 /*       */
#define NV_PFAULT_ACCESS_TYPE_WRITE      0x00000001 /*       */
#define NV_PFAULT_ACCESS_TYPE_ATOMIC     0x00000002 /*       */
#define NV_PFAULT_ACCESS_TYPE_PREFETCH   0x00000003 /*       */
#define NV_PFAULT_MMU_CLIENT_TYPE             20:20 /*       */
#define NV_PFAULT_MMU_CLIENT_TYPE_GPC    0x00000000 /*       */
#define NV_PFAULT_MMU_CLIENT_TYPE_HUB    0x00000001 /*       */
#define NV_PFAULT_GPC_ID                      28:24 /*       */

#endif // __dev_fault_h__
