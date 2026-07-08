# Phase 3 - camera without `ava` (full autonomy)

Phase 2 taps the camera from *inside* a running `ava` so Valetudo/navigation keep working. Phase 3 is the opposite: run the camera with **`ava` stopped**, for a setup where a custom stack (e.g. SangamIO driving the MCU/LDS directly) fully replaces the vendor daemon. With `ava` dead the sensor is free, so we drive the OV8856 ourselves and feed the same `ava_cam_relay` + go2rtc pipeline (`/tmp/camtap.shm` unchanged).

Build: `make docker` (aarch64). Deploy the binaries next to the relay and run with `ava` stopped.

## Running (`noava-cam.sh`)

One wrapper brings the whole stack up (needs `ava` already stopped - use it via sangamio's `w10-direct.sh`, which freezes ava's watchdogs first):

```sh
noava-cam.sh start        # RGB only     -> go2rtc stream "camera"    (rtsp://<ip>:8554/camera)
noava-cam.sh start tof    # IR/ToF only  -> go2rtc stream "camera"
noava-cam.sh start both   # BOTH at once -> "camera" (RGB) + "camera_ir" (IR/ToF)
noava-cam.sh stop | status
```

`both` runs two `w10-cam` + two `ava_cam_relay` (ports 8090/8091, one per `CAM_SHM`) into a single go2rtc serving both named streams. Or via the autonomy wrapper: `w10-direct.sh start both` (kills ava + runs sangamio + both cameras). HA over RTSP needs H264 transcode: `ffmpeg:http://<ip>:1984/api/stream.mjpeg?src=camera#video=h264` (and `...src=camera_ir...`).

## RGB - `src/w10-cam.c` (working)

Standalone RGB capture. `dlopen`s the vendor `libsunxicamera.so` and calls the `sunxi_cam::SunxiCam` C++ methods with **our own** `self` object (Phase 2's `libcamtap` reused `ava`'s object; here `ava` is gone, so we allocate a fresh 16-byte `self` - `OpenCamera` from state 0 does the full V4L2 + ISP bring-up itself):

```
OpenCamera(self, /dev/video2, fourcc=NV21, a3=15, 672, 504)  // args RE'd from a live ava
loop: GetImageFrame(self, &frame) -> data = *(void**)(frame+0x20) -> memcpy 508032B NV21 -> camtap.shm
      ReturnImageFrame(self, &frame)
```

The OpenCamera args were read live from `ava`'s memory (`libcamtap` statics `g_a1=2`, `g_fourcc="NV21"`, `g_a3=15`, cross-checked by `g_w/g_h=672/504`). `libsunxicamera` is self-contained for bring-up (it pulls in `libAWIspApi`/`libisp` and configures the media pipeline + ISP internally), so a plain V4L2 open is **not** enough - see below. Verified on the robot: ~12 fps, 0 misses, real color frames through go2rtc.

Env: `CAM_INDEX`(2) `CAM_W`(672) `CAM_H`(504) `CAM_A3`(15) `CAM_FOURCC`(NV21) `CAM_SHM`(/tmp/camtap.shm) `CAM_LIB`.

## IR / ToF - working

The cleaning-time IR feed is a **separate sensor**: `ofilm0092` (a structured-light ToF), not the OV8856. It has its own full VIN pipeline, parallel to the RGB one:

```
RGB:  ov8856(e3)   -> mipi.1 -> csi.1 -> isp0(e38) -> scaler.0/2 -> /dev/video0, /dev/video2
ToF:  ofilm0092(e1)-> mipi.0 -> csi.0 -> isp1(e44) -> scaler.1   -> /dev/video1
```

**Concurrency is possible** (verified): during a real cleaning `ava` runs **both** pipelines at once (isp0 for RGB, isp1 for ToF - separate MIPI/CSI/ISP/scaler, no shared bottleneck), and opening `/dev/video1` never disturbed the RGB capture on `/dev/video2`. The vendor's dock-vs-cleaning switch is a software choice, not a hardware limit.

