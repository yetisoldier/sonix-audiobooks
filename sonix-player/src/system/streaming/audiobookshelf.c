#include "audiobookshelf.h"
#include "abs_sync.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/utils.h"
#include "src/system/decode/decode.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/playback/audiobook.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"

#define ABS_BODY_LIMIT (3u * 1024u * 1024u)
#define ABS_TIMEOUT_SECS 30
#define ABS_DOWNLOAD_CHUNK 32768
#define ABS_LOCAL_PATH_MAX 1280
#define ABS_SIDECAR_PATH_MAX 1310
#define ABS_TEMP_PATH_MAX 1320

static char server_url[512];
static char bearer_token[1024];
static pthread_mutex_t credentials_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread char last_error[256];

#define ABS_PROGRESS_PATH_MAX 1280

typedef struct {
	char book[ABS_PROGRESS_PATH_MAX];
	char file[ABS_PROGRESS_PATH_MAX];
	double seconds;
	bool finished;
} progress_job_t;

static pthread_once_t progress_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t progress_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t progress_cond = PTHREAD_COND_INITIALIZER;
static bool progress_worker_ready;
static bool progress_wake;
static bool progress_reconnect = true;
static char progress_force_book[512];
static char progress_watch_file[512];
static audiobookshelf_sync_status_t progress_status = ABS_SYNC_CHECKING;
static void progress_worker_start(void);

static void set_error(const char *message) {
	snprintf(last_error, sizeof(last_error), "%s", message ? message : "");
}

const char *audiobookshelf_last_error(void) { return last_error; }

static void normalize_server(const char *server, char *out, size_t size) {
	if (!out || size == 0) return;
	out[0] = '\0';
	if (!server) return;
	while (isspace((unsigned char)*server)) server++;
	snprintf(out, size, "%s", server);
	size_t len = strlen(out);
	while (len && isspace((unsigned char)out[len - 1])) out[--len] = '\0';
	while (len > 0 && out[len - 1] == '/') out[--len] = '\0';
}

void audiobookshelf_init(void) {
	normalize_server(config_get("audiobookshelf", "server", ""), server_url, sizeof(server_url));
	snprintf(bearer_token, sizeof(bearer_token), "%s", config_get("audiobookshelf", "token", ""));
#ifdef HOST_BUILD
	// Test a server without ever putting its secret in a checkout or simulator
	// config file. Device builds intentionally have no environment override.
	const char *host_server = getenv("SONIX_AUDIOBOOKSHELF_SERVER");
	const char *host_token = getenv("SONIX_AUDIOBOOKSHELF_TOKEN");
	if (host_server && host_server[0]) normalize_server(host_server, server_url, sizeof(server_url));
	if (host_token && host_token[0]) snprintf(bearer_token, sizeof(bearer_token), "%s", host_token);
#endif
	printf("audiobookshelf: %s\n", audiobookshelf_configured() ? "configured" : "not configured");
	if (audiobookshelf_configured()) pthread_once(&progress_once, progress_worker_start);
}

static void credentials_copy(char *server, size_t server_size, char *token, size_t token_size) {
	pthread_mutex_lock(&credentials_lock);
	if (server && server_size) snprintf(server, server_size, "%s", server_url);
	if (token && token_size) snprintf(token, token_size, "%s", bearer_token);
	pthread_mutex_unlock(&credentials_lock);
}

bool audiobookshelf_configured(void) {
	pthread_mutex_lock(&credentials_lock);
	bool configured = server_url[0] && bearer_token[0];
	pthread_mutex_unlock(&credentials_lock);
	return configured;
}
const char *audiobookshelf_server(void) { return server_url; }

void audiobookshelf_set_credentials(const char *server, const char *token) {
	pthread_mutex_lock(&credentials_lock);
	normalize_server(server, server_url, sizeof(server_url));
	snprintf(bearer_token, sizeof(bearer_token), "%s", token ? token : "");
	config_set("audiobookshelf", "server", server_url);
	config_set("audiobookshelf", "token", bearer_token);
	pthread_mutex_unlock(&credentials_lock);
	config_save();
	if (audiobookshelf_configured()) pthread_once(&progress_once, progress_worker_start);
	pthread_mutex_lock(&progress_lock);
	progress_wake = true;
	pthread_cond_signal(&progress_cond);
	pthread_mutex_unlock(&progress_lock);
}

void audiobookshelf_clear_credentials(void) { audiobookshelf_set_credentials("", ""); }

static void auth_headers(char *out, size_t size) {
	pthread_mutex_lock(&credentials_lock);
	snprintf(out, size, "Authorization: Bearer %s\r\n", bearer_token);
	pthread_mutex_unlock(&credentials_lock);
}

static void error_from_response(int status, const char *body) {
	if (status == 401 || status == 403) {
		set_error("Audiobookshelf refused the token.");
		return;
	}
	if (body && body[0]) {
		json_doc_t doc;
		if (json_parse(body, &doc)) {
			char message[192] = "";
			int root = json_root(&doc);
			json_obj_str(&doc, root, "error", message, sizeof(message));
			if (!message[0]) json_obj_str(&doc, root, "message", message, sizeof(message));
			json_free(&doc);
			if (message[0]) {
				set_error(message);
				return;
			}
		}
	}
	char message[96];
	snprintf(message, sizeof(message), "Audiobookshelf answered %d.", status);
	set_error(message);
}

static bool api_request(const char *path, const http_req_t *request, char **body, int *status_out,
						const char *expected_server) {
	char url[ABS_URL_MAX];
	char server[512], token[1024];
	credentials_copy(server, sizeof(server), token, sizeof(token));
	if (!server[0] || !token[0] || (expected_server && strcmp(server, expected_server) != 0)) {
		set_error("Audiobookshelf settings changed; checkpoint retained.");
		return false;
	}
	if (!path || (size_t)snprintf(url, sizeof(url), "%s%s", server, path) >= sizeof(url)) return false;
	char headers[1200];
	snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", token);
	http_req_t req = request ? *request : (http_req_t){0};
	req.extra_headers = headers;
	req.want_error_body = true;

	int status = 0;
	bool received = http_request(url, &req, body, NULL, ABS_BODY_LIMIT,
								 expected_server ? 10 : ABS_TIMEOUT_SECS, &status);
	if (!received || status < 200 || status >= 300) {
		const char *why = http_last_error();
		if (status >= 400) error_from_response(status, body && *body ? *body : NULL);
		else set_error(why && why[0] ? why : "Audiobookshelf did not reply.");
		if (status_out) *status_out = status;
		return false;
	}
	if (status_out) *status_out = status;
	set_error("");
	return true;
}

