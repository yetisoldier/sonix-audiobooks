#define _GNU_SOURCE 1 // strcasestr
#include "audiobookdb.h"

#include "src/system/decode/mp4.h"
#include "src/system/library/id3chap.h"
#include "src/system/library/vorbischap.h"
#include "src/system/library/metadata.h"
#include "src/system/playback/playlist.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/utils.h"
#include "src/system/db/sqlite3.h"
#include "src/system/library/library.h" // library_collate_listorder: the shared ordering

#define SCAN_MAX_DEPTH 12
#define SCAN_COMMIT_EVERY 50

// Entries read from one folder at most, files and subfolders together.
#define SCAN_MAX_ENTRIES 4096

// PRAGMA user_version of an index whose rows carry authors, series and folder
// books. Anything lower was written by an older scan.
#define SCHEMA_VERSION 3

static sqlite3 *db;
static pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;

// Bumped whenever the row ids of AUDIOBOOK_TABLE may have moved: a scan empties
// and refills it, a different card is a different set of books, and marking one
// finished changes which rows the finished list names. A handle built before
// the bump is reading somebody else's rows, so it is made to say so instead.
static unsigned generation = 1;

static bool outdated;

// Resume checkpoints are the only regular writes made while a book is
// playing. They used to run on the LVGL poll thread, so an SD card taking a
// while to finish an UPDATE also stopped touch, redraws and button feedback.
// Keep one pending checkpoint here: there is only one active book, and a newer
// position for it makes an older one obsolete. Deliberate actions flush the
// worker, while periodic checkpoints merely replace the pending value.
typedef struct {
	char book[512];
	char file[512];
	double seconds;
	long long updated_ms;
	unsigned generation;
} position_job_t;

static pthread_mutex_t position_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t position_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t position_once = PTHREAD_ONCE_INIT;
static pthread_t position_thread;
static position_job_t position_pending;
static bool position_has_pending;
static bool position_busy;
static bool position_worker_ready;
static bool position_accepting;

// Call with db_lock held.
static void bump_generation(void) { generation++; }

// Writes one checkpoint only while it still belongs to the database that was
// open when it was queued. This prevents a late write for a removed card from
// landing in a newly inserted card whose folders happen to have the same name.
static void position_write(const position_job_t *job) {
	pthread_mutex_lock(&db_lock);
	if (db && job->generation == generation) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(db,
						   "UPDATE AUDIOBOOK_TABLE SET resume_file=?1, resume_pos=?2, last_played=?3,"
						   "resume_updated_ms=?5,resume_revision=resume_revision+1,"
						   "resume_completed=CASE WHEN ?2>0 THEN 0 ELSE resume_completed END WHERE path=?4 AND "
						   "(resume_file IS NULL OR resume_file<>?1 OR resume_pos<>?2)",
						   -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, job->file, -1, SQLITE_TRANSIENT);
			sqlite3_bind_double(stmt, 2, job->seconds);
			sqlite3_bind_int64(stmt, 3, (sqlite3_int64)time(NULL));
			sqlite3_bind_text(stmt, 4, job->book, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int64(stmt, 5, job->updated_ms);
			int rc = sqlite3_step(stmt);
			if (rc != SQLITE_DONE) {
				fprintf(stderr, "audiobooks: resume checkpoint failed: %s\n", sqlite3_errmsg(db));
			}
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&db_lock);
}

static void *position_worker(void *unused) {
	(void)unused;
	for (;;) {
		pthread_mutex_lock(&position_lock);
		while (!position_has_pending) {
			pthread_cond_wait(&position_cond, &position_lock);
		}
		position_job_t job = position_pending;
		position_has_pending = false;
		position_busy = true;
		pthread_mutex_unlock(&position_lock);

		position_write(&job);

		pthread_mutex_lock(&position_lock);
		position_busy = false;
		pthread_cond_broadcast(&position_cond);
		pthread_mutex_unlock(&position_lock);
	}
	return NULL;
}

static void position_worker_start(void) {
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	// The worker holds two paths and calls one small SQLite UPDATE. A bounded
	// stack avoids paying the platform default for a thread that sleeps almost
	// all of its life on a memory-constrained player.
	pthread_attr_setstacksize(&attr, 64 * 1024);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&position_thread, &attr, position_worker, NULL) == 0) {
		position_worker_ready = true;
	} else {
		fprintf(stderr, "audiobooks: could not start resume checkpoint worker\n");
	}
	pthread_attr_destroy(&attr);
}

unsigned audiobookdb_revision(void) {
	pthread_mutex_lock(&db_lock);
	unsigned value = generation;
	pthread_mutex_unlock(&db_lock);
	return value;
}

static pthread_t scan_thread;
static volatile bool scan_running;
static volatile bool scan_cancel;
static volatile int scan_found;
static char scan_root[512];

// For statements that are expected to fail once the schema is already right:
// the ALTER TABLEs below run on every open and say "duplicate column name" on
// every open after the first.
static bool exec_quiet(const char *sql) {
	char *error = NULL;
	bool ok = sqlite3_exec(db, sql, NULL, NULL, &error) == SQLITE_OK;
	sqlite3_free(error);
	return ok;
}

static bool exec(const char *sql) {
	char *error = NULL;
	if (sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK) {
		fprintf(stderr, "audiobooks: %s\n", error ? error : "unknown error");
		sqlite3_free(error);
		return false;
	}
	return true;
}

static int user_version(void) {
	int version = 0;
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			version = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	return version;
}

bool audiobookdb_open(const char *db_path) {
	if (db) {
		return true;
	}
	if (!db_path || !db_path[0]) {
		return false;
	}

	if (sqlite3_open(db_path, &db) != SQLITE_OK) {
		fprintf(stderr, "audiobooks: cannot open %s: %s\n", db_path, sqlite3_errmsg(db));
		sqlite3_close(db);
		db = NULL;
		return false;
	}

	sqlite3_create_collation(db, "listorder", SQLITE_UTF8, NULL, library_collate_listorder);

	// Same journal choices as the music library: a slow card that may be
	// pulled at any moment.
	exec("PRAGMA synchronous=NORMAL");
	exec("PRAGMA journal_mode=TRUNCATE");
	exec("PRAGMA cache_size=-128");

	exec("CREATE TABLE IF NOT EXISTS AUDIOBOOK_TABLE("
		 "path TEXT PRIMARY KEY, name TEXT COLLATE NOCASE, size INT, mtime INT,"
		 "last_played INT DEFAULT 0, resume_file TEXT, resume_pos REAL,"
		 "author TEXT DEFAULT '', series TEXT DEFAULT '', series_part REAL, added INT DEFAULT 0,"
		 "folder INT DEFAULT 0, location TEXT DEFAULT '', summary TEXT DEFAULT '')");

	// The columns a database written by an older scan does not have. They fail
	// harmlessly once they are there, and must run after the CREATE, or on a
	// fresh card there is no table to alter.
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN resume_file TEXT");
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN resume_pos REAL");
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN author TEXT DEFAULT ''");
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN series TEXT DEFAULT ''");
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN series_part REAL");
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN added INT DEFAULT 0");
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN folder INT DEFAULT 0");
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN location TEXT DEFAULT ''");
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN summary TEXT DEFAULT ''");

	// The files of the folder books, a row each, in the book's order.
	exec("CREATE TABLE IF NOT EXISTS AUDIOBOOK_PARTS(path TEXT PRIMARY KEY, book TEXT, idx INT, title TEXT)");
	exec("CREATE INDEX IF NOT EXISTS AUDIOBOOK_PARTS_BOOK ON AUDIOBOOK_PARTS(book, idx)");

	// The books heard to the end. Deliberately not a column: a rescan wipes
	// AUDIOBOOK_TABLE and refills it, and having read a book is not something
	// a rescan may forget.
	exec("CREATE TABLE IF NOT EXISTS AUDIOBOOK_FINISHED("
		 "path TEXT PRIMARY KEY, finished_at INT DEFAULT 0)");

	// Manual marks belong to the card and survive a catalog rebuild. Joining
	// them to AUDIOBOOK_TABLE when listed hides marks for a removed book while
	// preserving them in case the book is copied back later.
	exec("CREATE TABLE IF NOT EXISTS AUDIOBOOK_BOOKMARKS("
		 "id INTEGER PRIMARY KEY AUTOINCREMENT, book TEXT NOT NULL, file TEXT NOT NULL,"
		 "seconds REAL NOT NULL, label TEXT DEFAULT '', created_at INT DEFAULT 0)");
	exec("CREATE INDEX IF NOT EXISTS AUDIOBOOK_BOOKMARKS_BOOK ON AUDIOBOOK_BOOKMARKS(book, file, seconds)");

	exec("CREATE TABLE IF NOT EXISTS AUDIOBOOK_ABS_SYNC("
		 "book TEXT PRIMARY KEY, server TEXT NOT NULL, linked INT NOT NULL,"
		 "file TEXT, seconds REAL, finished INT, attempted INT DEFAULT 0, failed INT DEFAULT 0)");
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN resume_updated_ms INT DEFAULT 0");
	exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN resume_revision INT DEFAULT 0");
	if (exec_quiet("ALTER TABLE AUDIOBOOK_TABLE ADD COLUMN resume_completed INT DEFAULT 0"))
		exec("UPDATE AUDIOBOOK_TABLE SET resume_completed=1 WHERE resume_pos=0 AND "
			"EXISTS(SELECT 1 FROM AUDIOBOOK_FINISHED f WHERE f.path=AUDIOBOOK_TABLE.path)");
	exec_quiet("ALTER TABLE AUDIOBOOK_ABS_SYNC ADD COLUMN revision INT DEFAULT -1");
	exec_quiet("ALTER TABLE AUDIOBOOK_ABS_SYNC ADD COLUMN server_updated_ms INT DEFAULT 0");

	int books = 0;
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM AUDIOBOOK_TABLE", -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			books = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	outdated = books > 0 && user_version() < SCHEMA_VERSION;

	// A different card is a different set of books, and the row ids that named
	// the last one's are now this one's. The position lock closes the small gap
	// in which a checkpoint could otherwise capture this generation before the
	// database is ready to accept it.
	pthread_mutex_lock(&position_lock);
	pthread_mutex_lock(&db_lock);
	bump_generation();
	position_accepting = true;
	pthread_mutex_unlock(&db_lock);
	pthread_mutex_unlock(&position_lock);

	printf("audiobooks: %s open, %d books indexed%s\n", db_path, books, outdated ? " (an older scan)" : "");
	return true;
}

