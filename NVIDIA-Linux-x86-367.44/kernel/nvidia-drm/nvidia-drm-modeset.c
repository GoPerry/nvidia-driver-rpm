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
#include "nvidia-drm-modeset.h"
#include "nvidia-drm-crtc.h"
#include "nvidia-drm-utils.h"
#include "nvidia-drm-fb.h"
#include "nvidia-drm-connector.h"
#include "nvidia-drm-encoder.h"
#include "nvidia-drm-os-interface.h"

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>

/*
 * In kernel versions before the addition of
 * drm_crtc_state::connectors_changed, connector changes were
 * reflected in drm_crtc_state::mode_changed.
 */
static inline bool
nvidia_drm_crtc_state_connectors_changed(struct drm_crtc_state *crtc_state)
{
#if defined(NV_DRM_CRTC_STATE_HAS_CONNECTORS_CHANGED)
    return crtc_state->connectors_changed;
#else
    return crtc_state->mode_changed;
#endif
}

inline static bool
nvidia_drm_atomic_crtc_needs_modeset(struct drm_crtc_state *crtc_state)
{
    return nvidia_drm_crtc_state_connectors_changed(crtc_state) ||
           crtc_state->planes_changed ||
           crtc_state->mode_changed;
}

static int drm_atomic_state_to_nvkms_requested_config(
    struct drm_atomic_state *state,
    struct NvKmsKapiRequestedModeSetConfig *requested_config)
{
    struct nvidia_drm_device *nv_dev = state->dev->dev_private;

    struct drm_crtc *crtc;
    struct drm_crtc_state *crtc_state;

    struct drm_plane *plane;
    struct drm_plane_state *plane_state;

    int i, ret = 0;

    memset(requested_config, 0, sizeof(*requested_config));

    /*
     * Validate state object for modeset changes, this detect changes
     * happened in state.
     */

    ret = drm_atomic_helper_check(state->dev, state);

    if (ret != 0)
    {
        NV_DRM_DEV_LOG_DEBUG(
            nv_dev,
            "drm_atomic_helper_check_modeset() failed");
        return ret;
    }

    /* Loops over all crtcs and fill head configuration for changes */

    for_each_crtc_in_state(state, crtc, crtc_state, i)
    {
        struct nvidia_drm_crtc *nv_crtc;
        unsigned int plane;

        struct NvKmsKapiHeadRequestedConfig *head_requested_config;

        /* Is this crtc is enabled and has anything changed? */

        if (!nvidia_drm_atomic_crtc_needs_modeset(crtc_state))
        {
            continue;
        }

        nv_crtc = DRM_CRTC_TO_NV_CRTC(crtc);

        requested_config->headsMask |= 1 << nv_crtc->head;

        head_requested_config =
            &requested_config->headRequestedConfig[nv_crtc->head];

        /* Copy present configuration */

        head_requested_config->modeSetConfig = nv_crtc->modeset_config;

        for (plane = 0; plane < ARRAY_SIZE(nv_crtc->plane_config); plane++) {
            struct NvKmsKapiPlaneConfig *plane_config =
                &head_requested_config->planeRequestedConfig[plane].config;

            *plane_config = nv_crtc->plane_config[plane];
        }

        /* Set mode-timing changes */

        if (crtc_state->mode_changed)
        {
            drm_mode_to_nvkms_display_mode(
                &crtc_state->mode,
                &head_requested_config->modeSetConfig.mode);

            head_requested_config->flags.modeChanged = NV_TRUE;
        }

        /* Set display changes */

        if (nvidia_drm_crtc_state_connectors_changed(crtc_state))
        {
            struct NvKmsKapiHeadModeSetConfig *head_modeset_config =
                &head_requested_config->modeSetConfig;

            struct drm_connector *connector;
            struct drm_connector_state *connector_state;

            int j;

            head_modeset_config->numDisplays = 0;

            memset(head_modeset_config->displays,
                   0,
                   sizeof(head_modeset_config->displays));

            head_requested_config->flags.displaysChanged = NV_TRUE;

            for_each_connector_in_state(state, connector, connector_state, j)
            {
                struct nvidia_drm_connector *nv_connector;
                struct nvidia_drm_encoder *nv_encoder;

                if (connector_state->crtc != crtc)
                {
                    continue;
                }

                nv_connector = DRM_CONNECTOR_TO_NV_CONNECTOR(connector);
                nv_encoder = nv_connector->nv_detected_encoder;

                if (nv_encoder == NULL)
                {
                    NV_DRM_DEV_LOG_DEBUG(
                        nv_dev,
                        "Connector(%u) has no connected encoder",
                        nv_connector->physicalIndex);
                    return -EINVAL;
                }

                head_modeset_config->displays[0] = nv_encoder->hDisplay;
                head_modeset_config->numDisplays = 1;
                break;
            }
        }
    }

