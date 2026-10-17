# Rebase Plan: Sonix Audiobooks onto upstream sonix-player v1.3.0

Author: Bob (architect), 2026-10-17. Status: DESIGN ONLY. No code written, nothing pushed.

**Decision (approved by Eric):** rebuild on upstream. The new base is `upstream/main` (Jepl4r/sonix-player,
v1.3.0), checked out as a fresh branch `rebase/upstream-1.3.0`. Our thin unique layer is re-applied on top.
Our diverged `main` line (17 commits ahead / 134 behind, merge base c096c7a 2026-09-30) is **not** merged;
it stays as the historical 0.x line.

**Why not merge:** the trial merge conflicts in ~45 files / ~100 hunks, and in nearly every hunk *our* side
is a fix that upstream has already absorbed and evolved (04fc728 ported our stability fixes; 2baa04a built
bookmarks/summaries/background position saves on our design with a schema, an epoch guard and a background
writer that supersede ours). Resolving conflicts would mean re-deriving upstream's own code. Re-applying the
small set of files upstream genuinely lacks is cheaper and produces a cleaner history.

---

## 1. Commit-by-commit disposition of our 17 unique commits

`git log main ^upstream/main`, oldest first. Branch `rebase/upstream-1.3.0` starts from `upstream/main`;
the column "landed as" names the commit on that base.

