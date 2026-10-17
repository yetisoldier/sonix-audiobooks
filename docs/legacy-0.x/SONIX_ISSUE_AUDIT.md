# Sonix Issue Audit

This document records the root-cause work performed for the main open Sonix
issues considered by this fork. A host test can prove software behavior, but
device-specific audio and Bluetooth changes remain candidates until they have
also passed the hardware checks listed below.

## MP3 gapless playback

**Root cause:** MP3 encoder delay and padding were already trimmed by the
decoder. The remaining interruption came from timing: natural end-of-track was
noticed by a 500 ms UI poll, while the audio thread held the PCM open for only
600 ms. Metadata lookup and decoder startup could not reliably finish in the
small time left.

**Change:** The audio thread now posts a completion event to the GUI immediately
at natural EOF. The existing latched completion state remains as a fallback if
the GUI queue is full. The audiobook host smoke test verifies both automatic
multipart advance and reuse of the open PCM.

**Hardware check:** Play consecutive MP3 tracks with matching sample format and
gapless enabled. Confirm there is no inserted silence at the boundary.

## Bluetooth pairing failures

**Root cause:** Some devices complete and persist the BlueZ bond but reply to
`Pair()` too late. Sonix treated the D-Bus timeout as final even when the device
was already paired, which explains a failed pairing dialog followed by the
device appearing after Bluetooth was toggled.

**Change:** After a failed or timed-out `Pair()` call, Sonix now reconciles the
result against BlueZ's durable `Paired` property for two seconds. A confirmed
bond proceeds to connection instead of being discarded.

**Hardware check:** Remove and pair the affected earbuds from a clean state,
then reconnect after toggling Bluetooth and after rebooting.

## R3 Pro II suspend pop

**Root cause:** Suspend parked the output route but did not digitally mute the
shared DAC before its rails changed. A second bug left the route parked if the
kernel suspend write failed.

**Change:** Applicable shared-DAC devices are digitally muted before route
parking, allowed 30 ms to settle, then restored after output reinitialization.
Restore now runs after every attempted suspend, including failed attempts. The
R1's CS43131 path remains unchanged.

**Hardware check:** This requires an R3 Pro II. Test repeated screen-off
suspend/resume with sensitive wired earphones, both while stopped and playing.

## USB-C volume

The upstream software-volume implementation already addresses the reported
coarse and arbitrary USB DAC levels: the USB hardware endpoint stays at 0 dB
and Sonix applies smooth software attenuation. No additional fork-specific
patch was added.

**Hardware check:** Test volume 0, 1, several middle levels, and maximum with a
USB-C DAC that previously reproduced the jump.

## R1 charge current

The R1's AXP2101 PMIC exposes a clear software charge-current field. Register
`0x62` value 11 is 500 mA. The existing implementation never raises a lower
hardware setting, reasserts the cap periodically, and restores the previous
value if the user disables it.

**Change:** The 500 mA cap is now enabled by default on applicable PMIC-charged
devices. This is separate from the optional battery-voltage/approximately-80%
limit and does not alter battery-percentage reporting.

**Hardware check:** Confirm the boot log reports the 500 mA limit enabled and,
if measurement equipment is available, verify battery charge current does not
exceed the target. USB input current may be higher because it also supplies the
running player.

## Audiobook organization

Sonix now provides audiobook library, Continue, author, series, ordered parts,
chapters, bookmarks, and per-book resume behavior. Manual metadata correction
and books belonging to more than one series require a broader catalog data
model and are tracked as future enhancements rather than stability fixes.

## R1 frozen touchscreen after firmware update

**Root cause:** The R1 CST8xx/Hynitron loader was copied into the firmware with
Windows CRLF line endings. It has no shebang, so the packer's earlier shebang
normalization skipped it. The carriage returns broke the shell continuations,
and `insmod` received invalid or missing I2C parameters. When touch failed to
register, Sonix's fixed `/dev/input/event1` path could open the ADC keyboard as
a pointer instead.

**Change:** The loader is stored with Unix line endings, all packaged shell
scripts are normalized, and the packer rejects carriage returns in module
loaders. Sonix waits for an input device with both absolute axes and the Linux
direct-touch property. Built-in button and wake-key threads use kernel device
identity and the discovered panel node rather than fixed event numbers.

**Hardware check:** Clean boot registers `hyn_ts` at `event1`. A controlled
late-probe test places a non-direct absolute device at `event1`, reloads
`hyn_ts` at `event4`, and verifies that both LVGL and the touch wake-key thread
open `event4`; the UI remains responsive.