    /* Loops over all planes and fill plane configuration for changes */

    for_each_plane_in_state(state, plane, plane_state, i)
    {
        struct NvKmsKapiHeadRequestedConfig *head_requested_config;

        struct NvKmsKapiPlaneRequestedConfig *plane_requested_config;
        struct NvKmsKapiPlaneConfig *plane_config;

        struct NvKmsKapiPlaneConfig old_plane_config;
        NvKmsKapiPlaneType type;
        NvU32 head;

        bool disable = false;

        if (!drm_plane_type_to_nvkms_plane_type(plane->type, &type)) {
            NV_DRM_DEV_LOG_DEBUG(
                nv_dev,
                "Unsupported drm plane type 0x%08x",
                plane->type);
            continue;
        }

        if (plane_state->crtc == NULL)
        {
            /*
             * Happens when the plane is being disabled.  If the plane was
             * previously enabled, disable it.  Otherwise, ignore this
             * plane.
             */

            if (plane->state->crtc)
            {
                struct nvidia_drm_crtc *nv_crtc =
                    DRM_CRTC_TO_NV_CRTC(plane->state->crtc);

                head = nv_crtc->head;

                disable = true;
            }
            else
            {
                continue;
            }
        }
        else
        {
            struct nvidia_drm_crtc *nv_crtc =
                DRM_CRTC_TO_NV_CRTC(plane_state->crtc);

            head = nv_crtc->head;
        }

        BUG_ON((requested_config->headsMask & (1 << head)) == 0x0);

        head_requested_config = &requested_config->headRequestedConfig[head];

        plane_requested_config =
            &head_requested_config->planeRequestedConfig[type];

        plane_config = &plane_requested_config->config;

        /* Save old configuration */

        old_plane_config = *plane_config;

        /* Disable plane if there is no display attached to crtc */

        if (head_requested_config->modeSetConfig.numDisplays == 0 || disable)
        {
            plane_config->surface = NULL;
        }
        else
        {
            struct nvidia_drm_framebuffer *nv_fb =
                DRM_FRAMEBUFFER_TO_NV_FRAMEBUFFER(plane_state->fb);

            if (nv_fb == NULL || nv_fb->pSurface == NULL)
            {
                NV_DRM_DEV_LOG_DEBUG(
                    nv_dev,
                    "Invalid framebuffer object 0x%p",
                    nv_fb);
                return -EINVAL;
            }

            plane_config->surface = nv_fb->pSurface;
        }

        /* Source values are 16.16 fixed point */

        plane_config->srcX = plane_state->src_x >> 16;
        plane_config->srcY = plane_state->src_y >> 16;
        plane_config->srcWidth  = plane_state->src_w >> 16;
        plane_config->srcHeight = plane_state->src_h >> 16;

        plane_config->dstX = plane_state->crtc_x;
        plane_config->dstY = plane_state->crtc_y;
        plane_config->dstWidth  = plane_state->crtc_w;
        plane_config->dstHeight = plane_state->crtc_h;

        /*
         * If plane surface remains NULL then ignore all other changes
         * because there is nothing to show.
         */
        if (old_plane_config.surface == NULL &&
            old_plane_config.surface == plane_config->surface) {
            continue;
        }

        if (old_plane_config.surface == NULL &&
            old_plane_config.surface != plane_config->surface) {
            plane_requested_config->flags.surfaceChanged = NV_TRUE;
            plane_requested_config->flags.srcXYChanged = NV_TRUE;
            plane_requested_config->flags.srcWHChanged = NV_TRUE;
            plane_requested_config->flags.dstXYChanged = NV_TRUE;
            plane_requested_config->flags.dstWHChanged = NV_TRUE;
            continue;
        }

        if (old_plane_config.surface != plane_config->surface) {
            plane_requested_config->flags.surfaceChanged = NV_TRUE;
        }

        if (old_plane_config.srcX != plane_config->srcX ||
            old_plane_config.srcY != plane_config->srcY) {
            plane_requested_config->flags.srcXYChanged = NV_TRUE;
        }

        if (old_plane_config.srcWidth != plane_config->srcWidth ||
            old_plane_config.srcHeight != plane_config->srcHeight) {
            plane_requested_config->flags.srcWHChanged = NV_TRUE;
        }

        if (old_plane_config.dstX != plane_config->dstX ||
            old_plane_config.dstY != plane_config->dstY) {
            plane_requested_config->flags.dstXYChanged = NV_TRUE;
        }

        if (old_plane_config.dstWidth != plane_config->dstWidth ||
            old_plane_config.dstHeight != plane_config->dstHeight) {
            plane_requested_config->flags.dstWHChanged = NV_TRUE;
        }
    }

