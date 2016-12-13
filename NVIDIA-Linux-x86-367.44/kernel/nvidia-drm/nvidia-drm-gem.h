/*
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
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

#ifndef __NVIDIA_DRM_GEM_H__
#define __NVIDIA_DRM_GEM_H__

#include "conftest.h"

#if defined(NV_DRM_AVAILABLE)

#include <drm/drmP.h>
#include "nvkms-kapi.h"

enum nvidia_drm_gem_object_type
{
#if defined(NV_DRM_ATOMIC_MODESET_AVAILABLE)
    NV_DRM_GEM_OBJECT_TYPE_NVKMS_MEMORY     = 1,
#endif
    NV_DRM_GEM_OBJECT_TYPE_USERSPACE_MEMORY = 2,
};

struct nvidia_drm_gem_object
{
    uint32_t handle;

    struct drm_gem_object base;
    enum nvidia_drm_gem_object_type type;

    union nvidia_drm_gem_object_union
    {
#if defined(NV_DRM_ATOMIC_MODESET_AVAILABLE)
        struct
        {
            struct NvKmsKapiMemory *pMemory;
            void *pLinearAddress;
            /*
             * Whether pLinearAddress is valid (0 is technically a valid
             * physical address, so we cannot rely on pLinearAddress==NULL
             * checks.
             */
            bool mapped;
        } nvkms_memory;
#endif
        struct
        {
            struct page **pages;
            unsigned long pages_count;
        } userspace_memory;
    } u;
};

#define DRM_GEM_OBJECT_TO_NV_GEM_OBJECT(__gem) \
    container_of(__gem, struct nvidia_drm_gem_object, base)

void nvidia_drm_gem_free(struct drm_gem_object *gem);

int nvidia_drm_gem_import_userspace_memory
(
    struct drm_device *dev,
    void *data,
    struct drm_file *file_priv
);

struct dma_buf *nvidia_drm_gem_prime_export
(
    struct drm_device *dev,
    struct drm_gem_object *gem, int flags
);

static inline struct drm_gem_object *nvidia_drm_gem_object_lookup(
    struct drm_device *dev,
    struct drm_file *filp,
    u32 handle)
{
    #if defined(NV_DRM_GEM_OBJECT_LOOKUP_PRESENT)
        #if (NV_DRM_GEM_OBJECT_LOOKUP_ARGUMENT_COUNT == 3)
            return drm_gem_object_lookup(dev, filp, handle);
        #elif (NV_DRM_GEM_OBJECT_LOOKUP_ARGUMENT_COUNT == 2)
            return drm_gem_object_lookup(filp, handle);
        #else
            #error "Unknow arguments count of drm_gem_object_lookup()"
        #endif
    #else
        #error "drm_gem_object_lookup() is not defined"
    #endif
}

struct sg_table *nvidia_drm_gem_prime_get_sg_table(struct drm_gem_object *gem);

void *nvidia_drm_gem_prime_vmap(struct drm_gem_object *gem);

void nvidia_drm_gem_prime_vunmap(struct drm_gem_object *gem, void *address);

#if defined(NV_DRM_ATOMIC_MODESET_AVAILABLE)

int nvidia_drm_dumb_create
(
    struct drm_file *file_priv,
    struct drm_device *dev, struct drm_mode_create_dumb *args
);

int nvidia_drm_gem_import_nvkms_memory
(
    struct drm_device *dev,
    void *data,
    struct drm_file *file_priv
);

int nvidia_drm_dumb_map_offset
(
    struct drm_file *file,
    struct drm_device *dev, uint32_t handle, uint64_t *offset
);

#endif /* NV_DRM_ATOMIC_MODESET_AVAILABLE */

#endif /* NV_DRM_AVAILABLE */

#endif /* __NVIDIA_DRM_GEM_H__ */
