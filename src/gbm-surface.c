/*
 * Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
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

#include "gbm-surface.h"
#include "gbm-display.h"
#include "gbm-utils.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <EGL/eglext.h>
#include <gbmint.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof(a[0]))

#define MAX_STREAM_IMAGES 10

typedef struct GbmSurfaceRec {
    GbmObject base;
    EGLStreamKHR stream;
    EGLSurface egl;
    struct {
        EGLImage image;
        struct gbm_bo *bo;
        bool locked;
    } images[MAX_STREAM_IMAGES];
    bool freeImages;
} GbmSurface;

/*
 * Returns a pointer to a pointer in the NV-private structure that wraps the
 * gbm_surface structure. This pointer is reserved for use by this library.
 */
static inline GbmSurface**
GetPrivPtr(struct gbm_surface* s)
{
    uint8_t *ptr = (uint8_t *)s;

    return (GbmSurface **)(ptr - sizeof(void*));
}

static inline GbmSurface*
GetSurf(struct gbm_surface* s)
{
    return s ? *GetPrivPtr(s) : NULL;
}

static inline void
SetSurf(struct gbm_surface* s, GbmSurface *surf)
{
    GbmSurface **priv = GetPrivPtr(s);

    *priv = surf;
}

static bool
AddSurfImage(GbmDisplay* display, GbmSurface* surf)
{
    GbmPlatformData* data = display->data;
    unsigned int i;

    for (i = 0; i < ARRAY_LEN(surf->images); i++) {
        if (surf->images[i].image == EGL_NO_IMAGE_KHR &&
            surf->images[i].bo == NULL) {
            surf->images[i].image =
                data->egl.CreateImageKHR(display->devDpy,
                                         EGL_NO_CONTEXT,
                                         EGL_STREAM_CONSUMER_IMAGE_NV,
                                         (EGLClientBuffer)surf->stream,
                                         NULL);
            if (surf->images[i].image == EGL_NO_IMAGE_KHR) break;

            return true;
        }
    }

    return false;
}

static void
RemoveSurfImage(GbmDisplay* display, GbmSurface* surf, EGLImage img)
{
    GbmPlatformData* data = display->data;
    int i;

    for (i = 0; i < ARRAY_LEN(surf->images); i++) {
        if (surf->images[i].image == img) {
            data->egl.DestroyImageKHR(display->devDpy, img);
            surf->images[i].image = EGL_NO_IMAGE_KHR;
            if (!surf->images[i].locked && surf->images[i].bo) {
                gbm_bo_destroy(surf->images[i].bo);
                surf->images[i].bo = NULL;
            }
            break;
        }
    }
}

static bool
PumpSurfEvents(GbmDisplay* display, GbmSurface* surf)
{
    GbmPlatformData* data = display->data;
    EGLenum event;
    EGLAttrib aux;

    /*
     * The image available event does not clear when queried, so it will
     * always be received in the loop below if a frame is still available.
     */
    surf->freeImages = false;

    do {
        EGLint evStatus = data->egl.QueryStreamConsumerEventNV(display->devDpy,
                                                               surf->stream,
                                                               0,
                                                               &event,
                                                               &aux);

        if (evStatus != EGL_TRUE) break;

        switch (event) {
        case EGL_STREAM_IMAGE_AVAILABLE_NV:
            surf->freeImages = true;
            break;
        case EGL_STREAM_IMAGE_ADD_NV:
            if (!AddSurfImage(display, surf)) return false;
            break;

        case EGL_STREAM_IMAGE_REMOVE_NV:
            RemoveSurfImage(display, surf, (EGLImage)aux);
            break;

        default:
            assert(!"Unhandled EGLImage stream consumer event");

        }
    /*
     * XXX Relies on knowledge of NV driver internals: As noted above, this
     * event is not drained by querying it, so this loop will run forever if
     * it waits for all available events to drain. Luckily, it also happens to
     * be generated after any available EGL_STREAM_IMAGE_AVAILABLE_NV events
     * by the driver, so it can be used as a sentinal for now.
     */
    } while (!surf->freeImages);

    return true;
}

int
eGbmSurfaceHasFreeBuffers(struct gbm_surface* s)
{
    GbmSurface* surf = GetSurf(s);

    if (!surf) return 0;

    if (surf->freeImages) return 1;

    if (!PumpSurfEvents(surf->base.dpy, surf)) return 0;

    return surf->freeImages;
}

