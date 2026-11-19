/* Android's camera HAL3 interface as Qualcomm's CamX (vendor/lib64/hw/camera.qcom.so) has it, for the probe and the
 * camera server. AOSP's hardware/camera_common.h and camera3.h, with this HAL's differences: its HMI has the torch
 * strength calls ahead of init, its camera3_device_ops has construct_default_physical_request_settings after
 * construct_default_request_settings (read off camera.qcom.so's symbols), camera3_stream_buffer_t is 40 bytes and
 * camera3_stream_t has a group id after physical_camera_id (the stride and the -1 test in CHI's
 * Usecase::PreprocessCaptureRequest); the structures the HAL reads get zeroed room past AOSP's fields.
 * libcamera_metadata and libnativewindow (AHardwareBuffer) are looked up at run time: hal3_load(). */
#ifndef ASTEROIDS_HAL3_H
#define ASTEROIDS_HAL3_H
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>

#ifndef HAL3_FN
#define HAL3_FN static
#endif
struct hw_device_t;
typedef struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version, hal_api_version;
    const char *id, *name, *author;
    struct hw_module_methods { int (*open)(const struct hw_module_t *, const char *, struct hw_device_t **); } *methods;
    void *dso;
    uint64_t reserved[32 - 7];
} hw_module_t;
typedef struct hw_device_t {
    uint32_t tag, version;
    hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(struct hw_device_t *);
} hw_device_t;
struct camera_info {
    int facing, orientation;
    uint32_t device_version;
    const void *static_camera_characteristics;
    int resource_cost;
    char **conflicting_devices;
    size_t conflicting_devices_length;
};
typedef struct camera_module_callbacks {
    void (*camera_device_status_change)(const struct camera_module_callbacks *, int, int);
    void (*torch_mode_status_change)(const struct camera_module_callbacks *, const char *, int);
} camera_module_callbacks_t;
typedef struct camera_module {
    hw_module_t common;
    int (*get_number_of_cameras)(void);
    int (*get_camera_info)(int, struct camera_info *);
    int (*set_callbacks)(const camera_module_callbacks_t *);
    void (*get_vendor_tag_ops)(void *);
    int (*open_legacy)(const hw_module_t *, const char *, uint32_t, void **);
    int (*set_torch_mode)(const char *, int);
    int (*turn_on_torch_with_strength_level)(const char *, int);
    int (*get_torch_strength_level)(const char *, int *);
    int (*init)(void);
    void *reserved[16 - 9];
} camera_module_t;