static bool update_progress(const char *item_id, double current, double duration, bool finished,
	long long updated_ms, const char *server) {
	char id[ABS_ID_MAX * 3];
	http_url_encode(item_id, id, sizeof(id));
	char path[ABS_ID_MAX * 3 + 32];
	snprintf(path, sizeof(path), "/api/me/progress/%s", id);
	char json[256], timestamp[64] = "";
	if (updated_ms > 0) snprintf(timestamp, sizeof(timestamp), ",\"lastUpdate\":%lld", updated_ms);
	snprintf(json, sizeof(json),
			 "{\"currentTime\":%.3f,\"duration\":%.3f,\"isFinished\":%s%s}", current, duration,
			 finished ? "true" : "false", timestamp);
	http_req_t req = {
		.method = "PATCH",
		.body = json,
		.content_type = "application/json",
		.want_error_body = true,
		.allow_empty_body = true,
	};
	char *body = NULL;
	bool ok = api_request(path, &req, &body, NULL, server);
	free(body);
	return ok;
}

static bool read_progress(const char *item_id, const char *server, abs_remote_progress_t *out) {
	memset(out, 0, sizeof(*out));
	char id[ABS_ID_MAX * 3], path[ABS_ID_MAX * 3 + 32];
	http_url_encode(item_id, id, sizeof(id));
	snprintf(path, sizeof(path), "/api/me/progress/%s", id);
	char *body = NULL;
	int status = 0;
	http_req_t req = {.allow_empty_body = true};
	bool ok = api_request(path, &req, &body, &status, server);
	out->server_now_ms = http_last_server_time_ms();
	if (!ok) {
		free(body);
		return status == 404;
	}
	json_doc_t doc;
	if (!body || !json_parse(body, &doc)) { free(body); return false; }
	int root = json_root(&doc);
	char returned_id[ABS_ID_MAX];
	json_obj_str(&doc, root, "libraryItemId", returned_id, sizeof(returned_id));
	out->seconds = json_double(&doc, json_get(&doc, root, "currentTime"), -1);
	out->finished = json_obj_bool(&doc, root, "isFinished", false);
	out->updated_ms = json_obj_llong(&doc, root, "lastUpdate", 0);
	out->exists = true;
	ok = strcmp(returned_id, item_id) == 0 && isfinite(out->seconds) && out->seconds >= 0;
	json_free(&doc);
	free(body);
	return ok;
}

static bool local_progress(const progress_job_t *job, char *item_id, size_t id_size,
						   double *current_out, double *duration_out, char *bound_server, size_t server_size,
						   char *remote_file, double *remote_seconds) {
	set_error("");
	char folder[ABS_PROGRESS_PATH_MAX];
	snprintf(folder, sizeof(folder), "%s", job->book);
	struct stat st;
	if (stat(folder, &st) != 0) {
		// A folder book can have a synthetic database key when several albums
		// share one directory. Its playing file still names the real directory.
		snprintf(folder, sizeof(folder), "%s", job->file);
		char *slash = strrchr(folder, '/');
		if (!slash) return false;
		*slash = '\0';
	} else if (!S_ISDIR(st.st_mode)) {
		char *slash = strrchr(folder, '/');
		if (!slash) return false;
		*slash = '\0';
	}
	char manifest[ABS_PROGRESS_PATH_MAX + 32];
	if ((size_t)snprintf(manifest, sizeof(manifest), "%s/.audiobookshelf", folder) >= sizeof(manifest))
		return false;
	FILE *f = fopen(manifest, "r");
	if (!f) {
		if (errno != ENOENT) set_error("The Audiobookshelf link could not be read.");
		return false;
	}

	item_id[0] = '\0';
	double duration = 0;
	double track_start = 0;
	bool track_found = false;
	if (remote_file) remote_file[0] = '\0';
	const char *basename = strrchr(job->file, '/');
	basename = basename ? basename + 1 : job->file;
	char line[1024];
	for (int lines = 0; lines < ABS_TRACK_MAX + 16 && fgets(line, sizeof(line), f); lines++) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strncmp(line, "item_id=", 8) == 0) {
			size_t len = strlen(line + 8);
			if (len >= id_size) len = id_size - 1;
			memcpy(item_id, line + 8, len);
			item_id[len] = '\0';
		}
		else if (strncmp(line, "server=", 7) == 0) snprintf(bound_server, server_size, "%.*s", (int)server_size - 1, line + 7);
		else if (strncmp(line, "duration=", 9) == 0) duration = strtod(line + 9, NULL);
		else if (strncmp(line, "track=", 6) == 0) {
			char *first = strchr(line, '\t');
			char *second = first ? strchr(first + 1, '\t') : NULL;
			char *third = second ? strchr(second + 1, '\t') : NULL;
			if (first && second && third && strcmp(third + 1, basename) == 0) {
				track_start = strtod(first + 1, NULL);
				track_found = true;
			}
			if (remote_file && first && second && third) {
				double start = strtod(first + 1, NULL), length = strtod(second + 1, NULL);
				double target = job->finished ? 0 : job->seconds;
				const char *name = third + 1;
				if (isfinite(start) && isfinite(length) && start >= 0 && length > 0 &&
					target >= start && (target < start + length ||
					(fabs(target - duration) <= 0.001 && fabs(start + length - duration) <= 0.01)) && name[0] &&
					!strchr(name, '/') && !strchr(name, '\\') && strcmp(name, ".") && strcmp(name, "..")) {
					if ((size_t)snprintf(remote_file, 512, "%s/%s", folder, name) >= 512) remote_file[0] = '\0';
					*remote_seconds = target - start;
				}
			}
		}
	}
	fclose(f);
	if (!item_id[0] || !track_found || !isfinite(duration) || duration <= 0 ||
		!isfinite(track_start) || track_start < 0 || !isfinite(job->seconds)) {
		set_error("The Audiobookshelf link does not match this file.");
		return false;
	}

	double current = job->finished ? duration : track_start + job->seconds;
	if (current < 0) current = 0;
	if (current > duration) current = duration;
	*current_out = current;
	*duration_out = duration;
	return true;
}

