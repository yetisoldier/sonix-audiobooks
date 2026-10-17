# 0.x device-validation records

The records below describe the 0.x line (pre-rebase) release packages: their
builds, hashes and server checks are historical and do not apply to the line
built on upstream 1.3.0.

## Audiobookshelf browsing validation — 2026-10-01 (0.3.0)

Published as Sonix Audiobooks **0.3.0**, build `011020261530`. The release uses the same tested package (49,418,240 bytes), without rebuilding after device validation.

- Added whole-library server search plus Authors and Series filters to Streaming > Audiobookshelf.
- Host mock test passed search submission, author/series filter encoding, numeric series ordering requests,
  back navigation, download, multipart resume import, and progress upload.
- Target ABI check passed (glibc <= 2.22). Installed through the R1's SD-card firmware updater.
- Installed `/usr/bin/sonix_player` SHA-256:
  `2d8989ed3c0bf08db4b0f9b9dbdc320233a7e18d7e79f52f7971271e4f80ec26`.
- Update package SHA-256:
  `a047c9fd3705e270a0bff1827d8750bd3d9b32c73f11aff7ef733322fea96674`.
- Verified against the configured live server: author list, A. American books,
  series list, 12 Miles Below in sequence, and keyboard search for `hope`.
- Search shows at most 40 results and prompts refinement at that limit.
  Group and book browsing use 40-entry pages. No long-duration playback test was performed for this UI change.
