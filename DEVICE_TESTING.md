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
