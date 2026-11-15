# ChobotCam OS (FRC 9585) - Buildroot SD-card image builder

**This ZIP is SOURCE/BUILDER, not a precompiled bootable IMG.** Download/build requires internet and Linux or a GitHub Actions runner. The C daemon was not tested on actual Raspberry Pi 3B or on the user's cameras.

## Make a flashable image

Option A: On a Linux PC or WSL2 Ubuntu, install Buildroot host dependencies (see GitHub workflow) and run `./tools/build.sh`. Output: `ChobotCamOS_FRC9585.img.xz`.

Option B: Upload this project to a new GitHub repository, keep the `.github/workflows/build-image.yml` file and run **Actions -> Build ChobotCam OS SD image -> Run workflow**. When complete, download the artifact ZIP, unzip it to get `ChobotCamOS_FRC9585.img.xz`.

Raspberry Pi Imager -> **Use custom** -> select `.img.xz` (or unpack to `.img`) -> flash SD card. Buildroot upstream Pi 3 64-bit image uses firmware and kernel. No Raspberry Pi OS, Python or GUI running on the Pi.

## Wiring

roboRIO DIO0 signal -> Pi GPIO17 (physical header pin 11). roboRIO DIO ground -> Pi GND (pin 6). NEVER connect the DIO +5V supply pin to Pi GPIO. Make sure GPIO high is <=3.3V. Power Pi from its own regulated 5V supply. Use approved radio/switch Ethernet, not independent onboard Pi Wi-Fi at an event.

LOW = camera 0; HIGH = camera 1. Both webcams always capture at 320x240 MJPEG; only selected camera is transmitted. It expects native camera MJPEG (YUYV-only webcams unsupported by design).

## Network

Default static Pi IP: `10.95.85.11/24` (only use if free/compatible with robot network). Change `chobot.cfg` on the FAT boot partition after flashing. Stream URL: `http://10.95.85.11:5800/stream.mjpg`. Status `http://10.95.85.11:5800/status`. TCP 5800 is a permitted FRC team port under 2026 R704. Total permitted radio bandwidth depends on which FRC radio is in use; enforce that bandwidth yourself.

To change camera devices edit `CHOBOT_CAM0` and `CHOBOT_CAM1` in `chobot.cfg` on FAT boot partition. Many USB cameras appear as multiple `/dev/video*` device nodes; the default `/dev/video0` and `/dev/video2` are guesses. Stable by-id paths may be substituted if available.

## Latency caveats

No transcoding: the server forwards camera MJPEG bytes via HTTP. No long application queue: newer camera frames overwrite older ones. Camera capture uses 3 kernel buffers, HTTP and dashboard rendering may still add latency. A target of <300ms must be measured, not promised. Lower latency and lower bandwidth might require a different protocol/device encoder/player.

## Important engineering caveats

This is a working prototype architecture, NOT an experimentally verified FRC competition build. C daemon uses GPIO character-device v2 API (Linux >=5.10), and assumes gpiochip0 line17 = BCM GPIO17. First verify gpiochip mapping and UVC MJPEG support on actual Pi. Buildroot/kernel versions might need adjustments. The kernel is configured by upstream Buildroot Raspberry Pi 3 64-bit defconfig. Limit MJPEG quality in camera hardware/settings if radio bitrate is too high. Consider a stable 5V regulator and compliant electrical mounting.

## Low-latency V2 improvements (2026-11-14)

- V4L2 MJPEG capture is **zero-transcode**, and uses only **2 mmap buffers**.
- Camera reader drains ready V4L2 frames and publishes the newest available JPEG.
- Streaming clients wait on a frame condition variable, not busy-looping every 5ms.
- TCP_NODELAY enabled; small socket send buffer (8192 requested) and 90ms nonblocking send deadline per fragment. **Slow clients are disconnected rather than allowed to build up seconds of stale video.** Your dashboard must reconnect when this occurs.
- GPIO is polled by a dedicated thread at ~2ms instead of by the HTTP client or accept loop. Both cameras remain active.
- Frames older than 200ms are suppressed at **start of transmission** (this is **not** a 200ms end-to-end guarantee; network and player may still buffer).

To get the lowest actual latency, use **native 320x240 MJPEG** on each webcam, a wired Ethernet connection, **one dashboard client**, and a dashboard viewer with minimal buffering. Camera MJPEG frame size and FRC bandwidth limits must be measured. 30 FPS reduces the frame interval from 50ms to 33ms, but increases bitrate, and may make radio performance worse. Change `CHOBOT_FPS=30` only after checking camera support and throughput.

**Important:** the C daemon is compile-tested on Linux; no complete Buildroot image build or Raspberry Pi hardware end-to-end delay measurement has been performed in this environment. The IMG is still created by Buildroot/GitHub Actions and must be verified before competition use.
