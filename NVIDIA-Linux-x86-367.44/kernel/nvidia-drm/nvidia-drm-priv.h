/*
 * Copyright (c) 2015, NVIDIA CORPORATION. All rights reserved.
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
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef __NVIDIA_DRM_PRIV_H__
#define __NVIDIA_DRM_PRIV_H__

#include "conftest.h" /* NV_DRM_AVAILABLE */

#if defined(NV_DRM_AVAILABLE)

#include <drm/drmP.h>

#if defined(NV_DRM_DRM_GEM_H_PRESENT)
#include <drm/drm_gem.h>
#endif

#include "nvidia-drm-os-interface.h"

#include "nvkms-kapi.h"

#define NV_DRM_LOG_DEBUG(__fmt, ...) \
    DRM_DEBUG("[nvidia-drm] " __fmt "\n", ##__VA_ARGS__)

#define NV_DRM_LOG_ERR(__fmt, ...) \
    DRM_ERROR("[nvidia-drm] " __fmt "\n", ##__VA_ARGS__)

#define NV_DRM_LOG_INFO(__fmt, ...) \
    DRM_INFO("[nvidia-drm] " __fmt "\n", ##__VA_ARGS__)

#define NV_DRM_DEV_LOG_INFO(__dev, __fmt, ...) \
    NV_DRM_LOG_INFO("[GPU ID 0x%08x] " __fmt, __dev->gpu_info.gpu_id, ##__VA_ARGS__)

#define NV_DRM_DEV_LOG_ERR(__dev, __fmt, ...) \
    NV_DRM_LOG_ERR("[GPU ID 0x%08x] " __fmt, __dev->gpu_info.gpu_id, ##__VA_ARGS__)

#define NV_DRM_DEV_LOG_DEBUG(__dev, __fmt, ...) \
    NV_DRM_LOG_DEBUG("[GPU ID 0x%08x] " __fmt, __dev->gpu_info.gpu_id, ##__VA_ARGS__)

/*
 * drm_dev_free() was renamed to drm_dev_unref().  One or the other
 * should be present whenever NV_DRM_AVAILABLE is defined.
 */
#if defined(NV_DRM_DEV_UNREF_PRESENT)
  #define NV_DRM_DEV_FREE(__dev) drm_dev_unref(__dev)
#else
  #define NV_DRM_DEV_FREE(__dev) drm_dev_free(__dev)
#endif

#if defined(NV_DRM_HELPER_MODE_FILL_FB_STRUCT_HAS_CONST_MODE_CMD_ARG)
  #define NV_DRM_MODE_FB_CMD2_T const struct drm_mode_fb_cmd2
#else
  #define NV_DRM_MODE_FB_CMD2_T struct drm_mode_fb_cmd2
#endif

struct nvidia_drm_device
{
    nv_gpu_info_t gpu_info;

    struct drm_device *dev;


#if defined(NV_DRM_ATOMIC_MODESET_AVAILABLE)
    /*
     * Lock to protect drm-subsystem and fields of this structure
     * from concurrent access.
     *
     * Do not hold this lock if some lock from core drm-subsystem
     * is already held, locking order should be like this -
     *
     *   mutex_lock(nvidia_drm_device::lock);
     *     ....
     *     mutex_lock(drm_device::mode_config::lock);
     *     ....
     *     .......
     *     mutex_unlock(drm_device::mode_config::lock);
     *     ........
     *     ..
     *     mutex_lock(drm_device::struct_mutex);
     *     ....
     *     ........
     *     mutex_unlock(drm_device::struct_mutex);
     *     ..
     *   mutex_unlock(nvidia_drm_device::lock);
     */
    struct mutex lock;

    struct NvKmsKapiDevice *pDevice;
    NvU32 pitchAlignment;

    struct nvidia_drm_crtc *nv_crtc[NVKMS_KAPI_MAX_HEADS];

    atomic_t enable_event_handling;

    wait_queue_head_t pending_commit_queue;
#endif

    struct nvidia_drm_device *next;
};

static inline bool nvidia_drm_modeset_enabled(const struct drm_device *dev)
{
    return ((dev->driver->driver_features & DRIVER_MODESET) != 0);
}

extern const struct NvKmsKapiFunctionsTable* const nvKms;

#endif /* defined(NV_DRM_AVAILABLE) */

#endif /* __NVIDIA_DRM_PRIV_H__ */
