#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PLAYER="$ROOT/sonix-player"
WORK="${SONIX_ABS_SMOKE_DIR:-$ROOT/build/audiobookshelf-host-smoke}"
RUN="$WORK/run"
SD="$WORK/sd"
CONFIG="$WORK/device_config.ini"
ASOUND="$WORK/asound.conf"
DB="$SD/.local/audiobooks.db"
PORT_FILE="$WORK/mock-port"
PROGRESS_FILE="$WORK/progress.json"
TOKEN=$(printf 't%.0s' {1..208})
PLAYER_PID=""
SERVER_PID=""
XVFB_PID=""
WINDOW=""
EXISTING=${SONIX_ABS_EXISTING:-0}
LOCAL_PROGRESS=${SONIX_ABS_LOCAL_PROGRESS:-0}
OFFLINE_RESTART=${SONIX_ABS_OFFLINE_RESTART:-0}
CONFLICTS=${SONIX_ABS_CONFLICTS:-0}

fail() { printf 'Audiobookshelf smoke test: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "missing required command: $1"; }

cleanup() {
	for pid in "$PLAYER_PID" "$SERVER_PID" "$XVFB_PID"; do
		if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then kill -TERM "$pid" 2>/dev/null || true; fi
	done
}
trap cleanup EXIT INT TERM

wait_for() {
	local description=$1 attempts=$2
	shift 2
	for ((i = 0; i < attempts; i++)); do
		if "$@"; then return 0; fi
		sleep 0.1
	done
	fail "timed out waiting for $description"
}

resume_ready() {
	[[ -f "$DB" ]] || return 1
	local file seconds
	file=$(sqlite3 "$DB" "SELECT resume_file FROM AUDIOBOOK_TABLE WHERE name='The Mock Journey';" 2>/dev/null)
	seconds=$(sqlite3 "$DB" "SELECT resume_pos FROM AUDIOBOOK_TABLE WHERE name='The Mock Journey';" 2>/dev/null)
	if [[ "$LOCAL_PROGRESS" == 1 ]]; then
		[[ "$file" == *"Local Chapter One.mp3" ]] || return 1
		awk -v value="$seconds" 'BEGIN { exit !(value >= 6.9 && value <= 7.1) }'
		return
	elif [[ "$EXISTING" == 1 ]]; then
		[[ "$file" == *"Local Chapter Two.mp3" ]] || return 1
	else
		[[ "$file" == *"002 - Part Two.mp3" ]] || return 1
	fi
	awk -v value="$seconds" 'BEGIN { exit !(value >= 2.9 && value <= 3.1) }'
}

local_catalog_ready() {
	[[ -f "$DB" ]] || return 1
	[[ $(sqlite3 "$DB" "SELECT COUNT(*) FROM AUDIOBOOK_TABLE WHERE name='The Mock Journey';" 2>/dev/null) == 1 ]]
}

progress_uploaded() {
	[[ -s "$PROGRESS_FILE" ]] || return 1
	if [[ "$LOCAL_PROGRESS" == 1 ]]; then
		python3 -c 'import json,sys; p=json.load(open(sys.argv[1])); t=float(p.get("currentTime", 0)); sys.exit(not (6.9 <= t <= 7.1 and p.get("isFinished") is False))' "$PROGRESS_FILE"
	else
		python3 -c 'import json,sys; p=json.load(open(sys.argv[1])); t=float(p.get("currentTime", 0)); sys.exit(not (13.5 <= t <= 20 and p.get("isFinished") is False))' "$PROGRESS_FILE"
	fi
}

click() {
	xdotool mousemove --window "$WINDOW" "$1" "$2" click 1
	sleep "${3:-0.6}"
}

for command in ffmpeg make python3 scrot sqlite3 Xvfb xdotool; do need "$command"; done
[[ "$ROOT" != *' '* ]] || fail "the checkout path contains spaces"
case "$WORK" in "$ROOT"/build/*) rm -rf -- "$WORK" ;; *) fail "unsafe output directory: $WORK" ;; esac
mkdir -p "$RUN/usr/resource" "$WORK/media" "$SD"

if [[ ${SONIX_SMOKE_SKIP_BUILD:-0} != 1 ]]; then make -C "$PLAYER" host -j2; fi
[[ -x "$PLAYER/sonix_player_host" ]] || fail "host binary is missing"
cp "$PLAYER/sonix_player_host" "$RUN/"
cp -a "$ROOT/sonix-packer/assets/R1/usr/resource/." "$RUN/usr/resource/"

ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:duration=10" -ac 1 -b:a 64k "$WORK/media/part1.mp3"
ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=550:duration=10" -ac 1 -b:a 64k "$WORK/media/part2.mp3"
ffmpeg -hide_banner -loglevel error -f lavfi -i "color=c=0x3584e4:s=64x64" -frames:v 1 "$WORK/media/cover.jpg"

if [[ "$EXISTING" == 1 ]]; then
	LOCAL="$SD/Audiobooks/Test Author/The Mock Journey"
	mkdir -p "$LOCAL"
	cp "$WORK/media/part1.mp3" "$LOCAL/Local Chapter One.mp3"
	cp "$WORK/media/part2.mp3" "$LOCAL/Local Chapter Two.mp3"
	for file in "$LOCAL"/*.mp3; do
		cat >"$file.tags" <<'EOF'
album=The Mock Journey
artist=Test Author
album_artist=Test Author
EOF
	done
	mkdir -p "$(dirname "$DB")"
	sqlite3 "$DB" <<EOF
CREATE TABLE AUDIOBOOK_TABLE(path TEXT PRIMARY KEY, name TEXT COLLATE NOCASE, size INT, mtime INT,
 last_played INT DEFAULT 0, resume_file TEXT, resume_pos REAL, author TEXT DEFAULT '',
 series TEXT DEFAULT '', series_part REAL, added INT DEFAULT 0, folder INT DEFAULT 0,
 location TEXT DEFAULT '', summary TEXT DEFAULT '');
CREATE TABLE AUDIOBOOK_PARTS(path TEXT PRIMARY KEY, book TEXT, idx INT, title TEXT);
CREATE INDEX AUDIOBOOK_PARTS_BOOK ON AUDIOBOOK_PARTS(book, idx);
INSERT INTO AUDIOBOOK_TABLE(path,name,size,mtime,author,series,folder,location)
 VALUES('$LOCAL','The Mock Journey',1,1,'Test Author','Fixture Series',1,'$LOCAL');
INSERT INTO AUDIOBOOK_PARTS(path,book,idx,title) VALUES
 ('$LOCAL/Local Chapter One.mp3','$LOCAL',0,'Local Chapter One'),
 ('$LOCAL/Local Chapter Two.mp3','$LOCAL',1,'Local Chapter Two');
EOF
	if [[ "$LOCAL_PROGRESS" == 1 ]]; then
		sqlite3 "$DB" "ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN resume_updated_ms INT DEFAULT 0; UPDATE AUDIOBOOK_TABLE SET resume_file='$LOCAL/Local Chapter One.mp3',resume_pos=7,last_played=1,resume_updated_ms=$(date +%s)000 WHERE path='$LOCAL';"
	fi
fi

python3 "$ROOT/tools/audiobookshelf_mock.py" --media-dir "$WORK/media" --port-file "$PORT_FILE" \
	--progress-file "$PROGRESS_FILE" --token "$TOKEN" >"$WORK/mock.stdout" 2>"$WORK/mock.stderr" &
SERVER_PID=$!
wait_for "mock server" 50 test -s "$PORT_FILE"
PORT=$(cat "$PORT_FILE")

cat >"$CONFIG" <<EOF
[player]
volume = 40
remember_track = 1
[ui]
language = English
[power]
screen_off_seconds = 0
[clock]
utc_offset_minutes = 0
last_seen = $(date +%s)
configured = 1
[audiobookshelf]
server = http://127.0.0.1:$PORT
token = $TOKEN
EOF
cat >"$ASOUND" <<'EOF'
pcm.!default { type null }
ctl.!default { type hw card 0 }
EOF

DISPLAY_FILE="$WORK/x-display"
Xvfb -nolisten unix -listen tcp -ac -displayfd 3 -screen 0 600x900x24 \
	>"$WORK/xvfb.stdout" 2>"$WORK/xvfb.log" 3>"$DISPLAY_FILE" &
XVFB_PID=$!
wait_for "Xvfb" 50 test -s "$DISPLAY_FILE"
export DISPLAY="localhost:$(cat "$DISPLAY_FILE")"

(
	cd "$RUN"
	exec env SONIX_SD_ROOT="$SD" SONIX_CONFIG="$CONFIG" SONIX_EBOOK_CONFIG="$WORK/ebook_config.ini" \
		SONIX_LOG="$WORK/player.log" ALSA_CONFIG_PATH="$ASOUND" \
		./sonix_player_host >"$WORK/player.stdout" 2>&1
) &
PLAYER_PID=$!
for ((i = 0; i < 100; i++)); do
	WINDOW=$(xdotool search --onlyvisible --pid "$PLAYER_PID" 2>/dev/null | head -n 1 || true)
	[[ -n "$WINDOW" ]] && break
	sleep 0.1
done
[[ -n "$WINDOW" ]] || fail "the simulator window did not appear"
sleep 1

if [[ "$EXISTING" == 1 ]]; then
	wait_for "existing local catalog" 100 local_catalog_ready
fi

click 355 245 1       # Home -> Streaming
click 175 700 2       # Streaming -> Audiobookshelf
click 280 305 2       # library
click 240 180 2       # Authors toolbar
scrot "$WORK/authors.png"
click 240 200 2       # Test Author
scrot "$WORK/author-books.png"
click 43 88 1         # author books -> authors
click 43 88 2         # authors -> all books
click 390 180 2       # Series toolbar
scrot "$WORK/series.png"
click 240 200 2       # Fixture Series
scrot "$WORK/series-books.png"
click 43 88 1
click 43 88 2
click 80 180 1        # search keyboard
scrot "$WORK/search-keyboard.png"
click 390 680 0.2     # m on keyboard
click 430 754 2       # submit search
scrot "$WORK/search-results.png"
grep -q '/search?q=m&limit=40' "$WORK/progress.requests" || fail "search did not reach server"
grep -q 'filter=authors.YXV0aG9yLTE' "$WORK/progress.requests" || fail "author filter missing"
grep -q 'sort=sequence.*filter=series.c2VyaWVzLTE' "$WORK/progress.requests" || fail "series order/filter missing"
click 43 88 1         # search results -> libraries
click 280 305 2       # reopen full library
click 240 277 1       # first title below toolbar
scrot "$WORK/action-confirm.png"
click 330 475 1       # confirm download or local link

wait_for "downloaded resume import" 900 resume_ready
if [[ "$EXISTING" == 1 ]]; then
	[[ -s "$SD/Audiobooks/Test Author/The Mock Journey/.audiobookshelf" ]] || fail "local link manifest missing"
	[[ ! -e "$SD/Audiobooks/Audiobookshelf/Test Author/The Mock Journey" ]] || fail "linked book was downloaded again"
	if [[ "$LOCAL_PROGRESS" == 1 ]]; then
		wait_for "uploaded existing local progress" 400 progress_uploaded
	fi
	sleep 2                # let the success toast release the title row
	click 240 277 2        # the linked server title must now play locally
	if [[ "$LOCAL_PROGRESS" == 1 ]]; then
		wait_for "linked local playback" 100 grep -q "play requested.*Local Chapter One.mp3.*from 7" "$WORK/player.log"
	else
		wait_for "linked local playback" 100 grep -q "play requested.*Local Chapter Two.mp3.*from 3" "$WORK/player.log"
	fi
else
	[[ -s "$SD/Audiobooks/Audiobookshelf/Test Author/The Mock Journey/001 - Part One.mp3" ]] || fail "part one missing"
	[[ -s "$SD/Audiobooks/Audiobookshelf/Test Author/The Mock Journey/002 - Part Two.mp3" ]] || fail "part two missing"
	[[ -s "$SD/Audiobooks/Audiobookshelf/Test Author/The Mock Journey/cover.jpg" ]] || fail "cover missing"
fi
scrot "$WORK/action-complete.png"

if [[ "$EXISTING" == 1 ]]; then
	if [[ "$LOCAL_PROGRESS" == 1 ]]; then
		printf 'Audiobookshelf smoke test passed: existing-copy link kept and uploaded local progress, with no duplicate download.\n'
	else
		printf 'Audiobookshelf smoke test passed: existing-copy link and part 2 resume at 3.0 seconds, with no duplicate download.\n'
	fi
	exit 0
fi

sleep 2                # let the success toast release the back button
click 43 88 2          # titles -> libraries
click 43 88 2          # Audiobookshelf -> Streaming
click 43 88 2          # Streaming -> Home
click 355 420 1        # Home -> Audiobooks
click 125 245 1        # Audiobooks -> Library
click 240 180 2        # resume the downloaded book
scrot "$WORK/local-playback.png"
wait_for "playback from imported part" 100 grep -q "play requested.*002 - Part Two.mp3.*from 3" "$WORK/player.log"
sleep 2
rm -f "$PROGRESS_FILE"       # ignore the immediate start checkpoint
if [[ "$OFFLINE_RESTART" == 1 ]]; then touch "$WORK/progress.offline"; fi
xdotool key --window "$WINDOW" n # pause, forcing a local + server checkpoint
if [[ "$OFFLINE_RESTART" == 1 ]]; then
	wait_for "failed upload retained" 200 grep -q 'saved checkpoint retained' "$WORK/player.log"
	[[ ! -s "$PROGRESS_FILE" ]] || fail "server accepted progress during outage"
	sleep 2                # allow the idle UI poll to draw the retry status
	scrot "$WORK/offline-pending.png"
	kill -TERM "$PLAYER_PID"
	wait "$PLAYER_PID" || true
	PLAYER_PID=""
	# No playback action on the restarted player: reconnect alone must drain it.
	rm -f "$WORK/progress.offline"
	(
		cd "$RUN"
		exec env SONIX_SD_ROOT="$SD" SONIX_CONFIG="$CONFIG" SONIX_EBOOK_CONFIG="$WORK/ebook_config.ini" \
			SONIX_LOG="$WORK/restart.log" ALSA_CONFIG_PATH="$ASOUND" \
			./sonix_player_host >"$WORK/restart.stdout" 2>&1
	) &
	PLAYER_PID=$!
	wait_for "offline checkpoint uploaded after restart" 900 progress_uploaded
	printf 'Audiobookshelf offline smoke passed: failed upload survived restart and uploaded without resuming playback.\n'
	exit 0
fi
wait_for "uploaded server progress" 400 progress_uploaded

if [[ "$CONFLICTS" == 1 ]]; then
	# The paused decoder is deliberately still on part two. A newer phone rewind
	# to part one must survive idle polls and be used by the physical Play key.
	python3 - "$DB" "$PROGRESS_FILE" <<'PY'
import json, sqlite3, sys, time
db = sqlite3.connect(sys.argv[1])
db.execute("UPDATE AUDIOBOOK_ABS_SYNC SET attempted=0")
db.commit()
with open(sys.argv[2], "w") as f:
    json.dump({"currentTime": 1, "isFinished": False, "lastUpdate": int(time.time()*1000)+1000}, f)
PY
	imported_rewind() { [[ $(sqlite3 "$DB" 'SELECT resume_pos FROM AUDIOBOOK_TABLE LIMIT 1;') == '1.0' ]]; }
	wait_for "newer remote rewind" 300 imported_rewind
	sleep 2
	imported_rewind || fail "paused decoder overwrote server import"
	xdotool key --window "$WINDOW" n
	wait_for "physical Play at imported part one" 100 grep -q 'play requested.*001 - Part One.mp3.*from 1' "$WORK/player.log"
	sleep 1
	xdotool key --window "$WINDOW" n
	kill -TERM "$PLAYER_PID"
	wait "$PLAYER_PID" || true
	PLAYER_PID=""

	for scenario in local-rewind missing-local missing-remote future-local equal-time; do
		python3 - "$DB" "$PROGRESS_FILE" "$scenario" <<'PY'
import json, sqlite3, sys, time
now = int(time.time()*1000)
case = sys.argv[3]
local, remote, ls, rs = {
    "local-rewind": (5, 17, now-1000, now-3000),
    "missing-local": (5, 17, 0, now-3000),
    "missing-remote": (15, 5, now-1000, 0),
    "future-local": (5, 17, now+3600000, now-3000),
    "equal-time": (15, 5, now-1000, now-1000),
}[case]
db = sqlite3.connect(sys.argv[1])
book, = db.execute("SELECT path FROM AUDIOBOOK_TABLE LIMIT 1").fetchone()
part = f'{book}/' + ('001 - Part One.mp3' if local < 10 else '002 - Part Two.mp3')
db.execute("UPDATE AUDIOBOOK_TABLE SET resume_file=?,resume_pos=?,resume_updated_ms=?,resume_revision=resume_revision+1,resume_completed=0", (part, local % 10, ls))
db.execute("UPDATE AUDIOBOOK_ABS_SYNC SET attempted=0")
db.commit()
with open(sys.argv[2], "w") as f:
    json.dump({"currentTime": remote, "isFinished": False, "lastUpdate": rs}, f)
PY
		(
			cd "$RUN"
			exec env SONIX_SD_ROOT="$SD" SONIX_CONFIG="$CONFIG" SONIX_EBOOK_CONFIG="$WORK/ebook_config.ini" \
				SONIX_LOG="$WORK/$scenario.log" ALSA_CONFIG_PATH="$ASOUND" \
				./sonix_player_host >"$WORK/$scenario.stdout" 2>&1
		) &
		PLAYER_PID=$!
		conflict_resolved() {
			python3 - "$DB" "$PROGRESS_FILE" "$scenario" <<'PY'
import json, sqlite3, sys
expected = {"local-rewind": 5, "missing-local": 17, "missing-remote": 15, "future-local": 17, "equal-time": 15}[sys.argv[3]]
try:
    db = sqlite3.connect(sys.argv[1])
    file, pos, ack, rev, ackrev = db.execute("SELECT a.resume_file,a.resume_pos,s.seconds,a.resume_revision,s.revision FROM AUDIOBOOK_TABLE a JOIN AUDIOBOOK_ABS_SYNC s ON a.path=s.book").fetchone()
    total = pos + (10 if '/002 - ' in file else 0)
    remote = json.load(open(sys.argv[2]))['currentTime']
    sys.exit(not (abs(total-expected)<0.01 and abs(remote-expected)<0.01 and pos==ack and rev==ackrev))
except (OSError, ValueError, sqlite3.Error):
    sys.exit(1)
PY
		}
		wait_for "conflict $scenario" 400 conflict_resolved
		kill -TERM "$PLAYER_PID"
		wait "$PLAYER_PID" || true
		PLAYER_PID=""
	done
	printf 'Audiobookshelf conflict smoke passed: paused multipart remote rewind, physical Play, local rewind, missing/future timestamps, and timestamp ties.\n'
fi

printf 'Audiobookshelf smoke test passed: download, part 2 resume at 3.0 seconds, and server progress upload.\n'