typedef const void *buffer_handle_t;
typedef struct camera3_stream {
    int stream_type;
    uint32_t width, height;
    int format;
    uint32_t usage, max_buffers;
    void *priv;
    int data_space, rotation;
    const char *physical_camera_id;
    int32_t group_id;                                            /* this HAL's: -1 when the stream is in no group */
    int32_t pad;
    void *reserved0[2];
    int64_t use_case;                                            /* ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES: 1 preview,
                                                                  * 2 still, 3 record, 4 preview+video+still; CHI's
                                                                  * stream intent (its multi-camera policy needs it) */
    void *reserved[13];
} camera3_stream_t;
typedef struct {
    uint32_t num_streams;
    camera3_stream_t **streams;
    uint32_t operation_mode;
    const void *session_parameters;
    void *reserved[8];
} camera3_stream_configuration_t;
typedef struct {
    camera3_stream_t *stream;
    buffer_handle_t *buffer;
    int status, acquire_fence, release_fence;
    int pad;
    uint64_t extra;                                              /* this HAL's: 40 bytes, not AOSP's 32 */
} camera3_stream_buffer_t;
typedef struct {
    uint32_t frame_number;
    const void *settings;
    camera3_stream_buffer_t *input_buffer;
    uint32_t num_output_buffers;
    const camera3_stream_buffer_t *output_buffers;
    uint32_t num_physcam_settings;
    const char **physcam_id;
    const void **physcam_settings;
    void *reserved[8];
} camera3_capture_request_t;
typedef struct {
    uint32_t frame_number;
    const void *result;
    uint32_t num_output_buffers;
    const camera3_stream_buffer_t *output_buffers;
    const camera3_stream_buffer_t *input_buffer;
    uint32_t partial_result;
} camera3_capture_result_t;
typedef struct {
    int type;                                                    /* 1 error, 2 shutter */
    union {
        struct { uint32_t frame_number; camera3_stream_t *stream; int error_code; } error;
        struct { uint32_t frame_number; uint64_t timestamp; } shutter;
    } message;
} camera3_notify_msg_t;
/* HAL buffer management (camera3.h 3.6): the HAL asks for buffers per stream when it needs them. */
typedef struct { camera3_stream_t *stream; uint32_t num_buffers_requested; } camera3_buffer_request_t;
typedef struct {
    camera3_stream_t *stream;
    int status;                                                  /* 0 ok, 1 no buffer available */
    uint32_t num_output_buffers;
    camera3_stream_buffer_t *output_buffers;                     /* the HAL's array, num_buffers_requested long */
} camera3_stream_buffer_ret_t;
typedef struct camera3_callback_ops {
    void (*process_capture_result)(const struct camera3_callback_ops *, const camera3_capture_result_t *);
    void (*notify)(const struct camera3_callback_ops *, const camera3_notify_msg_t *);
    int (*request_stream_buffers)(const struct camera3_callback_ops *, uint32_t, const camera3_buffer_request_t *,
                                  uint32_t *, camera3_stream_buffer_ret_t *);
    void (*return_stream_buffers)(const struct camera3_callback_ops *, uint32_t, const camera3_stream_buffer_t *const *);
} camera3_callback_ops_t;
struct camera3_device;
typedef struct {
    int (*initialize)(const struct camera3_device *, const camera3_callback_ops_t *);
    int (*configure_streams)(const struct camera3_device *, camera3_stream_configuration_t *);
    void *register_stream_buffers;
    const void *(*construct_default_request_settings)(const struct camera3_device *, int);
    void *construct_default_physical_request_settings;          /* this HAL's: not in AOSP's camera3.h */
    int (*process_capture_request)(const struct camera3_device *, camera3_capture_request_t *);
    void *get_metadata_vendor_tag_ops, *dump;
    int (*flush)(const struct camera3_device *);
} camera3_device_ops_t;
typedef struct camera3_device {
    hw_device_t common;
    camera3_device_ops_t *ops;
    void *priv;
} camera3_device_t;

/* libcamera_metadata and libnativewindow, looked up at run time. */
typedef struct {
    size_t index;
    uint32_t tag;
    uint8_t type;
    size_t count;
    union { const uint8_t *u8; const int32_t *i32; const int64_t *i64; } data;
} camera_metadata_ro_entry_t;
HAL3_FN size_t (*entry_count)(const void *);
HAL3_FN size_t (*data_count)(const void *);
HAL3_FN void *(*allocate_md)(size_t, size_t);
HAL3_FN int (*append_md)(void *, const void *);
HAL3_FN int (*add_entry)(void *, uint32_t, const void *, size_t);
HAL3_FN int (*update_entry)(void *, size_t, const void *, size_t, void *);
HAL3_FN int (*find_rw_entry)(void *, uint32_t, camera_metadata_ro_entry_t *);
HAL3_FN int (*find_entry)(const void *, uint32_t, camera_metadata_ro_entry_t *);
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct { uint32_t width, height, layers, format; uint64_t usage; uint32_t stride, rfu0; uint64_t rfu1; } AHB_Desc;
typedef struct { uint32_t count; struct { uint8_t *data; uint32_t pixel_stride, row_stride; } planes[4]; } AHB_Planes;
HAL3_FN int (*ahb_allocate)(const AHB_Desc *, AHardwareBuffer **);
HAL3_FN buffer_handle_t (*ahb_handle)(const AHardwareBuffer *);
HAL3_FN int (*ahb_lock)(AHardwareBuffer *, uint64_t, int32_t, const void *, void **);
HAL3_FN int (*ahb_lock_planes)(AHardwareBuffer *, uint64_t, int32_t, const void *, AHB_Planes *);
HAL3_FN void (*ahb_release)(AHardwareBuffer *);
HAL3_FN int (*ahb_unlock)(AHardwareBuffer *, int32_t *);


