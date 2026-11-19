/* SPDX-License-Identifier: MIT
 * asteroids: PAL's pal_stream_open asks android::hardware::IPCThreadState::self()->getCallingPid() for its log line.
 * That brings up hwbinder, and without /dev/hwbinder libhwbinder aborts the PAL server. Nothing calls into the server
 * over binder here, so the caller is this process: answer both without binder. */
extern int getpid(void);
static char state;
void *_ZN7android8hardware14IPCThreadState4selfEv(void) { return &state; }
int _ZNK7android8hardware14IPCThreadState13getCallingPidEv(const void *self) { (void)self; return getpid(); }
