/* SPDX-License-Identifier: MIT
 * asteroids: the vibration motor through Linux force feedback. The Richtek haptic driver (hapticdrv, I2C 8-005f)
 * registers the input device "haptic_rt" with FF_CONSTANT, FF_PERIODIC, FF_RUMBLE, FF_CUSTOM and FF_GAIN; a
 * constant effect of a given length is "vibrate for N ms". Used by the vibrator VDI and the asteroids_vibrate tool. */
#ifndef ASTEROIDS_HAPTIC_FF_H
#define ASTEROIDS_HAPTIC_FF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the haptic input device (by name, as event numbers change): the fd, or -1. */
int HapticOpen(void);
/* Vibrates for ms milliseconds at strength 1..100 (the motor's full range at 100); replaces what is playing. */
int HapticPlay(int fd, uint32_t ms, uint32_t strength);
/* Stops at once. */
int HapticStop(int fd);

#ifdef __cplusplus
}
#endif

#endif