#define BLOB 0x21                   /* HAL_PIXEL_FORMAT_BLOB */
#define YUV 0x23                    /* HAL_PIXEL_FORMAT_YCbCr_420_888 */
#define IMPLEMENTATION_DEFINED 0x22 /* HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED */
#define TEXTURE_COMPOSER 0x900u     /* GRALLOC_USAGE_HW_TEXTURE | HW_COMPOSER */
#define VIDEO_ENCODER 0x10000u      /* GRALLOC_USAGE_HW_VIDEO_ENCODER: CHI treats the stream as video */
#define JFIF 0x8C20000              /* HAL_DATASPACE_V0_JFIF */
#define CPU_READ 3ull               /* AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN */
#define CAMERA_WRITE 0x20000ull     /* BufferUsage::CAMERA_OUTPUT */
/* Metadata tags (system/media/camera/include/system/camera_metadata_tags.h). */
#define AE_MODE 0x00010003           /* ANDROID_CONTROL_AE_MODE */
#define AE_EXPOSURE_COMPENSATION 0x00010001
#define AE_TARGET_FPS_RANGE 0x00010005
#define AF_MODE 0x00010007           /* ANDROID_CONTROL_AF_MODE */
#define AF_TRIGGER 0x00010009           /* 0 idle, 1 start, 2 cancel */
#define AE_REGIONS 0x00010004           /* int32 (xmin, ymin, xmax, ymax, weight) in the active array */
#define AF_REGIONS 0x00010008
#define CONTROL_ZOOM_RATIO_RANGE 0x0001002E
#define CONTROL_ZOOM_RATIO 0x0001002F     /* float, Android 11 */
#define AF_STATE 0x00010020           /* 0 inactive, 1 passive scan, 2 passive focused, 3 active scan, 4 focused
                                       * locked, 5 not focused locked, 6 passive unfocused */
#define AE_STATE 0x0001001F
#define AE_AVAILABLE_TARGET_FPS_RANGES 0x00010014
#define AE_COMPENSATION_RANGE 0x00010015
#define AE_COMPENSATION_STEP 0x00010016
#define AF_AVAILABLE_MODES 0x00010017
#define VIDEO_STABILIZATION_MODE 0x00010011   /* ANDROID_CONTROL_VIDEO_STABILIZATION_MODE: 0 off, 1 on, 2 preview */
#define AVAILABLE_VIDEO_STABILIZATION_MODES 0x0001001A
#define OPTICAL_STABILIZATION_MODE 0x00080004 /* ANDROID_LENS_OPTICAL_STABILIZATION_MODE: 0 off, 1 on */
#define AVAILABLE_OPTICAL_STABILIZATION 0x00090003
#define FLASH_MODE 0x00040002        /* ANDROID_FLASH_MODE: 0 off, 1 single, 2 torch */
#define FLASH_INFO_AVAILABLE 0x00050000
#define JPEG_ORIENTATION 0x00070003
#define JPEG_QUALITY 0x00070004
#define JPEG_MAX_SIZE 0x00070008     /* ANDROID_JPEG_MAX_SIZE */
#define LENS_FOCAL_LENGTH 0x00080002
#define LENS_FACING 0x00080005
#define LENS_INFO_AVAILABLE_FOCAL_LENGTHS 0x00090002
#define SCALER_CROP_REGION 0x000D0000
#define SCALER_MAX_DIGITAL_ZOOM 0x000D0004
#define STREAM_CONFIGS 0x000D000A    /* ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS */
#define SENSOR_EXPOSURE_TIME 0x000E0000
#define SENSOR_SENSITIVITY 0x000E0002
#define SENSOR_ORIENTATION 0x000E000E
#define SENSOR_TIMESTAMP 0x000E0010
#define SENSOR_INFO_ACTIVE_ARRAY 0x000F0000
#define SENSOR_INFO_PHYSICAL_SIZE 0x000F0005
#define REQUEST_AVAILABLE_CAPABILITIES 0x000C000C  /* u8 list; 11 LOGICAL_MULTI_CAMERA */
#define LOGICAL_PHYSICAL_IDS 0x001A0000            /* ANDROID_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS: NUL-separated */
#define BUFFER_MANAGEMENT 0x00150001 /* ANDROID_INFO_SUPPORTED_BUFFER_MANAGEMENT_VERSION */
#define TEMPLATE_PREVIEW 1
#define TEMPLATE_STILL 2
#define TEMPLATE_RECORD 3

