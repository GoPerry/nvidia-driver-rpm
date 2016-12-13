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
#include "nvidia-drm-ioctl.h"
#include "nvidia-drm-fb.h"
#include "nvidia-drm-utils.h"
#include "nvidia-drm-gem.h"

#include <drm/drm_crtc_helper.h>

static void nvidia_framebuffer_destroy(struct drm_framebuffer *fb)
{
    struct nvidia_drm_device *nv_dev = fb->dev->dev_private;

    struct nvidia_drm_framebuffer *nv_fb =
                        DRM_FRAMEBUFFER_TO_NV_FRAMEBUFFER(fb);


    /* Unreference gem object */

    drm_gem_object_unreference_unlocked(nv_fb->gem);

    /* Cleaup core framebuffer object */

    drm_framebuffer_cleanup(fb);

    /* Free NvKmsKapiSurface associated with this framebuffer object */

    nvKms->destroySurface(nv_dev->pDevice, nv_fb->pSurface);

    /* Free framebuffer */

    nvidia_drm_free(nv_fb);
}

static int nvidia_framebuffer_create_handle
(
    struct drm_framebuffer *fb,
    struct drm_file *file, unsigned int *handle
)
{
    struct nvidia_drm_framebuffer *nv_fb =
                    DRM_FRAMEBUFFER_TO_NV_FRAMEBUFFER(fb);

    return drm_gem_handle_create(file, nv_fb->gem, handle);
}

static struct drm_framebuffer_funcs nv_framebuffer_funcs = {
    .destroy       = nvidia_framebuffer_destroy,
    .create_handle = nvidia_framebuffer_create_handle,
};

static struct drm_framebuffer *internal_framebuffer_create
(
    struct drm_device *dev,
    struct drm_file *file, NV_DRM_MODE_FB_CMD2_T *cmd,
    uint64_t nvkms_params_ptr,
    uint64_t nvkms_params_size
)
{
    struct nvidia_drm_device *nv_dev = dev->dev_private;

    struct nvidia_drm_framebuffer *nv_fb;

    struct drm_gem_object *gem;
    struct nvidia_drm_gem_object *nv_gem;

    enum NvKmsSurfaceMemoryFormat format;

    int ret;

    NV_DRM_DEV_LOG_DEBUG(
        nv_dev,
        "Creating a framebuffer of dimensions %ux%u from gem handle 0x%08x",
        cmd->width, cmd->height,
        cmd->handles[0]);

    if (!drm_format_to_nvkms_format(cmd->pixel_format, &format))
    {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Unsupported drm pixel format 0x%08x", cmd->pixel_format);
        return ERR_PTR(-EINVAL);
    }

    /*
     * In case of planar formats, this ioctl allows up to 4 buffer objects with
     * offsets and pitches per plane.
     *
     * We don't support any planar format, pick up first buffer only.
     */

    gem = nvidia_drm_gem_object_lookup(dev, file, cmd->handles[0]);

    if (gem == NULL)
    {
        NV_DRM_DEV_LOG_ERR(nv_dev, "Failed to find gem object");
        return ERR_PTR(-ENOENT);
    }

    nv_gem = DRM_GEM_OBJECT_TO_NV_GEM_OBJECT(gem);

    if (nv_gem->type != NV_DRM_GEM_OBJECT_TYPE_NVKMS_MEMORY)
    {
        ret = -EINVAL;

        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Invalid gem object 0x%08x for framebuffer creation",
            cmd->handles[0]);
        goto failed_fb_create;
    }

    /* Allocate memory for the framebuffer object */

    nv_fb = nvidia_drm_calloc(1, sizeof(*nv_fb));

    if (nv_fb == NULL)
    {
        ret = -ENOMEM;

        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to allocate memory for framebuffer obejct");
        goto failed_fb_create;
    }

    nv_fb->gem = gem;

    /* Fill out framebuffer metadata from the userspace fb creation request */

    drm_helper_mode_fill_fb_struct(&nv_fb->base, cmd);

    /* Initialize the base framebuffer object and add it to drm subsystem */

    ret = drm_framebuffer_init(dev, &nv_fb->base, &nv_framebuffer_funcs);

    if (ret != 0)
    {
        NV_DRM_DEV_LOG_ERR(nv_dev, "Failed to initialize framebuffer object");
        goto failed_fb_init;
    }

    /* Create NvKmsKapiSurface */

    nv_fb->pSurface = nvKms->createSurface(
        nv_dev->pDevice, nv_gem->u.nvkms_memory.pMemory,
        format, nv_fb->base.width, nv_fb->base.height, nv_fb->base.pitches[0],
        nvkms_params_ptr, nvkms_params_size);

    if (nv_fb->pSurface == NULL) {
        ret = -EINVAL;

        NV_DRM_DEV_LOG_ERR(nv_dev, "Failed to create NvKmsKapiSurface");
        goto failed_nvkms_create_surface;
    }

    return &nv_fb->base;

failed_nvkms_create_surface:

    drm_framebuffer_cleanup(&nv_fb->base);

failed_fb_init:

    nvidia_drm_free(nv_fb);

failed_fb_create:

    drm_gem_object_unreference_unlocked(&nv_gem->base);

    return ERR_PTR(ret);
}