static bool progress_online(void) {
#ifdef HOST_BUILD
	return true;
#else
	return wifi_get_enabled();
#endif
}

static void progress_publish(const char *file, audiobookshelf_sync_status_t status) {
	pthread_mutex_lock(&progress_lock);
	if (strcmp(file, progress_watch_file) == 0) progress_status = status;
	pthread_mutex_unlock(&progress_lock);
}

static void progress_refresh_status(void) {
	char file[512], book[512];
	char server[512];
	credentials_copy(server, sizeof(server), NULL, 0);
	pthread_mutex_lock(&progress_lock);
	snprintf(file, sizeof(file), "%s", progress_watch_file);
	pthread_mutex_unlock(&progress_lock);
	if (!file[0]) return;
	audiobookdb_abs_checkpoint_t checkpoint;
	if (!audiobookdb_book_for_file(file, book, sizeof(book)) || !audiobookdb_abs_get(book, &checkpoint)) {
		progress_publish(file, ABS_SYNC_LOCAL);
		return;
	}
	bool linked = checkpoint.linked;
	if (!checkpoint.known) {
		progress_job_t job = {0};
		snprintf(job.book, sizeof(job.book), "%s", book);
		snprintf(job.file, sizeof(job.file), "%s", file);
		char item[ABS_ID_MAX];
		double current, duration;
		snprintf(checkpoint.server, sizeof(checkpoint.server), "%s", server);
		linked = local_progress(&job, item, sizeof(item), &current, &duration, checkpoint.server, sizeof(checkpoint.server), NULL, NULL);
		if (!linked && last_error[0]) {
			progress_publish(file, ABS_SYNC_RETRY);
			return;
		}
	}
	audiobookshelf_sync_status_t status = !linked ? ABS_SYNC_LOCAL :
		!audiobookshelf_configured() ? ABS_SYNC_SETUP :
		strcmp(checkpoint.server, server) != 0 ? ABS_SYNC_OTHER_SERVER :
		!progress_online() ? (checkpoint.synced ? ABS_SYNC_SYNCED_OFFLINE : ABS_SYNC_OFFLINE) :
		checkpoint.synced ? ABS_SYNC_SYNCED :
		checkpoint.failed ? ABS_SYNC_RETRY :
		checkpoint.file[0] ? ABS_SYNC_PENDING : ABS_SYNC_LINKED;
	progress_publish(file, status);
}

// 0: retain/retry, 1: acknowledge sent checkpoint, 2: newer remote imported.
static int reconcile_progress(audiobookdb_abs_checkpoint_t *checkpoint, const progress_job_t *job,
	const char *item, double current, double duration, const char *server) {
	abs_remote_progress_t remote;
	if (!read_progress(item, server, &remote)) return 0;
	checkpoint->sync_error = 0;
	for (int check = 0; check < 2; check++) {
		abs_decision_t decision = abs_sync_decide(current, checkpoint->finished, checkpoint->updated_ms,
			&remote, (long long)time(NULL) * 1000);
		if (decision == ABS_DECISION_EQUAL) {
			checkpoint->server_updated_ms = remote.updated_ms;
			return 1;
		}
		if (decision == ABS_DECISION_HOLD) return 0;
		if (decision == ABS_DECISION_DOWNLOAD) {
			progress_job_t incoming = *job;
			incoming.seconds = remote.seconds;
			incoming.finished = remote.finished;
			char file[512], owner[512], bound[512], ignored[ABS_ID_MAX];
			double seconds = 0, unused, total;
			snprintf(bound, sizeof(bound), "%s", server);
			if (!local_progress(&incoming, ignored, sizeof(ignored), &unused, &total, bound, sizeof(bound), file, &seconds) ||
				!file[0] || strcmp(bound, server) || !audiobookdb_book_for_file(file, owner, sizeof(owner)) ||
				strcmp(owner, checkpoint->book) || access(file, R_OK)) return 0;
			long long stamp = remote.updated_ms >= 1577836800000LL &&
				remote.updated_ms <= remote.server_now_ms + 300000 ? remote.updated_ms : 0;
			if (!audiobook_sync_import(checkpoint, server, file, seconds, remote.finished, stamp)) return 0;
			printf("audiobookshelf: imported newer server progress %.1f s\n", remote.seconds);
			return 2;
		}
		// Re-read immediately before the write; ABS has no conditional PATCH.
		if (check == 0 && !read_progress(item, server, &remote)) return 0;
	}
	long long stamp = abs_sync_clock_valid(checkpoint->updated_ms, &remote, (long long)time(NULL) * 1000) ?
		checkpoint->updated_ms : 0;
	bool restarting = remote.exists && remote.finished && !checkpoint->finished && current > 0;
	if (!update_progress(item, current, duration, checkpoint->finished, stamp, server)) return 0;
	if (restarting) {
		// ABS clears currentTime when un-finishing; restore it only after
		// confirming that transition, not if another client moved meanwhile.
		if (!read_progress(item, server, &remote) || remote.finished ||
			(remote.seconds != 0 && fabs(remote.seconds - current) > 0.01)) return 0;
		if (remote.seconds == 0 && !update_progress(item, current, duration, false, stamp, server)) return 0;
	}
	// A successful status alone is not proof that the server kept our position.
	if (!read_progress(item, server, &remote) || !remote.exists || remote.finished != checkpoint->finished ||
		fabs(remote.seconds - current) > 0.01) return 0;
	checkpoint->server_updated_ms = remote.updated_ms;
	return 1;
}

