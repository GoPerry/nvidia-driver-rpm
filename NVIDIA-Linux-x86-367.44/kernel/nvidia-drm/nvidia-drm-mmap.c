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

#if defined(NV_DRM_ATOMIC_MODESET_AVAILABLE)

#include "nvidia-drm-priv.h"
#include "nvidia-drm-mmap.h"
#include "nvidia-drm-gem.h"

static void nvidia_drm_vma_open(struct vm_area_struct *vma)
{
    struct drm_gem_object *gem = vma->vm_private_data;

    drm_gem_object_reference(gem);
}

static void nvidia_drm_vma_release(struct vm_area_struct *vma)
{
    struct drm_gem_object *gem = vma->vm_private_data;

    drm_gem_object_unreference_unlocked(gem);
}

static struct vm_operations_struct nv_drm_vma_ops = {
    .open  = nvidia_drm_vma_open,
    .close = nvidia_drm_vma_release,
};

int nvidia_drm_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct drm_file *file_priv = (struct drm_file*)filp->private_data;
    struct drm_device *dev = file_priv->minor->dev;

    struct nvidia_drm_device *nv_dev = dev->dev_private;

    u32 handle = 0;

    struct drm_gem_object *gem = NULL;
    struct nvidia_drm_gem_object *nv_gem = NULL;

    enum nvidia_drm_memory_cache_type cache_type;

    unsigned long vma_size = vma->vm_end - vma->vm_start;
    int ret = 0;

    if (!nvidia_drm_modeset_enabled(dev))
    {
        return -EINVAL;
    }

    mutex_lock(&dev->struct_mutex);

    /* Look up the GEM object based on the offset passed in vma->vm_pgoff */

    idr_for_each_entry(&file_priv->object_idr, gem, handle)
    {
        unsigned long offset;

        nv_gem = DRM_GEM_OBJECT_TO_NV_GEM_OBJECT(gem);

        if (nv_gem->type != NV_DRM_GEM_OBJECT_TYPE_NVKMS_MEMORY)
        {
            continue;
        }

        if (!nv_gem->u.nvkms_memory.mapped)
        {
            continue;
        }

        offset = (unsigned long)(uintptr_t)nv_gem->u.nvkms_memory.pLinearAddress;
        offset = offset >> PAGE_SHIFT;

        if (offset == vma->vm_pgoff)
        {
            break;
        }
    }

    if (gem == NULL)
    {
        ret = -EINVAL;

        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to lookup gem object for vm_pgoff=0x%lx",
            vma->vm_pgoff);
        goto unlock_and_return;
    }

    drm_gem_object_reference(gem);

    /* Check the caller has been granted access to the buffer object */

    if (!drm_vma_node_is_allowed(&gem->vma_node, filp))
    {
        ret = -EACCES;

        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Invalid access to gem object 0x%p", gem);
        goto failed;
    }

    /* Validate vma size */

    if (gem->size < vma_size)
    {
        ret = -EINVAL;

        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Trying to map gem object 0x%p on larger virtual memory region",
            gem);
        goto failed;
    }

    cache_type = NV_DRM_MEMORY_CACHE_TYPE_UNCACHED_WEAK;

    if (nvKms->systemInfo.bAllowWriteCombining) {
        cache_type = NV_DRM_MEMORY_CACHE_TYPE_WRITECOMBINED;
    }

    ret = nvidia_drm_encode_pgprot(cache_type, &vma->vm_page_prot);

    if (ret != 0) {
        NV_DRM_DEV_LOG_ERR(nv_dev, "Failed to encode pgprot");
        goto failed;
    }

    ret = nvidia_drm_remap_pfn_range(vma,
                                     vma->vm_start, vma->vm_pgoff, vma_size,
                                     vma->vm_page_prot);

    if (ret != 0)
    {
        NV_DRM_DEV_LOG_ERR(nv_dev, "Failed to mmap() gem object 0x%p", gem);
        goto failed;
    }

    vma->vm_flags |= VM_IO;
    vma->vm_private_data = gem;
    vma->vm_ops = &nv_drm_vma_ops;

    goto unlock_and_return;

failed:

    drm_gem_object_unreference_unlocked(gem);

unlock_and_return:

    mutex_unlock(&dev->struct_mutex);

    return ret;
}

#endif