void audiobookdb_close(void) {
	// Stop accepting new checkpoints, then wait for the last one already
	// accepted. Taking both locks in this order makes the handoff atomic with
	// queue_position(); the worker never holds position_lock while taking
	// db_lock, so it can still drain normally.
	pthread_mutex_lock(&position_lock);
	pthread_mutex_lock(&db_lock);
	position_accepting = false;
	pthread_mutex_unlock(&db_lock);
	pthread_mutex_unlock(&position_lock);
	audiobookdb_flush_positions();

	// Waited on, not just asked: a scan still running holds the database open,
	// sqlite3_close() then answers SQLITE_BUSY without closing the file, and
	// an open file on the card stops the card being unmounted.
	audiobookdb_scan_stop();
	for (int i = 0; i < 400 && audiobookdb_scan_running(); i++) {
		usleep(10 * 1000);
	}

	pthread_mutex_lock(&db_lock);
	if (db) {
		int rc = sqlite3_close(db);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "audiobooks: sqlite3_close returned %d; the handle may be leaking\n", rc);
		}
		db = NULL;
	}
	outdated = false;
	bump_generation();
	pthread_mutex_unlock(&db_lock);
}

bool audiobookdb_needs_rescan(void) { return db && outdated && !scan_running; }

int audiobookdb_count(void) {
	pthread_mutex_lock(&db_lock);

	int count = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM AUDIOBOOK_TABLE", -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			count = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&db_lock);
	return count;
}

// ---------------------------------------------------------------------------
// List handles
//
// The one ordered pass happens at open and its result is four bytes a row; the
// rows themselves are read back a windowful at a time, by row id.
// ---------------------------------------------------------------------------

struct audiobookdb_index {
	int32_t *rows;
	int count;
	unsigned generation;
	bool with_part; // the names carry the book's number in its series
};

// The ORDER BY of a list. `t` is the book table's alias, `when` the column the
// date order reads.
static void order_clause(char *out, size_t size, audiobook_order_t order, bool desc, const char *when) {
	const char *dir = desc ? "DESC" : "ASC";
	switch (order) {
	// Books from the same moment, or never played, keep A-Z among themselves.
	case AUDIOBOOK_ORDER_ADDED:
		snprintf(out, size, "%s %s, t.name COLLATE listorder", when, dir);
		break;
	case AUDIOBOOK_ORDER_PLAYED:
		snprintf(out, size, "t.last_played %s, t.name COLLATE listorder", dir);
		break;
	case AUDIOBOOK_ORDER_SERIES:
		// The books with no number go after the numbered ones.
		snprintf(out, size, "t.series_part IS NULL, t.series_part %s, t.name COLLATE listorder %s", dir, dir);
		break;
	default:
		snprintf(out, size, "t.name COLLATE listorder %s", dir);
		break;
	}
}

audiobookdb_index_t *audiobookdb_index_open(audiobook_list_t kind, const char *value, audiobook_order_t order, bool desc) {
	struct audiobookdb_index *ix = calloc(1, sizeof(*ix));
	if (!ix) {
		return NULL;
	}
	ix->with_part = kind == AUDIOBOOK_LIST_SERIES;

	char orderby[160];
	char sql[640];
	switch (kind) {
	case AUDIOBOOK_LIST_FINISHED:
		// The join keeps a book that has left the card out of the list without
		// deleting the fact that it was finished: put the file back and it is
		// there again.
		order_clause(orderby, sizeof(orderby), order, desc, "f.finished_at");
		snprintf(sql, sizeof(sql), "SELECT t.rowid FROM AUDIOBOOK_TABLE t JOIN AUDIOBOOK_FINISHED f ON f.path = t.path ORDER BY %s", orderby);
		break;
	case AUDIOBOOK_LIST_CONTINUE:
		// Listened to, and left somewhere past the start of the first file: a
		// book finished goes back to that start, and so leaves this list.
		order_clause(orderby, sizeof(orderby), order, desc, "t.added");
		snprintf(sql, sizeof(sql),
				 "SELECT t.rowid FROM AUDIOBOOK_TABLE t WHERE t.last_played > 0 AND t.resume_file IS NOT NULL AND"
				 " (t.resume_pos > 1 OR t.resume_file != COALESCE((SELECT p.path FROM AUDIOBOOK_PARTS p"
				 " WHERE p.book = t.path ORDER BY p.idx LIMIT 1), t.path)) ORDER BY %s",
				 orderby);
		break;
	case AUDIOBOOK_LIST_AUTHOR:
		order_clause(orderby, sizeof(orderby), order, desc, "t.added");
		snprintf(sql, sizeof(sql), "SELECT t.rowid FROM AUDIOBOOK_TABLE t WHERE t.author = ?1 COLLATE NOCASE ORDER BY %s", orderby);
		break;
	case AUDIOBOOK_LIST_SERIES:
		order_clause(orderby, sizeof(orderby), order, desc, "t.added");
		snprintf(sql, sizeof(sql), "SELECT t.rowid FROM AUDIOBOOK_TABLE t WHERE t.series = ?1 COLLATE NOCASE ORDER BY %s", orderby);
		break;
	default:
		order_clause(orderby, sizeof(orderby), order, desc, "t.added");
		snprintf(sql, sizeof(sql), "SELECT t.rowid FROM AUDIOBOOK_TABLE t ORDER BY %s", orderby);
		break;
	}

	pthread_mutex_lock(&db_lock);
	ix->generation = generation;

	int capacity = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		if (kind == AUDIOBOOK_LIST_AUTHOR || kind == AUDIOBOOK_LIST_SERIES) {
			sqlite3_bind_text(stmt, 1, value ? value : "", -1, SQLITE_TRANSIENT);
		}
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			if (ix->count == capacity) {
				int grown = capacity ? capacity * 2 : 64;
				int32_t *bigger = realloc(ix->rows, (size_t)grown * sizeof(*bigger));
				if (!bigger) {
					break;
				}
				ix->rows = bigger;
				capacity = grown;
			}
			ix->rows[ix->count++] = (int32_t)sqlite3_column_int64(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return ix;
}

void audiobookdb_index_close(audiobookdb_index_t *ix) {
	if (!ix) {
		return;
	}
	free(ix->rows);
	free(ix);
}

int audiobookdb_index_count(const audiobookdb_index_t *ix) { return ix ? ix->count : 0; }

bool audiobookdb_index_stale(const audiobookdb_index_t *ix) {
	if (!ix) {
		return true;
	}
	pthread_mutex_lock(&db_lock);
	bool stale = ix->generation != generation;
	pthread_mutex_unlock(&db_lock);
	return stale;
}

int audiobookdb_index_window(const audiobookdb_index_t *ix, int offset, int count, audiobook_row_cb cb, void *user) {
	if (!ix || !cb || offset < 0 || count <= 0 || offset >= ix->count) {
		return 0;
	}
	if (offset + count > ix->count) {
		count = ix->count - offset;
	}

	pthread_mutex_lock(&db_lock);

	// A handle from before a rescan or a card change names rows that belong to
	// somebody else now. Answering nothing is what makes the caller reload.
	if (ix->generation != generation) {
		pthread_mutex_unlock(&db_lock);
		return 0;
	}

	int delivered = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT name, path, series_part FROM AUDIOBOOK_TABLE WHERE rowid=?", -1, &stmt, NULL) == SQLITE_OK) {
		for (int i = 0; i < count; i++) {
			sqlite3_reset(stmt);
			sqlite3_bind_int64(stmt, 1, ix->rows[offset + i]);
			if (sqlite3_step(stmt) != SQLITE_ROW) {
				// The row went while the window was being read. The window is
				// positional, so it gets a blank rather than the next row
				// shifted up into its place.
				if (!cb("", "", user)) {
					break;
				}
				delivered++;
				continue;
			}
			const char *name = (const char *)sqlite3_column_text(stmt, 0);
			const char *path = (const char *)sqlite3_column_text(stmt, 1);
			char numbered[300];
			if (ix->with_part && sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
				snprintf(numbered, sizeof(numbered), "%g. %s", sqlite3_column_double(stmt, 2), name ? name : "");
				name = numbered;
			}
			if (!cb(name ? name : "", path ? path : "", user)) {
				break;
			}
			delivered++;
		}
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&db_lock);
	return delivered;
}