static void *progress_worker(void *unused) {
	(void)unused;
	thread_be_background("abs-progress");
	for (;;) {
		// One sleeping worker, no directory walk or decoder work. Pending
		// uploads are differences from the existing durable local checkpoints.
		pthread_mutex_lock(&progress_lock);
		if (!progress_wake && !progress_online()) {
			pthread_cond_wait(&progress_cond, &progress_lock);
		} else if (!progress_wake) {
			struct timespec wake;
			clock_gettime(CLOCK_REALTIME, &wake);
			wake.tv_sec += 15;
			pthread_cond_timedwait(&progress_cond, &progress_lock, &wake);
		}
		progress_wake = false;
		bool reconnect = progress_reconnect;
		progress_reconnect = false;
		char force[512];
		snprintf(force, sizeof(force), "%s", progress_force_book);
		progress_force_book[0] = '\0';
		pthread_mutex_unlock(&progress_lock);
		audiobookdb_flush_positions();
		if (reconnect) audiobookdb_abs_reconnect();
		if (force[0]) audiobookdb_abs_retry(force);
		progress_refresh_status();
		if (!audiobookshelf_configured() || !progress_online()) continue;
		char server[512];
		credentials_copy(server, sizeof(server), NULL, 0);
		for (int count = 0; count < 8; count++) {
			audiobookdb_abs_checkpoint_t checkpoint;
			if (!audiobookdb_abs_next(server, &checkpoint)) break;
			progress_job_t job = {0};
			snprintf(job.book, sizeof(job.book), "%s", checkpoint.book);
			snprintf(job.file, sizeof(job.file), "%s", checkpoint.file);
			job.seconds = checkpoint.seconds;
			job.finished = checkpoint.finished;
			char item[ABS_ID_MAX], bound[512];
			snprintf(bound, sizeof(bound), "%s", server);
			double current = 0, duration = 0;
			bool linked = local_progress(&job, item, sizeof(item), &current, &duration, bound, sizeof(bound), NULL, NULL);
			bool ok = false, failed = last_error[0] != '\0';
			int result = 0;
			if (linked && strcmp(bound, server) == 0 && progress_online()) {
				progress_publish(checkpoint.file, ABS_SYNC_SENDING);
				result = reconcile_progress(&checkpoint, &job, item, current, duration, server);
				ok = result != 0;
				failed = !ok;
				if (ok) printf("audiobookshelf: synced progress %.1f / %.1f s\n", current, duration);
				else fprintf(stderr, "audiobookshelf: progress sync failed; saved checkpoint retained\n");
			}
			if (result != 2) audiobookdb_abs_result(&checkpoint, bound, linked || failed, ok, failed);
			progress_refresh_status();
			if (failed) break; // one unavailable server must not cause a burst of timeouts
		}
	}
	return NULL;
}

static void progress_worker_start(void) {
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 96 * 1024);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_t thread;
	progress_worker_ready = pthread_create(&thread, &attr, progress_worker, NULL) == 0;
	pthread_attr_destroy(&attr);
	if (!progress_worker_ready) fprintf(stderr, "audiobookshelf: could not start progress worker\n");
}

void audiobookshelf_queue_progress(const char *book_path, const char *file, double seconds,
								   bool finished, bool immediate) {
	(void)seconds;
	(void)finished;
	if (!audiobookshelf_configured() || !book_path || !book_path[0] || !file || !file[0]) return;
	pthread_once(&progress_once, progress_worker_start);
	if (!progress_worker_ready) return;
	pthread_mutex_lock(&progress_lock);
	if (strcmp(file, progress_watch_file) == 0 &&
		(progress_status == ABS_SYNC_SYNCED || progress_status == ABS_SYNC_SYNCED_OFFLINE)) {
		progress_status = progress_online() ? ABS_SYNC_PENDING : ABS_SYNC_OFFLINE;
	}
	if (immediate) snprintf(progress_force_book, sizeof(progress_force_book), "%s", book_path);
	progress_wake = true;
	pthread_cond_signal(&progress_cond);
	pthread_mutex_unlock(&progress_lock);
}

void audiobookshelf_network_changed(void) {
	pthread_mutex_lock(&progress_lock);
	progress_wake = true;
	progress_reconnect = true;
	pthread_cond_signal(&progress_cond);
	pthread_mutex_unlock(&progress_lock);
}

audiobookshelf_sync_status_t audiobookshelf_sync_status(const char *file) {
	if (!file || !file[0]) {
		pthread_mutex_lock(&progress_lock);
		progress_watch_file[0] = '\0';
		progress_status = ABS_SYNC_LOCAL;
		pthread_mutex_unlock(&progress_lock);
		return ABS_SYNC_LOCAL;
	}
	if (!audiobookshelf_configured()) return ABS_SYNC_LOCAL;
	pthread_once(&progress_once, progress_worker_start);
	pthread_mutex_lock(&progress_lock);
	if (strcmp(file, progress_watch_file) != 0) {
		snprintf(progress_watch_file, sizeof(progress_watch_file), "%s", file);
		progress_status = ABS_SYNC_CHECKING;
		progress_wake = true;
		pthread_cond_signal(&progress_cond);
	}
	audiobookshelf_sync_status_t status = progress_status;
	pthread_mutex_unlock(&progress_lock);
	return status;
}

static void first_author(const json_doc_t *doc, int metadata, char *out, size_t size) {
	out[0] = '\0';
	json_obj_str(doc, metadata, "authorName", out, size);
	if (out[0]) return;
	int authors = json_get(doc, metadata, "authors");
	int first = json_at(doc, authors, 0);
	json_obj_str(doc, first, "name", out, size);
}

static void first_series(const json_doc_t *doc, int metadata, char *name, size_t name_size,
						 char *sequence, size_t sequence_size) {
	name[0] = '\0';
	if (sequence && sequence_size) sequence[0] = '\0';
	json_obj_str(doc, metadata, "seriesName", name, name_size);
	int series = json_get(doc, metadata, "series");
	int first = json_at(doc, series, 0);
	if (!name[0]) json_obj_str(doc, first, "name", name, name_size);
	if (sequence && sequence_size) json_obj_str(doc, first, "sequence", sequence, sequence_size);
}