ToF format (RE'd from `ofilm0092` subdev + a live cleaning): sensor mbus **`0x3011`, 224x1558** all the way down the path; `/dev/video1` output is **MPLANE, `BG12`** (12-bit Bayer, 224x1558, `size = 224*1558*2`). Raw, ISP-passthrough - not the ISP-processed NV21 the RGB path produces. `ir_process.h` (Phase 2) decodes this 224x1558 frame (nine IR sub-frames) to grayscale.

Recipe (verified on the robot, `ava` stopped, on the dock): enable the two disabled links, set every subdev pad on the path to `224x1558/0x3011`, then run `w10-cam` on `/dev/video1` with the ToF format. Like the RGB path it drives the sensor through **`libsunxicamera`**, not raw V4L2:

```sh
media_link  /dev/media0 1  0 32 0 1          # ofilm0092 -> mipi.0
media_link  /dev/media0 26 1 44 0 1          # csi.0 -> isp1
for sd in "7 0" "7 1" "5 0" "5 1" "11 0" "11 2" "14 0" "14 1"; do   # mipi.0/csi.0/isp1/scaler.1 pads
  subdev_setfmt /dev/v4l-subdev$(echo $sd|cut -d' ' -f1) $(echo $sd|cut -d' ' -f2) 224 1558 3011
done
CAM_INDEX=1 CAM_W=224 CAM_H=1558 CAM_FOURCC=BG12 CAM_FRAMESZ=698368 CAM_FORMAT=100 \
  CAM_SHM=/tmp/camtap.shm w10-cam            # 60 frames, 0 misses; full-content IR
```

Result: ~12 fps, 0 misses, and the 698368-byte frame is full of real data (not empty/dark) - **on the dock, without navigation**. `ir_process.h` (Phase 2) decodes the 224x1558 raw frame (nine IR sub-frames, `CAM_FORMAT=100`) to grayscale.

**The "illuminator/navigation trigger" was a red herring.** A raw `v4l2probe` `STREAMON` on `/dev/video1` returns *no* frames (DQBUF times out), which looked like the sensor only emits while navigating. But `w10-cam` (i.e. `libsunxicamera::SunxiCam::start`) gets frames immediately, docked - so the missing piece was the vendor library's full sensor **start sequence**, not the IR laser or motion. (Ruled out along the way: sensor exposure/gain - unchanged 0/16 during an active stream; the three exported output GPIOs 118/355/360 - constant `1/0/1` even when the ToF is live; the `V4L2_CID_ILLUMINATORS_1/2` controls - `S_CTRL` returns `EINVAL`.)

Concurrency verified: RGB (`CAM_INDEX=2`) and ToF (`CAM_INDEX=1`) `w10-cam` run **at the same time**, both 60 frames / 0 misses, since they use separate ISPs (isp0 / isp1). Both-at-once is implemented in `noava-cam.sh start both`: each sensor gets its own `CAM_SHM` (`/tmp/camtap.shm` RGB, `/tmp/camtap_ir.shm` ToF) + its own `ava_cam_relay` (`IR_PORT=8090`/`8091`, path via the new `CAM_SHM` env) feeding one go2rtc as two named streams (`camera`, `camera_ir`). The vendor's single-feed auto-switch (RGB docked / IR cleaning) instead multiplexes one shm. Verified on the robot: both shm frame counters advance concurrently and go2rtc delivers a valid JPEG from each stream at once (RGB 672x504 color, IR 448x346 gray).

Gotcha found the hard way: with **mop pads attached** the robot wets them at the dock first and only *then* moves - short "start cleaning" probes for the live ToF config looked like the ToF never activated. Detach the pads (vacuum mode) to bring the live ToF up quickly.

## Tools (`tools/`, aarch64 static)

- `media_topo /dev/media0` - dump the VIN media graph (entities, dev nodes, enabled links).
- `media_link /dev/media0 SE SP DE DP [0|1]` - enable/disable a link (`MEDIA_IOC_SETUP_LINK`).
- `subdev_setfmt /dev/v4l-subdevN pad W H CODEHEX` - set a subdev pad format (`VIDIOC_SUBDEV_S_FMT`).
- `subdev_probe /dev/v4l-subdevN [pad]` - enumerate mbus codes/sizes + current ACTIVE format.
- `query_ctrl /dev/...` - enumerate V4L2 controls + current values (found the Illuminator controls here).
- `v4l2probe /dev/videoN [W H FOURCC]` - QUERYCAP/ENUM_FMT + S_FMT/REQBUFS/STREAMON/DQBUF capture test (single- or multi-planar). `ILLUM=1` also tries the illuminator controls.

### Entity -> node map (r2104)

`ofilm0092`=/dev/v4l-subdev0, `ov8856`=subdev1, `csi.0`=subdev5, `mipi.0`=subdev7, `isp0`=subdev9, `isp1`=subdev11, `scaler.0`=subdev13, `scaler.1`=subdev14. Video nodes: `video0/1/2` = vin_video0/1/2.
