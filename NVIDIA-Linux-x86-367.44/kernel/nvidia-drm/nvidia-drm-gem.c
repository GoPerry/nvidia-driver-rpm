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

#include "conftest.h" /* NV_DRM_ATOMIC_MODESET_AVAILABLE */

#if defined(NV_DRM_AVAILABLE)

#include "nvidia-drm-priv.h"
#include "nvidia-drm-ioctl.h"
#include "nvidia-drm-gem.h"

static struct nvidia_drm_gem_object *nvidia_drm_gem_new
(
    struct drm_file *file_priv,
    struct drm_device *dev,
    enum nvidia_drm_gem_object_type type,
    const union nvidia_drm_gem_object_union *nv_gem_union,
    size_t size,
    uint32_t *handle
)
{
    struct nvidia_drm_device *nv_dev = dev->dev_private;

    struct nvidia_drm_gem_object *nv_gem = NULL;
    int ret = 0;

    /* Allocate memory for the gem object */

    nv_gem = nvidia_drm_calloc(1, sizeof(*nv_gem));

    if (nv_gem == NULL)
    {
        NV_DRM_DEV_LOG_ERR(nv_dev, "Failed to allocate gem object");
        return ERR_PTR(-ENOMEM);
    }

    nv_gem->type = type;
    nv_gem->u = *nv_gem_union;

    /* Initialize the gem object */

    drm_gem_private_object_init(dev, &nv_gem->base, size);

    /* Create handle */

    ret = drm_gem_handle_create(file_priv, &nv_gem->base, handle);

    if (ret != 0)
    {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to create handle for gem object");
        drm_gem_object_release(&nv_gem->base);
        nvidia_drm_free(nv_gem);
        return ERR_PTR(ret);
    }

    drm_gem_object_unreference_unlocked(&nv_gem->base);

    nv_gem->handle = *handle;

    NV_DRM_DEV_LOG_DEBUG(nv_dev, "Created buffer with GEM handle 0x%x", *handle);

    return nv_gem;
}

void nvidia_drm_gem_free(struct drm_gem_object *gem)
{
    struct drm_device *dev = gem->dev;

    struct nvidia_drm_gem_object *nv_gem =
                    DRM_GEM_OBJECT_TO_NV_GEM_OBJECT(gem);

    WARN_ON(!mutex_is_locked(&dev->struct_mutex));

    /* Cleanup core gem object */

    drm_gem_object_release(&nv_gem->base);

    switch (nv_gem->type)
    {
#if defined(NV_DRM_ATOMIC_MODESET_AVAILABLE)
        case NV_DRM_GEM_OBJECT_TYPE_NVKMS_MEMORY:
        {
            struct nvidia_drm_device *nv_dev = dev->dev_private;

            if (nv_gem->u.nvkms_memory.mapped) {
                nvKms->unmapMemory(nv_dev->pDevice,
                                   nv_gem->u.nvkms_memory.pMemory,
                                   nv_gem->u.nvkms_memory.pLinearAddress);
            }

            /* Free NvKmsKapiMemory handle associated with this gem object */

            nvKms->freeMemory(nv_dev->pDevice, nv_gem->u.nvkms_memory.pMemory);
            break;
        }
#endif
        case NV_DRM_GEM_OBJECT_TYPE_USERSPACE_MEMORY:
            nvidia_drm_unlock_user_pages(
                nv_gem->u.userspace_memory.pages_count,
                nv_gem->u.userspace_memory.pages);
            break;
    }

    /* Free gem */

    nvidia_drm_free(nv_gem);
}

int nvidia_drm_gem_import_userspace_memory(struct drm_device *dev,
                                           void *data,
                                           struct drm_file *file_priv)
{
    struct nvidia_drm_device *nv_dev = dev->dev_private;

    struct drm_nvidia_gem_import_userspace_memory_params *params = data;
    struct nvidia_drm_gem_object *nv_gem;
    union nvidia_drm_gem_object_union nv_gem_union = { };

    struct page **pages = NULL;
    unsigned long pages_count = 0;

    int ret = 0;
    uint32_t handle = 0;

