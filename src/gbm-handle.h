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

#ifndef GBM_HANDLE_H
#define GBM_HANDLE_H

#include <EGL/egl.h>
#include <stdbool.h>

typedef struct GbmObjectRec {
    void (*free)(struct GbmObjectRec *obj);
    struct GbmDisplayRec* dpy;
    EGLenum type;
    int refCount;
} GbmObject;

typedef const GbmObject* GbmHandle;

GbmHandle eGbmAddObject(GbmObject* obj);
GbmObject* eGbmRefHandle(GbmHandle handle);
void eGbmUnrefObject(GbmObject* obj);
bool eGbmUnrefHandle(GbmHandle handle);

#endif /* GBM_HANDLE_H */
