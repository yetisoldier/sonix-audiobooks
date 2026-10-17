# Sonix Audiobooks: Project Notes

Sonix Audiobooks is an audiobook-focused fork of [Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player) for the original **HiBy R1**.

Since the rebase onto upstream v1.3.0, the audiobook experience the fork
originally built is upstream's own: upstream 1.2.0 absorbed the fork's
stability work and then built bookmarks, summaries, background position
saves, authors and series on a schema of its own (v4, with a card-epoch
guard and a background writer). This document describes what the fork now
adds on top, rather than claiming upstream's features for itself.

The existing [HiBy R1 Audiobook
Mod](https://github.com/yetisoldier/Hiby-R1-Audiobook-Mod) remains the stable
option for people who want the stock HiBy interface.

## What upstream 1.3.0 provides natively

All of this is inherited from upstream and not fork-modified:

- A separate `Audiobooks` catalog rooted at `/Audiobooks`, scanned on demand.
- Library, Series, Authors and Continue views; a Finished shelf and manual
  bookmarks as corner buttons, plus per-book bookmarks and summaries.
- Multipart folder books in natural file order, with direct resume into the
  saved part.
- Embedded chapters in M4B/MP4, MP3 ID3 chapter frames, and Vorbis comments.
- Resume checkpoints on a background writer (a slow card never holds up the
  interface), forced saves on pause/seek/stop/power-off, and the 45-second
  finish window: a book stopped within 45 s of its end is completed and
  starts over next time.
- Configurable playback speed, jump intervals, sleep timer (including end of
  chapter) and rewind-after-pause.
- The host simulator and the two-device (R1 / R3 Pro II) build.

## What this fork adds

- **Audiobookshelf integration** (Streaming > Audiobookshelf): server
  libraries, author and series browsing, server-wide search, downloads with
  artwork/series/description sidecars, linking of existing SD-card copies,
  and offline-first progress sync. Listen with Wi-Fi off; progress uploads
  once per saved checkpoint after reconnect, newer server progress imports
  on the next Play, and every state is visible on the Now Playing screen.
  See [docs/AUDIOBOOKSHELF_SYNC.md](docs/AUDIOBOOKSHELF_SYNC.md).
- **The host test suite**: an end-to-end audiobook simulator smoke
  (catalog, resume, bookmarks, auto-advance), a five-mode Audiobookshelf
  smoke against a mock server, an AddressSanitizer/UBSan outbox and
  conflict-decision unit test, and a metadata/kernel-log regression suite.
- **ADB device UI automation** (`devcontrol`): a root-only FIFO that drives
  a second LVGL pointer for controlled validation, leaving the physical
  touchscreen alone. Windows helper in `tools/sonix-device-control.ps1`.

## Priorities

1. Keep the fork's only unique runtime feature — Audiobookshelf sync —
   correct across offline listening, card changes, conflicts and reboots.
2. Follow upstream: take its player, hardware and format work wholesale, and
   re-apply the small fork layer on each pull (see R6 in the rebase plan:
   the three `resume_*` columns and `AUDIOBOOK_ABS_SYNC` ride upstream's
   scan and must be re-checked on every future upstream merge).
3. Recovery and validation tooling suitable for public R1 firmware releases.

## Device scope

The public Sonix Audiobooks release supports the original HiBy R1 only. The
upstream tree also builds for the R3 Pro II, and the Audiobookshelf feature
is written against the shared code base, but no R3 Pro II firmware is
released or tested by this fork.