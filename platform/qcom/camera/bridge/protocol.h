/* SPDX-License-Identifier: MIT
 * The camera bridge's wire protocol: camserver (camserver.c, in the camera root beside CamX) and HarmonyOS's camera host
 * (camera_host.cpp) over a SOCK_SEQPACKET unix socket, /dev/unix/socket/asteroids.camera in HarmonyOS's root.
 *
 * The host sends requests (struct cam_req) and gets one reply each (struct cam_reply, then op-specific data); the
 * server also sends events (struct cam_event: preview frames, the shutter, stills) at any time, so the host reads with
 * one thread and tells replies from events by `magic`. One client at a time: a new connection closes the camera the
 * last one had open.
 *
 * Frames travel through shared memory: CAM_CONFIGURE's reply carries a memfd (SCM_RIGHTS) holding `slots` preview
 * slots, each a packed NV21 frame (Y rows of `width` bytes, then interleaved V/U rows), and after them one JPEG slot of
 * `jpeg_capacity` bytes. A CAM_EV_FRAME event lends a slot to the host until it sends CAM_RELEASE for it; with no slot
 * free, the server drops frames rather than stall the HAL. */
#ifndef ASTEROIDS_CAMERA_PROTOCOL_H
#define ASTEROIDS_CAMERA_PROTOCOL_H
#include <stdint.h>

#define CAM_SOCKET "asteroids.camera"
#define CAM_REQ_MAGIC 0x31514341u   /* "ACQ1" */
#define CAM_REPLY_MAGIC 0x31524341u /* "ACR1" */
#define CAM_EVENT_MAGIC 0x31454341u /* "ACE1" */
#define CAM_MAX_CAMERAS 8
#define CAM_MAX_SIZES 48
#define CAM_MAX_FPS 8

enum cam_op {
    CAM_LIST = 1,      /* reply: struct cam_list */
    CAM_OPEN,          /* arg[0] camera index */
    CAM_CONFIGURE,     /* arg[0..1] preview w, h; arg[2..3] still w, h (0: no still stream); reply: struct cam_layout + fd */
    CAM_START,         /* starts the repeating preview */
    CAM_STOP,
    CAM_SETTINGS,      /* data: struct cam_settings, for the following requests */
    CAM_STILL,         /* arg[0] the still's id (echoed in its events); one still with the current settings */
    CAM_RELEASE,       /* arg[0] preview slot, back to the server; no reply */
    CAM_CLOSE,
};

enum cam_event_type {
    CAM_EV_FRAME = 1,  /* slot: a preview frame, lent until CAM_RELEASE */
    CAM_EV_SHUTTER,    /* id: the still's exposure started (timestamp) */
    CAM_EV_STILL,      /* id: the JPEG is in the JPEG slot, `size` bytes; status nonzero: it failed */
    CAM_EV_ERROR,      /* status: the device failed; the camera is closed */
    CAM_EV_STATE,      /* id: frame number; slot: the HAL's AF state, size: its AE state (ANDROID_CONTROL_AF_STATE,
                        * AE_STATE); sent when either changes */
};

struct cam_req {
    uint32_t magic, op, seq, bytes;  /* bytes of data after the header */
    int32_t arg[4];
};

struct cam_reply {
    uint32_t magic, seq;
    int32_t rc;                      /* 0 or -errno */
    uint32_t bytes;
};

struct cam_event {
    uint32_t magic, type;
    uint32_t slot, id;
    int32_t status;
    uint32_t size;
    uint64_t timestamp;              /* sensor start of exposure, ns, CLOCK_BOOTTIME */
    int64_t exposure_ns;
    int32_t iso;
    int32_t pad;
};

struct cam_size { uint32_t width, height; };

struct cam_info {
    int32_t facing;                  /* 0 back, 1 front (as the HAL's) */
    int32_t orientation;             /* degrees the sensor image is rotated from upright */
    int32_t flash;                   /* has a flash */
    uint32_t active_width, active_height;
    float focal_length;              /* mm */
    float min_zoom, max_zoom;        /* below 1: a wider lens (a logical camera switches lenses as it zooms) */
    float physical_width, physical_height;  /* sensor, mm */
    int32_t ev_min, ev_max;          /* exposure compensation steps */
    float ev_step;
    uint32_t n_preview, n_still, n_fps;
    struct cam_size preview[CAM_MAX_SIZES];  /* YUV sizes, largest first */
    struct cam_size still[CAM_MAX_SIZES];    /* JPEG sizes, largest first */
    int32_t fps[CAM_MAX_FPS][2];
    char hal_id[8];
};

struct cam_list {
    uint32_t count;
    struct cam_info camera[CAM_MAX_CAMERAS];
};

struct cam_layout {
    uint32_t slots, slot_size;       /* preview slots, each width * height * 3 / 2 */
    uint32_t width, height;
    uint64_t jpeg_offset;
    uint32_t jpeg_capacity, pad;
    uint64_t total;                  /* the memfd's size */
};

enum cam_flash { CAM_FLASH_OFF, CAM_FLASH_ON, CAM_FLASH_AUTO, CAM_FLASH_TORCH };
enum cam_focus { CAM_FOCUS_CONTINUOUS, CAM_FOCUS_AUTO, CAM_FOCUS_LOCKED };

struct cam_settings {
    int32_t flash;                   /* enum cam_flash */
    float zoom;                      /* min_zoom .. max_zoom */
    int32_t jpeg_orientation;        /* degrees, for the still */
    int32_t jpeg_quality;            /* 1..100 */
    int32_t ev;                      /* exposure compensation steps */
    int32_t af_trigger;              /* a new value starts a focus scan (at the focus point, in CAM_FOCUS_AUTO) */
    int32_t fps_min, fps_max;        /* 0: the template's */
    int32_t zoom_ms;                 /* reach `zoom` over this long (a smooth zoom); 0: at once */
    int32_t focus;                   /* enum cam_focus */
    float focus_x, focus_y;          /* the focus and metering point, 0..1 across the sensor's image; < 0: none */
    int32_t video;                   /* the recording template and video focus (a video session) */
    int32_t stabilization;           /* electronic stabilisation (with the gyro) in a video session */
};
#define CAM_SETTINGS_DEFAULT {CAM_FLASH_OFF, 1.0f, 0, 95, 0, 0, 0, 0, 0, CAM_FOCUS_CONTINUOUS, -1.0f, -1.0f, 0, 1}
#endif