// ---------------------------------------------------------------------------
// Authors and series
// ---------------------------------------------------------------------------

int audiobookdb_names_for_each(audiobook_names_t kind, bool by_added, bool desc, audiobook_name_cb cb, void *user) {
	if (!cb) {
		return 0;
	}
	const char *column = kind == AUDIOBOOK_NAMES_SERIES ? "series" : "author";
	const char *where = kind == AUDIOBOOK_NAMES_SERIES ? "WHERE series != ''" : "";
	const char *dir = desc ? "DESC" : "ASC";
	char orderby[128];
	if (by_added) {
		snprintf(orderby, sizeof(orderby), "MAX(added) %s, %s COLLATE listorder", dir, column);
	} else {
		snprintf(orderby, sizeof(orderby), "%s COLLATE listorder %s", column, dir);
	}
	// The books with no author are one group, and it goes last.
	char sql[400];
	snprintf(sql, sizeof(sql), "SELECT %s, COUNT(*) FROM AUDIOBOOK_TABLE %s GROUP BY %s COLLATE NOCASE ORDER BY %s = '', %s", column, where, column, column, orderby);

	pthread_mutex_lock(&db_lock);
	int delivered = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *name = (const char *)sqlite3_column_text(stmt, 0);
			if (!cb(name ? name : "", sqlite3_column_int(stmt, 1), user)) {
				break;
			}
			delivered++;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return delivered;
}

// ---------------------------------------------------------------------------
// The parts of a folder book
// ---------------------------------------------------------------------------

bool audiobookdb_is_folder_book(const char *book) {
	if (!book || !book[0]) {
		return false;
	}
	pthread_mutex_lock(&db_lock);
	bool folder = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT folder FROM AUDIOBOOK_TABLE WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, book, -1, SQLITE_TRANSIENT);
		folder = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) != 0;
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return folder;
}

int audiobookdb_parts_for_each(const char *book, audiobook_part_cb cb, void *user) {
	if (!book || !book[0] || !cb) {
		return 0;
	}
	pthread_mutex_lock(&db_lock);
	int delivered = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT path, title FROM AUDIOBOOK_PARTS WHERE book=? ORDER BY idx", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, book, -1, SQLITE_TRANSIENT);
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *path = (const char *)sqlite3_column_text(stmt, 0);
			const char *title = (const char *)sqlite3_column_text(stmt, 1);
			if (!cb(path ? path : "", title ? title : "", user)) {
				break;
			}
			delivered++;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return delivered;
}

// ---------------------------------------------------------------------------
// Where the listener got to
// ---------------------------------------------------------------------------

static void position_queue(const char *book_path, const char *file, double seconds) {
	if (!book_path || !book_path[0] || !file || !file[0]) {
		return;
	}

	pthread_once(&position_once, position_worker_start);

	position_job_t job;
	snprintf(job.book, sizeof(job.book), "%s", book_path);
	snprintf(job.file, sizeof(job.file), "%s", file);
	job.seconds = seconds;
	struct timespec saved_at;
	clock_gettime(CLOCK_REALTIME, &saved_at);
	job.updated_ms = (long long)saved_at.tv_sec * 1000 + saved_at.tv_nsec / 1000000;

	pthread_mutex_lock(&position_lock);
	pthread_mutex_lock(&db_lock);
	bool accept = db && position_accepting;
	job.generation = generation;
	pthread_mutex_unlock(&db_lock);

	if (accept && position_worker_ready) {
		position_pending = job;
		position_has_pending = true;
		pthread_cond_signal(&position_cond);
	}
	pthread_mutex_unlock(&position_lock);

	// Thread creation can fail only under severe resource pressure. A direct
	// write is preferable then to silently losing the listener's place.
	if (accept && !position_worker_ready) {
		position_write(&job);
	}
}

void audiobookdb_queue_position(const char *book_path, const char *file, double seconds) {
	position_queue(book_path, file, seconds);
}

void audiobookdb_flush_positions(void) {
	pthread_once(&position_once, position_worker_start);
	if (!position_worker_ready) {
		return;
	}
	pthread_mutex_lock(&position_lock);
	while (position_has_pending || position_busy) {
		pthread_cond_wait(&position_cond, &position_lock);
	}
	pthread_mutex_unlock(&position_lock);
}

void audiobookdb_save_position(const char *book_path, const char *file, double seconds) {
	position_queue(book_path, file, seconds);
	audiobookdb_flush_positions();
}

// Snapshot the current local checkpoint and compare with the acknowledged one.
#define ABS_CHECKPOINT_SELECT \
	"SELECT a.path,COALESCE(a.resume_file,''),COALESCE(a.resume_pos,0)," \
	"a.resume_completed," \
	"s.book,COALESCE(s.server,''),COALESCE(s.linked,0)," \
	"(s.file=a.resume_file AND s.seconds=a.resume_pos AND s.finished=" \
	"a.resume_completed AND " \
	"(s.revision=-1 OR s.revision=a.resume_revision)),COALESCE(s.failed,0)," \
	"a.resume_updated_ms,a.resume_revision,COALESCE(s.server_updated_ms,0) " \
	"FROM AUDIOBOOK_TABLE a LEFT JOIN AUDIOBOOK_ABS_SYNC s ON s.book=a.path "

static void abs_checkpoint_read(sqlite3_stmt *stmt, audiobookdb_abs_checkpoint_t *out) {
	memset(out, 0, sizeof(*out));
	snprintf(out->book, sizeof(out->book), "%s", sqlite3_column_text(stmt, 0));
	snprintf(out->file, sizeof(out->file), "%s", sqlite3_column_text(stmt, 1));
	out->seconds = sqlite3_column_double(stmt, 2);
	out->finished = sqlite3_column_int(stmt, 3) != 0;
	out->known = sqlite3_column_type(stmt, 4) != SQLITE_NULL;
	snprintf(out->server, sizeof(out->server), "%s", sqlite3_column_text(stmt, 5));
	out->linked = sqlite3_column_int(stmt, 6) != 0;
	out->synced = sqlite3_column_int(stmt, 7) != 0;
	out->failed = sqlite3_column_int(stmt, 8) != 0;
	out->sync_error = sqlite3_column_int(stmt, 8);
	out->updated_ms = sqlite3_column_int64(stmt, 9);
	out->revision = sqlite3_column_int64(stmt, 10);
	out->server_updated_ms = sqlite3_column_int64(stmt, 11);
	out->generation = generation;
}

