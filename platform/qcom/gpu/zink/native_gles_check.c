/* SPDX-License-Identifier: MIT
 * asteroids: GLES through HarmonyOS's EGL (/system/lib64/libEGL.so, which loads the vendor driver: Mesa's Zink on
 * Turnip here): initializes the default display, makes an ES 3 context on a pbuffer, prints the driver's strings, clears
 * to a colour and reads one pixel back. "native_gles_check [log]". */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc > 1 && !freopen(argv[1], "w", stdout)) return 2;
    setbuf(stdout, 0);
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (!eglInitialize(d, &major, &minor)) { printf("GLES_INIT_FAILED 0x%x\n", eglGetError()); return 3; }
    printf("GLES_EGL %d.%d vendor=%s\n", major, minor, eglQueryString(d, EGL_VENDOR));
    const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(d, ca, &cfg, 1, &n) || !n) { printf("GLES_NO_CONFIG 0x%x\n", eglGetError()); return 4; }
    const EGLint sa[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, cfg, sa);
    const EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
    if (s == EGL_NO_SURFACE || c == EGL_NO_CONTEXT || !eglMakeCurrent(d, s, s, c)) {
        printf("GLES_CONTEXT_FAILED 0x%x\n", eglGetError());
        return 5;
    }
    printf("GLES_RENDERER %s | %s | %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char px[4] = {0};
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("GLES_PIXEL %u %u %u %u glError=0x%x\n", px[0], px[1], px[2], px[3], glGetError());
    int ok = px[0] > 55 && px[0] < 75 && px[1] > 117 && px[1] < 138 && px[2] > 181 && px[2] < 201;
    printf("GLES_RESULT %s\n", ok ? "OK" : "FAIL");
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(d, c);
    eglDestroySurface(d, s);
    eglTerminate(d);
    return !ok;
}
