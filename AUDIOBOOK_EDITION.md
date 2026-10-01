# Sonix Player Audiobook Edition

This fork is an audiobook-focused development track for the HiBy R1. It is
based on [Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player) and stays
close enough to upstream to continue receiving its player, hardware, and
format improvements.

The existing [HiBy R1 Audiobook
Mod](https://github.com/yetisoldier/Hiby-R1-Audiobook-Mod) remains the stable
option for people who want the stock HiBy interface. This project is the
next-generation option: an open player whose catalog, playback, resume,
controls, and interface can be changed directly.

## What Sonix already provides

- A separate `Audiobooks` catalog rooted at `/Audiobooks`.
- Library, Authors, Series, Continue, and Finished views.
- Multipart folder books in natural file order and direct resume into the
  saved part, without stepping through earlier files.
- Embedded chapters in M4B/MP4, MP3 ID3 chapter frames, and Vorbis comments.
- Configurable playback speed, forward/back intervals, sleep timer, stop at
  chapter end, and rewind-after-pause.

This fork builds on those native features instead of reproducing the scripts
and UI workarounds required by the stock-player mod.

## Priorities

1. Reliable per-book resume for single-file and multipart books.
2. Fast Titles, Authors, Series, Continue, Finished, and Folders views.
3. Responsive controls while scanning or writing to slower SD cards.
4. Predictable behavior across card swaps, reboots, sleep, USB, Bluetooth, and
   wired output.
5. Recovery and validation tooling suitable for public R1 firmware releases.

## Implemented audiobook changes

- Periodic resume checkpoints run on a background worker instead of the LVGL
  interface thread.
- Pause, seek, stop, card removal, and shutdown still flush the latest position
  before continuing.
- A queued position is tied to the currently open audiobook database, so a late
  write from a removed card cannot be applied to a replacement card.
- A book stopped within 45 seconds of its final part is considered completed;
  its next play starts from the beginning.
- The host simulator keeps its databases on `SONIX_SD_ROOT` instead of
  incorrectly treating every simulated card as read-only.
- A headless R1 simulator test runs in CI. It scans a generated multipart MP3
  book and M4B, saves a listening position, restarts the player, and verifies
  that Continue resumes the correct part directly at that position.
- The Audiobooks home screen exposes Library, Series, Authors, Continue,
  Bookmarks, and Folders as direct views with distinct icons.
- Manual bookmarks can be added from Now Playing, opened per book, listed
  globally, resumed directly, and deleted.
- Book summaries are indexed from common MP4/M4B description/comment atoms and
  ID3 comment or description fields, with bounded allocations for malformed
  tags.
- The Folders view uses the indexed card hierarchy instead of walking the SD
  card synchronously on every tap.
- Album art and controls remain in the ordinary Now Playing surface, with
  audiobook skips, speed, chapters, summary, bookmarks, and sleep behavior.

## R1 reliability work

- The firmware packer converts shell scripts to Unix line endings and refuses
  to package a touch-loader script that still contains carriage returns.
- Touch is selected by `EV_ABS` plus `INPUT_PROP_DIRECT`; physical buttons are
  selected by their kernel device names. No feature relies on touch being
  `/dev/input/event1`.
- An adversarial device test occupies `event1` with a non-touch absolute-input
  device, reloads the real panel as `event4`, and verifies that UI touch and
  wake-key monitoring both open `event4` while hardware controls retain their
  own nodes.
- A local ADB control FIFO can tap, swipe, wake, query, and capture the real R1
  framebuffer without replacing the physical touchscreen.

## Testing on a PC

On Debian or Ubuntu, install the host dependencies listed in the main README,
plus `ffmpeg`, `sqlite3`, `xvfb`, `xdotool`, and `scrot`. Then run:

```bash
tools/audiobook-host-smoke.sh
```

The script uses the real R1 interface and player code in a headless display.
Its databases, screenshots, and logs are left in
`build/audiobook-host-smoke/` for inspection. The upstream Makefile currently
requires a checkout path without spaces. During quick iterations, set
`SONIX_SMOKE_SKIP_BUILD=1` to reuse an existing host binary.

## Upstream relationship

`upstream` is the original Sonix Player repository. Fork-specific work is kept
in focused commits so useful fixes can be proposed upstream and future upstream
changes can be reviewed before merging.

Sonix Player is GPL-3.0 software. This fork retains that license and the
original project history and attribution.

## Release status

Audiobook Edition 0.1.0 is a public preview for the original HiBy R1. Target
and host builds, ABI checks, catalog integrity, single- and multipart resume,
manual bookmarks, automatic part advance, live UI control, clean boot, and
normal plus delayed touch-probe scenarios have passed. Long-duration playback,
very large personal libraries, and the broad range of USB/Bluetooth hardware
still benefit from community testing.