bool audiobookdb_abs_get(const char *book, audiobookdb_abs_checkpoint_t *out) {
	if (!book || !out) return false;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	bool found = false;
	if (db && sqlite3_prepare_v2(db, ABS_CHECKPOINT_SELECT "WHERE a.path=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, book, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			abs_checkpoint_read(stmt, out);
			found = true;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

bool audiobookdb_abs_next(const char *server, audiobookdb_abs_checkpoint_t *out) {
	if (!server || !out) return false;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	bool found = false;
	const char *sql = ABS_CHECKPOINT_SELECT
		"WHERE a.last_played>0 AND a.resume_file<>'' AND (s.book IS NULL OR "
		"(s.linked=1 AND s.server=? AND (s.file IS NULL OR s.file<>a.resume_file OR "
		"s.seconds<>a.resume_pos OR s.revision<>a.resume_revision OR "
		"s.finished<>a.resume_completed OR s.attempted<=?3 OR s.attempted>?4))) "
		"AND (COALESCE(s.attempted,0)<=?2 OR s.attempted>?4) "
		"ORDER BY COALESCE(s.attempted,0),a.last_played DESC LIMIT 1";
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, server, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(NULL) - 60);
		sqlite3_bind_int64(stmt, 3, (sqlite3_int64)time(NULL) - 300);
		sqlite3_bind_int64(stmt, 4, (sqlite3_int64)time(NULL) + 60);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			abs_checkpoint_read(stmt, out);
			found = true;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

void audiobookdb_abs_result(const audiobookdb_abs_checkpoint_t *sent, const char *server,
						   bool linked, bool success, bool failed) {
	if (!sent || !server) return;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	// A slow response must never acknowledge a replacement card's checkpoint.
	const char *sql = "INSERT INTO AUDIOBOOK_ABS_SYNC(book,server,linked,file,seconds,finished,attempted,failed,revision,server_updated_ms) "
		"VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?12,?13) ON CONFLICT(book) DO UPDATE SET server=excluded.server,linked=excluded.linked,"
		"file=CASE WHEN ?9 THEN excluded.file ELSE file END,"
		"seconds=CASE WHEN ?10 THEN excluded.seconds ELSE seconds END,"
		"finished=CASE WHEN ?11 THEN excluded.finished ELSE finished END,attempted=excluded.attempted,failed=excluded.failed,"
		"revision=CASE WHEN ?9 THEN excluded.revision ELSE revision END,"
		"server_updated_ms=CASE WHEN ?9 THEN excluded.server_updated_ms ELSE server_updated_ms END";
	if (db && generation == sent->generation && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, sent->book, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 2, server, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 3, linked);
		if (success) sqlite3_bind_text(stmt, 4, sent->file, -1, SQLITE_TRANSIENT);
		else sqlite3_bind_null(stmt, 4);
		sqlite3_bind_double(stmt, 5, sent->seconds);
		sqlite3_bind_int(stmt, 6, sent->finished);
		sqlite3_bind_int64(stmt, 7, (sqlite3_int64)time(NULL));
		sqlite3_bind_int(stmt, 8, failed ? (sent->sync_error ? sent->sync_error : 1) : 0);
		for (int i = 9; i <= 11; i++) sqlite3_bind_int(stmt, i, success);
		sqlite3_bind_int64(stmt, 12, success ? sent->revision : -1);
		sqlite3_bind_int64(stmt, 13, sent->server_updated_ms);
		if (sqlite3_step(stmt) != SQLITE_DONE) fprintf(stderr, "audiobooks: sync acknowledgement could not be saved\n");
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
}

static void abs_change(const char *book, const char *sql) {
	if (!book || !book[0]) return;
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, book, -1, SQLITE_TRANSIENT);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
}

void audiobookdb_abs_retry(const char *book) {
	abs_change(book, "UPDATE AUDIOBOOK_ABS_SYNC SET attempted=0 WHERE book=?");
}

void audiobookdb_abs_reset(const char *book) {
	abs_change(book, "DELETE FROM AUDIOBOOK_ABS_SYNC WHERE book=?");
}

void audiobookdb_abs_reconnect(void) {
	pthread_mutex_lock(&db_lock);
	if (db) exec("UPDATE AUDIOBOOK_ABS_SYNC SET attempted=0 WHERE linked=1");
	pthread_mutex_unlock(&db_lock);
}

// Compare-and-swap the exact pre-request snapshot, including queued local edits.
bool audiobookdb_abs_import(const audiobookdb_abs_checkpoint_t *expected, const char *server,
	const char *file, double seconds, bool finished, long long updated_ms) {
	if (!expected || !file || !file[0] || !isfinite(seconds) || seconds < 0 || updated_ms < 0) return false;
	pthread_mutex_lock(&position_lock);
	pthread_mutex_lock(&db_lock);
	bool ok = false;
	sqlite3_stmt *stmt = NULL;
	if (!db || generation != expected->generation || position_has_pending || position_busy) goto done;
	if (sqlite3_prepare_v2(db, ABS_CHECKPOINT_SELECT "WHERE a.path=?", -1, &stmt, NULL) != SQLITE_OK) goto done;
	sqlite3_bind_text(stmt, 1, expected->book, -1, SQLITE_TRANSIENT);
	audiobookdb_abs_checkpoint_t current;
	bool found = sqlite3_step(stmt) == SQLITE_ROW;
	if (found) abs_checkpoint_read(stmt, &current);
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (!found || current.revision != expected->revision || current.seconds != expected->seconds ||
		current.finished != expected->finished || strcmp(current.file, expected->file) != 0) goto done;
	if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) goto done;
	if (sqlite3_prepare_v2(db, "UPDATE AUDIOBOOK_TABLE SET resume_file=?,resume_pos=?,resume_updated_ms=?,"
		"resume_revision=resume_revision+1,last_played=?,resume_completed=?6 WHERE path=?5", -1, &stmt, NULL) != SQLITE_OK) goto rollback;
	sqlite3_bind_text(stmt, 1, file, -1, SQLITE_TRANSIENT);
	sqlite3_bind_double(stmt, 2, seconds);
	sqlite3_bind_int64(stmt, 3, updated_ms);
	sqlite3_bind_int64(stmt, 4, updated_ms ? updated_ms / 1000 : time(NULL));
	sqlite3_bind_text(stmt, 5, expected->book, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 6, finished);
	if (sqlite3_step(stmt) != SQLITE_DONE) goto rollback;
	sqlite3_finalize(stmt);
	stmt = NULL;
	const char *finish_sql = finished ? "INSERT OR REPLACE INTO AUDIOBOOK_FINISHED(path,finished_at) VALUES(?1,?2)" :
		"DELETE FROM AUDIOBOOK_FINISHED WHERE path=?1";
	if (sqlite3_prepare_v2(db, finish_sql, -1, &stmt, NULL) != SQLITE_OK) goto rollback;
	sqlite3_bind_text(stmt, 1, expected->book, -1, SQLITE_TRANSIENT);
	if (finished) sqlite3_bind_int64(stmt, 2, updated_ms / 1000);
	if (sqlite3_step(stmt) != SQLITE_DONE) goto rollback;
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO AUDIOBOOK_ABS_SYNC"
		"(book,server,linked,file,seconds,finished,attempted,failed,revision,server_updated_ms)"
		"VALUES(?,?,1,?,?,?,?,0,?,?)", -1, &stmt, NULL) != SQLITE_OK) goto rollback;
	sqlite3_bind_text(stmt, 1, expected->book, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, server, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, file, -1, SQLITE_TRANSIENT);
	sqlite3_bind_double(stmt, 4, seconds);
	sqlite3_bind_int(stmt, 5, finished);
	sqlite3_bind_int64(stmt, 6, time(NULL));
	sqlite3_bind_int64(stmt, 7, expected->revision + 1);
	sqlite3_bind_int64(stmt, 8, updated_ms);
	if (sqlite3_step(stmt) != SQLITE_DONE) goto rollback;
	sqlite3_finalize(stmt);
	stmt = NULL;
	ok = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
	if (ok) goto done;
rollback:
	sqlite3_finalize(stmt);
	stmt = NULL;
	sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
done:
	sqlite3_finalize(stmt);
	pthread_mutex_unlock(&db_lock);
	pthread_mutex_unlock(&position_lock);
	return ok;
}

bool audiobookdb_get_position(const char *book_path, char *file_out, size_t file_size, double *seconds_out) {
	if (file_out && file_size) {
		file_out[0] = '\0';
	}
	if (seconds_out) {
		*seconds_out = 0;
	}
	if (!book_path || !book_path[0]) {
		return false;
	}

	pthread_mutex_lock(&db_lock);

	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT resume_file, resume_pos FROM AUDIOBOOK_TABLE WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, book_path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *file = (const char *)sqlite3_column_text(stmt, 0);
			if (file && file[0]) {
				if (file_out && file_size) {
					snprintf(file_out, file_size, "%s", file);
				}
				if (seconds_out) {
					*seconds_out = sqlite3_column_double(stmt, 1);
				}
				found = true;
			}
		}
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&db_lock);
	return found;
}

// One or two primary-key lookups per track change, with nothing cached to go
// stale: the file as a book of its own, then as a part of a folder book.
bool audiobookdb_book_for_file(const char *file_path, char *book_out, size_t book_size) {
	if (book_out && book_size) {
		book_out[0] = '\0';
	}
	if (!db || !file_path || !file_path[0]) {
		return false;
	}

	pthread_mutex_lock(&db_lock);

	char book[512] = "";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT path FROM AUDIOBOOK_TABLE WHERE path=? AND folder=0", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, file_path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			snprintf(book, sizeof(book), "%s", file_path);
		}
		sqlite3_finalize(stmt);
	}
	if (!book[0] && sqlite3_prepare_v2(db, "SELECT book FROM AUDIOBOOK_PARTS WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, file_path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *owner = (const char *)sqlite3_column_text(stmt, 0);
			snprintf(book, sizeof(book), "%s", owner ? owner : "");
		}
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&db_lock);

	if (book[0] && book_out && book_size) {
		snprintf(book_out, book_size, "%s", book);
	}
	return book[0] != '\0';
}

bool audiobookdb_book_details(const char *book_path, char *title_out, size_t title_size,
								  char *author_out, size_t author_size, char *series_out, size_t series_size,
								  char *summary_out, size_t summary_size) {
	if (title_out && title_size) title_out[0] = '\0';
	if (author_out && author_size) author_out[0] = '\0';
	if (series_out && series_size) series_out[0] = '\0';
	if (summary_out && summary_size) summary_out[0] = '\0';
	if (!book_path || !book_path[0]) {
		return false;
	}

	pthread_mutex_lock(&db_lock);
	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT name,author,series,summary FROM AUDIOBOOK_TABLE WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, book_path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *title = (const char *)sqlite3_column_text(stmt, 0);
			const char *author = (const char *)sqlite3_column_text(stmt, 1);
			const char *series = (const char *)sqlite3_column_text(stmt, 2);
			const char *summary = (const char *)sqlite3_column_text(stmt, 3);
			if (title_out && title_size) snprintf(title_out, title_size, "%s", title ? title : "");
			if (author_out && author_size) snprintf(author_out, author_size, "%s", author ? author : "");
			if (series_out && series_size) snprintf(series_out, series_size, "%s", series ? series : "");
			if (summary_out && summary_size) snprintf(summary_out, summary_size, "%s", summary ? summary : "");
			found = true;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return found;
}

