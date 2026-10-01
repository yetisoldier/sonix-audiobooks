# Changelog

All entries here are specific to Sonix Audiobooks. The project also inherits ongoing work from [Sonix Player](https://github.com/Jepl4r/sonix-player); upstream changes are not repeated line-by-line here.

## Sonix Audiobooks 0.2.0 - 2026-10-01

Public feedback release for the original HiBy R1. Includes the changes below since 0.1.0. Firmware build identifier: `011020261348`.

### Selective upstream improvements

- Backported upstream compilation-artist display: Now Playing, the screensaver, and the quick panel show the track performer for tagged compilations or albums credited to Various Artists. Ordinary album-artist preference and audiobook catalog author/grouping are unchanged.
- Read compilation tags from ID3v2, MP4, Vorbis comments and APE tags. Extended the backport to UTF-16 TXXX flags and exact boolean-value matching.
- Bounded kernel-log reads per watchdog pass, including repeated interrupted/overrun reads, so a log flood cannot keep that loop draining indefinitely. This does not enable automatic reboot on a kernel oops.
- Added sanitizer-backed metadata fixtures and mocked kernel-log flood tests. See [upstream integration review](docs/UPSTREAM_REVIEW_2026-10-01.md) for scope and deferred changes.

### Audiobookshelf integration

- Added an optional Audiobookshelf client in Streaming.
- Added server and token configuration, library browsing, and book detail views.
- Added downloads to `/Audiobooks/Audiobookshelf/<author>/<title>/` with artwork, title, author, series, and description sidecars that the local audiobook scanner understands.
- Added safe handling for interrupted downloads: incomplete files keep a `.part` suffix and are not indexed as playable books.
- Added local-book linking for existing SD-card copies when the server book has matching parts and duration, avoiding a redundant download.
- Added server-position import when downloading/linking a book and background progress uploads for downloaded and linked books.
- Made uploads offline-first: the existing local checkpoint remains pending until the server acknowledges that exact position. Pending progress survives Wi-Fi being off, server errors, book changes and reboot; turning Wi-Fi on resumes attempts without reopening the book.
- Added bidirectional conflict resolution for linked books: newest listening timestamp wins, including intentional rewinds. Missing, implausible or tied timestamps fall back to the farther whole-book position (completed counts as the end).
- Added server-clock plausibility checks, millisecond local event timestamps preserved through scans, verified uploads, and transactional imports guarded against concurrent local edits. Incoming progress waits while the book is playing; paused books use the imported file/position on their next Play.
- Added reconnect and bounded periodic checks for newer server progress on already-synced books. The supported GET/PATCH progress API is used instead of the removed legacy sync route; simultaneous server writes are not atomic across that API.
- Added a Now Playing status indicator for local-only, linked, pending, waiting for Wi-Fi, syncing, synced, synced-offline and retry-pending states.
- Centered the sync status above the artwork and sized its shaded background to the wording, with compact padding and wrapping for longer text.
- Kept Wi-Fi under user control: sync never enables it, makes no upload attempts while it is off, and has no periodic retry wake-up while it stays off. UI reads a cached status rather than querying the network or SD card.
- Added server binding to new link manifests and card-generation checks on acknowledgements. An old response cannot mark a newer backward seek or a replacement card as synced.
- Added direct mapping between Audiobookshelf whole-book progress and the correct local multipart file and offset.
- Added an Audiobookshelf host smoke suite covering download, resume import, existing-copy linking, local-progress upload, and remote progress upload.
- Added an outage/restart smoke test and sanitizer-backed durable-progress tests covering multiple books, backward seeks, completion, stale acknowledgements and card/server isolation.
- Added simulator conflict tests for local and remote rewinds, paused multipart resume through the physical Play handler, missing/future timestamps and timestamp ties.

### Stability and correctness

- Fixed non-terminating pathname copies in the audio playback path.
- Fixed an empty folder browser path that could call `qsort` with a null entry buffer.
- Increased the saved setting value capacity so ordinary Audiobookshelf API tokens are not truncated.
- Added bounded parsing for server-supplied titles and descriptions.
- Kept `Authorization` headers on same-origin redirects only; credentials are not forwarded to a redirected media host.

### Validation

- Added sanitizer, static-analysis, target ABI, package, local audiobook, and Audiobookshelf smoke coverage.
- Validated the installed candidate on an R1: normal boot, empty-folder handling, direct local resume, playback/pause, and 15 repeated Audiobooks enter/exit cycles with stable process memory, thread count, and file-descriptor count.

## 0.1.0 - 2026-09-30

First public Sonix Audiobooks release, based on Sonix Player 1.1.0.

### Audiobook experience

- Added a dedicated Audiobooks home with Library, Series, Authors, Continue, Bookmarks, and Folders views.
- Added custom audiobook navigation icons and a distinct home-screen icon.
- Added indexed folder-hierarchy browsing without synchronous SD-card directory walks.
- Added manual per-book bookmarks with a global list, direct resume, and deletion.
- Added summary/description indexing and display for common M4B/MP4 and MP3 metadata.
- Hardened single-file and multipart resume with background checkpoints, saves at important transitions, direct saved-part return, completion reset, and card-swap isolation.
- Added Audiobook Now Playing actions for chapters, adding and opening bookmarks, and book descriptions.
- Improved multipart auto-advance by sending decoder completion to the controller immediately instead of waiting for the UI progress timer.

### R1 reliability

- Fixed R1 touch-loader line endings that could leave the screen unresponsive after firmware update.
- Replaced fixed Linux input-event assumptions with capability and name discovery for touch, physical keys, headset controls, and touch-wake monitoring.
- Added a packaging audit that rejects malformed module-loader scripts.
- Reconciled successfully saved Bluetooth bonds when BlueZ reports a late pairing timeout.
- Restored audio routing and DAC state after every suspend attempt, including a rejected kernel suspend.
- Bounded real-time CPU behavior so a faulty worker cannot indefinitely starve the single-core interface and kernel tasks.
- Enabled the existing 500 mA R1 charge-current cap by default without changing battery-percentage reporting.

### Development and validation

- Added a root-only local ADB UI-control channel for tap, swipe, wake, status, and framebuffer screenshots.
- Added a Windows helper and adversarial input probe-order test.
- Expanded the headless host smoke test to cover multipart indexing, direct resume, bookmarks, restart persistence, and automatic part advance.
- Added ABI, package, installed-hash, database-integrity, boot, memory, and input-routing release checks.