/* camera3_jpeg_blob_t at the end of a JPEG buffer: id 0x00FF, then the JPEG's size. */
struct jpeg_blob { uint16_t id; uint8_t pad[2]; uint32_t size; };

static inline int hal3_load(void)
{
    void *md = dlopen("libcamera_metadata.so", RTLD_NOW), *nw = dlopen("libnativewindow.so", RTLD_NOW);
    if (!md || !nw) return -1;
    entry_count = (size_t (*)(const void *))dlsym(md, "get_camera_metadata_entry_count");
    data_count = (size_t (*)(const void *))dlsym(md, "get_camera_metadata_data_count");
    find_entry = (int (*)(const void *, uint32_t, camera_metadata_ro_entry_t *))dlsym(md, "find_camera_metadata_ro_entry");
    allocate_md = (void *(*)(size_t, size_t))dlsym(md, "allocate_camera_metadata");
    append_md = (int (*)(void *, const void *))dlsym(md, "append_camera_metadata");
    add_entry = (int (*)(void *, uint32_t, const void *, size_t))dlsym(md, "add_camera_metadata_entry");
    update_entry = (int (*)(void *, size_t, const void *, size_t, void *))dlsym(md, "update_camera_metadata_entry");
    find_rw_entry = (int (*)(void *, uint32_t, camera_metadata_ro_entry_t *))dlsym(md, "find_camera_metadata_entry");
    ahb_allocate = (int (*)(const AHB_Desc *, AHardwareBuffer **))dlsym(nw, "AHardwareBuffer_allocate");
    ahb_release = (void (*)(AHardwareBuffer *))dlsym(nw, "AHardwareBuffer_release");
    ahb_handle = (buffer_handle_t (*)(const AHardwareBuffer *))dlsym(nw, "AHardwareBuffer_getNativeHandle");
    ahb_lock = (int (*)(AHardwareBuffer *, uint64_t, int32_t, const void *, void **))dlsym(nw, "AHardwareBuffer_lock");
    ahb_lock_planes = (int (*)(AHardwareBuffer *, uint64_t, int32_t, const void *, AHB_Planes *))
        dlsym(nw, "AHardwareBuffer_lockPlanes");
    ahb_unlock = (int (*)(AHardwareBuffer *, int32_t *))dlsym(nw, "AHardwareBuffer_unlock");
    return entry_count && find_entry && allocate_md && append_md && add_entry && update_entry && find_rw_entry &&
        ahb_allocate && ahb_release && ahb_handle && ahb_lock && ahb_lock_planes && ahb_unlock ? 0 : -1;
}

/* Sets one entry of a settings buffer of ours (allocate_md'd with room to spare). */
static inline int md_set(void *m, uint32_t tag, const void *v, size_t n)
{
    camera_metadata_ro_entry_t e;
    return find_rw_entry(m, tag, &e) ? add_entry(m, tag, v, n) : update_entry(m, e.index, v, n, NULL);
}
#endif