// Copies the part of `location` immediately below `folder` into `out`.
static bool folder_child(const char *folder, const char *location, char *out, size_t size) {
	size_t base = strlen(folder);
	if (strncmp(folder, location, base) != 0 || location[base] != '/' || !location[base + 1]) {
		return false;
	}
	const char *start = location + base + 1;
	const char *slash = strchr(start, '/');
	size_t len = slash ? (size_t)(slash - start) : strlen(start);
	if (len == 0 || base + 1 + len >= size) {
		return false;
	}
	memcpy(out, folder, base);
	out[base] = '/';
	memcpy(out + base + 1, start, len);
	out[base + 1 + len] = '\0';
	return true;
}

int audiobookdb_folder_entries_for_each(const char *folder, audiobook_folder_cb cb, void *user) {
	if (!folder || !folder[0] || !cb) {
		return 0;
	}
	pthread_mutex_lock(&db_lock);
	int delivered = 0;

	// Locations are ordered so every book below one child is adjacent. Emit
	// that child once. When the child itself is a folder book it is a playable
	// row; otherwise it is another level of the hierarchy.
	sqlite3_stmt *locations = NULL;
	sqlite3_stmt *exact = NULL;
	if (db && sqlite3_prepare_v2(db,
			"SELECT location FROM AUDIOBOOK_TABLE WHERE substr(location,1,length(?1)+1)=?1||'/'"
			" ORDER BY location COLLATE listorder", -1, &locations, NULL) == SQLITE_OK &&
		sqlite3_prepare_v2(db, "SELECT name FROM AUDIOBOOK_TABLE WHERE path=? AND folder=1", -1, &exact, NULL) == SQLITE_OK) {
		sqlite3_bind_text(locations, 1, folder, -1, SQLITE_TRANSIENT);
		char previous[768] = "";
		while (sqlite3_step(locations) == SQLITE_ROW) {
			const char *location = (const char *)sqlite3_column_text(locations, 0);
			char child[768];
			if (!location || !folder_child(folder, location, child, sizeof(child)) || strcmp(child, previous) == 0) {
				continue;
			}
			snprintf(previous, sizeof(previous), "%s", child);

			sqlite3_reset(exact);
			sqlite3_clear_bindings(exact);
			sqlite3_bind_text(exact, 1, child, -1, SQLITE_TRANSIENT);
			bool is_book = sqlite3_step(exact) == SQLITE_ROW;
			const char *name = is_book ? (const char *)sqlite3_column_text(exact, 0) : strrchr(child, '/');
			name = is_book ? (name ? name : "") : (name ? name + 1 : child);
			if (!cb(name, child, !is_book, user)) {
				break;
			}
			delivered++;
		}
	}
	sqlite3_finalize(locations);
	sqlite3_finalize(exact);

	// Loose single-file books and multiple album-groups that sit directly in
	// this folder. A normal folder book has path==location and was emitted one
	// level above, so it is excluded here.
	sqlite3_stmt *books = NULL;
	if (db && sqlite3_prepare_v2(db,
			"SELECT name,path FROM AUDIOBOOK_TABLE WHERE location=? AND NOT(folder=1 AND path=location)"
			" ORDER BY name COLLATE listorder", -1, &books, NULL) == SQLITE_OK) {
		sqlite3_bind_text(books, 1, folder, -1, SQLITE_TRANSIENT);
		while (sqlite3_step(books) == SQLITE_ROW) {
			const char *name = (const char *)sqlite3_column_text(books, 0);
			const char *path = (const char *)sqlite3_column_text(books, 1);
			if (!cb(name ? name : "", path ? path : "", false, user)) {
				break;
			}
			delivered++;
		}
	}
	sqlite3_finalize(books);
	pthread_mutex_unlock(&db_lock);
	return delivered;
}

bool audiobookdb_bookmark_add(const char *book, const char *file, double seconds, const char *label) {
	if (!book || !book[0] || !file || !file[0] || seconds < 0) {
		return false;
	}
	pthread_mutex_lock(&db_lock);
	bool ok = false;
	if (db) {
		sqlite3_stmt *near = NULL;
		if (sqlite3_prepare_v2(db, "DELETE FROM AUDIOBOOK_BOOKMARKS WHERE book=? AND file=? AND abs(seconds-?)<2", -1, &near, NULL) == SQLITE_OK) {
			sqlite3_bind_text(near, 1, book, -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(near, 2, file, -1, SQLITE_TRANSIENT);
			sqlite3_bind_double(near, 3, seconds);
			sqlite3_step(near);
			sqlite3_finalize(near);
		}

		sqlite3_stmt *count = NULL;
		int marks = AUDIOBOOK_BOOKMARKS_PER_BOOK;
		if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM AUDIOBOOK_BOOKMARKS WHERE book=?", -1, &count, NULL) == SQLITE_OK) {
			sqlite3_bind_text(count, 1, book, -1, SQLITE_TRANSIENT);
			if (sqlite3_step(count) == SQLITE_ROW) marks = sqlite3_column_int(count, 0);
			sqlite3_finalize(count);
		}
		if (marks < AUDIOBOOK_BOOKMARKS_PER_BOOK) {
			sqlite3_stmt *insert = NULL;
			if (sqlite3_prepare_v2(db, "INSERT INTO AUDIOBOOK_BOOKMARKS(book,file,seconds,label,created_at) VALUES(?,?,?,?,?)", -1, &insert, NULL) == SQLITE_OK) {
				sqlite3_bind_text(insert, 1, book, -1, SQLITE_TRANSIENT);
				sqlite3_bind_text(insert, 2, file, -1, SQLITE_TRANSIENT);
				sqlite3_bind_double(insert, 3, seconds);
				sqlite3_bind_text(insert, 4, label ? label : "", -1, SQLITE_TRANSIENT);
				sqlite3_bind_int64(insert, 5, (sqlite3_int64)time(NULL));
				ok = sqlite3_step(insert) == SQLITE_DONE;
				sqlite3_finalize(insert);
			}
		}
	}
	pthread_mutex_unlock(&db_lock);
	return ok;
}

bool audiobookdb_bookmark_remove(int64_t id) {
	if (id <= 0) return false;
	pthread_mutex_lock(&db_lock);
	bool ok = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "DELETE FROM AUDIOBOOK_BOOKMARKS WHERE id=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
		ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0;
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return ok;
}

int audiobookdb_bookmark_count(const char *book) {
	pthread_mutex_lock(&db_lock);
	int count = 0;
	sqlite3_stmt *stmt = NULL;
	const char *sql = book && book[0]
		? "SELECT COUNT(*) FROM AUDIOBOOK_BOOKMARKS b JOIN AUDIOBOOK_TABLE t ON t.path=b.book WHERE b.book=?"
		: "SELECT COUNT(*) FROM AUDIOBOOK_BOOKMARKS b JOIN AUDIOBOOK_TABLE t ON t.path=b.book";
	if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		if (book && book[0]) sqlite3_bind_text(stmt, 1, book, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return count;
}

int audiobookdb_bookmarks_for_each(const char *book, audiobook_bookmark_cb cb, void *user) {
	if (!book || !book[0] || !cb) return 0;
	pthread_mutex_lock(&db_lock);
	int delivered = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db,
			"SELECT b.id,b.file,b.seconds,b.label,b.created_at FROM AUDIOBOOK_BOOKMARKS b"
			" LEFT JOIN AUDIOBOOK_PARTS p ON p.path=b.file WHERE b.book=?"
			" ORDER BY COALESCE(p.idx,0),b.seconds", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, book, -1, SQLITE_TRANSIENT);
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *file = (const char *)sqlite3_column_text(stmt, 1);
			const char *label = (const char *)sqlite3_column_text(stmt, 3);
			if (!cb((int64_t)sqlite3_column_int64(stmt, 0), file ? file : "", sqlite3_column_double(stmt, 2),
					label ? label : "", (int64_t)sqlite3_column_int64(stmt, 4), user)) break;
			delivered++;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return delivered;
}

int audiobookdb_bookmarked_books_for_each(audiobook_bookmarked_book_cb cb, void *user) {
	if (!cb) return 0;
	pthread_mutex_lock(&db_lock);
	int delivered = 0;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db,
			"SELECT t.name,t.path,COUNT(*) FROM AUDIOBOOK_BOOKMARKS b"
			" JOIN AUDIOBOOK_TABLE t ON t.path=b.book GROUP BY t.path ORDER BY t.name COLLATE listorder",
			-1, &stmt, NULL) == SQLITE_OK) {
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *title = (const char *)sqlite3_column_text(stmt, 0);
			const char *book = (const char *)sqlite3_column_text(stmt, 1);
			if (!cb(title ? title : "", book ? book : "", sqlite3_column_int(stmt, 2), user)) break;
			delivered++;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return delivered;
}

