/* SPDX-License-Identifier: MIT
 * asteroids: imports native buffers as EGLImages the way ArkWeb shows hardware-decoded video (EGL_NATIVE_BUFFER_OHOS
 * on an OHNativeWindowBuffer) and samples them in GLES: RGBA, then NV12 with and without the decoder's usage.
 * "native_eglimage_check [log]"; run with EGL_LOG_LEVEL=debug for Mesa's reasons. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <stdbool.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>
#include <stdio.h>

#ifndef EGL_NATIVE_BUFFER_OHOS
#define EGL_NATIVE_BUFFER_OHOS 0x34E1
#endif

static int Try(EGLDisplay d, const char *name, int format, uint64_t usage)
{
    OH_NativeBuffer_Config config = {.width = 640, .height = 360, .format = format, .usage = usage};
    OH_NativeBuffer *buffer = OH_NativeBuffer_Alloc(&config);
    if (!buffer) { printf("%s: alloc failed\n", name); return 1; }
    OHNativeWindowBuffer *native = OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer(buffer);
    PFNEGLCREATEIMAGEKHRPROC create = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR image = create(d, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_OHOS, native, attributes);
    int failed = image == EGL_NO_IMAGE_KHR;
    printf("%s: format %d usage 0x%llx -> image %s (EGL 0x%x)", name, format, (unsigned long long)usage,
           failed ? "FAILED" : "ok", eglGetError());
    if (!failed) {
        GLuint texture;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
        target(GL_TEXTURE_EXTERNAL_OES, image);
        printf(", external texture GL 0x%x", glGetError());
        glDeleteTextures(1, &texture);
        destroy(d, image);
    }
    printf("\n");
    OH_NativeWindow_DestroyNativeWindowBuffer(native);
    OH_NativeBuffer_Unreference(buffer);
    return failed;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !freopen(argv[1], "w", stdout)) return 2;
    setbuf(stdout, 0);
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(d, NULL, NULL)) { printf("EGL init failed 0x%x\n", eglGetError()); return 3; }
    const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    eglChooseConfig(d, ca, &cfg, 1, &n);
    const EGLint sa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    const EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, cfg, sa);
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
    if (!n || !eglMakeCurrent(d, s, s, c)) { printf("EGL context failed 0x%x\n", eglGetError()); return 4; }
    const uint64_t texture = NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE | NATIVEBUFFER_USAGE_MEM_DMA |
                             NATIVEBUFFER_USAGE_HW_TEXTURE;
    int failures = Try(d, "rgba", NATIVEBUFFER_PIXEL_FMT_RGBA_8888, texture);
    failures += Try(d, "nv12", NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP, texture);
    failures += Try(d, "nv12-decoder", NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP, 0x840b);  /* as the video decoder's */
    printf("EGLIMAGE_RESULT %s\n", failures ? "FAIL" : "OK");
    return failures != 0;
}