static void read_item(const json_doc_t *doc, int object, audiobookshelf_item_t *item) {
	memset(item, 0, sizeof(*item));
	json_obj_str(doc, object, "id", item->id, sizeof(item->id));
	int media = json_get(doc, object, "media");
	int metadata = json_get(doc, media, "metadata");
	json_obj_str(doc, metadata, "title", item->title, sizeof(item->title));
	first_author(doc, metadata, item->author, sizeof(item->author));
	char unused[2];
	first_series(doc, metadata, item->series, sizeof(item->series), unused, sizeof(unused));
	item->duration = json_double(doc, json_get(doc, media, "duration"), 0);
	item->tracks = (int)json_obj_long(doc, media, "numTracks", 0);
	if (!item->tracks) item->tracks = json_len(doc, json_get(doc, media, "tracks"));

	char cover_path[ABS_URL_MAX] = "";
	json_obj_str(doc, media, "coverPath", cover_path, sizeof(cover_path));
	if (cover_path[0] && item->id[0])
		snprintf(item->cover, sizeof(item->cover), "/api/items/%s/cover", item->id);

	int progress = json_get(doc, object, "userMediaProgress");
	item->progress = json_double(doc, json_get(doc, progress, "currentTime"), 0);
	item->finished = json_obj_bool(doc, progress, "isFinished", false);
	if (!item->title[0]) snprintf(item->title, sizeof(item->title), "Untitled");
}

int audiobookshelf_libraries(audiobookshelf_library_t *out, int max) {
	if (!out || max <= 0) return 0;
	char *body = NULL;
	if (!api_request("/api/libraries", NULL, &body, NULL, NULL)) {
		free(body);
		return -1;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		set_error("Audiobookshelf returned invalid library data.");
		return -1;
	}
	int libraries = json_get(&doc, json_root(&doc), "libraries");
	int count = json_len(&doc, libraries);
	if (count > max) count = max;
	int written = 0;
	for (int i = 0; i < count; i++) {
		int object = json_at(&doc, libraries, i);
		audiobookshelf_library_t entry = {0};
		json_obj_str(&doc, object, "id", entry.id, sizeof(entry.id));
		json_obj_str(&doc, object, "name", entry.name, sizeof(entry.name));
		json_obj_str(&doc, object, "mediaType", entry.media_type, sizeof(entry.media_type));
		if (entry.id[0] && (!entry.media_type[0] || strcmp(entry.media_type, "book") == 0)) out[written++] = entry;
	}
	json_free(&doc);
	free(body);
	return written;
}

int audiobookshelf_items(const char *library_id, int page, audiobookshelf_item_t *out, int max, int *total_out) {
	if (!library_id || !library_id[0] || !out || max <= 0) return 0;
	if (max > ABS_PAGE_LIMIT) max = ABS_PAGE_LIMIT;
	char id[ABS_ID_MAX * 3];
	http_url_encode(library_id, id, sizeof(id));
	char path[512];
	snprintf(path, sizeof(path), "/api/libraries/%s/items?limit=%d&page=%d&sort=media.metadata.title&minified=0&collapseSeries=0",
			 id, max, page < 0 ? 0 : page);
	char *body = NULL;
	if (!api_request(path, NULL, &body, NULL, NULL)) {
		free(body);
		return -1;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		set_error("Audiobookshelf returned invalid book data.");
		return -1;
	}
	int root = json_root(&doc);
	if (total_out) *total_out = (int)json_obj_long(&doc, root, "total", 0);
	int results = json_get(&doc, root, "results");
	int count = json_len(&doc, results);
	if (count > max) count = max;
	for (int i = 0; i < count; i++) read_item(&doc, json_at(&doc, results, i), &out[i]);
	json_free(&doc);
	free(body);
	return count;
}

bool audiobookshelf_book(const char *item_id, audiobookshelf_book_t *out) {
	if (!item_id || !item_id[0] || !out) return false;
	memset(out, 0, sizeof(*out));
	char id[ABS_ID_MAX * 3];
	http_url_encode(item_id, id, sizeof(id));
	char path[384];
	snprintf(path, sizeof(path), "/api/items/%s?expanded=1&include=progress,authors", id);
	char *body = NULL;
	if (!api_request(path, NULL, &body, NULL, NULL)) {
		free(body);
		return false;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		set_error("Audiobookshelf returned invalid book details.");
		return false;
	}
	int root = json_root(&doc);
	read_item(&doc, root, &out->item);
	int media = json_get(&doc, root, "media");
	int metadata = json_get(&doc, media, "metadata");
	json_obj_str(&doc, metadata, "description", out->description, sizeof(out->description));
	first_series(&doc, metadata, out->item.series, sizeof(out->item.series), out->series_part, sizeof(out->series_part));
	int progress = json_get(&doc, root, "userMediaProgress");
	out->progress_updated_ms = json_obj_llong(&doc, progress, "lastUpdate", 0);

	int tracks = json_get(&doc, media, "tracks");
	int count = json_len(&doc, tracks);
	if (count > ABS_TRACK_MAX) count = ABS_TRACK_MAX;
	for (int i = 0; i < count; i++) {
		int object = json_at(&doc, tracks, i);
		audiobookshelf_track_t *track = &out->tracks[i];
		track->index = (int)json_obj_long(&doc, object, "index", i + 1);
		track->start_offset = json_double(&doc, json_get(&doc, object, "startOffset"), 0);
		track->duration = json_double(&doc, json_get(&doc, object, "duration"), 0);
		json_obj_str(&doc, object, "title", track->title, sizeof(track->title));
		json_obj_str(&doc, object, "contentUrl", track->content_url, sizeof(track->content_url));
		json_obj_str(&doc, object, "mimeType", track->mime, sizeof(track->mime));
		json_obj_str(&doc, json_get(&doc, object, "metadata"), "filename", track->filename, sizeof(track->filename));
		if (!track->filename[0]) {
			size_t title_len = strnlen(track->title, sizeof(track->title));
			if (title_len >= sizeof(track->filename)) title_len = sizeof(track->filename) - 1;
			memcpy(track->filename, track->title, title_len);
			track->filename[title_len] = '\0';
		}
	}
	out->track_count = count;
	out->item.tracks = count;
	json_free(&doc);
	free(body);
	if (count == 0) {
		set_error("This Audiobookshelf item has no downloadable audio tracks.");
		return false;
	}
	return true;
}

