#include "audiobookshelfpage.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/net/wifi.h"
#include "src/system/streaming/audiobookshelf.h"

#include <ctype.h>

#define ABS_LIBRARY_MAX 16
#define LOCAL_MATCH_WINDOW 8

lv_obj_t *audiobookshelf_screen;

static gui_config_t *config;
static lv_obj_t *content;
static lv_obj_t *page_title;
static lv_obj_t *setup_screen;
static lv_obj_t *server_field;
static lv_obj_t *token_field;
static lv_obj_t *token_eye_icon;
static keyboard_t *setup_keyboard;

typedef enum { VIEW_LIBRARIES = 0, VIEW_ITEMS } view_t;
static view_t view;
static char library_id[ABS_ID_MAX];
static char library_name[ABS_NAME_MAX];
static int item_page;

static audiobookshelf_library_t libraries[ABS_LIBRARY_MAX];
static int library_count;
static audiobookshelf_item_t items[ABS_PAGE_LIMIT];
static int item_count;
static int item_total;

typedef enum { JOB_NONE = 0, JOB_TEST, JOB_LIBRARIES, JOB_ITEMS, JOB_DOWNLOAD, JOB_LINK } job_kind_t;
typedef struct {
	job_kind_t kind;
	char id[ABS_ID_MAX];
	char path[1280];
	int page;
} job_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER;
static job_t pending;
static bool have_pending;
static bool worker_started;
static bool busy;

static job_kind_t result_kind;
static bool result_ok;
static char result_error[256];
static char downloaded_path[1024];
static char downloaded_resume_file[1280];
static double downloaded_resume_seconds;
static bool downloaded_finished;
static long long downloaded_updated_ms;
static char downloaded_server[512];
static lv_timer_t *resume_import_timer;
static int resume_import_attempts;
static int resume_scan_attempts;
static bool resume_scan_needed;

static int progress_percent;
static int progress_posted = -1;

static bool wifi_connected(void) {
#ifdef HOST_BUILD
	return true;
#else
	// Association snapshots and address ioctls both flap on some R1 builds
	// while the link is already carrying traffic. Only block a request when the
	// user has actually switched Wi-Fi off; the HTTP result is authoritative.
	return wifi_get_enabled();
#endif
}

static void show_note(const char *message) {
	lv_obj_t *note = lv_label_create(content);
	lv_label_set_text(note, tr(message));
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_obj_set_style_pad_ver(note, 22, 0);
}

static void open_setup_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(server_field, audiobookshelf_server());
	lv_textarea_set_text(token_field, "");
	lv_textarea_set_password_mode(token_field, true);
	lv_image_set_src(token_eye_icon, &icon_eye);
	keyboard_refresh_password(token_field);
	keyboard_reset(setup_keyboard);
	keyboard_set_field(setup_keyboard, server_field);
	lv_obj_add_state(server_field, LV_STATE_FOCUSED);
	lv_obj_remove_state(token_field, LV_STATE_FOCUSED);
	keyboard_show_caret(server_field, true);
	keyboard_show_caret(token_field, false);
	switch_screen(setup_screen);
}

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	if (!busy) {
		pending = *job;
		have_pending = true;
		busy = true;
		pthread_cond_signal(&job_cond);
	}
	pthread_mutex_unlock(&job_lock);
}

static void run_loading(const job_t *job) {
	if (busy) return;
	toast_busy("loading");
	submit(job);
}

static void library_clicked_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (busy || index < 0 || index >= library_count) return;
	snprintf(library_id, sizeof(library_id), "%s", libraries[index].id);
	snprintf(library_name, sizeof(library_name), "%s", libraries[index].name);
	item_page = 0;
	job_t job = {.kind = JOB_ITEMS, .page = 0};
	snprintf(job.id, sizeof(job.id), "%s", library_id);
	run_loading(&job);
}

static int selected_item = -1;
static char selected_local_book[1280];

