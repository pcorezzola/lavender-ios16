# iOS 16 on the Redmi Note 7 (lavender)

A non-commercial research port of **XNU 8796** (Darwin 22 / iOS 16) and the **iPhone X (D22AP) iOS 16.7.16 userland** to the **Redmi Note 7** (`lavender`, Qualcomm SDM660 / Snapdragon 660).

![iOS 16 home screen on the Redmi Note 7](docs/images/home-screen.jpg)

> **Status: boots to the iOS 16.7.16 lock screen and home screen in ~2–3 minutes.** This is an experimental bring-up, not a daily driver. The home screen renders icons, the dock and badges, Settings opens and the first-run Siri screen appears; touch (taps and swipes), physical buttons, USB networking (CDC-NCM: ping, SSH and a live kernel log from Windows) and fast eMMC reads (ADMA2) work on the device. The GPU, Wi-Fi, Bluetooth, audio, camera and a real battery gauge do not: rendering is done in software on the CPU. No Apple binaries are distributed in this repository.

The port is built on top of [`aLpHa-Git-69/ios-lavender`](https://github.com/aLpHa-Git-69/ios-lavender) (branch `lavender`), itself derived from [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet). This repository documents the lavender-specific bring-up, the patches that made eMMC writes reliable, and the current state of the device.

## Progress

| Home screen | Settings | Siri setup |
| --- | --- | --- |
| <img src="docs/images/home-screen.jpg" width="230" alt="iOS 16 home screen on the Redmi Note 7"> | <img src="docs/images/settings.jpg" width="230" alt="Settings app on the Redmi Note 7"> | <img src="docs/images/siri-setup.jpg" width="230" alt="Siri setup screen on the Redmi Note 7"> |
| SpringBoard's grid: app icons, dock, notification badge and the App Library pill. The default Bokeh wallpaper now renders, the dock and widgets are translucent, the Weather widget shows its loading placeholder and the Calendar widget is still a blank white tile. | The whole Settings list renders and scrolls. | The first-run Siri screen appears. Without a microphone or speaker, Siri itself cannot work. |

<img src="docs/images/boot-self-tests.jpg" width="230" alt="Boot console with Skylight self-tests on the Redmi Note 7">

Under the Apple logo and progress bar, the userland **Skylight** window server runs its software blit self-tests: `sk_copy64_8` through `sk_copy64_64x`, `sk_copy32_*`, `sk_zero_16` to `sk_zero_128` and a `memcmp` test all print `PASSED`. That is the software path that draws every frame on this port.

## What works

| Area | Status | Evidence / notes |
| --- | --- | --- |
| XNU boot | **Works** | `xnu-8796`, `kernel.development.lavender`, Mach-O arm64 |
| CPU bring-up | **Works** | 4× Cortex-A73 Gold, 16 KiB pages; Silver A53 cores parked (they do not support 16 KiB) |
| GICv3 / timers | **Works** | GIC 0x17a00000/0x17b00000, virtual/physical timers |
| eMMC (SDCC v5) | **Works** | Enumeration, HFSX root, journal replay, reads and writes; log ring flushes |
| HFSX root in `userdata` | **Works** | `rd=emmcroot0`, read-write; no Android dual boot |
| Framebuffer / AppleCLCD | **Works** | 1080×2340, stride 4320, software composition via IOSurface |
| `launchd` / daemons | **Works** | `launchd`, `backboardd`, `SpringBoard`, `securityd`, `keybagd`, `usermanagerd`, etc. |
| iOS GUI | **Works** | Apple logo, progress bar and the iOS 16 lock screen |
| Home screen | **Works** | SpringBoard grid with app icons, the dock, a notification badge and the App Library pill (`docs/images/home-screen.jpg`); with the Bokeh wallpaper behind it; the Weather widget stays on its loading placeholder and the Calendar widget on a blank tile (see the widgets row below) |
| Settings | **Works** | The whole Settings list renders and scrolls (`docs/images/settings.jpg`) |
| Siri setup | **UI only** | The first-run Siri screen appears (`docs/images/siri-setup.jpg`); without a microphone or speaker Siri cannot answer |
| Skylight self-tests | **Pass** | The boot console prints the window server's software blit self-tests as `PASSED` (`docs/images/boot-self-tests.jpg`) |
| Code signing bring-up | **Works** | `cs_enforcement_disable=1`, `unentitled_ios_sim_launch=1`, `launchd_unsecure_cache=1` |
| Persistent kernel log | **Works** | 4 MiB `SANTONILOG1` ring on the SYSTEM partition |
| 4 Gold cores | **Works** | `cpus=4`; `SWEET CPU: requested=4 selected=4` |
| Panel orientation | **Works** | The lock screen and the home screen are portrait and upright, filling the 1080×2340 panel (`docs/images/lock-screen.jpg`, `docs/images/home-screen.jpg`) |
| Physical buttons | **Works** | Patch `0201`: Power/Volume Up/Volume Down through the SDM660 SPMI arbiter v3, delivered to iOS HID |
| USB network | **Works** | Patches `0200`, `0210`–`0213`: DWC3 + QUSB2 bring-up, CDC-NCM (Windows UsbNcm, macOS, Linux), `ping 192.168.7.2`, live kernel log on UDP 5140 (`sweetusb=2`; `tools/usbnet-windows.ps1`). Root SSH over the link (dropbear, `docs/SSH.md`) |
| Touch | **Works** | Novatek NT36672A on QUP1 I2C (bit-banged): patches `0193`–`0208`, `0230`, `0231`, `0234`. Taps, icons and the swipe-up unlock work; ~249 frames per 10 s with 0 I2C errors (~14 ms per read). `0234` makes SweetHID use the real 1080×2340 panel (it assumed 2400, which shifted touches and hid the bottom edge). Kernel must be built by the CI (Xcode 15): a local Xcode 16.2 build read garbage firmware info and left touch disabled |
| eMMC reads by ADMA2 | **Works** | Patches `0232`, `0233`: the SDCC v5 core has no SDMA, only ADMA2. Cold reads ~465 KB/s → ~16 MB/s, boot to the lock screen ~23 min → ~2–3 min. Writes stay PIO |
| Wallpaper | **Works** | The selected wallpaper (Bokeh) is drawn behind the home screen (`docs/images/home-screen.jpg`); the gallery opens, but most previews are still blank white (see Wallpapers below) |

## What does not work yet

| Area | Status | Notes |
| --- | --- | --- |
| GPU acceleration | **Works (not optimised)** | Own Adreno 512 kernel driver plus a Metal device plug-in ([lavender-metal](https://github.com/pcorezzola/lavender-metal)); `backboardd` draws the lock screen, Control Centre and wallpaper on the GPU. Animations and transitions are still slow (the CPU copy into the scanout IOSurface and shader compiles dominate; the GPU cannot touch memory above 4 GB). After a reboot the plug-in must be switched on (`lavmetal_switch on`) |
| Camera | **Missing** | Camera button and camera stack are not implemented |
| Wi-Fi / internet | **Missing** | No Wi-Fi driver or firmware for the WCN3990 radio, so no internet; the only network is USB |
| Bluetooth | **Missing** | No Bluetooth transport; `bluetoothd` respawns in a loop without hardware |
| Audio | **Data path verified, no sound** | ADSP/GLINK/APR, AFE, ADM, ASM and the 16 KiB tone work (12 × `WRITE_DONE`); the missing link is the LPASS digital codec/SoundWire master that feeds the PM660L's SWR DAC port. AP reads of `0x152c0000`/`0x152c1000` stall once the DSP audio is up. See [Audio](#audio). |
| Brightness | **Missing** | No backlight control: the brightness slider does nothing |
| Wallpaper gallery previews | **Partial** | The gallery opens and the wallpaper itself shows, but the previews are blank white (a real iPhone shows the picture, `docs/images/wallpaper-editor.jpg`); Astronomy (Metal) aborts on launch. See Wallpapers |
| Battery / charging UI | **Partial** | Static battery fallback (`100%`, 4200 mV); no gauge driver |
| Home screen widgets | **Partial** | The slots now draw: Weather shows a loading placeholder and Calendar a blank white tile. The Calendar widget extension spends minutes of CPU in SwiftUI layout on the slow cores and hands over no content yet |
| Screen colours | **To verify** | The date, clock and status bar render red/orange on the lock screen and home screen: wallpaper tint or a framebuffer colour-format issue, not checked yet |
| RTC / date | **Partial** | Patch `0220` reads the PM660 RTC. Its raw count is not Unix time (Android keeps its own offset), so each phone needs `sweetrtcoff=` measured once in TWRP (`docs/BOOT.md`); the home-screen clock still reads `mié 4 feb 05:28` instead of the real date, so this unit's offset is still to be set |
| `preboot` / data volume | **Partial** | `libignition` cannot find the preboot volume; the boot continues unsealed |
| Second boot / boot time | **Works** | The second boot runs without the data migration and reaches the lock screen in ~2–3 min since the ADMA2 read path (`0233`); it was ~18–23 min with PIO reads |

## Performance

Everything on screen is drawn by the CPU: QuartzCore falls back to its software OpenGL renderer (`CA::OGL::SW`) because there is no GPU. Measured on the device (4× Cortex-A73, panel 1080×2340):

| Metric | Value | How |
| --- | --- | --- |
| Frames presented | median **~13 fps** over a boot, peaks of **30–34 fps** while animating | `SWEET FB: swap` lines in the live log, counted per second (patch `0215`) |
| Panel refresh | **30 Hz** | `sweetfb_hz=30`; the framebuffer copies at most one frame every other 60 Hz vsync |
| `backboardd` CPU | **~150–170 %** (of 400 %) on the idle lock screen | `ps` cumulative CPU time over 60 s windows; sampled with `spindump` |
| Where the time goes | `CA::OGL::SW::scan_convert`, `image_sampler`, `tex_color_matrix_4x4_bias`, `tex_multiply_color`, `fill_round_rect` | `spindump` of `backboardd`, symbolised against the 20H392 dyld shared cache |
| eMMC read | **~16 MB/s** (was ~465 KB/s) | ADMA2 read path, `0233` |
| Touch | **~25 frames/s** read from the controller, 0 I2C errors | `WHYRED TOUCH: 10s frames` log line |
| Boot to lock screen | **~2–3 min** (was ~23 min) | live log timestamps |

What was tried to lower the compositor load, and what it did:

- Notifying the vsync to CoreAnimation at the panel rate instead of a fixed 60 Hz (`0235`): the idle lock-screen CPU stayed at ~150–170 %; not a measurable gain at idle, it only caps animations at 30 fps.
- *Reduce Transparency* and *Reduce Motion* in Settings: no change in idle CPU (still ~149 %).
- Freezing PosterBoard (the wallpaper process): no change, so the wallpaper is not what keeps the compositor busy. The idle lock screen still presents 7–16 frames per second; the process that drives them is not identified yet.

A real fix needs a GPU path (a kernel driver for the Adreno 512 plus a Metal-compatible layer, which does not exist for iOS) or a much cheaper software compositing path. Neither is started.

## Wallpapers

Opening *Settings → Wallpaper → Add New Wallpaper* used to show a blank gallery and abort PosterBoard. Findings:

- `lsd` could not load `/System/Library/xpc/extensions.plist` (AMFI is missing), so PlugInKit saw no extensions. The precompiled catalog was set aside so iOS scans natively, the LaunchServices databases were rebuilt once, and an extension-point definition for `com.apple.posterkit.provider` was added. `GradientPosterExtension` and `CollectionsPoster` now register and start.
- PosterBoard then aborted in `-[PBFHomeScreenPreviewImageBuilder _imageForView:]`: it renders the preview with Metal (`MTLCreateSystemDefaultDevice()` → `newCommandQueue`) and put a nil queue into an `NSDictionary`. A small replacement for that method (`tools/posterboard-preview.m`) avoids the crash.
- Result: the gallery opens and **Color shows a preview**. Applying it to the home and lock screens is still to be verified.
- **Astronomy** and other wallpapers that link Metal still fail: `AegirPoster` and `CollectionsPoster` are terminated with `extension check-in timeout` and the system gets very slow. This needs a Metal device, so it is the same limit as the GPU.

**Status on 2026-11-27.** Working: USB (network, SSH, live log), touch and graphics (GPU rendering, not optimised yet). Not working: audio, internet (Wi-Fi), brightness control, camera and Bluetooth.

**2026-11-27 — wallpapers working.** After repairing the root filesystem (the poster data store had unreadable files) and bringing `AegirPoster` back, the poster gallery opens and the home screen draws the wallpaper. The first photo is the home screen, the second *Settings → General → About* (iPhone X, iOS 16.7.16), the third the wallpaper editor next to a real iPhone: here the previews are still blank white.

| Home screen | About | Wallpaper editor (left: this port, right: a real iPhone) |
| --- | --- | --- |
| <img src="docs/images/home-screen.jpg" width="230" alt="Home screen with the Bokeh wallpaper"> | <img src="docs/images/settings-about.jpg" width="230" alt="Settings, General, About"> | <img src="docs/images/wallpaper-editor.jpg" width="300" alt="Wallpaper editor on the port and on a real iPhone"> |

## Milestones

- **2026-11-27 — Animated wallpaper with transparencies, PhotosPicker Metal pipeline, and LAVTSENS (`0385`).** With GPU batching/perf (`0365`–`0370`), TSENS temperature telemetry (`0371`, `lavtsens=30`), `LavenderMetal` (`newDefaultLibraryWithBundle:error:` + Mesa `libair.a` `array<texture2d/3d>` slot `memcpy`), and XNU sandbox `op 4`/`5`/`6` (`sandbox_container_path_for_pid` → `"/var/tmp"` + TCC Photos extensions), the lock/home screens render animated wallpapers (`CollectionsPoster`) with system transparencies on the Adreno 512 GPU (`shim-gpu-lavender-0385-picker-tsens.img`).
- **2026-11-26 — Audio data path verified, no sound yet.** The ADSP boots, GLINK/APR answers, the AFE `INT4_MI2S_RX` port starts, ADM routes and ASM plays: a 16 KiB 1 kHz buffer is consumed twelve times with `WRITE_DONE` events. The LPASS MCLK (`0x15020018 = 1`) and the PM660L clock block/PA are applied. There is no audible output because the LPASS digital codec/SWR master that feeds the PM660L's SWR DAC port is not clocked/configured; AP reads of that block stall once the DSP audio is up. See [Audio](#audio).
- **2026-11-21 — Home screen, Settings and Siri setup on screen.** A device screenshot shows SpringBoard's home screen with app icons, the dock, a notification badge and the App Library pill; the two widget slots wait on `Esperando…`. Settings opens with its full list and the first-run Siri screen appears. The boot console shows Skylight's software blit self-tests passing before the lock screen. Evidence: `docs/images/home-screen.jpg`, `settings.jpg`, `siri-setup.jpg`, `boot-self-tests.jpg`.
- **2026-11-19 — Phase 0 safety net.** Verified stock fastboot ROM, TWRP, 15 partition backups + GPT, and a complete restore path.
- **2026-11-19 — First XNU boot.** Kernel built for `LAVENDER` in GitHub Actions; the first boot reached `launchd` but died with `CS_KILLED`.
- **2026-11-19 — Code-signing bring-up.** The CS/RW boot arguments let `launchd`, `xpcproxy`, `cfprefsd`, `powerd`, `distnoted`, `watchdogd` and `SpringBoard` run.
- **2026-11-19 — eMMC writes fixed.** Patch `0192` waits for DAT0 + `TRAN`/`READY_FOR_DATA` (CMD13) after every PIO write. Before it, CMD24/CMD25 timed out and the log ring stayed empty; after it, the ring flushed 100+ KiB with zero write timeouts.
- **2026-11-21 — eMMC reads by ADMA2.** The SDCC v5 core has no SDMA, only ADMA2. Patch `0233` moves reads to ADMA2 (PIO fallback, writes stay PIO): cold reads go from ~465 KB/s to ~16 MB/s and the boot to the lock screen from ~23 min to ~2 min.
- **2026-11-21 — Touch fixed for good.** Local kernel builds read garbage firmware info from the Novatek (`nvt fwinfo invalid`) and left touch disabled; kernels from the CI (Xcode 15) do not. `0234` makes SweetHID use the real 1080×2340 panel, so the swipe from the bottom edge now unlocks and icon taps land where they should.
- **2026-11-21 — Wallpaper gallery opens.** Extension points and providers registered, the Metal-less preview crash worked around: Color shows a preview. Astronomy is still slow.
- **2026-11-21 — Compositor profile.** `spindump` shows the CPU going into CoreAnimation's software renderer; `0235` aligns the vsync notification with the panel rate. No idle gain.
- **2026-11-19 — Four Gold cores.** `cpus=4` brings up all four A73 cores; the A53 Silver cores stay parked because they cannot run 16 KiB pages.
- **2026-11-19 — First-boot data migration.** A fresh root image needs one long `DataMigration` pass. The image now carries `com.apple.DataMigration.plist` with `LastSystemVersion = 20H392`, so subsequent boots should skip it.
- **2026-11-19 — iOS 16 lock screen.** The device reached the iOS 16.7.16 lock screen: `Wednesday, December 31`, `4:37`, the camera shortcut and the home indicator are visible. This is the current state.
- **2026-11-20 — USB and button drivers (host-tested).** `sweet_usb.c` and `sweet_keys.c` were compiled for lavender but never called (their only caller is the UFS path), and they used other SoCs' addresses. Patches `0200`/`0201` port them to SDM660 with addresses from the phone's DTB and start them on lavender. On the device the keys worked at once; USB needed three more fixes found with ~1-minute `sweetusbhold` boots: `0210` powers the QUSB2 PHY ABL leaves off, `0211` brings the DWC3 core up as Linux does, `0212` releases the PHY's UTMI test mux.
- **2026-11-20 — Touch works: iOS home screen reached by touch.** The Novatek driver had been reading the controller in slow motion: on this port `delay()` never spins (the CPUs are registered with `powergate_latency` 0, so XNU blocks for every delay), and the bit-banged I2C does ~1800 delays per frame. `0230` times the bit-bang with a busy-wait: 0 I2C errors, frames in ~14 ms instead of seconds, and the setup screens, home screen and app icons respond to taps.
- **2026-11-20 — 30 s off every boot.** Every boot stopped for exactly 30.000 s while XNU waited for an `IORTC` resource nothing publishes; that wait is also why the calendar started at 1970. `0220` skips it (as XNU does for BCM2837) and reads the PM660 RTC: eMMC and USB now come up at ~16 s instead of ~46 s.
- **2026-11-20 — USB network on Windows.** `0213` turns the gadget into CDC-NCM: Windows 11 binds its in-box UsbNcm driver, `ping 192.168.7.2` answers (70/70, 1400-byte frames) and the kernel console streams live to the PC. No more TWRP round-trips to read a log.

## Audio

Status (2026-11-26): **the AFE/ADM/ASM data path is verified; there is no audible output yet.**
Patches `0500`–`0522`. The register-level detail and the full ladder are in
[`docs/AUDIO.md`](docs/AUDIO.md).

```
iOS userland HAL -> /dev/lavender-audio -> ASM stream -> ADM COPP
  -> AFE INT4_MI2S_RX (0x1036) -> LPASS digital codec / SoundWire master (0x152c0000 / 0x152c1000)
  -> PM660L analog codec (SPMI SID 3, 0xf000): SWR DAC port -> speaker PA/boost -> speaker
```

The PM660L is the speaker codec. The **WSA881x amplifiers are not fitted** on lavender
(`qcom,wsa-disable`; the `msm-sdw-codec` WSA children are `status = "disabled"`), and the
`tasha`/`tavil` SLIMbus codecs are disabled by the DTBO. The PM660L headset path uses the
same LPASS digital codec/SWR, so there is no AP-only shortcut for the headset either.

### What works

| Stage | Evidence |
| --- | --- |
| ADSP power/PAS | `0507`/`0508`: GDSC on, SMMU CBCR on, all 21 ADSP segments copied, `AUTH_AND_RESET` |
| GLINK `lpass` / APR | `0509`/`0509b`: descriptor 478, TX 479, RX 480, `VERSION_ACK`, `OPEN_ACK apr_audio_svc`, APR basic response |
| AFE | `0510b`–`0511c`: `INT4_MI2S_RX 0x1036` clock (IBIT `0x113`, 1.536 MHz), I2S config (48 kHz, 16-bit, SD0, stereo) and `DEVICE_START`, all status 0 |
| ADM | `0512`/`0513c`: `DEVICE_OPEN_V5` response `0x10329`, COPP `0`, `MATRIX_MAP_ROUTINGS_V5` status 0 |
| ASM | `0513`–`0513d`: `OPEN_WRITE_V3`, `MEDIA_FMT_UPDATE_V2`, `RUN_V2`, map regions, `RUN` status 0 |
| Tone buffer | `0514b`: 16 KiB contiguous buffer mapped and written; the DSP returned `ASM_DATA_EVENT_WRITE_DONE_V2` (`0x10D99`) |
| Repeated writes | `0517c`: with **map handle 0**, twelve 16 KiB writes produced twelve `WRITE_DONE` events and no hang |
| PM660L clock block/PA | `0514b`/`0520`: `MASTER_BIAS`/`RST_CTL`/`TOP_CLK`/`ANA_CLK` and speaker PA/boost writes return status 0; `0x15020018` (MCLK) reads 1 after the write |

### What is missing

- **The LPASS digital codec/SWR master is not clocked/configured for the AP.** Reading
  `0x152c0000` or `0x152c1000` after the audio subsystem is up stalls the kernel (`0519`,
  `0522`); the read-only probe at ~51 s, before the audio boot, returned zeros safely.
  The vendor `swr-mstr-ctrl` driver requests the **LPASS core/audio HW vote** before
  touching the block. `0526` shows the AFE vote opcode is implemented for **AVTIMER (2)**
  only: MACRO (3) and DCODEC (4) return `ADSP_EUNSUPPORTED`, so this port cannot enable
  those clocks through that command. The AFE NPL (`0x307`, 153.6 MHz) and INT_MCLK_1
  (`0x306`, 9.6 MHz) clocks are accepted (`0525`), but the SWR master APB write still
  stalls.
- **The PM660L SWR DAC port is never fed.** The AFE `INT4_MI2S_RX` starts and the tone is
  consumed by the ADSP, but the LPASS digital codec does not route that MI2S to the SWR
  master, so the PM660L DAC sees no data. The PM660L analog PA/boost being on only makes
  silence louder.
- **The vendor mixer path is `INT4_MI2S_RX -> RX4 MIX1 INP1=RX4 -> COMP1 -> SpkrLeft
  COMP/BOOST/VISENSE -> SWR DAC_Port`**, i.e. the PM660L's internal SWR DAC port, not an
  external amplifier.
- **VDD_SPK/BOB and the PM660L supplies are RPM-managed**; this port has no RPM audio
  regulator driver. The PM660L registers are accessible over SPMI, but the speaker boost
  supply state is not verified.

### Patch ladder (audio, 0500–0599)

| Patch | Purpose | State |
| --- | --- | --- |
| `0500` | read-only LPASS/ADSP probe | device-verified |
| `0501`–`0502` | LPASS power-up and ADSP firmware dry run | device-verified |
| `0503`–`0508b` | TrustZone PAS load, SMP2P, carve-out copy | device-verified |
| `0509`/`0509b` | GLINK `lpass` + APR (`apr_audio_svc`) | device-verified |
| `0510` | lavender SPMI arbiter v3, PM660L SID 3 reads | device-verified |
| `0510b`–`0511c` | AFE clock/I2S/start, `mem_mapping_hdr`, 32-byte APR header | device-verified |
| `0512`–`0513d` | ADM open/route, ASM open/format/run, intents | device-verified |
| `0514`/`0514b` | 16 KiB 1 kHz tone + PM660L PA/boost | data path verified, no sound |
| `0515`/`0515b` | read-only PM660L clock/PA state | device-verified |
| `0516`–`0518` | MCLK register, clock block, PA with map handle 0 | device-verified, no sound |
| `0519` | read-only LPASS digital codec state | **hangs the kernel; do not use** |
| `0520` | LPASS MCLK `0x15020018 = 1` | device-verified |
| `0521` | primary MI2S `INT0 0x102e` experiment | device-verified, no sound (the speaker is INT4) |
| `0522` | read-only SWR master state | **stalls at the read**; the block is not AP-accessible while the DSP owns it |
| `0523` | AFE remote LPASS vote (`0x100f4`) | status 3 (`unsupported`) for MACRO/DCODEC |
| `0524` | SWR master soft reset | **stalls the audio thread** |
| `0525` | SWR NPL (`0x307`) + INT_MCLK_1 (`0x306`) AFE clocks | clocks accepted; SWR write still stalls |
| `0526` | vote variants (blocks 2/3/4, src 1) | only **AVTIMER (2)** returns a handle |

### Traps and lessons

- **SM6225 addresses do not apply.** The lavender SPMI arbiter v3 is at `0x0800f000`,
  observer `0x09400000 + 0x8000*apid`; the PM660L is SPMI **SID 3** (`pm660l@3`), base
  `0xf000`; the keys PMIC is SID 2.
- **APR headers are 32 bytes.** The response payload starts at `hdr_len*4`
  (`hdr_len = 8`), not at a fixed 20/32; the first `SET_PARAM_V2` attempts were rejected
  because the 12-byte `mem_mapping_hdr` was missing.
- **Intents must be freed on `RX_DONE`/`W_REUSE`.** The ADSP's `CMD_INTENT` uses `p1 = 2`;
  filtering by `p1 == rcid` was the GLINK bug.
- **ADM's `DEVICE_OPEN_V5` response is `{u32 status; u16 copp_id; u16 reserved}`**, and
  the COPP is **0**. Parsing `0x408` from an older layout produced `EBADPARAM`.
- **ASM commands use `src_port = dest_port = (session << 8) | stream = 0x0101`**; APD/ADM
  use port 0.
- **The ASM map handle is not `0xb095aad8`.** The DSP consumes the write with **map
  handle 0**; using the value from the map response made the DSP never emit
  `WRITE_DONE`. This was the single biggest fix in the data path.
- **16 KiB per write.** A 64 KiB `ASM_DATA_CMD_WRITE_V2` hangs the DSP/kernel; repeated
  writes must wait for the `0x10D99` WRITE_DONE event (12 × 16 KiB worked).
- **The AFE clock set `0x306` does not generate the codec MCLK.** The real MCLK enable is
  `audio_ext_clk`'s `qcom,mclk-clk-reg` at **`0x15020018` write 1** (read back 1).
- **The PM660L clock block order matters**: `MASTER_BIAS` (0x146), `CDC_RST_CTL`
  (0x46), `TOP_CLK_CTL` (0x48), then `ANA_CLK_CTL` (0x49) and the PA/boost registers.
  Enabling `ANA_CLK`/PA without the clock block and MCLK hung the phone in `0514c`.
- **The speaker is INT4_MI2S_RX, mono**, not INT0; the vendor `mixer_paths.xml` confirms
  it. INT0 is the headset/handset path.
- **The LPASS SWR/digital-codec block is not AP-accessible once the DSP audio is up.**
  Read-only probes at boot (before the audio firmware runs) return zeros; after the ADSP
  is running, the same reads stall. Do not add another read-only probe of `0x152c0000` or
  `0x152c1000` without first solving the block's clock/power.
- **The AFE LPASS vote only supports AVTIMER.** `0x100f4` with block 2 returns a handle;
  blocks 3 (MACRO) and 4 (DCODEC) return `ADSP_EUNSUPPORTED` (3). Do not retry those two.
  The SWR NPL/MCLK clocks (`0x307`/`0x306`) are accepted but do not make the SWR master
  APB accessible: its write still stalls.

### Next milestones

1. **DSP topology or LPASS clock controller.** The AFE vote can only enable AVTIMER, so
   the SWR master must be brought up by the AFE topology (`AFE_CMD_ADD_TOPOLOGIES`, ACDB
   id 15) or by the LPASS clock controller before any AP register access.
2. **SWR master init** (`swr-mstr-ctrl.c` sequence: reset, frame row/col, clock divider,
   bus control) at `0x152c1000`.
3. **PM660L SWR DAC port** route (`SpkrLeft SWR DAC_Port`, RX4/COMP1) and speaker
   PA/boost, with VDD_SPK/BOB verified.
4. **Userland**: `/dev/lavender-audio` + HAL plug-in, then mic/headset detection.

The shims used in this phase are archived in `artifacts/shim/`; the boot argument
`lavaud=N` selects the ladder step (`lavaud=14` = full data path + PA; `lavaud=15` =
read-only state probes; `lavaud=13` = data path without the PM660L PA).

## Hardware

| | Redmi Note 7 (`lavender`) |
| --- | --- |
| SoC | Qualcomm SDM660 / Snapdragon 660 |
| CPU | 4× Cortex-A73 Gold + 4× Cortex-A53 Silver |
| Page size | 16 KiB on Gold only; Silver parked |
| RAM / storage | 4 GB / 64 GB eMMC 5.1 |
| eMMC controller | Qualcomm SDCC v5 at `0x0c0c4000`, GIC SPI 110 |
| Display | 1080×2340, stride 4320, framebuffer `0x9d400000` |
| Touch | Novatek NT36672 (QUP1 I2C, `0x62`); Synaptics is the alternative panel |
| PMIC | PM660 / PM660L |
| GPU | Adreno 512 (own kernel driver + Metal plug-in, not optimised) |
| Bootloader | Unlocked Xiaomi ABL; `fastboot boot` is stubbed on this unit |

See [`docs/HARDWARE.md`](docs/HARDWARE.md) for the full audit and constraints.

## How it boots

```
Xiaomi ABL
  -> shim in the recovery partition (Power + Volume Up)
  -> XNU kernel + ADT + boot arguments
  -> HFSX root from userdata (rd=emmcroot0)
  -> launchd -> mount-phase-1/2 -> backboardd -> SpringBoard
  -> iOS 16 lock screen
```

The shim is flashed to `recovery`; `fastboot boot` is stubbed by this ABL. `fastboot reboot recovery` boots Android and MIUI restores the stock recovery, so the working sequence is: flash the shim, power off, then **Power + Volume Up**.

The current boot arguments include:

```
rd=emmcroot0 emmcrw=1 rootrw=1
cs_enforcement_disable=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1
sweetlog=1 sweetfb_noconsole=1 sweetfb_stayon=1 sweetfb_hz=30
wdt=-1 wdt_test=no_user_checkin cpus=4
```

See [`docs/BOOT.md`](docs/BOOT.md) for the full chain and the log-pull procedure.

## Build

The repository does not contain Apple binaries, the IPSW, the 16 GiB root image or the stock ROM. You need to provide those yourself.

1. **Kernel**: push a change to `xnu/patches/` (or run the workflow manually) and `.github/workflows/xnu.yml` builds `kernel.development.lavender` on `macos-15`.
2. **Shim**: `tools/build-shim.sh <kernel>` packs the kernel, ADT and boot arguments into an Android boot image.
3. **Root image** — extract `OS.dmg`, `SystemOS.dmg` and `AppOS.dmg` from the iPhone X 16.7.16 IPSW and build the HFSX image with `bergamota/tools/mkiosroot-macos.sh` plus `sweet-root-tweaks.py`.
4. **Flash** — `fastboot -S 512M flash userdata ios16-root.simg`, then `fastboot flash recovery shim-boot.img`, then Power + Volume Up.

The exact commands, hashes and caveats are in [`docs/BUILD.md`](docs/BUILD.md).

## Repository layout

```
README.md                 this file
README.es.md              Spanish summary
docs/                     hardware, build, boot, audio, achievements, roadmap, legal
  images/                 lock screen, home screen, Settings, Siri setup and boot self-tests
xnu/patches/              the complete XNU patch series (0001-0192 base, 0200+ lavender)
shim/                     the boot shim (Android boot image -> XNU)
resources/lavender_dtb.bin the board DTB the ABL accepts
drivers/                  generated mirrors of the main drivers (read-only views of the patches)
tools/                    shim/ADT/payload builders, log pull/decoder, root and DataMigration helpers
tests/                    patch checks and host tests; tests/host/ runs the USB/keys drivers on simulated registers
.github/workflows/xnu.yml kernel CI
```

The repository holds everything needed to build the lavender kernel and shim. Apple's XNU source comes from Apple's open-source release at build time. Apple binaries (IPSW, ADT, root image) are never included.

The audio bring-up (the PM660L/LPASS/ADSP chain, patches `0500`+) is summarised in [Audio](#audio); the register-level notes, patch ladder and traps are in [`docs/AUDIO.md`](docs/AUDIO.md).

## Companion repository: lavender-metal

The GPU work lives in its own repository, [pcorezzola/lavender-metal](https://github.com/pcorezzola/lavender-metal): 
the Adreno 512 (A5xx) bring-up notes, the host-side register layer and PM4 builder with a fake command processor, and the 
plan towards a Metal-compatible layer. The kernel driver itself is shipped here, as patches `0236`-`0243`, `0249`-`0253` 
and `0300`+ in `xnu/patches/` (source mirror in `drivers/gpu/lavender_gpu.c`), and is enabled with the `lavgpu=N` boot-arg. 
GPU patches use the number range 0300-0399.

## Credits

- [`aLpHa-Git-69/ios-lavender`](https://github.com/aLpHa-Git-69/ios-lavender) — the lavender/whyred XNU tree, eMMC driver, shim and notes. This port would not exist without it.
- [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet) — upstream tooling and bring-up methodology.
- [`ry-diffusion/bergamota`](https://github.com/ry-diffusion/bergamota) — `mkiosroot-macos.sh`, shim ideas and the Poco F1 port.
- [`MakrSas/ios-topaz`](https://github.com/MakrSas/ios-topaz) — reference for the A73/A53 topology, Wi-Fi and GPU work.
- [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) — methodology reference for XNU-on-Android.
- Apple's open-source XNU (`xnu-8796`) and the iOS 16.7.16 userland, which are **not** redistributed here.

## Legal

This is a research project. XNU is open source under the Apple Public Source License; the iOS userland, IPSW, device trees and other Apple binaries are **not** distributed in this repository. Downloading and using Apple firmware may be subject to Apple's EULA and local law. Do not use this project with personal data, an Apple ID, or as a daily driver. See [`docs/LEGAL.md`](docs/LEGAL.md).

## Contributing

The most useful next steps are faster touch (hardware QUP I2C instead of bit-bang), the boot time (~18 min to the lock screen) and the system-wide `delay()` issue (CPU `powergate_latency` 0). If you have a lavender unit and can test, open an issue with the boot log and the exact panel/touch variant.