void audiobookdb_mark_finished(const char *path) {
	if (!path || !path[0]) {
		return;
	}

	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO AUDIOBOOK_FINISHED(path, finished_at) VALUES(?,?)", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(NULL));
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	if (db && sqlite3_prepare_v2(db, "UPDATE AUDIOBOOK_TABLE SET resume_completed=1,"
		"resume_updated_ms=?,resume_revision=resume_revision+1 WHERE path=? AND resume_completed=0", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000);
		sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	// Not the row ids, but which rows the finished list names -- a handle over
	// it is describing a list that has just gained a book.
	bump_generation();
	pthread_mutex_unlock(&db_lock);
}

bool audiobookdb_is_finished(const char *path) {
	if (!path || !path[0]) {
		return false;
	}

	pthread_mutex_lock(&db_lock);
	bool hit = false;
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "SELECT 1 FROM AUDIOBOOK_FINISHED WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		hit = sqlite3_step(stmt) == SQLITE_ROW;
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&db_lock);
	return hit;
}

void audiobookdb_touch(const char *path) {
	if (!path || !path[0]) {
		return;
	}

	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *stmt = NULL;
	if (db && sqlite3_prepare_v2(db, "UPDATE AUDIOBOOK_TABLE SET last_played=? WHERE path=?", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, (sqlite3_int64)time(NULL));
		sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	// The order by last listened has changed, which a handle over that list
	// has to be told. Not done in audiobookdb_save_position(), which writes
	// the same column every ten seconds while a book plays.
	bump_generation();
	pthread_mutex_unlock(&db_lock);
}

// ---------------------------------------------------------------------------
// scanning
//
// Only the Audiobooks folder at the root of the card, walked to any depth.
//
// A folder is read whole before anything in it is indexed, and closed before
// its subfolders are walked, so one directory is open at a time whatever the
// depth. In each folder:
//
//   an .m4b, or an .m4a, .mp3, .opus, .ogg or .flac with chapter marks, is a
//   book of its own;
//
//   the other audio files, together with those of its "CD 1" / "Disc 2"
//   subfolders, are grouped by album tag: a group of two or more is a folder
//   book, a group of one a book of its own. Loose files directly in the
//   Audiobooks folder are always a book each.
// ---------------------------------------------------------------------------

typedef struct {
	char **items;
	int count;
	int capacity;
} namelist_t;

static bool namelist_add(namelist_t *l, const char *name) {
	if (l->count >= SCAN_MAX_ENTRIES) {
		return false;
	}
	if (l->count == l->capacity) {
		int grown = l->capacity ? l->capacity * 2 : 32;
		char **bigger = realloc(l->items, (size_t)grown * sizeof(*bigger));
		if (!bigger) {
			return false;
		}
		l->items = bigger;
		l->capacity = grown;
	}
	char *copy = strdup(name);
	if (!copy) {
		return false;
	}
	l->items[l->count++] = copy;
	return true;
}

static void namelist_free(namelist_t *l) {
	for (int i = 0; i < l->count; i++) {
		free(l->items[i]);
	}
	free(l->items);
	l->items = NULL;
	l->count = 0;
	l->capacity = 0;
}

// Case-insensitive, with runs of digits compared as numbers: "2 x" before
// "10 x", which is how chapter files are numbered.
static int natural_compare(const char *a, const char *b) {
	while (*a && *b) {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			while (*a == '0') {
				a++;
			}
			while (*b == '0') {
				b++;
			}
			size_t la = 0;
			size_t lb = 0;
			while (isdigit((unsigned char)a[la])) {
				la++;
			}
			while (isdigit((unsigned char)b[lb])) {
				lb++;
			}
			if (la != lb) {
				return la < lb ? -1 : 1;
			}
			int c = strncmp(a, b, la);
			if (c) {
				return c;
			}
			a += la;
			b += lb;
			continue;
		}
		int ca = tolower((unsigned char)*a);
		int cb = tolower((unsigned char)*b);
		if (ca != cb) {
			return ca < cb ? -1 : 1;
		}
		a++;
		b++;
	}
	return *a ? 1 : (*b ? -1 : 0);
}

static int natural_qsort(const void *a, const void *b) { return natural_compare(*(const char *const *)a, *(const char *const *)b); }

// Reads the names in `path`: files into `files`, subfolders into `dirs`, each
// in natural order. Dot files and the folders a desktop leaves behind are
// skipped, and so are links to folders -- a link to an ancestor would walk the
// same branch at every level.
static bool read_folder(const char *path, namelist_t *files, namelist_t *dirs) {
	DIR *dir = opendir(path);
	if (!dir) {
		return false;
	}
	struct dirent *de;
	while (!scan_cancel && (de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.' || playlist_is_junk_name(de->d_name)) {
			continue;
		}
		char child[768];
		if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >= (int)sizeof(child)) {
			continue;
		}
		struct stat st;
		if (lstat(child, &st) != 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			namelist_add(dirs, de->d_name);
		} else if (S_ISREG(st.st_mode) || (S_ISLNK(st.st_mode) && stat(child, &st) == 0 && S_ISREG(st.st_mode))) {
			namelist_add(files, de->d_name);
		}
	}
	closedir(dir);
	if (files->count > 1) {
		qsort(files->items, (size_t)files->count, sizeof(char *), natural_qsort);
	}
	if (dirs->count > 1) {
		qsort(dirs->items, (size_t)dirs->count, sizeof(char *), natural_qsort);
	}
	return true;
}

// The audio a folder book can be made of.
static bool is_audio(const char *name) {
	static const char *const EXTENSIONS[] = {".mp3", ".m4a", ".m4b", ".aac", ".flac", ".ogg", ".opus", ".wav", ".wv", ".ape", ".mp4"};
	for (size_t i = 0; i < sizeof(EXTENSIONS) / sizeof(EXTENSIONS[0]); i++) {
		if (has_extension(name, EXTENSIONS[i])) {
			return true;
		}
	}
	return false;
}

// Whether an .m4a or .mp4 carries chapter marks.
static bool mp4_has_chapters(const char *path) {
	mp4_file_t *m = mp4_open(path);
	if (!m) {
		return false;
	}
	bool marked = mp4_chapter_count(m) > 0;
	mp4_close(m);
	return marked;
}

// A book in one file whatever its neighbours: an .m4b, or an .m4a, .mp3,
// .opus, .ogg or .flac whose chapters are marked inside it.
static bool is_book_file(const char *path, const char *name) {
	if (has_extension(name, ".m4b")) {
		return true;
	}
	if (has_extension(name, ".m4a") || has_extension(name, ".mp4")) {
		return mp4_has_chapters(path);
	}
	if (has_extension(name, ".mp3")) {
		return id3chap_present(path);
	}
	if (has_extension(name, ".opus") || has_extension(name, ".ogg") || has_extension(name, ".flac")) {
		return vorbischap_present(path);
	}
	return false;
}

// "CD 1", "CD01", "Disc 2", "Disk 3": the halves of one book split the way a
// set of discs was.
static bool is_disc_folder(const char *name) {
	const char *rest = NULL;
	if (strncasecmp(name, "cd", 2) == 0) {
		rest = name + 2;
	} else if (strncasecmp(name, "disc", 4) == 0 || strncasecmp(name, "disk", 4) == 0) {
		rest = name + 4;
	} else {
		return false;
	}
	while (*rest == ' ' || *rest == '_' || *rest == '-' || *rest == '.') {
		rest++;
	}
	if (!isdigit((unsigned char)*rest)) {
		return false;
	}
	while (isdigit((unsigned char)*rest)) {
		rest++;
	}
	return *rest == '\0';
}

static void strip_extension(char *name) {
	char *dot = strrchr(name, '.');
	if (dot && dot != name) {
		*dot = '\0';
	}
}

// A book's number in its series, from the tag ("3", "2.5", "3/12") or, when
// there is none, from the front of its name: "3 - Title", "03. Title", "Book 3
// - Title", "Vol. 2", "#4", or the series' own name and then the number,
// "Mistborn 1 The Final Empire". -1 when there is no number.
static double series_number(const char *text, bool from_name, const char *series) {
	if (!text || !text[0]) {
		return -1;
	}
	const char *p = text;
	while (*p == ' ') {
		p++;
	}
	size_t series_len = series ? strlen(series) : 0;
	if (from_name && series_len && strncasecmp(p, series, series_len) == 0) {
		p += series_len;
		while (*p == ' ' || *p == '-' || *p == ',' || *p == ':' || *p == '_') {
			p++;
		}
	}
	if (from_name) {
		static const char *const PREFIXES[] = {"book", "vol.", "vol", "volume", "part", "tome", "band", "libro", "#"};
		for (size_t i = 0; i < sizeof(PREFIXES) / sizeof(PREFIXES[0]); i++) {
			size_t n = strlen(PREFIXES[i]);
			if (strncasecmp(p, PREFIXES[i], n) == 0 && (isdigit((unsigned char)p[n]) || p[n] == ' ')) {
				p += n;
				while (*p == ' ') {
					p++;
				}
				break;
			}
		}
	}
	if (!isdigit((unsigned char)*p)) {
		return -1;
	}
	char *end = NULL;
	double value = strtod(p, &end);
	if (end == p || value < 0 || value > 100000) {
		return -1;
	}
	// From a name the number has to stand apart from the title: "1984" is a
	// title, not book 1984 of anything.
	if (from_name && *end && *end != ' ' && *end != '.' && *end != '-' && *end != ')' && *end != ':' && *end != '_') {
		return -1;
	}
	if (from_name && value >= 1000) {
		return -1;
	}
	return value;
}