    return 0;
}


int nvidia_drm_atomic_check(struct drm_device *dev,
                            struct drm_atomic_state *state)
{
    struct nvidia_drm_device *nv_dev = dev->dev_private;

    struct NvKmsKapiRequestedModeSetConfig *requested_config;

    int ret = 0;

    requested_config = nvidia_drm_calloc(1, sizeof(*requested_config));

    if (requested_config == NULL)
    {
        return -ENOMEM;
    }

    ret = drm_atomic_state_to_nvkms_requested_config(state, requested_config);

    if (ret != 0)
    {
        goto done;
    }

    if (!nvKms->applyModeSetConfig(nv_dev->pDevice,
                                   requested_config, NV_FALSE))
    {
        ret = -EINVAL;

        NV_DRM_DEV_LOG_DEBUG(
            nv_dev,
            "Failed to validate NvKmsKapiModeSetConfig");
    }

done:

    nvidia_drm_free(requested_config);

    return ret;
}

static void nvidia_drm_update_head_mode_config(
    const struct drm_atomic_state *state,
    const struct NvKmsKapiRequestedModeSetConfig *requested_config)
{
    unsigned int head;

    for (head = 0;
         head < ARRAY_SIZE(requested_config->headRequestedConfig); head++)
    {
        struct drm_crtc *crtc = NULL;
        struct drm_crtc_state *crtc_state = NULL;

        int i;

        if ((requested_config->headsMask & (1 << head)) == 0x0)
        {
            continue;
        }

        for_each_crtc_in_state(state, crtc, crtc_state, i)
        {
            struct nvidia_drm_crtc *nv_crtc = DRM_CRTC_TO_NV_CRTC(crtc);

            if (nv_crtc->head == head)
            {
                const struct NvKmsKapiHeadRequestedConfig
                    *head_requested_config =
                     &requested_config->headRequestedConfig[head];
                unsigned plane;

                nv_crtc->modeset_config =
                    head_requested_config->modeSetConfig;

                for (plane = 0;
                     plane < ARRAY_SIZE(head_requested_config->planeRequestedConfig);
                     plane++)
                {
                    nv_crtc->plane_config[plane] =
                        head_requested_config->
                            planeRequestedConfig[plane].config;
                }

                break;
            }
        }
    }
}

static bool nvidia_drm_has_pending_flip(struct drm_device *dev,
                                        struct drm_atomic_state *state)
{
    struct nvidia_drm_device *nv_dev =
        (struct nvidia_drm_device*)dev->dev_private;

    int i;
    struct drm_plane *plane;
    struct drm_plane_state *plane_state;