struct gbm_bo*
eGbmSurfaceLockFrontBuffer(struct gbm_surface* s)
{
    GbmSurface* surf = GetSurf(s);
    GbmPlatformData* data;
    EGLDisplay dpy;
    EGLImage img;
    struct gbm_bo** bo = NULL;
    unsigned int i;
    EGLBoolean res;

    if (!surf) return NULL;

    data = surf->base.dpy->data;
    dpy = surf->base.dpy->devDpy;

    /* Must pump events to ensure images are created before acquiring them */
    if (!PumpSurfEvents(surf->base.dpy, surf)) return NULL;

    /* XXX Pass in a reusable sync object and wait for it here? */
    res = data->egl.StreamAcquireImageNV(dpy,
                                         surf->stream,
                                         &img,
                                         EGL_NO_SYNC_KHR);

    if (!res) {
        /*
         * Match Mesa EGL dri2 platform behavior when no buffer is available
         * even though this function is not called from an EGL entry point
         */
        eGbmSetError(data, EGL_BAD_SURFACE);
        return NULL;
    }

    surf->freeImages = false;

    for (i = 0; i < ARRAY_LEN(surf->images); i++) {
        if (surf->images[i].image == img) {
            surf->images[i].locked = true;
            bo = &surf->images[i].bo;
            break;
        }
    }

    assert(bo);

    if (!*bo) {
        struct gbm_import_fd_modifier_data buf;
        uint64_t modifier;
        EGLint stride; /* XXX support planar formats */
        EGLint offset; /* XXX support planar formats */
        int format;
        int planes;
        int fd; /* XXX support planar separate memory objects */

        if (!data->egl.ExportDMABUFImageQueryMESA(dpy,
                                                  img,
                                                  &format,
                                                  &planes,
                                                  &modifier)) goto fail;

        assert(planes == 1); /* XXX support planar formats */

        if (!data->egl.ExportDMABUFImageMESA(dpy, img, &fd, &stride, &offset))
            goto fail;

        memset(&buf, 0, sizeof(buf));
        buf.width = s->v0.width;
        buf.height = s->v0.height;
        buf.format = s->v0.format;
        buf.num_fds = 1; /* XXX support planar separate memory objects */
        buf.fds[0] = fd;
        buf.strides[0] = stride;
        buf.offsets[0] = offset;
        buf.modifier = modifier;
        *bo = gbm_bo_import(surf->base.dpy->gbm,
                            GBM_BO_IMPORT_FD_MODIFIER,
                            &buf, 0);

        if (!*bo) goto fail;
    }

    return *bo;

fail:
    surf->images[i].locked = false;
    /* XXX Can this be called from outside an EGL entry point? */
    eGbmSetError(data, EGL_BAD_ALLOC);
    data->egl.StreamReleaseImageNV(dpy, surf->stream, img, EGL_NO_SYNC_KHR);

    return NULL;

}

void
eGbmSurfaceReleaseBuffer(struct gbm_surface* s, struct gbm_bo *bo)
{
    GbmSurface* surf = GetSurf(s);
    GbmDisplay* display;
    EGLImage img = EGL_NO_IMAGE_KHR;
    unsigned int i;

    if (!surf || !bo) return;

    display = surf->base.dpy;

    for (i = 0; i < ARRAY_LEN(surf->images); i++) {
        if (surf->images[i].bo == bo) {
            surf->images[i].locked = false;
            img = surf->images[i].image;

            if (!img) {
                /*
                 * The stream removed this image while it was locked. Free the
                 * buffer object associated with it as well.
                 */
                gbm_bo_destroy(surf->images[i].bo);
            }

            break;
        }
    }

    assert(img != EGL_NO_IMAGE_KHR);

    if (img != EGL_NO_IMAGE_KHR) {
        display->data->egl.StreamReleaseImageNV(display->devDpy,
                                                surf->stream,
                                                img,
                                                EGL_NO_SYNC_KHR);
    }
}