static bool make_dirs(const char *path) {
	char copy[1024];
	if (!path || !path[0] || strlen(path) >= sizeof(copy)) return false;
	snprintf(copy, sizeof(copy), "%s", path);
	for (char *p = copy + 1; *p; p++) {
		if (*p != '/') continue;
		*p = '\0';
		if (mkdir(copy, 0777) != 0 && errno != EEXIST) return false;
		*p = '/';
	}
	return mkdir(copy, 0777) == 0 || errno == EEXIST;
}

static void safe_component(const char *input, char *out, size_t size) {
	if (!out || size == 0) return;
	size_t w = 0;
	for (size_t i = 0; input && input[i] && w + 1 < size; i++) {
		unsigned char c = (unsigned char)input[i];
		if (c < 32 || strchr("<>:\"/\\|?*", c)) c = '_';
		if ((c == ' ' || c == '.') && w == 0) continue;
		out[w++] = (char)c;
	}
	while (w && (out[w - 1] == ' ' || out[w - 1] == '.')) w--;
	out[w] = '\0';
	if (!out[0]) snprintf(out, size, "Unknown");
}

static void encode_content_path(const char *input, char *out, size_t size) {
	static const char hex[] = "0123456789ABCDEF";
	size_t w = 0;
	for (size_t i = 0; input && input[i] && w + 4 < size; i++) {
		unsigned char c = (unsigned char)input[i];
		bool safe = isalnum(c) || strchr("/-._~%?=&", c);
		if (safe) out[w++] = (char)c;
		else {
			out[w++] = '%';
			out[w++] = hex[c >> 4];
			out[w++] = hex[c & 15];
		}
	}
	out[w] = '\0';
}

static bool write_sidecars(const char *audio, const audiobookshelf_book_t *book,
						   const audiobookshelf_track_t *track, int number) {
	char tags[ABS_SIDECAR_PATH_MAX], temporary[ABS_TEMP_PATH_MAX];
	if ((size_t)snprintf(tags, sizeof(tags), "%s.tags", audio) >= sizeof(tags) ||
		(size_t)snprintf(temporary, sizeof(temporary), "%s.tmp", tags) >= sizeof(temporary)) return false;
	FILE *f = fopen(temporary, "w");
	if (!f) return false;
	const char *display_title = book->track_count == 1 ? book->item.title :
		(track->title[0] ? track->title : track->filename);
	fprintf(f, "title=%s\n", display_title);
	fprintf(f, "artist=%s\n", book->item.author);
	fprintf(f, "album_artist=%s\n", book->item.author);
	fprintf(f, "album=%s\n", book->item.title);
	fprintf(f, "series=%s\n", book->item.series);
	fprintf(f, "series_part=%s\n", book->series_part);
	fprintf(f, "track=%d\n", number);
	fprintf(f, "abs_item_id=%s\n", book->item.id);
	if (fclose(f) != 0 || rename(temporary, tags) != 0) {
		remove(temporary);
		return false;
	}
	return true;
}

static bool write_description(const char *audio, const char *description) {
	if (!description || !description[0]) return true;
	char path[ABS_SIDECAR_PATH_MAX], temporary[ABS_TEMP_PATH_MAX];
	if ((size_t)snprintf(path, sizeof(path), "%s.description", audio) >= sizeof(path) ||
		(size_t)snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= sizeof(temporary)) return false;
	FILE *f = fopen(temporary, "wb");
	if (!f) return false;
	size_t len = strlen(description);
	bool ok = fwrite(description, 1, len, f) == len;
	if (fclose(f) != 0) ok = false;
	if (!ok || rename(temporary, path) != 0) {
		remove(temporary);
		return false;
	}
	return true;
}

static bool download_file(const char *url, const char *destination, int track_no, int track_count,
						  audiobookshelf_progress_cb cb, void *user) {
	struct stat existing;
	if (stat(destination, &existing) == 0 && existing.st_size > 0) return true;

	char headers[1200];
	auth_headers(headers, sizeof(headers));
	http_stream_t stream;
	if (!http_stream_open_ex(&stream, url, headers, ABS_TIMEOUT_SECS)) {
		const char *why = http_last_error();
		set_error(why && why[0] ? why : "The audiobook file could not be downloaded.");
		return false;
	}
	char partial[ABS_TEMP_PATH_MAX];
	if ((size_t)snprintf(partial, sizeof(partial), "%s.part", destination) >= sizeof(partial)) {
		http_stream_close(&stream);
		set_error("The audiobook download path is too long.");
		return false;
	}
	FILE *f = fopen(partial, "wb");
	if (!f) {
		http_stream_close(&stream);
		set_error(errno == ENOSPC ? "The SD card is full." : "The audiobook file could not be written.");
		return false;
	}
	char buffer[ABS_DOWNLOAD_CHUNK];
	long long done = 0;
	bool ok = true;
	for (;;) {
		int n = http_stream_read(&stream, buffer, sizeof(buffer));
		if (n == 0) break;
		if (n < 0 || fwrite(buffer, 1, (size_t)n, f) != (size_t)n) {
			ok = false;
			break;
		}
		done += n;
		if (cb) cb(track_no, track_count, done, stream.content_length, user);
	}
	if (fclose(f) != 0) ok = false;
	http_stream_close(&stream);
	if (!ok || done == 0 || (stream.content_length > 0 && done != stream.content_length) || rename(partial, destination) != 0) {
		remove(partial);
		set_error(errno == ENOSPC ? "The SD card is full." : "The audiobook download was interrupted.");
		return false;
	}
	return true;
}

static bool local_track_name(const audiobookshelf_track_t *track, int index, char *out, size_t size) {
	char filename[240];
	safe_component(track->filename[0] ? track->filename : track->title, filename, sizeof(filename));
	return (size_t)snprintf(out, size, "%03d - %s", index + 1, filename) < size;
}

