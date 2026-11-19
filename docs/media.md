# Video decoding and encoding

Hardware H.264 and HEVC, both ways, through HarmonyOS's own path (AVCodec → HCodec → the codec HDI's OMX core →
Qualcomm's msm_video), beyond sheng (who has none). Before: av_codec offered the donor's `OMX.hisi.video.decoder.*`,
Huawei's `libomxvdec.so` failed to load, and it fell back to software; it had no video encoder at all, hardware or
software, so the camera couldn't record.

## Decoding

- msm_video: `/dev/video32` (decoder: H.264/HEVC/VP9/HEIC in, NV12 out) and `/dev/video33` (encoder), V4L2 stateful
  M2M, DMABUF only (from `/dev/dma_heap/system`). Firmware `vpu20_2v.mbn` from Android's vendor_b, in our
  `/vendor/firmware` (staged on the PC in hmos-stage, not in git). `platform/qcom/media/v4l2_probe.c`, `v4l2_decode_test`.
- `platform/qcom/media/omx_core.c` is our `libOMX_Core.z.so` (in the /vendor overlay); its two components, under the hisi names,
  wrap `platform/qcom/media/v4l2dec.c`. Frames are copied (with dma-buf cache syncs) into HCodec's SurfaceBuffers. Output
  `eColorFormat` is a GraphicPixelFormat (24 = NV12). Runs inside av_codec_service (uid media). hilog tag `asteroids_omx`.
- Elsewhere: HarmonyOS's ueventd gives `/dev/video*` to camera_host 0660, so svc/booted.sh opens 32 and 33 once the
  desktop is up; the allocator VDI allows `BUFFER_USAGE_VIDEO_DECODER`; HEVC config buffers are held only when they
  carry no picture.
- Results (`native_video_decode_check`, frame 30 bit-identical to software): H.264 720p 433 fps (software 476), HEVC
  1080p 233 fps (software 107), 150/150 frames; the copy costs 0.24 ms (720p) and 0.52 ms (1080p) per frame.
- Test clips go into the gallery with `mediatool send <file>` (in the HarmonyOS root).
- Videos play in the browser ([gpu.md](gpu.md), EGLImages).

## Encoding (2026-11-16)

Huawei's Camera app records 1080p30 HEVC (or H.264 with its "High efficiency format" off) with sound, and the clips
play in the gallery. The same OMX core has two more components, `OMX.hisi.video.encoder.avc` and `.hevc`, on
`platform/qcom/media/v4l2enc.c` (msm_video's encoder, `/dev/video33`): NV12 or NV21 in, Annex-B out.

No codec HDI service was needed: av_codec_service runs the codec HDI in passthrough (as for decoding), and the donor's
capability list (`/chip_prod/etc/hdfconfig/hdf_default.hcb`) already names both encoders, 144×144 to 4096×4096,
10 kbit/s to 100 Mbit/s. The codec HDI drops a component whose device path doesn't exist, and the encoders' is
`/dev/vcodec_venc` (`GetGroupCapabilities() access /dev/vcodec_venc failed, ignore`): `asteroids-desktop.cfg` links it
to `/dev/video33` at post-fs-data, before any media service starts (av_codec_service builds its list once).
`codec_image_service` and `codec_component_manager_service` missing from hdf_devmgr (the errors in hilog) are the image
codec and the IPC form of the manager; HCodec uses neither.

- Frames: HCodec gives every encoder input as a SurfaceBuffer handle (`CODEC_BUFFER_TYPE_DYNAMIC_HANDLE`), in surface
  mode straight from the encoder's input surface (the camera host writes the video stream there), in buffer mode from
  buffers it allocates with `BUFFER_USAGE_VIDEO_ENCODER`, which the allocator VDI (`platform/qcom/display/buffer_vdi.cpp`)
  now accepts (CreateAVBuffer failed before). Each frame is copied into one of the driver's dma-heap buffers (luma
  stride 1920, chroma after 1088 rows at 1080p), swapping chroma when the buffer's format (the camera's NV21) differs
  from what the encoder was opened with: 1.2 ms a frame at 1080p.
- Bitstream: in HCodec's shared memory. The driver puts the parameter sets in front of the first frame; they go out
  first in a buffer of their own (`CODECCONFIG`), which the muxer needs; key frames are `SYNCFRAME`.
- Timestamps: OMX's are microseconds and HCodec's buffer mode gives those, but its surface mode passes the surface's
  nanoseconds through. msm_vidc's rate control spaces bits by the timestamps, so with nanoseconds it took frames as
  33 s apart and made 120 Mbit/s of a 17 Mbit/s recording. The driver gets a microsecond timeline of the component's
  own (the gaps between frames, one of a second or more counted as nanoseconds), and each packet goes out with its
  frame's own timestamp.
- Rate control: always the driver's CBR. Its VBR overshoots (54 Mbit/s from the camera for 17 asked; setting the peak
  changes nothing); its CBR keeps to the bitrate and doesn't pad simple pictures (17 Mbit/s asked: 15 Mbit/s from the
  camera, 0.2 Mbit/s of flat colour). GOP from the client's I-frame interval, no B-frames, Main (HEVC) or High (H.264).
- Stop: the recorder ends a video at the first frame stamped after its stop time, and Huawei's app stops the camera's
  video stream just before stopping the recorder, so it waits its 2 s timeout every time and frames still in the
  encoder are dropped (the recorder doesn't send an end of stream). An end of stream does drain the encoder.
- The Camera app needed one camera host fix to start recording at all: [camera.md](camera.md).

Tools (in `/system/bin`): `v4l2_encode_test <h264|hevc> <out> [frames] [WxH] [nv12|nv21] [bands|noise|<raw file>]
[bitrate] [first timestamp] [paced]`, msm_video on its own (from M1's shell), coloured bands, faint noise or raw NV12
frames; `native_video_encode_check <log> <avc|hevc> <out> [frames] [WxH] [encoder]`, synthetic frames through the
public AVCodec API in buffer mode (from a root shell in init's namespace: `nsenter -t 1 -m -- ...`). hilog tag
`asteroids_omx`: the encoder's settings when it opens and, when it stops, its frames, bytes and bitrate.

Results: `native_video_encode_check` 90 frames at 1080p, HEVC and H.264, 57-72 fps (the check's drawing included), the
stream decodes to the drawn pattern. Camera: 1080p30 HEVC, 166 frames in 5.5 s, 14.9 Mbit/s, AAC 192 kbit/s, decodes
cleanly with ffmpeg and plays in Huawei's Gallery (through the hardware decoder).

## Testing a library without an image

av_codec_service runs in a sandbox mount namespace built at its start from the root as the handoff left it, so a bind
mount made later in init's namespace doesn't reach it, and `/vendor` is an overlay whose layers are fixed when prep
mounts it. A file bound over the root's copy before the handoff does: `/data/asteroids/m2-test.sh` (sourced at the end
of prep.sh, with R set) with `mount --bind /data/local/tmp/libOMX_Core.z.so $R/vendor/lib64/passthrough/indirect/libOMX_Core.z.so`,
then `port reboot`. Delete it afterwards.

## Open

- VP9 (the driver has it, HCodec's list doesn't); zero copy (decode straight into SurfaceBuffers, encode straight
  from them).
- Screen recording: HCodec asks for RGBA input, which the encoder refuses (msm_video takes NV12/NV21 only).
- The encoder's optional features (LTR, QP ranges, temporal layers, ROI) are accepted and ignored.