    for_each_plane_in_state(state, plane, plane_state, i) {
        NvBool has_pending_flip;
        NvKmsKapiPlaneType nv_plane;
        struct nvidia_drm_crtc *nv_crtc;
        struct drm_crtc *crtc;

        crtc = plane->state->crtc;
        if (crtc == NULL) {
            /*
             * Plane state is changing from active ---> disabled or
             * from disabled ---> active.
             */
            crtc = plane_state->crtc;
        }

        if (crtc == NULL) {
            continue;
        }

        if (!drm_plane_type_to_nvkms_plane_type(plane->type, &nv_plane)) {
            NV_DRM_DEV_LOG_ERR(
                nv_dev,
                "Unsupported drm plane type 0x%08x",
                plane->type);
            continue;
        }

        nv_crtc = DRM_CRTC_TO_NV_CRTC(crtc);

        if (!nvKms->getFlipPendingStatus(nv_dev->pDevice,
                                  nv_crtc->head, nv_plane, &has_pending_flip)) {
            NV_DRM_DEV_LOG_ERR(
                nv_dev,
                "->getFlipPendingStatus() failed for head = %u and "
                "plane = 0x%08x",
                nv_crtc->head,
                nv_plane);
            continue;

        }

        if (has_pending_flip) {
            return true;
        }
    }

    return false;
}

static void nvidia_drm_wait_pending_flip(struct drm_device *dev,
                                         struct drm_atomic_state *state)
{
    struct nvidia_drm_device *nv_dev =
        (struct nvidia_drm_device*)dev->dev_private;

    bool has_flip_complete = false;

    uint64_t timeout = nvidia_drm_get_time_usec() + 3000000 /* 3 seconds */;

    do {
        if (!nvidia_drm_has_pending_flip(dev, state)) {
            has_flip_complete = true;
            break;
        }
    } while (nvidia_drm_get_time_usec() < timeout);


    if (!has_flip_complete) {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Flip completion timeout occurred");
    }
}

static int nvidia_drm_wait_for_pending_commit(struct drm_crtc *crtc)
{
    struct nvidia_drm_crtc *nv_crtc = DRM_CRTC_TO_NV_CRTC(crtc);

    struct drm_device *dev = crtc->dev;
    struct nvidia_drm_device *nv_dev = dev->dev_private;

    if (wait_event_timeout(nv_dev->pending_commit_queue,
                           !atomic_read(&nv_crtc->has_pending_commit),
                           3 * HZ /* 3 second */) == 0) {
        return -EBUSY;
    }

    return 0;
}

struct nvidia_drm_atomic_commit_task {
    struct drm_device *dev;
    struct drm_atomic_state *state;
    bool async;

    struct NvKmsKapiRequestedModeSetConfig *requested_config;

    struct work_struct work;
};

#if (NV_INIT_WORK_ARGUMENT_COUNT == 2)
    #define NVIDIA_DRM_INIT_WORK(_work_struct, _proc) \
        INIT_WORK(_work_struct, _proc)
    #define NVIDIA_DRM_WORK_FUNC_ARG_T struct work_struct
#elif (NV_INIT_WORK_ARGUMENT_COUNT == 3)
    #define NVIDIA_DRM_INIT_WORK(_work_struct, _proc) \
        INIT_WORK(_work_struct, _proc, _work_struct)
    #define NVIDIA_DRM_WORK_FUNC_ARG_T void
#else
    #error "NV_INIT_WORK_ARGUMENT_COUNT value unrecognized!"
#endif