static void normalized_name(const char *input, char *out, size_t size) {
	size_t written = 0;
	for (const unsigned char *p = (const unsigned char *)input; p && *p && written + 1 < size; p++) {
		if (*p >= 'A' && *p <= 'Z') out[written++] = (char)(*p - 'A' + 'a');
		else if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) out[written++] = (char)*p;
	}
	out[written] = '\0';
}

static void normalized_book_title(const char *input, char *out, size_t size) {
	char title[ABS_NAME_MAX];
	snprintf(title, sizeof(title), "%s", input ? input : "");
	size_t len = strlen(title);
	while (len && isspace((unsigned char)title[len - 1])) title[--len] = '\0';

	const char *extensions[] = {".mp3", ".m4b", ".m4a", ".flac", ".ogg", ".opus", ".aac", ".wav"};
	for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
		size_t ext_len = strlen(extensions[i]);
		if (len > ext_len && strcasecmp(title + len - ext_len, extensions[i]) == 0) {
			title[len -= ext_len] = '\0';
			break;
		}
	}

	// Plex-style audiobook names commonly append a series marker and year to
	// the album title. They are useful in filenames but are not part of the
	// Audiobookshelf title used for matching.
	for (;;) {
		while (len && isspace((unsigned char)title[len - 1])) title[--len] = '\0';
		if (len >= 2 && title[len - 1] == ']') {
			char *open = strrchr(title, '[');
			if (open) {
				len = (size_t)(open - title);
				title[len] = '\0';
				continue;
			}
		}
		if (len >= 6 && title[len - 1] == ')') {
			char *open = strrchr(title, '(');
			if (open && strlen(open) == 6 && isdigit((unsigned char)open[1]) &&
				isdigit((unsigned char)open[2]) && isdigit((unsigned char)open[3]) &&
				isdigit((unsigned char)open[4])) {
				len = (size_t)(open - title);
				title[len] = '\0';
				continue;
			}
		}
		break;
	}
	normalized_name(title, out, size);
}

typedef struct {
	const audiobookshelf_item_t *remote;
	char path[1280];
	int matches;
} local_match_t;

typedef struct {
	char paths[LOCAL_MATCH_WINDOW][1280];
	int count;
} local_window_t;

static bool count_local_part(const char *path, const char *title, void *user);

static bool collect_local_row(const char *name, const char *path, void *user) {
	(void)name;
	local_window_t *window = user;
	if (window->count >= LOCAL_MATCH_WINDOW) return false;
	snprintf(window->paths[window->count++], sizeof(window->paths[0]), "%s", path);
	return true;
}

static void match_local_path(local_match_t *match, const char *path) {
	char title[ABS_NAME_MAX], author[ABS_NAME_MAX];
	char local_title[ABS_NAME_MAX], remote_title[ABS_NAME_MAX];
	char local_author[ABS_NAME_MAX], remote_author[ABS_NAME_MAX];
	if (!audiobookdb_book_details(path, title, sizeof(title), author, sizeof(author), NULL, 0, NULL, 0))
		return;
	normalized_book_title(title, local_title, sizeof(local_title));
	normalized_book_title(match->remote->title, remote_title, sizeof(remote_title));
	normalized_name(author, local_author, sizeof(local_author));
	normalized_name(match->remote->author, remote_author, sizeof(remote_author));
	bool title_matches = strcasecmp(title, match->remote->title) == 0 ||
		(strlen(local_title) >= 8 && strcmp(local_title, remote_title) == 0);
	if (!title_matches) return;
	if (author[0] && match->remote->author[0]) {
		bool author_matches = strcasecmp(author, match->remote->author) == 0 ||
			(strlen(local_author) >= 4 && (strcmp(local_author, remote_author) == 0 ||
			 strstr(remote_author, local_author) != NULL || strstr(local_author, remote_author) != NULL));
		if (!author_matches) return;
	}
	int part_count = 0;
	int parts = audiobookdb_is_folder_book(path)
		? audiobookdb_parts_for_each(path, count_local_part, &part_count)
		: 1;
	if (match->remote->tracks > 0 && parts != match->remote->tracks) return;
	match->matches++;
	if (match->matches == 1) snprintf(match->path, sizeof(match->path), "%s", path);
	else match->path[0] = '\0';
}