    if ((params->size % PAGE_SIZE) != 0)
    {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Userspace memory 0x%llx size should be in a multiple of page "
            "size to create a gem object",
            params->address);
        return -EINVAL;
    }

    pages_count = params->size / PAGE_SIZE;

    ret = nvidia_drm_lock_user_pages(params->address, pages_count, &pages);

    if (ret != 0)
    {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to lock user pages for address 0x%llx: %d",
            params->address, ret);
        return ret;
    }

    nv_gem_union.userspace_memory.pages = pages;
    nv_gem_union.userspace_memory.pages_count = pages_count;

    nv_gem = nvidia_drm_gem_new(file_priv,
                                dev,
                                NV_DRM_GEM_OBJECT_TYPE_USERSPACE_MEMORY,
                                &nv_gem_union,
                                params->size,
                                &handle);
    if (IS_ERR(nv_gem))
    {
        ret = PTR_ERR(nv_gem);

        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to create gem object for userspace memory 0x%llx",
            params->address);
        goto failed;
    }

    params->handle = handle;

    return 0;

failed:

    nvidia_drm_unlock_user_pages(pages_count, pages);

    return ret;
}

struct dma_buf *nvidia_drm_gem_prime_export
(
    struct drm_device *dev,
    struct drm_gem_object *gem, int flags
)
{
    struct nvidia_drm_device *nv_dev = dev->dev_private;

    struct nvidia_drm_gem_object *nv_gem =
        DRM_GEM_OBJECT_TO_NV_GEM_OBJECT(gem);

    if (nv_gem->type != NV_DRM_GEM_OBJECT_TYPE_USERSPACE_MEMORY)
    {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Gem object 0x%p is not suitable to export", gem);
        return ERR_PTR(-EINVAL);
    }

    return drm_gem_prime_export(dev, gem, flags);
}

struct sg_table *nvidia_drm_gem_prime_get_sg_table(struct drm_gem_object *gem)
{
    struct nvidia_drm_gem_object *nv_gem =
        DRM_GEM_OBJECT_TO_NV_GEM_OBJECT(gem);

    if (nv_gem->type != NV_DRM_GEM_OBJECT_TYPE_USERSPACE_MEMORY)
    {
        return ERR_PTR(-EINVAL);
    }

    return drm_prime_pages_to_sg(nv_gem->u.userspace_memory.pages,
                                 nv_gem->u.userspace_memory.pages_count);
}

void *nvidia_drm_gem_prime_vmap(struct drm_gem_object *gem)
{
    struct nvidia_drm_gem_object *nv_gem =
        DRM_GEM_OBJECT_TO_NV_GEM_OBJECT(gem);

    if (nv_gem->type != NV_DRM_GEM_OBJECT_TYPE_USERSPACE_MEMORY)
    {
        return ERR_PTR(-EINVAL);
    }

    return nvidia_drm_vmap(nv_gem->u.userspace_memory.pages,
                           nv_gem->u.userspace_memory.pages_count);
}

void nvidia_drm_gem_prime_vunmap(struct drm_gem_object *gem, void *address)
{
    struct nvidia_drm_gem_object *nv_gem =
        DRM_GEM_OBJECT_TO_NV_GEM_OBJECT(gem);

    if (nv_gem->type != NV_DRM_GEM_OBJECT_TYPE_USERSPACE_MEMORY)
    {
        return;
    }

    nvidia_drm_vunmap(address);
}

#if defined(NV_DRM_ATOMIC_MODESET_AVAILABLE)

int nvidia_drm_dumb_create
(
    struct drm_file *file_priv,
    struct drm_device *dev, struct drm_mode_create_dumb *args
)
{
    struct nvidia_drm_device *nv_dev = dev->dev_private;
    struct nvidia_drm_gem_object *nv_gem;
    union nvidia_drm_gem_object_union nv_gem_union = { };
    int ret = 0;

    args->pitch = roundup(args->width * ((args->bpp + 7) >> 3),
                          nv_dev->pitchAlignment);

    args->size = args->height * args->pitch;

    /* Core DRM requires gem object size to be aligned with PAGE_SIZE */

    args->size = roundup(args->size, PAGE_SIZE);

    nv_gem_union.nvkms_memory.pMemory =
        nvKms->allocateMemory(nv_dev->pDevice, args->size);

    if (nv_gem_union.nvkms_memory.pMemory == NULL)
    {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to allocate NvKmsKapiMemory for dumb object of size %llu",
            args->size);
        return -ENOMEM;
    }

    if (!nvKms->mapMemory(nv_dev->pDevice,
                          nv_gem_union.nvkms_memory.pMemory,
                          &nv_gem_union.nvkms_memory.pLinearAddress))
    {
        ret = -ENOMEM;

        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to map NvKmsKapiMemory 0x%p",
            nv_gem_union.nvkms_memory.pMemory);
        goto failed;
    }

    nv_gem_union.nvkms_memory.mapped = true;

    nv_gem = nvidia_drm_gem_new(file_priv,
                                dev,
                                NV_DRM_GEM_OBJECT_TYPE_NVKMS_MEMORY,
                                &nv_gem_union,
                                args->size, &args->handle);

    if (IS_ERR(nv_gem))
    {
        ret = PTR_ERR(nv_gem);

        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to create gem object for NvKmsKapiMemory 0x%p",
            nv_gem_union.nvkms_memory.pMemory);
        goto failed;
    }

    return 0;