static void
FreeSurface(GbmObject* obj)
{
    if (obj) {
        GbmSurface* surf = (GbmSurface*)obj;
        GbmPlatformData* data = obj->dpy->data;
        EGLDisplay dpy = obj->dpy->devDpy;
        unsigned int i;

        for (i = 0; i < ARRAY_LEN(surf->images); i++) {
            if (surf->images[i].image != EGL_NO_IMAGE_KHR)
                data->egl.DestroyImageKHR(dpy, surf->images[i].image);

            if (surf->images[i].bo != NULL)
                gbm_bo_destroy(surf->images[i].bo);
        }

        if (surf->egl != EGL_NO_SURFACE)
            data->egl.DestroySurface(dpy, surf->egl);
        if (surf->stream != EGL_NO_STREAM_KHR)
            data->egl.DestroyStreamKHR(dpy, surf->stream);

        /* Drop reference to the display acquired at creation time */
        eGbmUnrefObject(&obj->dpy->base);

        free(obj);
    }
}

EGLSurface
eGbmCreatePlatformWindowSurfaceHook(EGLDisplay dpy,
                                    EGLConfig config,
                                    void* nativeWin,
                                    const EGLAttrib* attribs)
{
    GbmDisplay* display = (GbmDisplay*)eGbmRefHandle(dpy);
    GbmPlatformData* data;
    struct gbm_surface* s = nativeWin;
    GbmSurface* surf = NULL;
    EGLint surfType;
    EGLint err = EGL_SUCCESS;
    EGLBoolean res;
    const EGLint surfAttrs[] = {
        /* XXX Merge in relevant <attribs> here as well */
        EGL_WIDTH, s->v0.width,
        EGL_HEIGHT, s->v0.height,
        EGL_NONE
    };
    static const EGLint streamAttrs[] = {
        EGL_STREAM_FIFO_LENGTH_KHR, 2, /* One front, one back. */
        EGL_NONE
    };

    (void)attribs;

    if (!display) {
        /*  No platform data. Can't set error EGL_NO_DISPLAY */
        return EGL_NO_SURFACE;
    }

    data = display->data;
    dpy = display->devDpy;

    if (!s) {
        err = EGL_BAD_NATIVE_WINDOW;
        goto fail;
    }

    res = data->egl.GetConfigAttrib(dpy, config, EGL_SURFACE_TYPE, &surfType);

    if (!res || !(surfType & EGL_STREAM_BIT_KHR)) {
        err = EGL_BAD_CONFIG;
        goto fail;
    }

    surf = calloc(1, sizeof(*surf));

    if (!surf) {
        err = EGL_BAD_ALLOC;
        goto fail;
    }

    surf->base.dpy = display;
    surf->base.type = EGL_OBJECT_SURFACE_KHR;
    surf->base.refCount = 1;
    surf->base.free = FreeSurface;
    surf->stream = data->egl.CreateStreamKHR(dpy, streamAttrs);

    if (!surf->stream) goto fail;

    if (!data->egl.StreamImageConsumerConnectNV(dpy,
                                                surf->stream,
                                                s->v0.count,
                                                s->v0.modifiers,
                                                NULL)) {
        goto fail;
    }

    surf->egl = data->egl.CreateStreamProducerSurfaceKHR(dpy,
                                                         config,
                                                         surf->stream,
                                                         surfAttrs);

    if (!surf->egl) goto fail;

    if (!PumpSurfEvents(display, surf)) {
        err = EGL_BAD_ALLOC;
        goto fail;
    }

    /* The reference to the display object is retained by surf */
    if (!eGbmAddObject(&surf->base)) {
        err = EGL_BAD_ALLOC;
        goto fail;
    }

    SetSurf(s, surf);

    return (EGLSurface)surf;

fail:
    FreeSurface(&surf->base);

    eGbmSetError(display->data, err);

    return EGL_NO_SURFACE;
}

void*
eGbmSurfaceUnwrap(GbmObject* obj)
{
    return ((GbmSurface*)obj)->egl;
}

EGLBoolean
eGbmDestroySurfaceHook(EGLDisplay dpy, EGLSurface eglSurf)
{
    GbmDisplay* display = (GbmDisplay*)eGbmRefHandle(dpy);
    EGLBoolean ret = EGL_FALSE;

    if (!display) return ret;

    if (eGbmDestroyHandle(eglSurf)) ret = EGL_TRUE;

    eGbmUnrefObject(&display->base);

    return ret;
}