static bool count_local_part(const char *path, const char *title, void *user) {
	(void)path;
	(void)title;
	int *count = user;
	(*count)++;
	return true;
}

static bool unique_local_match(const audiobookshelf_item_t *remote, char *path, size_t size) {
	local_match_t match = {.remote = remote};
	audiobookdb_index_t *index = audiobookdb_index_open(AUDIOBOOK_LIST_ALL, "", AUDIOBOOK_ORDER_NAME, false);
	if (!index) return false;
	int count = audiobookdb_index_count(index);
	for (int offset = 0; offset < count; offset += LOCAL_MATCH_WINDOW) {
		local_window_t window = {0};
		audiobookdb_index_window(index, offset,
								 count - offset > LOCAL_MATCH_WINDOW ? LOCAL_MATCH_WINDOW : count - offset,
								 collect_local_row, &window);
		for (int i = 0; i < window.count; i++) match_local_path(&match, window.paths[i]);
	}
	audiobookdb_index_close(index);
	if (match.matches != 1 || !match.path[0]) return false;
	snprintf(path, size, "%s", match.path);
	return true;
}

static void begin_download(void *user) {
	(void)user;
	if (selected_item < 0 || selected_item >= item_count || busy) return;
	job_t job = {.kind = JOB_DOWNLOAD};
	snprintf(job.id, sizeof(job.id), "%s", items[selected_item].id);
	progress_percent = 0;
	progress_posted = -1;
	gui_modal_show(&icon_menu_audiobooks, theme()->accent, "audiobookshelf_downloading", items[selected_item].title);
	gui_modal_progress(0);
	submit(&job);
}

static void begin_link(void *user) {
	(void)user;
	if (selected_item < 0 || selected_item >= item_count || !selected_local_book[0] || busy) return;
	job_t job = {.kind = JOB_LINK};
	snprintf(job.id, sizeof(job.id), "%s", items[selected_item].id);
	snprintf(job.path, sizeof(job.path), "%s", selected_local_book);
	run_loading(&job);
}

static void item_clicked_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (busy || index < 0 || index >= item_count) return;
	selected_item = index;
	selected_local_book[0] = '\0';
	char message[ABS_NAME_MAX + 80];
	if (unique_local_match(&items[index], selected_local_book, sizeof(selected_local_book))) {
		if (audiobookshelf_local_linked(selected_local_book, items[index].id)) {
			audiobooks_play_book(selected_local_book);
			return;
		}
		snprintf(message, sizeof(message), tr("audiobookshelf_link_question"), items[index].title);
		confirm_show("audiobookshelf_link", message, "link", begin_link, NULL);
		return;
	}
	snprintf(message, sizeof(message), tr("audiobookshelf_download_question"), items[index].title);
	confirm_show("audiobookshelf_download", message, "download", begin_download, NULL);
}

static void page_nav_cb(lv_event_t *e) {
	int page = (int)(intptr_t)lv_event_get_user_data(e);
	if (page < 0 || busy) return;
	job_t job = {.kind = JOB_ITEMS, .page = page};
	snprintf(job.id, sizeof(job.id), "%s", library_id);
	run_loading(&job);
}