static void
nvidia_drm_atomic_commit_task_callback(NVIDIA_DRM_WORK_FUNC_ARG_T *arg)
{
    struct nvidia_drm_atomic_commit_task *nv_commit_task =
        container_of(arg, struct nvidia_drm_atomic_commit_task, work);

    struct drm_device *dev = nv_commit_task->dev;
    struct drm_atomic_state *state = nv_commit_task->state;
    bool async = nv_commit_task->async;

    struct nvidia_drm_device *nv_dev =
        (struct nvidia_drm_device*)dev->dev_private;

    struct NvKmsKapiRequestedModeSetConfig *requested_config =
        nv_commit_task->requested_config;

    int i;
    struct drm_crtc *crtc;
    struct drm_crtc_state *crtc_state;

    if (nvKms->systemInfo.bAllowWriteCombining) {
        /*
         * XXX This call is required only if dumb buffer is going
         * to be presented.
         */
         nvidia_drm_write_combine_flush();
    }

    if (!nvKms->applyModeSetConfig(nv_dev->pDevice,
                                   requested_config, NV_TRUE)) {
        NV_DRM_DEV_LOG_ERR(
            nv_dev,
            "Failed to commit NvKmsKapiModeSetConfig");
    }

    /*
     * Wait for flip completion if synchronous commit is requested.
     */
    if (!async) {
        nvidia_drm_wait_pending_flip(dev, state);
    }

    drm_atomic_helper_cleanup_planes(dev, state);

    for_each_crtc_in_state(state, crtc, crtc_state, i) {
        struct nvidia_drm_crtc *nv_crtc = DRM_CRTC_TO_NV_CRTC(crtc);
        atomic_set(&nv_crtc->has_pending_commit, false);
    }

    wake_up_all(&nv_dev->pending_commit_queue);

    drm_atomic_state_free(state);

    nvidia_drm_free(requested_config);

    nvidia_drm_free(nv_commit_task);
}

int nvidia_drm_atomic_commit(struct drm_device *dev,
                             struct drm_atomic_state *state, bool async)
{
    int ret = 0;

    int i;
    struct drm_crtc *crtc = NULL;
    struct drm_crtc_state *crtc_state = NULL;

    struct nvidia_drm_atomic_commit_task *nv_commit_task = NULL;

    struct NvKmsKapiRequestedModeSetConfig *requested_config = NULL;

    nv_commit_task = nvidia_drm_calloc(1, sizeof(*nv_commit_task));

    if (nv_commit_task == NULL) {
        ret = -ENOMEM;
        goto failed;
    }

    requested_config = nvidia_drm_calloc(1, sizeof(*requested_config));

    if (requested_config == NULL)
    {
        ret = -ENOMEM;
        goto failed;
    }

    ret = drm_atomic_state_to_nvkms_requested_config(state, requested_config);

    if (ret != 0)
    {
        NV_DRM_LOG_ERR("Failed to convert atomic state to NvKmsKapiModeSetConfig");
        goto failed;
    }

    /*
     * Wait for previous flips to complete if synchronous commit is requested.
     */
    if (!async) {
        nvidia_drm_wait_pending_flip(dev, state);
    }

    /*
     * DRM mandates to return EBUSY error if previous flip is not completed yet.
     *
     * DRM client has to listen DRM_MODE_PAGE_FLIP_EVENT otherwise
     * use synchronous ioctl.
     */

    if (nvidia_drm_has_pending_flip(dev, state)) {
        ret = -EBUSY;
        goto failed;
    }

    /*
     * Serialize commits on crtc, wait for pending commits to finish.
     */
    for_each_crtc_in_state(state, crtc, crtc_state, i) {
        ret = nvidia_drm_wait_for_pending_commit(crtc);

        if (ret != 0) {
            goto failed;
        }
    }

    ret = drm_atomic_helper_prepare_planes(dev, state);

    if (ret != 0)
    {
        goto failed;
    }

    drm_atomic_helper_swap_state(dev, state);

    nvidia_drm_update_head_mode_config(state, requested_config);

    NVIDIA_DRM_INIT_WORK(&nv_commit_task->work,
                         nvidia_drm_atomic_commit_task_callback);

    nv_commit_task->dev = dev;

    nv_commit_task->state = state;
    nv_commit_task->async = async;

    nv_commit_task->requested_config = requested_config;

    if (async)
    {
        schedule_work(&nv_commit_task->work);
    }
    else
    {
        nvidia_drm_atomic_commit_task_callback(&nv_commit_task->work);
    }

    return 0;

failed:

    nvidia_drm_free(requested_config);

    nvidia_drm_free(nv_commit_task);

    return ret;
}

