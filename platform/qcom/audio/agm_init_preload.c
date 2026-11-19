/* Test only: initialise AGM in-process (no AGM binder service here), as sheng's PAL server does, and print the
 * Android log calls (there is no logd) to stderr. */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
static const char prio[] = "??VDIWEF";
int __android_log_vprint(int p, const char *tag, const char *fmt, va_list ap)
{
    fprintf(stderr, "%c/%s: ", p >= 0 && p < 8 ? prio[p] : '?', tag ? tag : "");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    return 0;
}
int __android_log_print(int p, const char *tag, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); __android_log_vprint(p, tag, fmt, ap); va_end(ap); return 0;
}
int __android_log_write(int p, const char *tag, const char *msg) { return __android_log_print(p, tag, "%s", msg); }
int __android_log_buf_print(int b, int p, const char *tag, const char *fmt, ...)
{
    (void)b; va_list ap; va_start(ap, fmt); __android_log_vprint(p, tag, fmt, ap); va_end(ap); return 0;
}
int __android_log_buf_write(int b, int p, const char *tag, const char *msg) { (void)b; return __android_log_write(p, tag, msg); }
int __android_log_is_loggable(int p, const char *tag, int d) { (void)p; (void)tag; (void)d; return 1; }
/* Run AGM's init after all constructors and outside the loader lock (sheng's pal_server does the same): take over
 * bionic's __libc_init, initialise AGM, then run the program's own main. */
static int (*program_main)(int, char **, char **);
static int main_after_agm_init(int argc, char **argv, char **envp)
{
    int (*init)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "agm_init");
    fprintf(stderr, "AGM_INIT %d\n", init ? init() : -999);
    return program_main(argc, argv, envp);
}
__attribute__((noreturn)) void __libc_init(void *args, void (*onexit)(void), int (*main)(int, char **, char **),
                                           const void *structors)
{
    void (*real)(void *, void (*)(void), int (*)(int, char **, char **), const void *) =
        (void (*)(void *, void (*)(void), int (*)(int, char **, char **), const void *))dlsym(RTLD_NEXT, "__libc_init");
    program_main = main;
    real(args, onexit, main_after_agm_init, structors);
    __builtin_unreachable();
}