| # | Commit | Subject | Disposition | Evidence / landed as |
|---|--------|---------|-------------|----------------------|
| 1 | 0edcf4a | fix(host): keep simulated card databases on the card | **DROP** | Upstream's host path already keeps card dbs on the card (reworked in the 1.2.0 scan/identity work; `audiobookdb_reopen_if_replaced` + `file_identity` in upstream `audiobookdb.c:195`). Our `main.c`/`system.c` tweaks are obsoleted by the rework. |
| 2 | 9700804 | fix(audiobooks): write resume checkpoints off the UI thread | **DROP** | Absorbed & superseded by 2baa04a: upstream `audiobookdb_queue_position()` background writer with `card_epoch` guard (`audiobookdb.c:615-643`); upstream `playback/audiobook.c:510-512` calls save/queue. |
| 3 | 548a4a3 | test(audiobooks): add end-to-end host smoke coverage | **PORT-ADAPTED** | Keep `.github/workflows/host-build.yml` + `tools/audiobook-host-smoke.sh`; adapt paths to upstream's current tree (scanner tables unchanged; verify against schema v4). |
| 4 | 9e787f1 | build: keep shell tooling portable | **PORT-AS-IS** | `.gitattributes` only. Check upstream's first; union merge. |
| 5 | af0e446 | fix(usb): size realpath buffer to its required limit | **DROP** | Absorbed in 04fc728 (realpath PATH_MAX fix). Verify `git show upstream/main:sonix-player/src/system/device/usb.c` contains a PATH_MAX-sized buffer before dropping; if the hunk is absent, resurrect as a one-file patch. |
| 6 | 01cd0c3 | ci: avoid duplicate branch and pull request runs | **PORT-ADAPTED** | Fold into our new host-build.yml triggers (upstream has no host workflow; only `bug-log-check.yml`, `issue-title-check.yml` — no dup-run conflict to reconcile). |
| 7 | 60e7ca0 | ci: use Node 24 GitHub actions | **PORT-ADAPTED** | Same: apply to the actions versions inside our new host-build.yml (`actions/checkout@v7`, `upload-artifact@v7`). |
| 8 | 2a04489 | docs: describe the inherited audiobook experience | **PORT-ADAPTED** | `AUDIOBOOK_EDITION.md`: copy, then rewrite the "inherited experience" claims against upstream 1.2.0's actual feature set (upstream now covers nearly all of it natively). |
| 9 | 568204c | ci: use current GitHub action runtimes | **PORT-ADAPTED** | Ditto #7. |
| 10 | acd5e8d | Merge PR #1 codex/audiobook-foundation | **N/A** | Merge commit; its content is commits 1–3 & file adds listed under file plan §3. Nothing to cherry-pick. |
| 11 | e0da1ed | fix: harden playback, pairing, and power behavior | **DROP** | Absorbed in 04fc728: input-by-capability, late BT bond accept, DAC mute through suspend. Upstream additionally added kernel module swap, R1 pop fix, fuel-gauge recovery, 5%/1% battery handling, panic auto-reboot — strictly broader than our `powersettings.c`/`audio.c`/`bluetooth.c` edits. Keep only `tools/audiobook-host-smoke.sh` changes (already covered by #3). `SONIX_ISSUE_AUDIT.md` becomes historical; see §8. |
| 12 | 292c16f | Merge upstream/main into codex/sonix-stability | **N/A** | Merge commit; content disposed via its parents. |
| 13 | 2f01283 | feat: add ADB device UI automation | **PORT-ADAPTED** | Copy `tools/sonix-device-control.ps1`, `tools/uinput-abs-holder.c` as-is. `devcontrol.c/.h` + the three `main.c` call sites re-applied by patch onto upstream `main.c` (upstream `main.c` has diverged: our `#include devcontrol.h` anchor at ours line 36 conflicts with upstream's new includes `btlog.h`/`chargescreen.h`/`podcaststate.h`; apply at the matching include block and at `main.c` poll-loop region — find upstream's poll fd array and init site, same shape). |
| 14 | 00d420d | feat: release audiobook edition preview | **SPLIT** | DROP all language .ini changes (upstream's are newer — see §4), DROP `module_driver/cst8xx_touch.sh` (kernel module set replaced upstream). PORT-ADAPTED: branding/docs (`AUDIOBOOK_EDITION.md`, `CHANGELOG.md`, screenshot pngs, README branding block, `.gitignore` additions) reapplied onto upstream files. |
| 15 | 509ffa6 | docs: rebrand as Sonix Audiobooks | **PORT-ADAPTED** | Reapply README/`AUDIOBOOK_EDITION.md`/`docs/DEVELOPMENT.md` branding by hand onto upstream's current README (4-hunk conflict area); do not overwrite upstream feature descriptions. |
| 16 | 8a71d09 | Release 0.2.0 offline-first ABS sync | **PORT-ADAPTED (core)** | The Audiobookshelf stack proper: see §2/§3. Language .ini portions DROP (handled by §4 union). |
| 17 | f595a95 | Release 0.3.0 search + authors/series browsing | **PORT-ADAPTED (core)** | Same stack as #16 (search, authors/series panes are in the same two files). Screenshots copy as-is. |

**Resulting new history on `rebase/upstream-1.3.0` (Forge's order):**

1. `chore: .gitattributes portability` (#4)
2. `feat: ADB device UI automation` (#13 adaptation)
3. `feat(audiobookshelf): offline-first sync module + GUI page` (#16+#17 code, adapted per §2/§3)
4. `i18n: Audiobookshelf strings for all shipped languages` (§4 union output)
5. `ci: host simulator build + smoke tests` (#3+#6+#7+#9 combined)
6. `docs: Sonix Audiobooks branding, changelog, testing notes` (#8+#14-docs+#15)

---

## 2. Audiobookshelf sync hooks onto upstream's schema v4

### 2.1 Upstream model we attach to (verified against `upstream/main`)

- **Schema v4**, `PRAGMA user_version`, gated by `outdated` flag. Create/ALTER block: upstream
  `sonix-player/src/system/library/audiobookdb.c:117-170` (`audiobookdb_open`); `SCHEMA_VERSION 4` at line 39.
- **Background position writer:** single-slot queue + `queue_thread_func` + `card_epoch` guard +
  `position_seq` dedup, upstream lines 540-670:
  - `write_position_locked()` 542-557 — UPDATE resume_file/resume_pos/last_played.
  - `write_queued()` 561-568 — epoch check `entry->epoch == card_epoch` before write.
  - `audiobookdb_save_position()` 596-613 — synchronous; drops a still-queued checkpoint of the same book.
  - `audiobookdb_queue_position()` 615-643 — lazy thread start, one-slot replace, stamps `queued.epoch`.
  - `position_queue_drain()` 646-666 — called from `audiobookdb_close()` at line 224.
  - `card_epoch` bumped on open (line 184) and close (line 227), both `__ATOMIC_RELEASE`.
  - `db_identity` (`file_identity_read`) at open line 188; `audiobookdb_reopen_if_replaced()` 194-211.
- **Playback call site:** upstream `playback/audiobook.c`, `audiobook_note_position()` ~lines 460-514:
  force path calls `audiobookdb_save_position(book, resume_file, seconds)` (line 510), periodic path calls
  `audiobookdb_queue_position(...)` (line 512); finish window is 45 s (`FINISHED_MARGIN`, upstream), not our 15 s.
- **Resume read:** upstream `audiobook_resume_point()` at `playback/audiobook.c:360` — same signature ours uses.

### 2.2 What our sync module adds (from our `main`)

Our `src/system/streaming/audiobookshelf.c` (1223 lines) persists sync state in
`AUDIOBOOK_ABS_SYNC` (created in our `audiobookdb.c:246-248`) plus three added columns on
`AUDIOBOOK_TABLE` (`resume_updated_ms`, `resume_revision`, `resume_completed`, our lines 250-255),
and drives it through `audiobookdb_abs_get/next/result/retry/reset/reconnect/import`
(our `audiobookdb.h:166-184`). Upstream has **none** of these (`grep abs_ upstream audiobookdb.h` = 0 hits).
The only playback call site is one line at the end of our `audiobook_note_position()`:
`audiobookshelf_queue_progress(book, resume_file, seconds, completed, force || completed);`

### 2.3 Hook placements (exact)

**H1 — ABS sync-state schema.** In upstream `audiobookdb_open()`, after the
`AUDIOBOOK_BOOKMARKS` CREATE (upstream line 168-169-ish, immediately before the `int books` count at
line 172) insert our `CREATE TABLE IF NOT EXISTS AUDIOBOOK_ABS_SYNC(...)` and the three
`ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN resume_*` statements verbatim from our lines 246-256.
No `SCHEMA_VERSION` bump: the ADD COLUMNs are idempotent via `exec_quiet`, and v4's `outdated`
rescan path is untouched. The `resume_completed` backfill UPDATE (our lines 252-254) is retained;
upstream's finish semantics differ (position zeroed + FINISHED row), so the backfill still computes
correctly from `AUDIOBOOK_FINISHED`.

**H2 — Progress-save acknowledgment feed.** At the end of upstream `audiobookdb.c`
`write_position_locked()` (after line 556, before the closing brace — i.e. after
`position_written = seq;` + `snprintf(position_written_book...)`), we do **not** hook. The design keeps
db/write path *absolutely* unchanged. Instead the sync trigger stays at the playback layer, exactly as
in our tree: append one line to upstream `playback/audiobook.c` `audiobook_note_position()` after the
save/queue block (after line 512):

```c
audiobookshelf_queue_progress(book, resume_file, seconds, completed, force || completed);
```

This requires deriving `completed` in upstream's function: upstream does not currently set a `completed`
flag; add `bool completed = false;` next to `bool last_part` and set `completed = true;` inside the
finish branch (the `if (last_part && total > FINISHED_MARGIN && seconds >= total - FINISHED_MARGIN)`
block, upstream lines 477-492, right where `seconds = 0; force = true;` happen). This is a 3-line diff
to upstream `audiobook.c` plus the one call line. Do not hook `write_queued`/the db thread: the
acknowledgment model keys on the *logical* checkpoint, which the playback layer knows; the db thread
only knows bytes.

**H3 — Outbox pending state.** Implemented entirely inside our ported block in
`audiobookdb.c` (see H4) using upstream's existing primitives; it needs the checkpoint snapshot the
worker took to be acknowledged *exactly*. Achieve this by reading
`AUDIOBOOK_TABLE.resume_file/resume_pos/resume_updated_ms/resume_completed` in
`audiobookdb_abs_get()` (our ABS_CHECKPOINT_SELECT LEFT JOIN already does this — port as-is), and by
the rule already in our worker: a checkpoint stays `PENDING` until `audiobookdb_abs_result(sent, ...)`
is called with the identical snapshot. Upstream's `card_epoch` + `file_identity` give us a stronger
guarantee than our old `position_accepting` flag had: on card change, `card_epoch` bumps and a stale
queued write is silently dropped — our outbox row remains `attempted=0`, so the worker re-reads the
live resume row and re-sends. Correct by construction; no drain hook needed.

**H4 — Ported db block (verbatim-with-renames from our tree).** Port, as one contiguous block after
upstream `audiobookdb_get_position()` (upstream ~line 700, before `audiobookdb_book_for_file`):
`ABS_CHECKPOINT_SELECT`, `abs_checkpoint_read`, `audiobookdb_abs_get`, `_abs_next`, `_abs_result`,
`_abs_retry`, `_abs_reset`, `_abs_reconnect`, `_abs_import` (our `audiobookdb.c:651-830`).
One adaptation: our block references our `position_lock`/`position_accepting` pair — upstream has
neither; delete those references and rely on upstream's `queue_lock`+`card_epoch` discipline.
Our port must wrap reads in upstream's `db_lock` (same as its other readers).

**H5 — Resume-import guard.** Port `audiobook_sync_import`,
`audiobook_take_synced_resume`, `audiobook_cancel_synced_resume` and the `synced_resume_file[]` early
gate at the top of `audiobook_note_position()` from our `playback/audiobook.c:445-477` and
496-507. Upstream's function structure is otherwise compatible (same `lock`, `current_book`,
`current_path`, `last_saved_position`). Declare the three functions in upstream
`playback/audiobook.h` next to `audiobook_resume_point` (upstream line 101).

**H6 — Boot-time resume precedence.** Our `main.c` `restore_saved_track()` block that prefers
`audiobook_resume_point()` over the generic saved position (our `main.c` hunk shown in the diff,
lines ~100-110 of ours) must be re-applied onto upstream `main.c`'s `restore_saved_track()`.
Upstream removed nothing equivalent, so apply as an insertion in the same spot (immediately after
the `access(saved_track, R_OK)` check in upstream).

**H7 — main.c init.** `audiobookshelf_init();` alongside upstream's other `*_init` calls in `main()`
(port from our `main.c`; ours calls it after `config_init()`).

### 2.4 API-drift audit (our module vs upstream signatures)

| Our call | Ours (schema 3 tree) | Upstream (schema 4 tree) | Resolution |
|---|---|---|---|
| `audiobookdb_open/close/scan_*`, `audiobookdb_book_for_file`, `audiobookdb_get_position`, `audiobookdb_save_position`, `audiobookdb_queue_position` | identical signatures | identical | none needed |
| `metadata_read_with_description()` | used by our scan sidecar path (via scanner) | upstream has it: `metadata.h:79`, impl `metadata.c:2567` | none — our `.audiobookshelf` manifest/sidecar flow feeds the *scanner* indirectly via real ID3/Vorbis tags we write at download; no direct call in `audiobookshelf.c`. Double-check at port time with `git grep metadata_ audiobookshelf.c` (expected: none). |
| `audiobookdb_flush_positions()` | ours only | absent upstream (replaced by `position_queue_drain` in close) | delete from ported headers; our `audiobookshelf.c` never calls it (verify at port). |
| `audiobookdb_abs_*` (7 fns) | ours only | absent | port per H4. |
| `audiobook_resume_point()` | ours | upstream `audiobook.c:360`, same signature | none. |
| `audiobookdb_mark_finished/get finished` | same semantics both sides; upstream also zeroes position in `note_position` | superset | none; our `completed` derivation rides upstream's branch. |
| `SAVE_EVERY_SECONDS`, `FINISHED_MARGIN` | our 15 s finish window | upstream 45 s window | take upstream's constant. Sync math uses *duration-absolute* values from the manifest (`local_progress()`), so the window size only changes when "finished" fires, matching server semantics at book end. No change to our module. |
| `library_collate_listorder` (host-test stub) | stub lives in our outbox test | upstream exports the same symbol | outbox test stub remains valid; keep it (it shadows upstream's when linking only `audiobookdb.c`). |
| `thread_be_background()` | used by upstream's queue thread | present upstream | ensure our ported block doesn't re-declare. |

**Schema 3 → 4 migration note for real devices:** a card that was indexed by our 0.x release carries
`user_version=3` plus our `AUDIOBOOK_ABS_SYNC`/`resume_*` additions. Upstream v4 treats
`user_version<4` as `outdated`, schedules a rescan, and *wipes and refills* `AUDIOBOOK_TABLE` — but
leaves `AUDIOBOOK_FINISHED`, `AUDIOBOOK_BOOKMARKS`, and (by our ported CREATE-IF-NOT-EXISTS)
`AUDIOBOOK_ABS_SYNC` intact. Resume rows are rebuilt from the scan; abs linkage survives because it
lives outside `AUDIOBOOK_TABLE` except the three `resume_*` columns, which are refreshed by the first
checkpoint after rescan. Net effect on upgrade: positions kept (rewritten), sync linkage kept,
one full rescan. Acceptable; document in release notes.

---

## 3. File-level apply plan (unique files)

| File (under repo root) | Action | Notes / upstream anchors |
|---|---|---|
| `sonix-player/src/system/streaming/audiobookshelf.c` (1223) | **COPY** verbatim; compile-check only against §2.4 drift table. | includes unchanged; `abs_sync.h` lives beside it. |
| `sonix-player/src/system/streaming/audiobookshelf.h` (120) | **COPY** as-is. | |
| `sonix-player/src/system/streaming/abs_sync.h` | **COPY** as-is. | policy header used by outbox test. |
| `sonix-player/src/gui/streaming/audiobookshelfpage.c` (890) + `.h` | **COPY + LVGL 9.6 sweep** (§7 risk R1). No LVGL-private headers used; verify `lv_obj_set_hidden`/flag API usage matches 9.6. | |
| `docs/AUDIOBOOKSHELF_SYNC.md` | **COPY**; append "Schema v4 note" paragraph (§2.4 migration note). | |
| `tools/audiobookshelf-host-smoke.sh` | **COPY**; expect to need path/behaviour tweaks only if upstream renamed player binary (it didn't: `sonix_player_host`). | drives simulator + `audiobookshelf_mock.py`. |
| `tools/audiobookshelf_mock.py` | **COPY** as-is. | |
| `tools/audiobookshelf-outbox-test.c` + `.sh` | **COPY**; keep stub `library_collate_listorder`; compiled against upstream `audiobookdb.c` + our ported block. | links `src/system/library/audiobookdb.c src/system/db/sqlite3_impl.c` — paths exist upstream. |
| `tools/audiobook-host-smoke.sh` | **COPY**; adapt to upstream scan behaviour (folder books, join rules). Test fixtures are self-contained; main risk is timing only. | |
| `tools/metadata-regression.py`, `tools/metadata-probe.c`, `tools/kernel-log-test.c`, `tools/upstream-regression.sh` | **COPY**; metadata expectations must be regenerated against upstream metadata.c (much larger: 1409/-575 churn). Regenerate golden outputs; do not carry ours. | |
| `tools/sonix-device-control.ps1`, `tools/uinput-abs-holder.c` | **COPY** as-is; devcontrol counterpart below. | |
| `sonix-player/src/system/device/devcontrol.c` + `.h` | **PORT-ADAPTED**: copy file; re-apply three `main.c` call sites (`devcontrol_init(disp, panel_w, panel_h)` ~ours line 2327; poll fd entry ~ours 2378; `devcontrol_service()` ~ours 2387) at the matching spots in upstream `main.c`, which has the same poll-loop skeleton but different includes/context lines. | upstream has no equivalent (`grep devcontrol upstream` = 0). |
| `AUDIOBOOK_EDITION.md`, `README.md` branding, `CHANGELOG.md`, `docs/releases/*`, `docs/screenshots/*.png`, `DEVICE_TESTING.md`, `SONIX_ISSUE_AUDIT.md` | **PORT-ADAPTED**: rewrite README/AUDIOBOOK_EDITION against upstream 1.3.0 feature list (most of our old "exclusive" claims are now upstream's). Move `SONIX_ISSUE_AUDIT.md` + old release notes under `docs/legacy-0.x/` to keep history without presenting stale audits as current. Screenshots: copy. `.gitignore` additions: union. | README is a 4-hunk conflict zone; do it by hand, taking upstream's README as the base and adding our branding paragraphs. |
| `.github/workflows/host-build.yml` | **REGENERATE** (§5). | upstream has no host workflow to merge with. |
| language `.ini` edits (R1 + R3PII, both trees) | **REGENERATE** per §4; never copy `.ini` files over upstream's. | upstream added Polski, Português BR, Čeština and reworked hundreds of keys. |
| `sonix-packer/assets/R1/module_driver/cst8xx_touch.sh` change (from 00d420d) | **DROP** — module set replaced upstream (touch module now one of the open-source ones). Verify the upstream unit handles cst8xx identically; if not, resurrect the two-line diff. | |
| `components/system-info.json` edits (e0da1ed) | **DROP** — upstream's JSON evolved; our version string belongs to the new line anyway. Regenerate at release time. | |

### GUI integration points (upstream anchors)

- **Streaming menu tile:** upstream `src/gui/streaming/streaming.c` lines 9-27: add
  `#include "src/gui/streaming/audiobookshelfpage.h"`, call `audiobookshelfpage_init(cfg);` beside
  `podcastpage_init(cfg)` (line 18), add tile entry
  `{"audiobookshelf", &icon_menu_audiobooks, &audiobookshelf_screen, NULL}` after the podcasts row
  (line 24). `icon_menu_audiobooks` exists upstream (`gui/shell/icons.h:252`, `icons.c:309828`).
- **Now Playing sync status:** port the `abs_sync_label` widget and the whole
  `update_abs_sync(const device_state_t *state)` function from our
  `gui/nowplaying/player.c:2994-3012`, call it from upstream `update_progress()` (upstream has the
  same function at `player.c`, near its `apply_audiobook_mode()` call ~line 3558; insert the call
  right after the `device_state_get(&state)` in upstream's `update_progress`). Label creation next to
  upstream's chapter-label creation in the audiobook-mode block of the layout setup (search
  `apply_audiobook_mode` at `player.c:504-531`; the widget sits in the same container stack).
  Guard: our function early-returns when `!audiobook_mode || state->live`, so normal playback is untouched.
- **Wi-Fi event → sync wake:** upstream `system/net/wifi.c`: insert `audiobookshelf_network_changed();`
  at the two spots ours has (after connect and after disconnect transitions; ours at lines 1383/1420 —
  find upstream's equivalent state-transition log lines and add one call each).
- **Settings entry (server/token):** our page hosts its own setup screen (`audiobookshelf_setup`)
  reachable from the tile; no settings-page surgery needed.
- **Audiobook library page interplay:** upstream 1.2.0's `gui/library/audiobooks.c` owns book
  presentation; our download completion already triggers a library rescan via the scanner path that
  1.2.0 kept (`audiobookshelf_downloaded` string, "refreshing Audiobooks"). No edits to
  `audiobooks.c` planned; verify at build time that a download lands in `Audiobooks/Audiobookshelf/...`
  and appears after scan. If a manual nudge is needed, call upstream's rescan trigger from the page's
  completion callback only.

---

## 4. Language `.ini` strategy

Upstream now ships 12 languages for R1 and R3PII: Chinese, Deutsch, English, Español, Français,
Italiano, Japanese, Nederlands, **Polski (new upstream)**, **Português BR (new upstream)**, Russian,
**Čeština (new upstream)**. Our tree has 9 (no Polski/Português BR/Čeština). Ours added 35 keys
(`audiobookshelf*` + `abs_sync_*`, e.g. `English.ini` lines 1165-1199) translated into each shipped
language; upstream contains zero of those keys.

**Procedure (per language, per device tree R1 & R3PII):**

1. Take the **upstream file as the base**, byte for byte. Our 0.x edits to shared keys
   (`audiobook_*`, bootlogo, battery, queue, etc.) are **dropped** — upstream rewrote those sections
   with more keys and better text; ours is stale.
2. Append our 35-key Audiobookshelf block (from our 0.3.0 files) at the end under
   `; --- audiobookshelfpage.c` / `; --- abs_sync` section markers.
3. For the three upstream-only languages (Polski, Português BR, Čeština): append the **English**
   values for the 35 keys as placeholders (consistent with how upstream seeds new languages; English
   fallback is the project norm).
4. Mechanical check: `grep -c '^audiobookshelf\|^abs_sync' <file>` == 35 for every one of the
   12×2 = 24 files, and `tr()` key resolution smoke-check in the host simulator covers
   `abs_sync_synced` at minimum.

---

## 5. CI plan

Upstream `.github/workflows/` contains only `bug-log-check.yml` and `issue-title-check.yml`
(issue-policy bots). **No upstream host-build workflow exists** — ours is the union. Keep both
upstream files untouched and regenerate one workflow of ours:

`.github/workflows/host-build.yml` (new, on the rebase branch):

- Triggers: `push: [main, rebase/**]` + `pull_request`, `concurrency.cancel-in-progress: true`
  (keeps 01cd0c3's no-duplicate-runs intent on the new line).
- Actions: `actions/checkout@v7`, `actions/upload-artifact@v7` (Node-24 era, covers 568204c/60e7ca0).
- Job `build`: apt deps (SDL2, freetype, opusfile, wavpack, asound2, ffmpeg, sqlite3, xvfb,
  xdotool, scrot), then **four** steps in `sonix-player/`:
  1. `make sonix_player_host` — compile gate, now runs against LVGL 9.6.0 (Makefile clones it).
  2. `tools/audiobook-host-smoke.sh` — end-to-end simulator catalog/resume.
  3. `tools/audiobookshelf-host-smoke.sh` — full ABS flow against `tools/audiobookshelf_mock.py`.
  4. `tools/audiobookshelf-outbox-test.sh` — sanitizer build + outbox/decision unit assertions.
- Artifacts: upload `build/audiobook-host-smoke/` and `build/audiobookshelf-host-smoke/` on always().

---

## 6. Build & verification plan (Forge executes; Karen validates)

1. `git checkout -b rebase/upstream-1.3.0 upstream/main`.
2. Apply the six commits from §1 in order (each must build the host binary before the next lands;
   run step 3's compile gate per commit).
3. Host build: `cd sonix-player && make sonix_player_host` — must succeed on a clean clone
   (LVGL 9.6.0 cloned by Makefile).
4. `tools/audiobook-host-smoke.sh` — exit 0; artifacts: `build/audiobook-host-smoke/`
   screenshots + log show catalog, resume, finish→restart→resume at 0.
5. `tools/audiobookshelf-host-smoke.sh` (all modes: default, `SONIX_ABS_EXISTING=1`,
   `SONIX_ABS_LOCAL_PROGRESS=1`, `SONIX_ABS_OFFLINE_RESTART=1`, `SONIX_ABS_CONFLICTS=1`) —
   exit 0; mock server log shows progress uploads with monotone `updated_ms`.
6. `tools/audiobookshelf-outbox-test.sh` — AddressSanitizer/UBSan clean exit 0.
7. `tools/metadata-regression.py` — regenerate goldens first run, then green.
8. Cross build: `make sonix_player` with the rockbox toolchain — compile gate only (no device flash
   in this plan; R1 flash belongs to Karen's validation).
9. `git diff upstream/main...rebase/upstream-1.3.0 --stat` review: only expected file set shows.

**Acceptance criteria Karen validates on device (R1):**

- Audio plays gapless; pause/play/seek/track change are pop-free (upstream fix intact).
- Bluetooth pairing and input routing behave as upstream 1.3.0.
- Audiobooks: Library/Series/Authors/Continue/Finished lists correct; resume after standby works.
- ABS: link-or-download flow against a real server; kill Wi-Fi mid-book → status label shows
  "waiting for Wi-Fi"; position uploads exactly once per checkpoint after reconnect; finished book
  syncs `finished=true`; conflict case (newer server progress) imports without seeking a playing book.
- Battery: 5% warn and 1% shutdown thresholds; charge screen in standby.
- Upgrade path: boot the new build over a card last used by 0.3.0 → one rescan; positions and
  ABS linkage survive.

---

## 7. Risks

- **R1 — LVGL 9.6.0 surface changes vs our GUI pages.** Upstream moved 9.5.0 → 9.6.0 and its Makefile
  notes the per-flag object API and `lv_text_ap_strlen()` as 9.6-only, plus private-header includes
  (`lv_timer_private.h`, `lv_event_private.h`, `lv_roller_private.h`). Our `audiobookshelfpage.c`
  was written against 9.5 but uses only public widgets; expected impact is low. Mitigation: host build
  is the first gate; if any 9.6 break appears it will be in widget styling/scroll APIs and will be a
  compile error rather than silent misbehaviour (LVGL renamed, not re-typed). The `abs_sync_label` in
  `player.c` must be created inside the upstream layout container, not ours — the layout code moved.
- **R2 — Upstream kernel/module swap vs ADB UI-automation.** Upstream replaced ~20 closed HiBy
  modules (touch, charger, DAC, axp2101, soc_*) with open ones on a custom 4.4.94 kernel. Touch input
  device numbering can move (`/dev/input/eventN`); upstream's own `system.c:2048-2062` already probes
  by capability and notes /dev/input ordering isn't fixed — devcontrol's uinput holder
  (`tools/uinput-abs-holder.c`) creates a *virtual* device and must ensure the player doesn't grab it
  as the physical touchscreen. Upstream `system.c` comments (line ~2155) explicitly skip
  `BUS_VIRTUAL` devices — confirm at device-validation time that this check still exists; if it
  doesn't, devcontrol taps will double-inject.
- **R3 — Fuel gauge / battery behaviour changes** (stuck-0% recovery, 5%/1% thresholds) alter
  suspend timing; our offline sync worker wakes on `audiobookshelf_network_changed()` only, so a
  faster forced-off could drop an in-flight upload. The outbox model survives this by design
  (acknowledged-or-pending), but Karen's device test should include the 1%-off path with a pending
  upload on the card.
- **R4 — Power panic auto-reboot**: a reboot loop would previously have been noticed as a hang; now
  it self-recovers and could mask a regression our ported code introduces. Watch boot counter / logs
  during validation.
- **R5 — Schema v4 rescan on upgrade**: first boot over an old card triggers a full rescan; the
  user-visible symptom is an empty Audiobooks list for the scan duration. Release-note it.
- **R6 — Our ported `AUDIOBOOK_ABS_SYNC` + `resume_*` columns ride upstream's wipe-on-rescan**: by
  design the scan refills resume data, but any future upstream change that drops/rebuilds
  `AUDIOBOOK_TABLE` columns must keep our three `exec_quiet` ALTERs. Flag in code review on every
  future upstream pull.
- **R7 — README/branding hand-merge**: 4-hunk conflict area; risk is purely textual (claiming
  features upstream now owns as ours). Mitigated by §3 rewrite instruction.

---

## 8. What we deliberately give up (from our 17 commits)

- All 0.x playback/pairing/power hardening edits (`e0da1ed`) — upstream's absorption is strictly
  broader and better tested.
- Our background-checkpoint implementation (`9700804`) and simulated-card-db plumbing (`0edcf4a`) —
  superseded by upstream's queue writer + epoch/identity model.
- The USB `realpath` sizing fix (`af0e446`) — absorbed (with a pre-drop verification step).
- All 0.x language `.ini` edits outside our 35-key ABS block, including our Japanese/Nederlands
  additions — upstream's are newer and more complete; our translations of shared keys are stale.
- The `cst8xx_touch.sh` tweak — the closed module is gone upstream.
- `SONIX_ISSUE_AUDIT.md` as a living doc — demoted to `docs/legacy-0.x/`; upstream's tracker and our
  new line make its remaining open items historical.
- Merge commits `acd5e8d`, `292c16f` — structure only.
- Our 15 s finish window — upstream's 45 s becomes the behaviour (server-side effect unchanged).

Everything else — the entire Audiobookshelf sync stack, its GUI page, the host test suite, the ADB
automation, the branding — lands on the new base.
