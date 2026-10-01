# Selective upstream integration, 2026-10-01

This is a development-candidate review, not a public-release announcement.

Reviewed upstream `main` at `2ef6ecfb65e8f63ffff7ce366c4a6bf4c5b6de90`, fetched on October 1. The fork's common ancestor is `c096c7a0fa34c63f18df2debbdf5ff91afae7e06`. Five upstream commits are beyond that ancestor; the earlier four-commit summary missed the podcast/audio commit.

## Included

The metadata/display portions of [eed607f](https://github.com/Jepl4r/sonix-player/commit/eed607f2842bacb61a08c33d373e3a0bfdb257cf) are selectively adapted, not cherry-picked wholesale:

- Recognize compilation markers: ID3 TCMP/TCP/TXXX, MP4 cpil, Vorbis COMPILATION/ITUNESCOMPILATION, and APE Compilation.
- Use the performer on compilations in Now Playing, the screensaver and quick panel. If the performer is missing, keep the album artist. Ordinary albums retain album-artist preference.
- Retain the fork's descriptions, series, ID3v2.2 COMM support, and Audiobookshelf sidecars. The catalog still uses original artist/album-artist tags; display selection does not rewrite them or change book identity.
- Extend upstream's TXXX handling through our existing decoded-text parser, including UTF-16. Boolean flags accept complete 1/true/yes values with padding, not matching prefixes such as 10 or yesterday.

The recovery review also identified a separate, smaller hardening change: limit the existing kernel-log follower to 64 read attempts per watchdog pass, including EINTR/EPIPE. The existing 20-warning output limit remains. This bounds the drain loop without adding a thread, poller, allocation, reboot policy or SD write path. It does not guarantee a time limit on other logging/I/O operations.

## Not included

| Upstream commit | Decision and reason |
| --- | --- |
| [99e7f66](https://github.com/Jepl4r/sonix-player/commit/99e7f668bb50eaf31172e8a133e4d5e27e5821ff), podcast downloads plus audio changes | Keep as a separate follow-up. Our fork already has immediate multipart completion handling; importing its completion hook blindly would duplicate/conflict with that work. Mono output, DSD-to-PCM over Bluetooth and WAV gapless change the audio path and need output-specific tests. |
| [b62a4cf](https://github.com/Jepl4r/sonix-player/commit/b62a4cf51a1dcf8898487935d43ae31bbacb6166), Bluetooth SonixLink | Defer. Adds a remote-control subsystem, D-Bus FD passing and workers, with overlapping power/Bluetooth behavior. Not needed for Audiobookshelf sync. |
| [e3c0cf4](https://github.com/Jepl4r/sonix-player/commit/e3c0cf4b500f3845ef033ae8235c0d0829515cd5), SonixLink database refresh | Defer with its SonixLink dependency; this is not a standalone fix for our audiobook catalog. |
| eed607f, kernel-oops recovery portion | Do not enable automatic reset in this candidate. See below. |
| [2ef6ecf](https://github.com/Jepl4r/sonix-player/commit/2ef6ecfb65e8f63ffff7ce366c4a6bf4c5b6de90), lyrics | Defer the larger Now Playing/cover-blur changes until tested against our audiobook controls and R1 memory limits. |

## Audio follow-up concerns

The unimported MP3 fast-open path estimates constant bitrate from the first 200 frames. A headerless stream that starts with uniform frames but changes bitrate later can be misclassified. This needs adversarial fixtures before it determines duration or completion in our build.

AAC gapless trimming changes the decoder's time origin by accounting for priming/edit lists. Before adopting it, test seek-to-zero, saved offsets, chapter boundaries, end-of-book completion, HE-AAC sample-rate changes, malformed edit lists and long M4B files. It is promising, but compile success is not evidence that audiobook resume stays correct.

## Recovery follow-up concerns

Upstream's reset guard is armed after report copying and a logging call. A continuously replenished log or a stuck operation before that point can prevent the deadline from being armed. The oops detector is also a broad textual suffix test, and an emergency reset may interrupt pending card writes or lose the latest checkpoint. It mitigates a damaged kernel state; it does not repair the kernel bug.

Our `logging_follow_kernel()` runs in the watchdog thread, not the GUI event loop. A future experimental recovery design should arm its deadline before potentially blocking work, bound parsing/report copying, distinguish real MIPS oops headers from ordinary messages, and default to opt-in. Test the detector and reset state machine against mocked logs/reset calls first. Do not deliberately inject kernel faults into a user's R1.

## Validation

- Host and original-R1 MIPS builds pass; target ABI audit reports no symbol newer than glibc 2.22.
- 156 metadata fixtures pass under ASan/UBSan: ID3v2.2/2.3/2.4, text encodings, true/false/malformed flags, FLAC, Ogg Vorbis, Opus, WavPack, MP4 and APE tags. Also checks display fallbacks and book author/series/description sidecars. The APE fixture is tag-only, not an APE decoder/playback test.
- Mocked kernel-log tests cover endless warning, informational and malformed records; repeated EINTR/EPIPE; normal EAGAIN and EOF. No real kernel fault or reset is triggered.
- Local audiobook host smoke passes multipart indexing, bookmarks, persisted direct resume and part advance.
- Audiobookshelf host suites pass download/remote progress import and upload, existing-copy linking without a duplicate download, and preservation/upload of existing local progress.

Candidate build stamp: `011020261221`.

- Player SHA-256: `e4da1e4a218978ecb1f9fbc372a493579f52e8879df468655a255000b3561bf8`
- Firmware SHA-256: `f531ee676b069b05b25de7965935d5da70bbecd2999d1967a171faf0de9be123`

## Original R1 validation

- Installed using the normal Settings > System > Update firmware > From SD card path after matching the SD image checksum. Device returned with ADB; the installed binary checksum matches the candidate above.
- Normal main-menu boot, one launcher and one player; local control FIFO remains root-only (0600).
- Beyond Exile resumed from its stored 827.745 seconds (13:47), displayed 13:51 after startup, and advanced to 14:15. Pause/checkpoint saved 857.281 seconds. The audiobook database passed `PRAGMA integrity_check` before and after.
- Twelve audiobook-home/main-menu navigation cycles showed no resource growth after playback warm-up: RSS 21,544 -> 21,412 KiB, threads 27 -> 27, descriptors 21 -> 20 as playback settled.
- Played a disposable silent AAC/MP4 tagged with album artist `Album Credit`, performer `Test Performer`, and cpil=1. The real Now Playing screen displayed `Test Performer` correctly.
- No oops, panic, OOM kill, segfault or invalid-I2C message found in the post-boot kernel log during these checks.

The silent fixture tests decoding, UI metadata and controls, not audible sound quality. Screenshots/logs and the candidate image are retained locally under `build/upstream-validation/`. No battery-life improvement or long-term reliability claim is implied by these short regression checks. Bluetooth/USB audio paths and the screensaver's visual layout were not revalidated in this pass; their routing code is unchanged.
