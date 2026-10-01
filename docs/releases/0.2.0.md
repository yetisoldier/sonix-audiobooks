# Sonix Audiobooks 0.2.0

An early public feedback release for the **original HiBy R1 only**, based on Sonix Player. Do not install on the R1 MIDI or R3 Pro II.

## New since 0.1.0

- Optional Audiobookshelf setup, library browsing, downloads with artwork and summaries, and linking compatible existing SD-card books without downloading a second copy.
- Offline-first progress sync: listen with Wi-Fi off, then turn it on to synchronize. Pending checkpoints survive book changes, server outages and reboot. Sync never turns Wi-Fi on itself.
- Two-way progress reconciliation. The newest listening timestamp wins, including a newer rewind. If timestamps are missing, implausible or tied, the farther whole-book position wins instead.
- Direct mapping between server progress and the correct file/position in multipart books.
- A compact, centered Now Playing status showing local-only, waiting for Wi-Fi, syncing, synced or retry-pending states.
- Safer resume imports, including protection against concurrent local edits, replacement-card responses and stale boot-time playback positions.
- Selective upstream compilation-artist display improvements, additional compilation tag parsing, and bounded kernel-log processing. Larger upstream audio-engine changes were deliberately not included.
- Additional path handling, allocation and cleanup fixes, plus simulator, sanitizer and device validation tools.

See the [full changelog](https://github.com/yetisoldier/sonix-audiobooks/blob/audiobook-v0.2.0/CHANGELOG.md) and [Audiobookshelf guide](https://github.com/yetisoldier/sonix-audiobooks/blob/audiobook-v0.2.0/docs/AUDIOBOOKSHELF_SYNC.md).

## Install

1. Download the attached `r1.upt` and copy it to the root of your microSD card. Keep that exact filename.
2. Charge above 30% and keep the official HiBy R1 firmware available for recovery.
3. Open **Settings > System > Update firmware > From SD card**. From stock HiBy firmware, use its normal SD update command.
4. Do not remove power or the card during installation. After reboot, refresh the Music/Audiobooks libraries as needed.
5. Remove or rename `r1.upt` after installation to avoid selecting it accidentally.

Use `/Music` and `/Audiobooks` at the card root. Audiobookshelf is optional: local playback needs no account or network. To use it, open **Streaming > Audiobookshelf** and configure your server URL and API token. Do not share your token in bug reports.

## Known limitations and useful feedback

- This is community-tested firmware, with short device validation on one R1, not exhaustive certification. Reports about large libraries, USB-C audio, Bluetooth and long listening sessions are especially useful.
- Clock checks cannot detect every historical clock error. The farther-position fallback may discard an intentional rewind when its timestamp cannot be trusted.
- Incoming progress does not jump an actively playing book. A paused book takes the imported position on its next Play and may display the older position until then.
- The server API does not provide an atomic conditional progress write. Simultaneous listening on two devices can still race in the short read/write interval.
- Local bookmarks are not synchronized. Existing local books need an explicit link; matching a title alone does not enable sync.
- API tokens remain private device configuration. ADB is disabled by default and is not required for ordinary use.

When reporting a problem, include **0.2.0 / build 011020261348**, the file format, folder layout, audio output, Wi-Fi state and reproduction steps. Never include credentials.

## Validation and checksums

Host and R1 target builds passed. Tests cover download/linking, multipart resume, offline/restart recovery, local/remote rewinds, timestamp fallback, completion, stale responses and card isolation. The packaged executable was verified on the R1 and the new status layout checked on its screen.

`r1.upt`: 49,418,240 bytes

SHA-256: `c0a1f23971497be584965a9cbd52a7783b920f6d3c63c5de426124ce61d6b390`

Thanks to [Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player) and the HiBy modding community. GPL-3.0 source is included in this repository; the fork preserves upstream attribution.