bool audiobookshelf_resume_target(const audiobookshelf_book_t *book, const char *book_path,
									 char *file_out, size_t file_size, double *seconds_out,
									 bool *finished_out) {
	if (file_out && file_size) file_out[0] = '\0';
	if (seconds_out) *seconds_out = 0;
	if (finished_out) *finished_out = false;
	if (!book || !book_path || !book_path[0] || !file_out || file_size == 0 || book->track_count <= 0)
		return false;

	bool finished = book->item.finished;
	double whole = finished ? 0 : book->item.progress;
	if (whole < 0) whole = 0;
	if (book->item.duration > 0 && whole > book->item.duration) whole = book->item.duration;

	int selected = 0;
	if (!finished) {
		for (int i = 1; i < book->track_count; i++) {
			if (book->tracks[i].start_offset <= whole + 0.001) selected = i;
			else break;
		}
	}

	char local_name[280];
	if (!local_track_name(&book->tracks[selected], selected, local_name, sizeof(local_name)) ||
		(size_t)snprintf(file_out, file_size, "%s/%s", book_path, local_name) >= file_size) {
		file_out[0] = '\0';
		return false;
	}

	double relative = whole - book->tracks[selected].start_offset;
	if (relative < 0) relative = 0;
	if (book->tracks[selected].duration > 0 && relative > book->tracks[selected].duration)
		relative = book->tracks[selected].duration;
	if (seconds_out) *seconds_out = relative;
	if (finished_out) *finished_out = finished;
	return true;
}

typedef struct {
	char (*paths)[ABS_LOCAL_PATH_MAX];
	int capacity;
	int count;
} local_files_t;

static bool collect_local_file(const char *path, const char *title, void *user) {
	(void)title;
	local_files_t *files = user;
	if (files->count >= files->capacity) return false;
	snprintf(files->paths[files->count++], ABS_LOCAL_PATH_MAX, "%s", path);
	return true;
}

static bool local_file_duration(const char *path, double *seconds) {
	decode_format_t format = decode_detect_format(path);
	if (format == DECODE_FORMAT_UNKNOWN) return false;
	decoder_t *dec = decoder_open(path, format);
	if (!dec) return false;
	int rate = decoder_sample_rate(dec);
	uint64_t frames = decoder_total_pcm_frames(dec);
	decoder_close(dec);
	if (rate <= 0 || frames == 0) return false;
	*seconds = (double)frames / (double)rate;
	return *seconds > 0;
}

bool audiobookshelf_local_linked(const char *local_book, const char *item_id) {
	if (!local_book || !local_book[0] || !item_id || !item_id[0]) return false;

	char folder[ABS_LOCAL_PATH_MAX];
	snprintf(folder, sizeof(folder), "%s", local_book);
	if (!audiobookdb_is_folder_book(local_book)) {
		char *slash = strrchr(folder, '/');
		if (!slash) return false;
		*slash = '\0';
	}

	char manifest[ABS_LOCAL_PATH_MAX];
	if ((size_t)snprintf(manifest, sizeof(manifest), "%s/.audiobookshelf", folder) >= sizeof(manifest))
		return false;
	FILE *f = fopen(manifest, "r");
	if (!f) return false;

	bool linked = false;
	char line[ABS_ID_MAX + 16];
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "item_id=", 8) != 0) continue;
		char *value = line + 8;
		value[strcspn(value, "\r\n")] = '\0';
		linked = strcmp(value, item_id) == 0;
		break;
	}
	fclose(f);
	return linked;
}

bool audiobookshelf_link_local(const audiobookshelf_book_t *book, const char *local_book,
								 char *file_out, size_t file_size, double *seconds_out,
								 bool *finished_out) {
	if (file_out && file_size) file_out[0] = '\0';
	if (seconds_out) *seconds_out = 0;
	if (finished_out) *finished_out = false;
	if (!book || !local_book || !local_book[0] || book->track_count <= 0 ||
		!file_out || file_size == 0 || book->item.duration <= 0) {
		set_error("This local book cannot be linked.");
		return false;
	}

	local_files_t files = {
		.paths = calloc((size_t)book->track_count, ABS_LOCAL_PATH_MAX),
		.capacity = book->track_count,
	};
	if (!files.paths) {
		set_error("There is not enough memory to link this book.");
		return false;
	}
	if (audiobookdb_is_folder_book(local_book))
		audiobookdb_parts_for_each(local_book, collect_local_file, &files);
	else {
		snprintf(files.paths[0], ABS_LOCAL_PATH_MAX, "%s", local_book);
		files.count = 1;
	}
	if (files.count != book->track_count) {
		free(files.paths);
		set_error("The local and server copies have different part counts.");
		return false;
	}

	double *durations = calloc((size_t)files.count, sizeof(*durations));
	if (!durations) {
		free(files.paths);
		set_error("There is not enough memory to link this book.");
		return false;
	}
	double total = 0;
	for (int i = 0; i < files.count; i++) {
		if (!local_file_duration(files.paths[i], &durations[i])) {
			free(durations);
			free(files.paths);
			set_error("A local part could not be measured.");
			return false;
		}
		total += durations[i];
	}
	double difference = total - book->item.duration;
	if (difference < 0) difference = -difference;
	double tolerance = 30.0 + book->item.duration * 0.001;
	if (difference > tolerance) {
		free(durations);
		free(files.paths);
		set_error("The local and server copies have different running times.");
		return false;
	}

	char folder[ABS_LOCAL_PATH_MAX];
	snprintf(folder, sizeof(folder), "%s", files.paths[0]);
	char *slash = strrchr(folder, '/');
	if (!slash) {
		free(durations);
		free(files.paths);
		set_error("The local audiobook path is invalid.");
		return false;
	}
	*slash = '\0';
	char manifest[ABS_LOCAL_PATH_MAX], temporary[ABS_TEMP_PATH_MAX];
	if ((size_t)snprintf(manifest, sizeof(manifest), "%s/.audiobookshelf", folder) >= sizeof(manifest) ||
		(size_t)snprintf(temporary, sizeof(temporary), "%s.tmp", manifest) >= sizeof(temporary)) {
		free(durations);
		free(files.paths);
		set_error("The local audiobook path is too long.");
		return false;
	}
	FILE *f = fopen(temporary, "w");
	if (!f) {
		free(durations);
		free(files.paths);
		set_error("The Audiobookshelf link could not be written.");
		return false;
	}
	fprintf(f, "version=1\nitem_id=%s\nserver=%s\nduration=%.3f\nprogress=%.3f\nprogress_updated=%lld\n",
			book->item.id, server_url, total, book->item.progress, book->progress_updated_ms);
	double start = 0;
	for (int i = 0; i < files.count; i++) {
		const char *base = strrchr(files.paths[i], '/');
		base = base ? base + 1 : files.paths[i];
		fprintf(f, "track=%d\t%.3f\t%.3f\t%s\n", i + 1, start, durations[i], base);
		start += durations[i];
	}
	bool written = fclose(f) == 0 && rename(temporary, manifest) == 0;
	if (!written) {
		remove(temporary);
		free(durations);
		free(files.paths);
		set_error("The Audiobookshelf link could not be written.");
		return false;
	}

	bool finished = book->item.finished;
	audiobookdb_abs_reset(local_book);
	double whole = finished ? 0 : book->item.progress;
	if (whole < 0) whole = 0;
	if (whole > total) whole = total;
	int selected = 0;
	start = 0;
	for (int i = 0; i < files.count; i++) {
		if (i + 1 < files.count && whole >= start + durations[i]) {
			start += durations[i];
			selected = i + 1;
			continue;
		}
		break;
	}
	snprintf(file_out, file_size, "%s", files.paths[selected]);
	if (seconds_out) *seconds_out = whole - start;
	if (finished_out) *finished_out = finished;
	free(durations);
	free(files.paths);
	set_error("");
	return true;
}