static void draw_libraries(void) {
	view = VIEW_LIBRARIES;
	lv_label_set_text(page_title, tr("audiobookshelf"));
	lv_obj_clean(content);

	lv_obj_t *server_value = NULL;
	settingsrow_add(content, "audiobookshelf_server", &server_value, open_setup_cb, NULL);
	if (server_value) {
		lv_label_set_text(server_value,
						  audiobookshelf_server()[0] ? audiobookshelf_server() : tr("audiobookshelf_not_configured"));
		lv_label_set_long_mode(server_value, LV_LABEL_LONG_DOT);
		lv_obj_set_width(server_value, lv_pct(52));
		lv_obj_set_height(server_value, lv_font_get_line_height(&font_ui_20));
		lv_obj_set_style_text_font(server_value, &font_ui_20, 0);
	}

	if (!audiobookshelf_configured()) {
		show_note("audiobookshelf_setup_note");
		return;
	}
	if (!wifi_connected()) {
		show_note("enable_wi_fi_first");
		return;
	}
	if (library_count == 0) {
		show_note("audiobookshelf_no_libraries");
		return;
	}
	for (int i = 0; i < library_count; i++) {
		lv_obj_t *row = settingsrow_add(content, libraries[i].name, NULL, library_clicked_cb, (void *)(intptr_t)i);
		settingsrow_name_lines(row, 2);
	}
}

