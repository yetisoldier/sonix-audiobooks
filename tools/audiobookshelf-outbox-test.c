#include "src/system/library/audiobookdb.h"
#include "src/system/db/sqlite3.h"
#include "src/system/streaming/abs_sync.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// These tests do not sort titles, but opening the real database registers its collation.
int library_collate_listorder(void *unused, int na, const void *a, int nb, const void *b) {
	(void)unused;
	int cmp = memcmp(a, b, na < nb ? na : nb);
	return cmp ? cmp : na - nb;
}

static void seed(const char *path) {
	sqlite3 *db;
	assert(sqlite3_open(path, &db) == SQLITE_OK);
	assert(sqlite3_exec(db, "INSERT OR IGNORE INTO AUDIOBOOK_TABLE(path,name) VALUES('book-a','A'),('book-b','B')",
		NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_close(db);
}

static void decision_tests(void) {
	long long now = 1790850000000LL;
	abs_remote_progress_t r = {.exists=true,.seconds=50,.updated_ms=now-20000,.server_now_ms=now};
	assert(abs_sync_decide(5, false, now-10000, &r, now) == ABS_DECISION_UPLOAD); // newer rewind
	assert(abs_sync_decide(80, false, now-30000, &r, now) == ABS_DECISION_DOWNLOAD); // newer phone rewind
	assert(abs_sync_decide(80, false, 0, &r, now) == ABS_DECISION_UPLOAD); // legacy/missing timestamp
	assert(abs_sync_decide(5, false, 0, &r, now) == ABS_DECISION_DOWNLOAD);
	assert(abs_sync_decide(5, false, now+3600000, &r, now) == ABS_DECISION_DOWNLOAD); // bad future RTC
	assert(abs_sync_decide(80, false, now-10000, &r, now+3600000) == ABS_DECISION_UPLOAD);
	assert(abs_sync_decide(80, false, r.updated_ms, &r, now) == ABS_DECISION_UPLOAD); // timestamp tie
	r.updated_ms=0;
	assert(abs_sync_decide(5, false, now-10000, &r, now) == ABS_DECISION_DOWNLOAD);
	r.finished=true;
	assert(abs_sync_decide(80, false, 0, &r, now) == ABS_DECISION_DOWNLOAD); // completion is book end
	r.finished=false;
	assert(abs_sync_decide(80, true, 0, &r, now) == ABS_DECISION_UPLOAD);
	assert(abs_sync_decide(50, false, 0, &r, now) == ABS_DECISION_EQUAL);
	r.exists=false;
	assert(abs_sync_decide(5, false, 0, &r, now) == ABS_DECISION_UPLOAD);
}

int main(int argc, char **argv) {
	assert(argc == 3);
	decision_tests();
	assert(audiobookdb_open(argv[1]));
	seed(argv[1]);
	audiobookdb_save_position("book-a", "part-two.mp3", 61);
	audiobookdb_save_position("book-b", "other.mp3", 25);
	audiobookdb_abs_checkpoint_t old, now;
	assert(audiobookdb_abs_get("book-a", &old) && !old.known);
	assert(old.updated_ms > 1577836800000LL && old.revision > 0);
	long long stamp = old.updated_ms, revision = old.revision;
	audiobookdb_save_position("book-a", "part-two.mp3", 61);
	assert(audiobookdb_abs_get("book-a", &old) && old.updated_ms == stamp && old.revision == revision);
	audiobookdb_abs_result(&old, "server-a", true, false, true);
	audiobookdb_close();
	assert(audiobookdb_open(argv[1]));
	assert(audiobookdb_abs_get("book-a", &old) && old.linked && old.failed && !old.synced);
	assert(old.seconds == 61 && strcmp(old.file, "part-two.mp3") == 0);
	assert(audiobookdb_abs_get("book-b", &now) && now.seconds == 25 && !now.synced);
	// A response to an older upload must not acknowledge a newer backward seek.
	audiobookdb_save_position("book-a", "part-one.mp3", 5);
	audiobookdb_abs_result(&old, "server-a", true, true, false);
	assert(audiobookdb_abs_get("book-a", &now) && !now.synced && now.seconds == 5);
	audiobookdb_abs_result(&now, "server-a", true, true, false);
	assert(audiobookdb_abs_get("book-a", &now) && now.synced);
	audiobookdb_mark_finished("book-a");
	assert(audiobookdb_abs_get("book-a", &now) && !now.synced && now.finished);
	audiobookdb_abs_result(&now, "server-a", true, true, false);
	assert(audiobookdb_abs_get("book-a", &now) && now.synced);
	// A local-only book is ignored, and a different server cannot drain this queue.
	assert(audiobookdb_abs_get("book-b", &now));
	audiobookdb_abs_result(&now, "server-a", false, false, false);
	audiobookdb_save_position("book-a", "part-two.mp3", 70);
	audiobookdb_abs_retry("book-a");
	assert(!audiobookdb_abs_next("server-b", &now));
	assert(audiobookdb_abs_next("server-a", &old) && old.seconds == 70);
	// Remote imports are atomic, preserve event time, and cannot undo a local edit.
	assert(audiobookdb_abs_import(&old, "server-a", "part-one.mp3", 12, false, stamp + 5000));
	assert(audiobookdb_abs_get("book-a", &now) && now.synced && now.seconds == 12 && now.updated_ms == stamp + 5000);
	assert(!audiobookdb_abs_import(&old, "server-a", "part-two.mp3", 90, false, stamp + 6000));
	old = now;
	audiobookdb_save_position("book-a", "part-two.mp3", 70);
	assert(!audiobookdb_abs_import(&old, "server-a", "part-two.mp3", 99, false, stamp + 6000));
	assert(audiobookdb_abs_get("book-a", &old));
	// The old card's in-flight acknowledgement cannot mark the new card synced.
	audiobookdb_close();
	assert(audiobookdb_open(argv[2]));
	seed(argv[2]);
	audiobookdb_save_position("book-a", "new-card.mp3", 3);
	audiobookdb_abs_result(&old, "server-a", true, true, false);
	assert(audiobookdb_abs_get("book-a", &now) && !now.known && !now.synced && now.seconds == 3);
	audiobookdb_close();
	assert(audiobookdb_open(argv[1]));
	assert(audiobookdb_abs_get("book-a", &now) && !now.synced && now.seconds == 70);
	audiobookdb_close();
	puts("Audiobookshelf outbox passed: restart, multiple books, backward seek, stale acknowledgement, completion, server/card isolation");
	return 0;
}
