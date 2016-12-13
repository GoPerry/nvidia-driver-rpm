/*
 * Copyright (c) 2015-2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef _UAPI_NVIDIA_DRM_IOCTL_H_
#define _UAPI_NVIDIA_DRM_IOCTL_H_

#include <drm/drm.h>

#define DRM_NVIDIA_GEM_IMPORT_NVKMS_MEMORY          0x00
#define DRM_NVIDIA_ADD_NVKMS_FB                     0x01
#define DRM_NVIDIA_GEM_IMPORT_USERSPACE_MEMORY      0x02
#define DRM_NVIDIA_GET_DEV_INFO                     0x03
#define DRM_NVIDIA_MIGRATE_MODESET_OWNERSHIP        0x04

#define DRM_IOCTL_NVIDIA_GEM_IMPORT_NVKMS_MEMORY                           \
    DRM_IOWR((DRM_COMMAND_BASE + DRM_NVIDIA_GEM_IMPORT_NVKMS_MEMORY),      \
             struct drm_nvidia_gem_import_nvkms_memory_params)

#define DRM_IOCTL_NVIDIA_ADD_NVKMS_FB                                      \
    DRM_IOWR((DRM_COMMAND_BASE + DRM_NVIDIA_ADD_NVKMS_FB),                 \
             struct drm_nvidia_add_nvkms_fb_params)

#define DRM_IOCTL_NVIDIA_GEM_IMPORT_USERSPACE_MEMORY                       \
    DRM_IOWR((DRM_COMMAND_BASE + DRM_NVIDIA_GEM_IMPORT_USERSPACE_MEMORY),  \
             struct drm_nvidia_gem_import_userspace_memory_params)

#define DRM_IOCTL_NVIDIA_GET_DEV_INFO                                      \
    DRM_IOWR((DRM_COMMAND_BASE + DRM_NVIDIA_GET_DEV_INFO),                 \
             struct drm_nvidia_get_dev_info_params)

#define DRM_IOCTL_NVIDIA_MIGRATE_MODESET_OWNERSHIP                         \
    DRM_IOW((DRM_COMMAND_BASE + DRM_NVIDIA_MIGRATE_MODESET_OWNERSHIP),     \
            struct drm_nvidia_migrate_modeset_ownership_params)

struct drm_nvidia_gem_import_nvkms_memory_params {
    uint64_t mem_size;           /* IN */

    uint64_t nvkms_params_ptr;   /* IN */
    uint64_t nvkms_params_size;  /* IN */

    uint32_t handle;             /* OUT */

    uint32_t __pad;
};

struct drm_nvidia_add_nvkms_fb_params {
    uint64_t nvkms_params_ptr;   /* IN */
    uint64_t nvkms_params_size;  /* IN */

    /* This must be last, because its size varies between kernels */
    struct drm_mode_fb_cmd2 cmd; /* IN/OUT */
};

struct drm_nvidia_gem_import_userspace_memory_params {
    uint64_t size;               /* IN Size of memory in bytes */
    uint64_t address;            /* IN Virtual address of userspace memory */
    uint32_t handle;             /* OUT Handle to gem object */
};

struct drm_nvidia_get_dev_info_params {
    uint32_t gpu_id;             /* OUT */
    uint32_t primary_index;      /* OUT; the "card%d" value */
};

struct drm_nvidia_migrate_modeset_ownership_params {
    uint32_t nvKmsFd;            /* IN */
    uint32_t nvKmsDeviceHandle;  /* IN */
};

#endif /* _UAPI_NVIDIA_DRM_IOCTL_H_ */
