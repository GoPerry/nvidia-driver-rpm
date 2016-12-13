/* _NVRM_COPYRIGHT_BEGIN_
 *
 * Copyright 2015 by NVIDIA Corporation.  All rights reserved.  All
 * information contained herein is proprietary and confidential to NVIDIA
 * Corporation.  Any use, reproduction, or disclosure without the written
 * permission of NVIDIA Corporation is prohibited.
 *
 * _NVRM_COPYRIGHT_END_
 */

#if !defined(__NVKMS_KAPI_USER_H__)
#define __NVKMS_KAPI_USER_H__

#include "nvtypes.h"

struct NvKmsImportMemoryParams {
    NvU32 hClient;
    NvU32 hMemory;
};

struct NvKmsSurfaceParams {
    struct {
        NvU32 x;
        NvU32 y;
        NvU32 z;
    } log2GobsPerBlock;

    NvU32 vrrTopPadding;
};

#endif /* !defined(__NVKMS_KAPI_USER_H__) */
