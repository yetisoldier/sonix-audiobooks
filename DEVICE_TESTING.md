# Device UI Testing

Development builds expose a root-only FIFO at `/tmp/sonix-control`. It is a
local ADB test interface, not a network service. Commands feed a second LVGL
pointer and do not replace or disable the R1's physical touchscreen.

The main event loop already waits in `poll()`, so the FIFO adds no periodic
timer or worker thread while idle. Its status is written to
`/tmp/sonix-control.status`.

## Windows helper

With ADB enabled and the R1 connected:

```powershell
./tools/sonix-device-control.ps1 status
./tools/sonix-device-control.ps1 tap -X 360 -Y 420
./tools/sonix-device-control.ps1 swipe -X 420 -Y 650 -X2 420 -Y2 200 -Duration 500
./tools/sonix-device-control.ps1 wake
./tools/sonix-device-control.ps1 screenshot -Output ./screen.png
```

Coordinates match the upright 480x800 framebuffer screenshot. The device
translates them when its 180-degree display option is active.

## Direct ADB use

```bash
adb shell "printf 'tap 360 420\n' > /tmp/sonix-control"
adb shell "printf 'swipe 420 650 420 200 500\n' > /tmp/sonix-control"
adb shell "printf 'wake\n' > /tmp/sonix-control"
adb shell "cat /tmp/sonix-control.status"
```

`tap` accepts an optional hold time in milliseconds. `swipe` accepts an
optional duration. Both are clamped to the screen and to a maximum of ten
seconds. A tap sent while the screen is blank wakes it and waits briefly for
the panel before pressing.

## Input probe-order test

`tools/uinput-abs-holder.c` creates a harmless absolute-input device without
the Linux `INPUT_PROP_DIRECT` touch property. It is used only during a
controlled ADB test to occupy the usual touch event number before the real
panel driver is reloaded. This proves the player discovers the panel by its
capabilities rather than by `/dev/input/event1`.

Build it with the R1 cross compiler:

```bash
mipsel-rockbox-linux-gnu-gcc -Os -static -Wall -Wextra \
  -o uinput-abs-holder tools/uinput-abs-holder.c
```

The full procedure stops the supervised player, unloads and reloads the touch
module, and must finish with a reboot. It is a developer recovery test, not a
normal user command. A passing run has the holder at `event1`, `hyn_ts` at a
later event, the LVGL pointer and touch wake-key thread both on `hyn_ts`, and no
player descriptor on the holder.

## Release checks performed on the R1

- Compare SHA-256 of the packaged and installed `/usr/bin/sonix_player`.
- Confirm one `sonix_launch` parent and one `sonix_player` child after boot.
- Confirm `hyn_ts` has `PROP=2` and no invalid-I2C errors in `dmesg`.
- Check player RSS/thread count and the devcontrol FIFO while idle.
- Pull `.local/audiobooks.db`, run `PRAGMA integrity_check`, and inspect book,
  part, bookmark, and finished counts.
- Open the six audiobook views, resume a saved book, verify progress advances,
  and pause it again.