static void add_item_row(int index) {
	lv_obj_t *row = lv_btn_create(content);
	lv_obj_set_size(row, lv_pct(100), 112);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 20, 0);
	lv_obj_set_style_pad_ver(row, 10, 0);
	lv_obj_set_style_pad_row(row, 2, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_add_event_cb(row, item_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	lv_obj_t *title = lv_label_create(row);
	lv_label_set_text(title, items[index].title);
	lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(title, lv_pct(100));
	lv_obj_set_style_max_height(title, 2 * lv_font_get_line_height(&font_ui_22), 0);
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, &font_ui_22, 0);
	lv_obj_add_flag(title, LV_OBJ_FLAG_EVENT_BUBBLE);

	char detail[ABS_NAME_MAX + 32];
	if (items[index].progress > 0 && !items[index].finished && items[index].duration > 0)
		snprintf(detail, sizeof(detail), "%s  %.0f%%", items[index].author,
				 items[index].progress * 100.0 / items[index].duration);
	else snprintf(detail, sizeof(detail), "%s", items[index].author);
	lv_obj_t *subtitle = lv_label_create(row);
	lv_label_set_text(subtitle, detail);
	lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
	lv_obj_set_width(subtitle, lv_pct(100));
	lv_obj_set_height(subtitle, lv_font_get_line_height(&font_ui_18));
	lv_obj_add_style(subtitle, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(subtitle, &font_ui_18, 0);
	lv_obj_add_flag(subtitle, LV_OBJ_FLAG_EVENT_BUBBLE);
}

static void draw_items(void) {
	view = VIEW_ITEMS;
	lv_label_set_text(page_title, library_name[0] ? library_name : tr("audiobookshelf"));
	lv_obj_clean(content);
	if (item_page > 0) settingsrow_action(content, "audiobookshelf_previous_page", page_nav_cb,
										 (void *)(intptr_t)(item_page - 1));
	for (int i = 0; i < item_count; i++) add_item_row(i);
	if (item_count == 0) show_note("audiobookshelf_no_books");
	if ((item_page + 1) * ABS_PAGE_LIMIT < item_total)
		settingsrow_action(content, "audiobookshelf_next_page", page_nav_cb, (void *)(intptr_t)(item_page + 1));
}

static void resume_import_cb(lv_timer_t *timer) {
	(void)timer;
	if (audiobookdb_scan_running()) return;
	if (resume_scan_needed) {
		const char *root = config->sd_root_path;
		char scan_root[1024];
		if (root && root[0] &&
			(size_t)snprintf(scan_root, sizeof(scan_root), "%s/%s", root, AUDIOBOOKDB_FOLDER) < sizeof(scan_root) &&
			audiobookdb_scan_start(scan_root)) {
			resume_scan_needed = false;
			resume_scan_attempts++;
		}
		return;
	}

	char book[1280];
	if (audiobookdb_book_for_file(downloaded_resume_file, book, sizeof(book))) {
		audiobookdb_save_position(book, downloaded_resume_file, downloaded_resume_seconds);
		if (downloaded_finished) audiobookdb_mark_finished(book);
		audiobookdb_abs_checkpoint_t checkpoint;
		if (downloaded_updated_ms > 0 && audiobookdb_abs_get(book, &checkpoint))
			audiobookdb_abs_import(&checkpoint, downloaded_server, downloaded_resume_file,
				downloaded_resume_seconds, downloaded_finished, downloaded_updated_ms);
		printf("audiobookshelf: imported resume %.1f s into %s\n", downloaded_resume_seconds,
			   downloaded_resume_file);
		lv_timer_delete(resume_import_timer);
		resume_import_timer = NULL;
		return;
	}
	// A scan already in progress when the download completed may have taken
	// its directory snapshot before the new files were committed. One queued
	// follow-up scan closes that race without repeatedly rescanning the card.
	if (resume_scan_attempts < 2) resume_scan_needed = true;

	if (++resume_import_attempts >= 120) {
		fprintf(stderr, "audiobookshelf: downloaded book was not indexed for resume import\n");
		lv_timer_delete(resume_import_timer);
		resume_import_timer = NULL;
	}
}

static void start_resume_import(bool scan_first) {
	if (!downloaded_resume_file[0]) return;
	resume_import_attempts = 0;
	resume_scan_attempts = 0;
	resume_scan_needed = scan_first;
	if (resume_import_timer) lv_timer_delete(resume_import_timer);
	resume_import_timer = lv_timer_create(resume_import_cb, 500, NULL);
}

static bool back_guard(void) {
	if (view != VIEW_ITEMS) return false;
	draw_libraries();
	return true;
}

static void progress_async(void *user) {
	(void)user;
	gui_modal_progress(progress_percent);
}

static void download_progress(int track, int tracks, long long done, long long total, void *user) {
	(void)user;
	int within = total > 0 ? (int)(done * 100 / total) : 0;
	int overall = tracks > 0 ? ((track - 1) * 100 + within) / tracks : within;
	if (overall < 0) overall = 0;
	if (overall > 100) overall = 100;
	progress_percent = overall;
	if (overall != progress_posted) {
		progress_posted = overall;
		gui_post(progress_async, NULL);
	}
}

static void job_done_async(void *user) {
	(void)user;
	pthread_mutex_lock(&job_lock);
	busy = false;
	pthread_mutex_unlock(&job_lock);
	toast_busy_end();

	if (result_kind == JOB_DOWNLOAD) gui_modal_hide();
	if (!result_ok) {
		gui_notify_popup(result_error[0] ? result_error : tr("audiobookshelf_request_failed"));
		return;
	}

	switch (result_kind) {
	case JOB_TEST:
		if (lv_screen_active() == setup_screen) back_btn_cb(NULL);
		toast_success("audiobookshelf_connected");
		draw_libraries();
		break;
	case JOB_LIBRARIES:
		draw_libraries();
		break;
	case JOB_ITEMS:
		draw_items();
		break;
	case JOB_DOWNLOAD: {
		start_resume_import(true);
		toast_success("audiobookshelf_downloaded");
		break;
	}
	case JOB_LINK:
		start_resume_import(false);
		toast_success("audiobookshelf_linked");
		break;
	default:
		break;
	}
}

static void *worker_main(void *user) {
	(void)user;
	thread_be_background("audiobookshelf");
	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!have_pending) pthread_cond_wait(&job_cond, &job_lock);
		job_t job = pending;
		have_pending = false;
		pthread_mutex_unlock(&job_lock);

		result_kind = job.kind;
		result_ok = false;
		result_error[0] = '\0';
		if (job.kind == JOB_TEST || job.kind == JOB_LIBRARIES) {
			int count = audiobookshelf_libraries(libraries, ABS_LIBRARY_MAX);
			if (count >= 0) {
				library_count = count;
				result_ok = true;
			}
		} else if (job.kind == JOB_ITEMS) {
			int count = audiobookshelf_items(job.id, job.page, items, ABS_PAGE_LIMIT, &item_total);
			if (count >= 0) {
				item_count = count;
				item_page = job.page;
				result_ok = true;
			}
		} else if (job.kind == JOB_DOWNLOAD || job.kind == JOB_LINK) {
			audiobookshelf_book_t *book = calloc(1, sizeof(*book));
			if (!book) snprintf(result_error, sizeof(result_error), "%s", tr("out_of_memory"));
			else {
				downloaded_resume_file[0] = '\0';
				downloaded_resume_seconds = 0;
				downloaded_finished = false;
				downloaded_updated_ms = 0;
				snprintf(downloaded_server, sizeof(downloaded_server), "%s", audiobookshelf_server());
				if (job.kind == JOB_DOWNLOAD) {
					const char *root = config->sd_root_path;
					result_ok = root && root[0] && audiobookshelf_book(job.id, book) &&
						audiobookshelf_download(book, root, download_progress, NULL,
										 downloaded_path, sizeof(downloaded_path));
					if (result_ok)
						audiobookshelf_resume_target(book, downloaded_path, downloaded_resume_file,
											 sizeof(downloaded_resume_file), &downloaded_resume_seconds,
											 &downloaded_finished);
				} else {
					result_ok = audiobookshelf_book(job.id, book) &&
						audiobookshelf_link_local(book, job.path, downloaded_resume_file,
										 sizeof(downloaded_resume_file), &downloaded_resume_seconds,
										 &downloaded_finished);
					if (result_ok) {
						char local_file[1280];
						double local_seconds = 0;
						if (audiobookdb_get_position(job.path, local_file, sizeof(local_file), &local_seconds)) {
							audiobookshelf_queue_progress(job.path, local_file, local_seconds,
													 audiobookdb_is_finished(job.path), true);
							downloaded_resume_file[0] = '\0';
						}
					}
				}
				if (result_ok) downloaded_updated_ms = book->progress_updated_ms;
				free(book);
			}
		}
		if (!result_ok && !result_error[0])
			snprintf(result_error, sizeof(result_error), "%s", audiobookshelf_last_error());
		gui_post(job_done_async, NULL);
	}
	return NULL;
}