bool audiobookshelf_download(const audiobookshelf_book_t *book, const char *sd_root,
								 audiobookshelf_progress_cb cb, void *user, char *book_path, size_t path_size) {
	if (!book || !sd_root || !sd_root[0] || book->track_count <= 0) {
		set_error("No SD card is available for the download.");
		return false;
	}
	char author[160], title[220];
	safe_component(book->item.author[0] ? book->item.author : "Unknown Author", author, sizeof(author));
	safe_component(book->item.title, title, sizeof(title));
	char folder[900];
	if ((size_t)snprintf(folder, sizeof(folder), "%s/Audiobooks/Audiobookshelf/%s/%s", sd_root, author, title) >= sizeof(folder) ||
		!make_dirs(folder)) {
		set_error("The Audiobookshelf download folder could not be created.");
		return false;
	}

	for (int i = 0; i < book->track_count; i++) {
		const audiobookshelf_track_t *track = &book->tracks[i];
		char numbered[280];
		char destination[ABS_LOCAL_PATH_MAX];
		if (!local_track_name(track, i, numbered, sizeof(numbered)) ||
			(size_t)snprintf(destination, sizeof(destination), "%s/%s", folder, numbered) >= sizeof(destination)) {
			set_error("The audiobook download path is too long.");
			return false;
		}

		char encoded[ABS_URL_MAX * 2];
		encode_content_path(track->content_url, encoded, sizeof(encoded));
		char url[ABS_URL_MAX * 2 + 512];
		if (strncmp(track->content_url, "http://", 7) == 0 || strncmp(track->content_url, "https://", 8) == 0) {
			// Schemes need their colon intact. Audiobookshelf normally returns a
			// relative /s/item path; tolerate an absolute one without treating
			// its authority as a path segment.
			const char *path = strstr(track->content_url, "://");
			path = path ? strchr(path + 3, '/') : NULL;
			if (path) {
				char origin[512];
				size_t origin_len = (size_t)(path - track->content_url);
				if (origin_len >= sizeof(origin)) origin_len = sizeof(origin) - 1;
				memcpy(origin, track->content_url, origin_len);
				origin[origin_len] = '\0';
				encode_content_path(path, encoded, sizeof(encoded));
				snprintf(url, sizeof(url), "%s%s", origin, encoded);
			} else snprintf(url, sizeof(url), "%s", track->content_url);
		} else if (encoded[0] == '/') snprintf(url, sizeof(url), "%s%s", server_url, encoded);
		else snprintf(url, sizeof(url), "%s/%s", server_url, encoded);

		if (!download_file(url, destination, i + 1, book->track_count, cb, user) ||
			!write_sidecars(destination, book, track, i + 1) ||
			(i == 0 && !write_description(destination, book->description))) return false;
	}

	// Embedded artwork remains the first choice. Audiobookshelf's own cover is
	// a useful fallback for books whose source files carry none; asking for JPEG
	// keeps it compatible with the R1's folder-art decoder. A missing cover must
	// never turn a successful audio download into a failed book download.
	if (book->item.cover[0]) {
		char encoded[ABS_URL_MAX * 2];
		encode_content_path(book->item.cover, encoded, sizeof(encoded));
		char cover_url[ABS_URL_MAX * 2 + 544];
		if (encoded[0] == '/') snprintf(cover_url, sizeof(cover_url), "%s%s?format=jpeg", server_url, encoded);
		else snprintf(cover_url, sizeof(cover_url), "%s/%s?format=jpeg", server_url, encoded);
		char cover_path[ABS_LOCAL_PATH_MAX];
		if ((size_t)snprintf(cover_path, sizeof(cover_path), "%s/cover.jpg", folder) < sizeof(cover_path))
			(void)download_file(cover_url, cover_path, 1, 1, NULL, NULL);
	}

	char manifest[1000], temporary[1020];
	snprintf(manifest, sizeof(manifest), "%s/.audiobookshelf", folder);
	snprintf(temporary, sizeof(temporary), "%s.tmp", manifest);
	FILE *f = fopen(temporary, "w");
	if (f) {
		fprintf(f, "version=1\nitem_id=%s\nserver=%s\nduration=%.3f\nprogress=%.3f\nprogress_updated=%lld\n",
				book->item.id, server_url, book->item.duration, book->item.progress, book->progress_updated_ms);
		for (int i = 0; i < book->track_count; i++) {
			char local_name[280];
			if (!local_track_name(&book->tracks[i], i, local_name, sizeof(local_name))) continue;
			fprintf(f, "track=%d\t%.3f\t%.3f\t%s\n", i + 1, book->tracks[i].start_offset,
					book->tracks[i].duration, local_name);
		}
		fclose(f);
		rename(temporary, manifest);
	}
	if (book_path && path_size) snprintf(book_path, path_size, "%s", folder);
	audiobookdb_abs_reset(folder);
	set_error("");
	return true;
}
