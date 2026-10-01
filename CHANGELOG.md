# Changelog

## Audiobook Edition 0.1.0 - 2026-09-30

First public Audiobook Edition release, based on Sonix Player 1.1.0.

### Audiobooks

- Added a dedicated six-view audiobook home: Library, Series, Authors,
  Continue, Bookmarks, and Folders.
- Added custom audiobook navigation icons and a distinct home icon.
- Added indexed folder-hierarchy browsing without synchronous SD-card walks.
- Added manual per-book bookmarks with global listing, direct resume, and
  deletion.
- Added summary/description indexing and display for common M4B/MP4 and MP3
  metadata.
- Hardened per-book resume with background checkpoints, forced transition
  saves, direct multipart resume, completion reset, and card-swap isolation.
- Added audiobook Now Playing actions for chapters, adding/opening bookmarks,
  and book summaries.
- Improved natural multipart advance by sending decoder completion directly
  to the controller instead of waiting for a UI timer.

### Device reliability

- Fixed the R1 touch-loader line endings that could leave the screen entirely
  unresponsive after an update.
- Replaced fixed input-event assumptions with capability/name discovery for
  touch, physical keys, headset controls, and touch wake monitoring.
- Added a packaging audit that rejects malformed module-loader scripts.
- Reconciled successful Bluetooth bonds when BlueZ reports a late pairing
  timeout.
- Restored audio routing and DAC state after every suspend attempt, including
  a rejected kernel suspend.
- Bounded real-time CPU behavior to prevent a faulty worker from starving the
  single-core interface.
- Enabled the existing 500 mA R1 charge-current cap by default without changing
  battery-percentage reporting.

### Development and validation

- Added a root-only local ADB UI-control channel for tap, swipe, wake, status,
  and framebuffer screenshots.
- Added a Windows control helper and an adversarial input probe-order test.
- Expanded the headless host smoke test to cover multipart indexing, direct
  resume, bookmarks, restart persistence, and automatic part advance.
- Added ABI, package, installed-hash, database-integrity, boot, memory, and
  input-routing release checks.
- Added public fork comparison, installation guidance, screenshots, issue
  analysis, and device-testing documentation.