static void start_worker(void) {
	if (worker_started) return;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 192 * 1024);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_t thread;
	if (pthread_create(&thread, &attr, worker_main, NULL) == 0) worker_started = true;
	pthread_attr_destroy(&attr);
}

static void page_loaded_cb(lv_event_t *e) {
	(void)e;
	if (!audiobookshelf_configured() || !wifi_connected()) {
		draw_libraries();
		return;
	}
	job_t job = {.kind = JOB_LIBRARIES};
	run_loading(&job);
}

static void field_focus_cb(lv_event_t *e) {
	lv_obj_t *field = lv_event_get_target(e);
	lv_obj_t *other = field == server_field ? token_field : server_field;
	keyboard_set_field(setup_keyboard, field);
	lv_obj_add_state(field, LV_STATE_FOCUSED);
	lv_obj_remove_state(other, LV_STATE_FOCUSED);
	keyboard_show_caret(field, true);
	keyboard_show_caret(other, false);
}

static void token_eye_cb(lv_event_t *e) {
	(void)e;
	bool hidden = !lv_textarea_get_password_mode(token_field);
	lv_textarea_set_password_mode(token_field, hidden);
	lv_image_set_src(token_eye_icon, hidden ? &icon_eye : &icon_eye_off);
	keyboard_refresh_password(token_field);
}