typedef struct {
	char title[256];
	char author[256];
	char series[256];
	char location[768]; // real folder containing the file(s), never a synthetic key
	char *summary;
	double series_part; // -1: none
	long long added;
	long long size;
	long long mtime;
	bool folder;
} book_t;

// Fills what the tags left empty from the folders the book sits in, read as
// Audiobooks/Author/Series. `rel` is the path of those folders under the
// Audiobooks folder, without the book's own folder.
static void fill_from_folders(book_t *b, const char *rel) {
	char first[256] = "";
	char second[256] = "";
	const char *slash = rel ? strchr(rel, '/') : NULL;
	if (rel && rel[0]) {
		size_t n = slash ? (size_t)(slash - rel) : strlen(rel);
		if (n >= sizeof(first)) {
			n = sizeof(first) - 1;
		}
		memcpy(first, rel, n);
		first[n] = '\0';
	}
	if (slash) {
		const char *next = slash + 1;
		const char *end = strchr(next, '/');
		size_t n = end ? (size_t)(end - next) : strlen(next);
		if (n >= sizeof(second)) {
			n = sizeof(second) - 1;
		}
		memcpy(second, next, n);
		second[n] = '\0';
	}
	if (!b->author[0]) {
		snprintf(b->author, sizeof(b->author), "%s", first);
	}
	if (!b->series[0]) {
		snprintf(b->series, sizeof(b->series), "%s", second);
	}
}

static void book_from_tags(book_t *b, const song_metadata_t *tags) {
	snprintf(b->author, sizeof(b->author), "%s", tags->album_artist[0] ? tags->album_artist : tags->artist);
	snprintf(b->series, sizeof(b->series), "%s", tags->series);
	b->series_part = series_number(tags->series_part, false, NULL);
}

// Under db_lock, inside the scan's transaction.
static void insert_book(const char *path, const book_t *b) {
	sqlite3_stmt *stmt = NULL;
	// Where the listener was survives a rescan: the scan stashes it in a temp
	// table before wiping, and each insert copies its own back.
	if (sqlite3_prepare_v2(db,
						   "INSERT OR REPLACE INTO AUDIOBOOK_TABLE"
						   "(path,name,size,mtime,author,series,series_part,added,folder,location,summary,"
						   "last_played,resume_file,resume_pos,resume_updated_ms,resume_revision,resume_completed)"
						   " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,"
						   "COALESCE((SELECT last_played FROM old_state WHERE path=?1),0),"
						   "(SELECT resume_file FROM old_state WHERE path=?1),"
						   "(SELECT resume_pos FROM old_state WHERE path=?1),"
						   "COALESCE((SELECT resume_updated_ms FROM old_state WHERE path=?1),0),"
						   "COALESCE((SELECT resume_revision FROM old_state WHERE path=?1),0),"
						   "COALESCE((SELECT resume_completed FROM old_state WHERE path=?1),0))",
						   -1, &stmt, NULL) != SQLITE_OK) {
		return;
	}
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, b->title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, b->size);
	sqlite3_bind_int64(stmt, 4, b->mtime);
	sqlite3_bind_text(stmt, 5, b->author, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, b->series, -1, SQLITE_TRANSIENT);
	if (b->series[0] && b->series_part >= 0) {
		sqlite3_bind_double(stmt, 7, b->series_part);
	} else {
		sqlite3_bind_null(stmt, 7);
	}
	sqlite3_bind_int64(stmt, 8, b->added);
	sqlite3_bind_int(stmt, 9, b->folder ? 1 : 0);
	sqlite3_bind_text(stmt, 10, b->location, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 11, b->summary ? b->summary : "", -1, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	scan_found++;
	if (scan_found % SCAN_COMMIT_EVERY == 0) {
		exec("COMMIT");
		sqlite3_db_release_memory(db);
		exec("BEGIN");
	}
}

static void insert_part(const char *path, const char *book, int index, const char *title) {
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO AUDIOBOOK_PARTS(path,book,idx,title) VALUES(?,?,?,?)", -1, &stmt, NULL) != SQLITE_OK) {
		return;
	}
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, book, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 3, index);
	sqlite3_bind_text(stmt, 4, title, -1, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

// The path of `dir` under the Audiobooks folder, "" for the folder itself.
static const char *relative(const char *dir) {
	size_t root = strlen(scan_root);
	if (strncmp(dir, scan_root, root) != 0) {
		return "";
	}
	dir += root;
	return *dir == '/' ? dir + 1 : dir;
}

// The folder above `rel`, into `out`: "A/S/B" -> "A/S".
static void parent_of(const char *rel, char *out, size_t size) {
	snprintf(out, size, "%s", rel);
	char *slash = strrchr(out, '/');
	if (slash) {
		*slash = '\0';
	} else {
		out[0] = '\0';
	}
}

// Whether the folder `rel` ends in is the book's own: named like the file, or
// like the title, one inside the other.
static bool named_like(const char *rel, const char *stem, const char *title) {
	const char *folder = strrchr(rel, '/');
	folder = folder ? folder + 1 : rel;
	if (!folder[0]) {
		return false;
	}
	return strcasestr(folder, stem) || strcasestr(stem, folder) || (title[0] && (strcasestr(folder, title) || strcasestr(title, folder)));
}

// A book in one file, in the folder `rel` under the Audiobooks folder. `alone`
// says it is the only thing in that folder, which is then its own folder when
// it is named like it, and the folders above it are its author and series.
static void index_single(const char *path, const char *name, const char *rel, bool alone) {
	struct stat st;
	if (stat(path, &st) != 0) {
		return;
	}
	song_metadata_t *tags = calloc(1, sizeof(*tags));
	book_t *b = calloc(1, sizeof(*b));
	if (!tags || !b) {
		free(tags);
		free(b);
		return;
	}
	b->summary = metadata_read_with_description(path, tags);

	book_from_tags(b, tags);
	if (tags->title[0]) {
		snprintf(b->title, sizeof(b->title), "%s", tags->title);
	} else if (tags->album[0]) {
		snprintf(b->title, sizeof(b->title), "%s", tags->album);
	} else {
		snprintf(b->title, sizeof(b->title), "%s", name);
		strip_extension(b->title);
	}
	char stem[256];
	snprintf(stem, sizeof(stem), "%s", name);
	strip_extension(stem);
	char above[512];
	if (alone && named_like(rel, stem, b->title)) {
		parent_of(rel, above, sizeof(above));
	} else {
		snprintf(above, sizeof(above), "%s", rel);
	}
	fill_from_folders(b, above);
	if (b->series[0] && b->series_part < 0) {
		b->series_part = series_number(stem, true, b->series);
		if (b->series_part < 0) {
			b->series_part = series_number(b->title, true, b->series);
		}
	}
	b->size = (long long)st.st_size;
	b->mtime = (long long)st.st_mtime;
	b->added = (long long)st.st_ctime;
	b->folder = false;
	snprintf(b->location, sizeof(b->location), "%s", path);
	char *location_slash = strrchr(b->location, '/');
	if (location_slash) {
		*location_slash = '\0';
	}

	pthread_mutex_lock(&db_lock);
	if (db) {
		insert_book(path, b);
	}
	pthread_mutex_unlock(&db_lock);

	free(b->summary);
	free(tags);
	free(b);
}

// A book made of the files in `parts`, which sit in folder `dir` and its disc
// folders; `key` is the path the book is indexed under. `own_folder` says the
// folder holds this book alone, so it is the book's folder and the folders
// above it are its author and series; otherwise the folder is the series of
// several books.
static void index_folder(const char *dir, const char *key, const namelist_t *parts, bool own_folder) {
	song_metadata_t *tags = calloc(1, sizeof(*tags));
	book_t *b = calloc(1, sizeof(*b));
	if (!tags || !b) {
		free(tags);
		free(b);
		return;
	}

	const char *folder_name = strrchr(dir, '/');
	folder_name = folder_name ? folder_name + 1 : dir;

	b->folder = true;
	snprintf(b->location, sizeof(b->location), "%s", dir);
	b->series_part = -1;
	for (int i = 0; i < parts->count && !scan_cancel; i++) {
		const char *path = parts->items[i];
		struct stat st;
		if (stat(path, &st) != 0) {
			continue;
		}
		memset(tags, 0, sizeof(*tags));
		if (i == 0) {
			b->summary = metadata_read_with_description(path, tags);
			book_from_tags(b, tags);
			snprintf(b->title, sizeof(b->title), "%s", tags->album[0] ? tags->album : folder_name);
			b->added = (long long)st.st_ctime;
		} else {
			metadata_read(path, tags);
		}
		b->size += (long long)st.st_size;
		if ((long long)st.st_mtime > b->mtime) {
			b->mtime = (long long)st.st_mtime;
		}

		char title[256];
		if (tags->title[0]) {
			snprintf(title, sizeof(title), "%s", tags->title);
		} else {
			const char *base = strrchr(path, '/');
			snprintf(title, sizeof(title), "%s", base ? base + 1 : path);
			strip_extension(title);
		}
		pthread_mutex_lock(&db_lock);
		if (db) {
			insert_part(path, key, i, title);
		}
		pthread_mutex_unlock(&db_lock);
	}

	char above[512];
	if (own_folder) {
		parent_of(relative(dir), above, sizeof(above));
	} else {
		snprintf(above, sizeof(above), "%s", relative(dir));
	}
	fill_from_folders(b, above);
	if (b->series[0] && b->series_part < 0) {
		char stem[256] = "";
		if (parts->count > 0) {
			const char *base = strrchr(parts->items[0], '/');
			snprintf(stem, sizeof(stem), "%s", base ? base + 1 : parts->items[0]);
			strip_extension(stem);
		}
		b->series_part = series_number(own_folder ? folder_name : stem, true, b->series);
		if (b->series_part < 0) {
			b->series_part = series_number(b->title, true, b->series);
		}
	}

	pthread_mutex_lock(&db_lock);
	if (db) {
		insert_book(key, b);
	}
	pthread_mutex_unlock(&db_lock);

	free(b->summary);
	free(tags);
	free(b);
}

// Adds the loose audio of folder `dir` (its `files`) to `parts`, as full
// paths, and says how many single-file books sit beside them.
static int collect(const char *dir, const namelist_t *files, namelist_t *parts, namelist_t *singles) {
	int books = 0;
	for (int i = 0; i < files->count && !scan_cancel; i++) {
		const char *name = files->items[i];
		if (!is_audio(name)) {
			continue;
		}
		char path[768];
		if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) {
			continue;
		}
		if (is_book_file(path, name)) {
			if (singles) {
				namelist_add(singles, path);
			}
			books++;
		} else {
			namelist_add(parts, path);
		}
	}
	return books;
}