void nvidia_drm_handle_flip_occurred(struct nvidia_drm_device *nv_dev,
                                     NvU32 head,
                                     NvKmsKapiPlaneType plane)
{
    BUG_ON(!mutex_is_locked(&nv_dev->lock));

    switch (plane)
    {
        case NVKMS_KAPI_PLANE_PRIMARY:
        {
            struct drm_device *dev = nv_dev->dev;

            struct drm_crtc *crtc = &nv_dev->nv_crtc[head]->base;
            struct drm_crtc_state *crtc_state = crtc->state;

            if (crtc_state->event != NULL) {
                spin_lock(&dev->event_lock);
                drm_crtc_send_vblank_event(crtc, crtc_state->event);
                spin_unlock(&dev->event_lock);
            }

            break;
        }

        case NVKMS_KAPI_PLANE_OVERLAY: /* TODO */
        case NVKMS_KAPI_PLANE_CURSOR:
        default:
            BUG_ON(1);
    }
}

int nvidia_drm_shut_down_all_crtcs(struct drm_device *dev)
{
    struct drm_atomic_state *state;

    struct drm_plane *plane;
    struct drm_connector *connector;
    struct drm_crtc *crtc;

    unsigned plane_mask;

    int ret = 0;

    state = drm_atomic_state_alloc(dev);

    if (state == NULL)
    {
        return -ENOMEM;
    }

    drm_modeset_lock_all(dev);

    state->acquire_ctx = dev->mode_config.acquire_ctx;

    plane_mask = 0;
    list_for_each_entry(plane, &dev->mode_config.plane_list, head) {
        struct drm_plane_state *plane_state =
            drm_atomic_get_plane_state(state, plane);

        if (IS_ERR(plane_state)) {
            ret = PTR_ERR(plane_state);
            goto done;
        }

        plane->old_fb = plane->fb;
        plane_mask |= 1 << drm_plane_index(plane);

        ret = drm_atomic_set_crtc_for_plane(plane_state, NULL);
        if (ret != 0) {
            goto done;
        }

        drm_atomic_set_fb_for_plane(plane_state, NULL);
    }

    list_for_each_entry(connector,
                        &dev->mode_config.connector_list, head) {
        struct drm_connector_state *connector_state =
            drm_atomic_get_connector_state(state, connector);

        if (IS_ERR(connector_state)) {
            ret = PTR_ERR(connector_state);
            goto done;
        }

        ret = drm_atomic_set_crtc_for_connector(connector_state, NULL);
        if (ret != 0) {
            goto done;
        }
    }

    list_for_each_entry(crtc, &dev->mode_config.crtc_list, head) {
        struct drm_crtc_state *crtc_state =
            drm_atomic_get_crtc_state(state, crtc);

        if (IS_ERR(crtc_state)) {
            ret = PTR_ERR(crtc_state);
            goto done;
        }

#if defined(NV_DRM_ATOMIC_SET_MODE_FOR_CRTC)
        ret = drm_atomic_set_mode_for_crtc(crtc_state, NULL);
        if (ret != 0) {
            goto done;
        }
#else
        memset(&crtc_state->mode, 0, sizeof(crtc_state->mode));
#endif

        crtc_state->active = crtc_state->enable = false;
    }

    ret = drm_atomic_commit(state);

done:

#if defined(NV_DRM_ATOMIC_CLEAN_OLD_FB)
    drm_atomic_clean_old_fb(dev, plane_mask, ret);
#else
    drm_for_each_plane_mask(plane, dev, plane_mask) {
        if (ret == 0) {
            if (plane->old_fb != NULL) {
                drm_framebuffer_unreference(plane->old_fb);
            }

            plane->fb = NULL;
        }

        plane->old_fb = NULL;
    }
#endif

    /*
     * If drm_atomic_commit() succeeds, it will free the state, and thus we
     * only need to free the state explicitly if we didn't successfully call
     * drm_atomic_commit().
     */
    if (ret != 0) {
        drm_atomic_state_free(state);
    }

    drm_modeset_unlock_all(dev);

    return ret;
}

#endif