static void setup_accept_cb(lv_event_t *e) {
	(void)e;
	const char *server = lv_textarea_get_text(server_field);
	const char *token = lv_textarea_get_text(token_field);
	if (!server || !server[0]) {
		toast_error("audiobookshelf_server_required");
		return;
	}
	// An empty token means keep the saved one while correcting only the URL.
	if ((!token || !token[0]) && !audiobookshelf_configured()) {
		toast_error("audiobookshelf_token_required");
		return;
	}
	if (!wifi_connected()) {
		toast_error("enable_wi_fi_first");
		return;
	}
	if (token && token[0]) audiobookshelf_set_credentials(server, token);
	else {
		// The API intentionally does not expose the saved token. Keep it by
		// changing only the server through the existing config value.
		const char *saved = config_get("audiobookshelf", "token", "");
		audiobookshelf_set_credentials(server, saved);
	}
	lv_textarea_set_text(token_field, "");
	job_t job = {.kind = JOB_TEST};
	run_loading(&job);
}

static lv_obj_t *make_field(lv_obj_t *parent, const char *placeholder, int y) {
	lv_obj_t *field = lv_textarea_create(parent);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_max_length(field, 1023);
	lv_textarea_set_placeholder_text(field, tr(placeholder));
	lv_obj_set_size(field, config->screen_width - 2 * config->padding, 62);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, config->padding, y);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, 12, 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, 14, 0);
	lv_obj_set_style_text_font(field, &font_ui_22, 0);
	keyboard_style_caret(field);
	lv_obj_add_event_cb(field, field_focus_cb, LV_EVENT_CLICKED, NULL);
	return field;
}

static void build_setup(void) {
	setup_screen = lv_obj_create(NULL);
	lv_obj_add_style(setup_screen, &theme_style_screen, 0);
	settingsrow_title(setup_screen, config, "audiobookshelf_setup");
	int top = settingsrow_content_top(config);
	server_field = make_field(setup_screen, "audiobookshelf_server_address", top);
	token_field = make_field(setup_screen, "audiobookshelf_access_token", top + 78);
	keyboard_style_password(token_field, 0);
	lv_obj_set_style_pad_right(token_field, 60, 0);

	lv_obj_t *eye = lv_btn_create(setup_screen);
	lv_obj_set_size(eye, 56, 56);
	lv_obj_align(eye, LV_ALIGN_TOP_RIGHT, -config->padding - 4, top + 81);
	lv_obj_set_style_bg_opa(eye, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(eye, 0, 0);
	lv_obj_set_style_border_width(eye, 0, 0);
	lv_obj_add_event_cb(eye, token_eye_cb, LV_EVENT_CLICKED, NULL);
	token_eye_icon = lv_image_create(eye);
	lv_obj_add_style(token_eye_icon, &theme_style_icon, 0);
	lv_image_set_src(token_eye_icon, &icon_eye);
	lv_obj_center(token_eye_icon);

	lv_obj_t *note = lv_label_create(setup_screen);
	lv_label_set_text(note, tr("audiobookshelf_token_note"));
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, config->screen_width - 2 * config->padding);
	lv_obj_align(note, LV_ALIGN_TOP_LEFT, config->padding, top + 154);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);

	setup_keyboard = keyboard_create(setup_screen, config->screen_width, 316, server_field, NULL, "connect",
									 setup_accept_cb, NULL);
	switcher_attach_back_gesture(setup_screen);
}

void audiobookshelfpage_init(gui_config_t *cfg) {
	config = cfg;
	audiobookshelf_screen = lv_obj_create(NULL);
	content = settingsrow_page(audiobookshelf_screen, cfg, "audiobookshelf");
	page_title = settingsrow_page_title(audiobookshelf_screen);
	build_setup();
	start_worker();
	draw_libraries();
	lv_obj_add_event_cb(audiobookshelf_screen, page_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_set_back_guard(audiobookshelf_screen, back_guard);
	switcher_attach_back_gesture(audiobookshelf_screen);
	player_sheet_attach_drag(audiobookshelf_screen, true);
	player_sheet_attach_drag(content, true);
}
