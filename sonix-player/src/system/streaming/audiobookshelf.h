#ifndef AUDIOBOOKSHELF_H
#define AUDIOBOOKSHELF_H

#include <stdbool.h>
#include <stddef.h>

#define ABS_ID_MAX 96
#define ABS_NAME_MAX 256
#define ABS_URL_MAX 1024
#define ABS_PAGE_LIMIT 40
#define ABS_TRACK_MAX 256

typedef struct {
	char id[ABS_ID_MAX];
	char name[ABS_NAME_MAX];
	char media_type[24];
} audiobookshelf_library_t;

typedef struct {
	char id[ABS_ID_MAX];
	char title[ABS_NAME_MAX];
	char author[ABS_NAME_MAX];
	char series[ABS_NAME_MAX];
	char cover[ABS_URL_MAX];
	double duration;
	double progress;
	bool finished;
	int tracks;
} audiobookshelf_item_t;

typedef struct {
	int index;
	char title[ABS_NAME_MAX];
	char filename[ABS_NAME_MAX];
	char content_url[ABS_URL_MAX];
	char mime[64];
	double start_offset;
	double duration;
} audiobookshelf_track_t;

typedef struct {
	audiobookshelf_item_t item;
	char description[8192];
	char series_part[32];
	long long progress_updated_ms;
	audiobookshelf_track_t tracks[ABS_TRACK_MAX];
	int track_count;
} audiobookshelf_book_t;

// Reads the saved server address and bearer token. Call after config_init().
void audiobookshelf_init(void);

bool audiobookshelf_configured(void);
const char *audiobookshelf_server(void);
void audiobookshelf_set_credentials(const char *server, const char *token);
void audiobookshelf_clear_credentials(void);

// All network calls block and belong on a worker thread.
int audiobookshelf_libraries(audiobookshelf_library_t *out, int max);
int audiobookshelf_items(const char *library_id, int page, audiobookshelf_item_t *out, int max, int *total_out);
// Server-wide search; returns at most max matches (no server search pagination).
int audiobookshelf_search(const char *library_id, const char *query, audiobookshelf_item_t *out, int max);
// Caller owns the allocated, alphabetically sorted filter list.
int audiobookshelf_groups(const char *library_id, bool series, audiobookshelf_library_t **out);
int audiobookshelf_filtered_items(const char *library_id, int page, const char *filter,
	 audiobookshelf_item_t *out, int max, int *total_out);
void audiobookshelf_group_filter(bool series, const char *id, char *out, size_t size);
bool audiobookshelf_book(const char *item_id, audiobookshelf_book_t *out);

// Downloads one complete book into
//   <sd>/Audiobooks/Audiobookshelf/<author>/<title>/
// and writes metadata sidecars understood by the local audiobook scanner.
// Existing complete files are kept. Temporary files end in .part and are not
// visible to the scanner.
typedef void (*audiobookshelf_progress_cb)(int track, int tracks, long long done, long long total, void *user);
bool audiobookshelf_download(const audiobookshelf_book_t *book, const char *sd_root,
								 audiobookshelf_progress_cb cb, void *user, char *book_path, size_t path_size);

// Maps Audiobookshelf's whole-book progress to the downloaded local track and
// its track-relative offset. Call after a successful download while `book` is
// still available; the scanner can then index the file before the caller saves
// the returned position in audiobookdb.
bool audiobookshelf_resume_target(const audiobookshelf_book_t *book, const char *book_path,
									 char *file_out, size_t file_size, double *seconds_out,
									 bool *finished_out);

// Links an already-indexed SD-card book to its Audiobookshelf item without
// copying audio. The local and server copies must have the same number of
// parts and nearly the same total duration. The generated manifest uses the
// local files' measured boundaries, so multipart progress maps directly.
bool audiobookshelf_link_local(const audiobookshelf_book_t *book, const char *local_book,
								 char *file_out, size_t file_size, double *seconds_out,
								 bool *finished_out);

// True when the local book's manifest already belongs to this server item.
// This lets the server library open a previously linked copy directly instead
// of offering to link the same book again.
bool audiobookshelf_local_linked(const char *local_book, const char *item_id);

typedef enum {
	ABS_SYNC_CHECKING, ABS_SYNC_LOCAL, ABS_SYNC_SETUP, ABS_SYNC_LINKED,
	ABS_SYNC_PENDING, ABS_SYNC_OFFLINE, ABS_SYNC_SENDING, ABS_SYNC_SYNCED,
	ABS_SYNC_RETRY, ABS_SYNC_OTHER_SERVER, ABS_SYNC_SYNCED_OFFLINE
} audiobookshelf_sync_status_t;

// Cached snapshot only: no file/database/network I/O on the UI thread.
audiobookshelf_sync_status_t audiobookshelf_sync_status(const char *file);
// Notification only; never enables Wi-Fi or waits for an upload.
void audiobookshelf_network_changed(void);

// Wakes the worker after a local checkpoint. The persisted audiobook position
// remains pending until the server acknowledges that exact snapshot, even
// across offline listening, multiple books and player restarts.
void audiobookshelf_queue_progress(const char *book_path, const char *file, double seconds,
								   bool finished, bool immediate);

// Why the most recent call on this thread failed.
const char *audiobookshelf_last_error(void);

#endif /* AUDIOBOOKSHELF_H */
