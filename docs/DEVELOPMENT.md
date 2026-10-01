# Development guide

This guide is for contributors building Sonix Audiobooks, not for people installing a release.

## Repository layout

- `sonix-player/`: player source and host/target builds.
- `sonix-packer/`: firmware packer, stock update inputs, and per-device overlays.
- `sonix-packer/assets/R1/`: original-R1 resources installed into the firmware image.
- `tools/`: host smoke tests, package checks, and controlled device-test helpers.

## Host build and audiobook smoke test

On Debian or Ubuntu, install the normal Sonix build dependencies plus `ffmpeg`, `sqlite3`, `xvfb`, `xdotool`, and `scrot`.

```bash
cd sonix-player
make host -j$(nproc)
cd ..
tools/audiobook-host-smoke.sh
```

The smoke test uses the real R1 UI in a headless X display. It creates disposable media and a disposable card under `build/audiobook-host-smoke/`, then validates multipart indexing, bookmarks, persisted direct resume, and automatic part advance.

For Audiobookshelf work, run the dedicated smoke suite:

```bash
tools/audiobookshelf-host-smoke.sh
SONIX_ABS_EXISTING=1 tools/audiobookshelf-host-smoke.sh
SONIX_ABS_EXISTING=1 SONIX_ABS_LOCAL_PROGRESS=1 tools/audiobookshelf-host-smoke.sh
```

## Target build

The first target build downloads and builds the MIPS toolchain and static dependencies. Keep the checkout in the Linux filesystem under WSL rather than a Windows-mounted path; dependency tracking and cross compilation are substantially faster there.

```bash
cd sonix-player
make target -j$(nproc)
```

The target build includes an ABI check that rejects glibc symbols newer than the R1 firmware provides. Do not bypass it: a binary that cannot start can look like a boot loop on the device.

## Package an original R1 firmware image

Place the official R1 firmware input beside `sonix-packer/sonix_firmware_packer.sh` as `r1_original.upt`, copy `sonix_player` and `sonix_launch` from the target build into `sonix-packer/`, then run:

```bash
cd sonix-packer
./sonix_firmware_packer.sh
```

The output is `r1.upt`. It is for the original R1 only. Before sharing an image, compare the packaged target binary with the installed `/usr/bin/sonix_player` by SHA-256 and perform the R1 checks listed in [DEVICE_TESTING.md](../DEVICE_TESTING.md).

## Device automation

An ADB-enabled development build exposes a root-only FIFO at `/tmp/sonix-control`. It drives a second LVGL pointer and does not replace the real touchscreen. Use the Windows helper or the command examples in [DEVICE_TESTING.md](../DEVICE_TESTING.md).

The automation interface is for controlled validation only. It is not a remote control service, and ADB remains disabled by default in normal user builds.

## Upstream and licensing

Keep fork-specific commits focused and preserve the GPL-3.0 license and upstream copyright notices. Bug fixes that are not specific to Sonix Audiobooks should be suitable for proposing back to [Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player).