static void scan_directory(const char *path, int depth) {
	if (scan_cancel || depth > SCAN_MAX_DEPTH) {
		return;
	}

	namelist_t files = {0};
	namelist_t dirs = {0};
	if (!read_folder(path, &files, &dirs)) {
		return;
	}

	namelist_t parts = {0};
	namelist_t singles = {0};
	collect(path, &files, &parts, &singles);

	// The disc folders belong to this folder's book: their files follow its
	// own, disc by disc. Everything else is walked on its own below.
	namelist_t walk = {0};
	for (int i = 0; i < dirs.count && !scan_cancel; i++) {
		char child[768];
		if (snprintf(child, sizeof(child), "%s/%s", path, dirs.items[i]) >= (int)sizeof(child)) {
			continue;
		}
		if (is_disc_folder(dirs.items[i])) {
			namelist_t disc_files = {0};
			namelist_t disc_dirs = {0};
			if (read_folder(child, &disc_files, &disc_dirs)) {
				collect(child, &disc_files, &parts, NULL);
			}
			namelist_free(&disc_files);
			namelist_free(&disc_dirs);
		} else {
			namelist_add(&walk, child);
		}
	}
	namelist_free(&files);
	namelist_free(&dirs);

	bool root = depth == 0;

	// The loose files, by book. In the Audiobooks folder itself each is a book
	// of its own; anywhere else they are grouped by their album tag, the
	// book's title in files split by chapter, and the files with none form one
	// group together.
	namelist_t albums = {0};
	int *group_of = calloc((size_t)(parts.count ? parts.count : 1), sizeof(int));
	int groups = 0;
	if (group_of) {
		song_metadata_t *tags = root ? NULL : calloc(1, sizeof(*tags));
		for (int i = 0; i < parts.count && !scan_cancel; i++) {
			if (!tags) {
				group_of[i] = groups++;
				continue;
			}
			memset(tags, 0, sizeof(*tags));
			metadata_read(parts.items[i], tags);
			int found = -1;
			for (int g = 0; g < albums.count; g++) {
				if (strcasecmp(albums.items[g], tags->album) == 0) {
					found = g;
					break;
				}
			}
			if (found < 0 && namelist_add(&albums, tags->album)) {
				found = albums.count - 1;
				groups++;
			}
			group_of[i] = found < 0 ? 0 : found;
		}
		free(tags);
	}

	// How many books the folder holds, counting a group of one file as a book
	// of its own.
	int books = singles.count;
	for (int g = 0; g < groups; g++) {
		int members = 0;
		for (int i = 0; i < parts.count; i++) {
			members += group_of && group_of[i] == g;
		}
		books += members >= 2 ? 1 : members;
	}

	const char *rel = relative(path);
	bool alone = !root && books == 1 && walk.count == 0;

	for (int i = 0; i < singles.count && !scan_cancel; i++) {
		const char *file = singles.items[i];
		const char *base = strrchr(file, '/');
		index_single(file, base ? base + 1 : file, rel, alone);
	}
	for (int g = 0; g < groups && group_of && !scan_cancel; g++) {
		namelist_t members = {0};
		for (int i = 0; i < parts.count; i++) {
			if (group_of[i] == g) {
				namelist_add(&members, parts.items[i]);
			}
		}
		if (members.count >= 2) {
			// One book per folder is indexed under the folder. Beside others,
			// each is indexed under the folder and its album: a path no file
			// has, since it only has to be unique.
			char key[768];
			if (groups == 1 || !albums.items[g][0]) {
				snprintf(key, sizeof(key), "%s", path);
			} else {
				snprintf(key, sizeof(key), "%s/\x1f%s", path, albums.items[g]);
			}
			index_folder(path, key, &members, books == 1);
		} else {
			for (int i = 0; i < members.count; i++) {
				const char *base = strrchr(members.items[i], '/');
				index_single(members.items[i], base ? base + 1 : members.items[i], rel, alone);
			}
		}
		namelist_free(&members);
	}
	free(group_of);
	namelist_free(&albums);
	namelist_free(&parts);
	namelist_free(&singles);

	for (int i = 0; i < walk.count && !scan_cancel; i++) {
		scan_directory(walk.items[i], depth + 1);
	}
	namelist_free(&walk);
}

static void *scan_thread_func(void *arg) {
	(void)arg;
	thread_be_background("audiobook scan");

	printf("audiobooks: scanning %s\n", scan_root);

	pthread_mutex_lock(&db_lock);
	// The rescan is wipe-and-refill, like the music library's, so books whose
	// files are gone simply never come back. What must survive the wipe is
	// where each book was left and when, so it is stashed in a temp table
	// first and each insert copies its own back.
	exec("CREATE TEMP TABLE IF NOT EXISTS old_state(path TEXT PRIMARY KEY, last_played INT, resume_file TEXT,"
		 " resume_pos REAL,resume_updated_ms INT,resume_revision INT,resume_completed INT)");
	exec("DELETE FROM old_state");
	exec("INSERT INTO old_state SELECT path, last_played, resume_file, resume_pos,resume_updated_ms,resume_revision,resume_completed FROM AUDIOBOOK_TABLE"
		 " WHERE last_played > 0 OR resume_file IS NOT NULL");
	exec("DELETE FROM AUDIOBOOK_TABLE");
	exec("DELETE FROM AUDIOBOOK_PARTS");
	// The row ids restart at one, so anything holding them is pointing at
	// other people's books.
	bump_generation();
	exec("BEGIN");
	pthread_mutex_unlock(&db_lock);

	scan_directory(scan_root, 0);

	pthread_mutex_lock(&db_lock);
	exec("COMMIT");
	if (db) {
		if (!scan_cancel) {
			char sql[48];
			snprintf(sql, sizeof(sql), "PRAGMA user_version=%d", SCHEMA_VERSION);
			exec(sql);
			outdated = false;
		}
		sqlite3_db_release_memory(db);
	}
	bump_generation();
	pthread_mutex_unlock(&db_lock);

	printf("audiobooks: scan finished, %d books%s\n", scan_found, scan_cancel ? " (stopped early)" : "");
	scan_running = false;
	return NULL;
}

bool audiobookdb_scan_start(const char *root) {
	if (!db || scan_running || !root || !root[0]) {
		return false;
	}

	snprintf(scan_root, sizeof(scan_root), "%s", root);
	scan_found = 0;
	scan_cancel = false;
	scan_running = true;

	if (pthread_create(&scan_thread, NULL, scan_thread_func, NULL) != 0) {
		scan_running = false;
		fprintf(stderr, "audiobooks: could not start the scan thread\n");
		return false;
	}

	pthread_detach(scan_thread);
	return true;
}

bool audiobookdb_scan_running(void) { return scan_running; }

int audiobookdb_scan_found(void) { return scan_found; }

void audiobookdb_scan_stop(void) {
	if (scan_running) {
		scan_cancel = true;
	}
}