failed:

    if (nv_gem_union.nvkms_memory.mapped)
    {
        nvKms->unmapMemory(nv_dev->pDevice,
                           nv_gem_union.nvkms_memory.pMemory,
                           nv_gem_union.nvkms_memory.pLinearAddress);
        nv_gem_union.nvkms_memory.mapped = false;
    }

    nvKms->freeMemory(nv_dev->pDevice, nv_gem_union.nvkms_memory.pMemory);

    return ret;
}

int nvidia_drm_gem_import_nvkms_memory
(
    struct drm_device *dev,
    void *data,
    struct drm_file *file_priv
)
{
    struct nvidia_drm_device *nv_dev = dev->dev_private;
    struct drm_nvidia_gem_import_nvkms_memory_params *p = data;
    union nvidia_drm_gem_object_union nv_gem_union = { };
    struct nvidia_drm_gem_object *nv_gem;

    if (!nvidia_drm_modeset_enabled(dev))
    {
        return -EINVAL;
    }

    nv_gem_union.nvkms_memory.pMemory =
        nvKms->importMemory(nv_dev->pDevice,
                            p->mem_size,
                            p->nvkms_params_ptr,
                            p->nvkms_params_size);

    if (nv_gem_union.nvkms_memory.pMemory == NULL)
    {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to import NVKMS memory to GEM object");
        return -EINVAL;
    }

    nv_gem_union.nvkms_memory.pLinearAddress = NULL;
    nv_gem_union.nvkms_memory.mapped = false;

    nv_gem = nvidia_drm_gem_new(file_priv,
                                dev,
                                NV_DRM_GEM_OBJECT_TYPE_NVKMS_MEMORY,
                                &nv_gem_union,
                                p->mem_size,
                                &p->handle);
    if (IS_ERR(nv_gem))
    {
        nvKms->freeMemory(nv_dev->pDevice, nv_gem_union.nvkms_memory.pMemory);

        return PTR_ERR(nv_gem);
    }

    return 0;
}

int nvidia_drm_dumb_map_offset
(
    struct drm_file *file,
    struct drm_device *dev, uint32_t handle, uint64_t *offset
)
{
    struct nvidia_drm_device *nv_dev = dev->dev_private;

    struct drm_gem_object *gem;
    struct nvidia_drm_gem_object *nv_gem;

    int ret = -EINVAL;

    mutex_lock(&dev->struct_mutex);

    gem = nvidia_drm_gem_object_lookup(dev, file, handle);

    if (gem == NULL)
    {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to lookup gem object for mapping: 0x%08x", handle);
        goto unlock_struct_mutex;
    }

    nv_gem = DRM_GEM_OBJECT_TO_NV_GEM_OBJECT(gem);

    if (nv_gem->type != NV_DRM_GEM_OBJECT_TYPE_NVKMS_MEMORY)
    {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Invalid gem object type for mapping: 0x%08x", handle);
        goto unlock_gem_object;
    }

    if (!nv_gem->u.nvkms_memory.mapped)
    {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Invalid gem object for mapping: 0x%08x", handle);
        goto unlock_gem_object;
    }

    *offset = (uint64_t)(uintptr_t)nv_gem->u.nvkms_memory.pLinearAddress;

    ret = 0;

unlock_gem_object:
    drm_gem_object_unreference_unlocked(&nv_gem->base);

unlock_struct_mutex:
    mutex_unlock(&dev->struct_mutex);

    return ret;
}

#endif /* NV_DRM_ATOMIC_MODESET_AVAILABLE */

#endif /* NV_DRM_AVAILABLE */