struct drm_framebuffer *nvidia_drm_framebuffer_create
(
    struct drm_device *dev,
    struct drm_file *file, NV_DRM_MODE_FB_CMD2_T *cmd
)
{
    return internal_framebuffer_create(dev, file, cmd, 0, 0);
}

int validate_drm_fb_params(struct drm_device *dev,
                           const struct drm_mode_fb_cmd2 *cmd)
{
    struct nvidia_drm_device *nv_dev = dev->dev_private;
    struct drm_mode_config *config = &dev->mode_config;
    unsigned int cpp;
    int i;

    if (cmd->flags != 0) {
        NV_DRM_DEV_LOG_ERR(nv_dev, "Bad framebuffer flags 0x%08x\n",
                           cmd->flags);
        return -EINVAL;
    }

    WARN_ON(config->min_width < 0);
    WARN_ON(config->max_width < 0);
    WARN_ON(config->min_height < 0);
    WARN_ON(config->max_height < 0);

    if (((unsigned)config->min_width > cmd->width) ||
        ((unsigned)config->max_width < cmd->width)) {
        NV_DRM_DEV_LOG_ERR(nv_dev,
                           "Bad framebuffer width %d, "
                           "should be in the range [%d, %d]\n",
                           cmd->width, config->min_width, config->max_width);
        return -EINVAL;
    }

    if (((unsigned)config->min_height > cmd->height) ||
        ((unsigned)config->max_height < cmd->height)) {
        NV_DRM_DEV_LOG_ERR(nv_dev,
                           "Bad framebuffer height %d, "
                           "should be in the range [%d, %d]\n",
                           cmd->height, config->min_height, config->max_height);
        return -EINVAL;
    }

    if (drm_format_num_planes(cmd->pixel_format) != 1) {
        NV_DRM_DEV_LOG_ERR(nv_dev, "Only single-plane formats supported\n");
        return -EINVAL;
    }

    if (!cmd->handles[0]) {
        NV_DRM_DEV_LOG_ERR(nv_dev, "No buffer object handle for plane 0\n");
        return -EINVAL;
    }

    cpp = drm_format_plane_cpp(cmd->pixel_format, 0);

    if ((uint64_t)cmd->width * cpp > UINT_MAX) {
        NV_DRM_DEV_LOG_ERR(nv_dev, "FB width(%u) * cpp(%u) overflows "
                           "uint32_t\n", cmd->width, cpp);
        return -ERANGE;
    }

    if ((uint64_t)cmd->height * cmd->pitches[0] + cmd->offsets[0] > UINT_MAX) {
        NV_DRM_DEV_LOG_ERR(nv_dev, "FB height(%u) * pitch(%u) + offset(%u) "
                           "overflows uint32_t\n",
                           cmd->height, cmd->pitches[0], cmd->offsets[0]);
        return -ERANGE;
    }

    if (cmd->modifier[0] != 0) {
        NV_DRM_DEV_LOG_ERR(nv_dev, "Invalid plane[0] modifier: 0x%08llx\n",
                           cmd->modifier[0]);
        return -EINVAL;
    }

    for (i = 1; i < 4; i++) {
        if (cmd->modifier[i]) {
            NV_DRM_DEV_LOG_ERR(nv_dev, "Non-zero modifier (0x%08llx) for unused plane "
                               "%d\n", cmd->modifier[i], i);
            return -EINVAL;
        }

        if (cmd->handles[i]) {
            NV_DRM_DEV_LOG_ERR(nv_dev,
                               "Non-zero GEM buffer handle for unused plane "
                               "%d\n", i);
            return -EINVAL;
        }

        if (cmd->pitches[i]) {
            NV_DRM_DEV_LOG_ERR(nv_dev,
                               "Non-zero pitch for unused plane %d\n",
                               i);
            return -EINVAL;
        }

        if (cmd->offsets[i]) {
            NV_DRM_DEV_LOG_ERR(nv_dev,
                               "Non-zero offset for unused plane %d\n",
                               i);
            return -EINVAL;
        }
    }

    return 0;
}

int nvidia_drm_add_nvkms_fb(struct drm_device *dev,
                            void *data, struct drm_file *file_priv)
{
    struct nvidia_drm_device *nv_dev = dev->dev_private;
    struct drm_nvidia_add_nvkms_fb_params *p = data;
    struct drm_framebuffer *fb;
    int status;

    if (!nvidia_drm_modeset_enabled(dev))
    {
        return -EINVAL;
    }

    status = validate_drm_fb_params(dev, &p->cmd);

    if (status != 0) {
        return status;
    }

    fb = internal_framebuffer_create(dev,
                                     file_priv,
                                     &p->cmd,
                                     p->nvkms_params_ptr,
                                     p->nvkms_params_size);

    if (IS_ERR(fb)) {
        return PTR_ERR(fb);
    }

    NV_DRM_DEV_LOG_DEBUG(nv_dev, "[FB:%d]\n", fb->base.id);
    mutex_lock(&file_priv->fbs_lock);
    p->cmd.fb_id = fb->base.id;
    list_add(&fb->filp_head, &file_priv->fbs);
    mutex_unlock(&file_priv->fbs_lock);

    return 0;
}

#endif
