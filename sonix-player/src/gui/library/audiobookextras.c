#include "audiobookextras.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"

#define ROW_HEIGHT 88
#define ROW_GAP 8
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)
#define ROW_POOL 10
#define ICON_SIZE 42

lv_obj_t *audiobookfolders_screen;
lv_obj_t *audiobookmarks_screen;
lv_obj_t *audiobookmarklist_screen;
lv_obj_t *audiobooksummary_screen;

static gui_config_t *g_cfg;
static char audiobook_root[512];

typedef struct {
	lv_obj_t *button;
	lv_obj_t *icon;
	lv_obj_t *title;
	lv_obj_t *subtitle;
	lv_obj_t *action;
	int index;
} list_row_t;

static lv_obj_t *make_list(lv_obj_t *screen, gui_config_t *cfg, lv_obj_t **body_out, lv_event_cb_t scroll_cb) {
	int top = settingsrow_content_top(cfg);
	lv_obj_t *list = lv_obj_create(screen);
	lv_obj_set_size(list, cfg->screen_width, cfg->screen_height - top);
	lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_bg_opa(list, 0, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_radius(list, 0, 0);
	lv_obj_set_style_pad_hor(list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(list, 0, 0);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_add_event_cb(list, scroll_cb, LV_EVENT_SCROLL, NULL);

	lv_obj_t *body = lv_obj_create(list);
	lv_obj_set_width(body, lv_pct(100));
	lv_obj_set_height(body, 1);
	lv_obj_set_style_bg_opa(body, 0, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_radius(body, 0, 0);
	lv_obj_set_style_pad_all(body, 0, 0);
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(body, LV_OBJ_FLAG_EVENT_BUBBLE);
	*body_out = body;
	return list;
}

static void build_row(list_row_t *row, lv_obj_t *body, int width, lv_event_cb_t clicked) {
	row->button = lv_btn_create(body);
	lv_obj_set_size(row->button, width, ROW_HEIGHT);
	lv_obj_add_style(row->button, &theme_style_card, 0);
	lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row->button, 12, 0);
	lv_obj_set_style_border_width(row->button, 0, 0);
	lv_obj_set_style_shadow_width(row->button, 0, 0);
	lv_obj_set_style_pad_all(row->button, 14, 0);
	lv_obj_set_style_pad_column(row->button, 14, 0);
	lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_flag(row->button, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_add_flag(row->button, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(row->button, clicked, LV_EVENT_CLICKED, NULL);

	row->icon = lv_image_create(row->button);
	lv_obj_set_size(row->icon, ICON_SIZE, ICON_SIZE);
	lv_obj_add_style(row->icon, &theme_style_icon, 0);
	lv_obj_add_flag(row->icon, LV_OBJ_FLAG_EVENT_BUBBLE);

	lv_obj_t *text = lv_obj_create(row->button);
	lv_obj_set_height(text, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(text, 1);
	lv_obj_set_style_bg_opa(text, 0, 0);
	lv_obj_set_style_border_width(text, 0, 0);
	lv_obj_set_style_pad_all(text, 0, 0);
	lv_obj_set_style_pad_row(text, 2, 0);
	lv_obj_remove_flag(text, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(text, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);

	row->title = lv_label_create(text);
	lv_label_set_long_mode(row->title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(row->title, lv_pct(100));
	lv_obj_add_style(row->title, &theme_style_text, 0);
	lv_obj_set_style_text_font(row->title, &font_ui_22, 0);
	lv_obj_add_flag(row->title, LV_OBJ_FLAG_EVENT_BUBBLE);

	row->subtitle = lv_label_create(text);
	lv_label_set_long_mode(row->subtitle, LV_LABEL_LONG_DOT);
	lv_obj_set_width(row->subtitle, lv_pct(100));
	lv_obj_add_style(row->subtitle, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(row->subtitle, &font_ui_18, 0);
	lv_obj_add_flag(row->subtitle, LV_OBJ_FLAG_EVENT_BUBBLE);

	row->index = -1;
}

static int window_first(lv_obj_t *list, int count) {
	int first = lv_obj_get_scroll_y(list) / ROW_PITCH - 2;
	if (first < 0) first = 0;
	int max = count > ROW_POOL ? count - ROW_POOL : 0;
	if (first > max) first = max;
	return first;
}

// ---------------------------------------------------------------------------
// Indexed folder hierarchy
// ---------------------------------------------------------------------------

typedef struct {
	uint32_t name_at;
	uint32_t path_at;
	bool folder;
} folder_entry_t;

static folder_entry_t *folder_entries;
static int folder_count;
static int folder_capacity;
static char *folder_strings;
static size_t folder_strings_used;
static size_t folder_strings_capacity;
static char current_folder[768];
static lv_obj_t *folder_list;
static lv_obj_t *folder_body;
static lv_obj_t *folder_title;
static lv_obj_t *folder_empty;
static list_row_t folder_rows[ROW_POOL];

static void folder_data_clear(void) {
	free(folder_entries);
	free(folder_strings);
	folder_entries = NULL;
	folder_strings = NULL;
	folder_count = folder_capacity = 0;
	folder_strings_used = folder_strings_capacity = 0;
}

static bool folder_string_put(const char *text, uint32_t *at) {
	size_t len = strlen(text) + 1;
	if (folder_strings_used + len > folder_strings_capacity) {
		size_t grown = folder_strings_capacity ? folder_strings_capacity * 2 : 4096;
		while (grown < folder_strings_used + len) grown *= 2;
		char *bigger = realloc(folder_strings, grown);
		if (!bigger) return false;
		folder_strings = bigger;
		folder_strings_capacity = grown;
	}
	*at = (uint32_t)folder_strings_used;
	memcpy(folder_strings + folder_strings_used, text, len);
	folder_strings_used += len;
	return true;
}

static bool folder_add_cb(const char *name, const char *path, bool is_folder, void *user) {
	(void)user;
	if (folder_count == folder_capacity) {
		int grown = folder_capacity ? folder_capacity * 2 : 64;
		folder_entry_t *bigger = realloc(folder_entries, (size_t)grown * sizeof(*bigger));
		if (!bigger) return false;
		folder_entries = bigger;
		folder_capacity = grown;
	}
	folder_entry_t entry = {.folder = is_folder};
	if (!folder_string_put(name, &entry.name_at) || !folder_string_put(path, &entry.path_at)) return false;
	folder_entries[folder_count++] = entry;
	return true;
}

static void folder_window_update(void) {
	int first = window_first(folder_list, folder_count);
	for (int slot = 0; slot < ROW_POOL; slot++) {
		list_row_t *row = &folder_rows[slot];
		int index = first + slot;
		if (index >= folder_count) {
			row->index = -1;
			lv_obj_add_flag(row->button, LV_OBJ_FLAG_HIDDEN);
			continue;
		}
		folder_entry_t *entry = &folder_entries[index];
		row->index = index;
		lv_obj_set_y(row->button, index * ROW_PITCH);
		lv_label_set_text(row->title, folder_strings + entry->name_at);
		lv_label_set_text(row->subtitle, tr(entry->folder ? "audiobook_folder" : "audiobook_book"));
		lv_image_set_src(row->icon, entry->folder ? &icon_folder : &icon_book_headphones);
		lv_obj_remove_flag(row->button, LV_OBJ_FLAG_HIDDEN);
	}
}

static void folder_reload(void) {
	folder_data_clear();
	audiobookdb_folder_entries_for_each(current_folder, folder_add_cb, NULL);
	lv_obj_set_height(folder_body, folder_count ? folder_count * ROW_PITCH : 1);
	lv_obj_scroll_to_y(folder_list, 0, LV_ANIM_OFF);
	if (strcmp(current_folder, audiobook_root) == 0) {
		lv_label_set_text(folder_title, tr("audiobook_folders"));
	} else {
		const char *name = strrchr(current_folder, '/');
		lv_label_set_text(folder_title, name ? name + 1 : tr("audiobook_folders"));
	}
	if (folder_count == 0) lv_obj_remove_flag(folder_empty, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_add_flag(folder_empty, LV_OBJ_FLAG_HIDDEN);
	folder_window_update();
}

static void folder_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) return;
	lv_obj_t *button = lv_event_get_current_target(e);
	for (int i = 0; i < ROW_POOL; i++) {
		if (folder_rows[i].button != button) continue;
		int index = folder_rows[i].index;
		if (index < 0 || index >= folder_count) return;
		folder_entry_t *entry = &folder_entries[index];
		const char *path = folder_strings + entry->path_at;
		if (entry->folder) {
			snprintf(current_folder, sizeof(current_folder), "%s", path);
			folder_reload();
		} else {
			audiobooks_play_book(path);
		}
		return;
	}
}

static void folder_scroll_cb(lv_event_t *e) { (void)e; folder_window_update(); }

static bool folder_back(void) {
	if (strcmp(current_folder, audiobook_root) == 0) return false;
	char *slash = strrchr(current_folder, '/');
	if (!slash || slash < current_folder + strlen(audiobook_root)) snprintf(current_folder, sizeof(current_folder), "%s", audiobook_root);
	else *slash = '\0';
	folder_reload();
	return true;
}

static bool folder_back_peek(void) { return strcmp(current_folder, audiobook_root) != 0; }

void audiobookfolders_open(void) {
	snprintf(current_folder, sizeof(current_folder), "%s", audiobook_root);
	folder_reload();
	switch_screen(audiobookfolders_screen);
}

// ---------------------------------------------------------------------------
// Manual bookmarks: a shelf of books, then the marks in one book
// ---------------------------------------------------------------------------

typedef struct {
	char title[256];
	char book[512];
	int count;
} marked_book_t;

typedef struct {
	int64_t id;
	char file[512];
	char label[192];
	double seconds;
} mark_t;

static marked_book_t *marked_books;
static int marked_book_count;
static mark_t marks[AUDIOBOOK_BOOKMARKS_PER_BOOK];
static int mark_count;
static char marked_book_path[512];
static char marked_book_title[256];

static lv_obj_t *marked_books_list;
static lv_obj_t *marked_books_body;
static lv_obj_t *marked_books_empty;
static list_row_t marked_book_rows[ROW_POOL];
static lv_obj_t *marks_list;
static lv_obj_t *marks_body;
static lv_obj_t *marks_empty;
static lv_obj_t *marks_title;
static list_row_t mark_rows[ROW_POOL];

static bool marked_book_add_cb(const char *title, const char *book, int count, void *user) {
	(void)user;
	marked_book_t *grown = realloc(marked_books, (size_t)(marked_book_count + 1) * sizeof(*grown));
	if (!grown) return false;
	marked_books = grown;
	marked_book_t *entry = &marked_books[marked_book_count++];
	snprintf(entry->title, sizeof(entry->title), "%s", title);
	snprintf(entry->book, sizeof(entry->book), "%s", book);
	entry->count = count;
	return true;
}

static void marked_books_window_update(void) {
	int first = window_first(marked_books_list, marked_book_count);
	for (int slot = 0; slot < ROW_POOL; slot++) {
		list_row_t *row = &marked_book_rows[slot];
		int index = first + slot;
		if (index >= marked_book_count) {
			row->index = -1;
			lv_obj_add_flag(row->button, LV_OBJ_FLAG_HIDDEN);
			continue;
		}
		row->index = index;
		lv_obj_set_y(row->button, index * ROW_PITCH);
		lv_label_set_text(row->title, marked_books[index].title);
		lv_label_set_text_fmt(row->subtitle, "%d %s", marked_books[index].count,
			tr(marked_books[index].count == 1 ? "audiobook_bookmark" : "audiobook_bookmarks"));
		lv_image_set_src(row->icon, &icon_bookmark);
		lv_obj_remove_flag(row->button, LV_OBJ_FLAG_HIDDEN);
	}
}

static void marked_books_reload(void) {
	free(marked_books);
	marked_books = NULL;
	marked_book_count = 0;
	audiobookdb_bookmarked_books_for_each(marked_book_add_cb, NULL);
	lv_obj_set_height(marked_books_body, marked_book_count ? marked_book_count * ROW_PITCH : 1);
	lv_obj_scroll_to_y(marked_books_list, 0, LV_ANIM_OFF);
	if (marked_book_count == 0) lv_obj_remove_flag(marked_books_empty, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_add_flag(marked_books_empty, LV_OBJ_FLAG_HIDDEN);
	marked_books_window_update();
}

static bool mark_add_cb(int64_t id, const char *file, double seconds, const char *label, int64_t created_at, void *user) {
	(void)created_at;
	(void)user;
	if (mark_count >= AUDIOBOOK_BOOKMARKS_PER_BOOK) return false;
	mark_t *mark = &marks[mark_count++];
	mark->id = id;
	mark->seconds = seconds;
	snprintf(mark->file, sizeof(mark->file), "%s", file);
	snprintf(mark->label, sizeof(mark->label), "%s", label);
	return true;
}

static void time_text(double seconds, char *out, size_t size) {
	int total = seconds > 0 ? (int)seconds : 0;
	int hours = total / 3600;
	int mins = (total / 60) % 60;
	int secs = total % 60;
	if (hours) snprintf(out, size, "%d:%02d:%02d", hours, mins, secs);
	else snprintf(out, size, "%d:%02d", mins, secs);
}

static void marks_window_update(void) {
	int first = window_first(marks_list, mark_count);
	for (int slot = 0; slot < ROW_POOL; slot++) {
		list_row_t *row = &mark_rows[slot];
		int index = first + slot;
		if (index >= mark_count) {
			row->index = -1;
			lv_obj_add_flag(row->button, LV_OBJ_FLAG_HIDDEN);
			continue;
		}
		char when[32];
		time_text(marks[index].seconds, when, sizeof(when));
		row->index = index;
		lv_obj_set_y(row->button, index * ROW_PITCH);
		lv_label_set_text(row->title, marks[index].label[0] ? marks[index].label : tr("audiobook_bookmark"));
		lv_label_set_text(row->subtitle, when);
		lv_image_set_src(row->icon, &icon_bookmark_check);
		lv_obj_remove_flag(row->button, LV_OBJ_FLAG_HIDDEN);
	}
}

static void marks_reload(void) {
	mark_count = 0;
	audiobookdb_bookmarks_for_each(marked_book_path, mark_add_cb, NULL);
	lv_obj_set_height(marks_body, mark_count ? mark_count * ROW_PITCH : 1);
	lv_obj_scroll_to_y(marks_list, 0, LV_ANIM_OFF);
	lv_label_set_text(marks_title, marked_book_title);
	if (mark_count == 0) lv_obj_remove_flag(marks_empty, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_add_flag(marks_empty, LV_OBJ_FLAG_HIDDEN);
	marks_window_update();
}

static void open_marked_book(int index, bool from_player) {
	if (index < 0 || index >= marked_book_count) return;
	snprintf(marked_book_path, sizeof(marked_book_path), "%s", marked_books[index].book);
	snprintf(marked_book_title, sizeof(marked_book_title), "%s", marked_books[index].title);
	marks_reload();
	if (from_player) player_sheet_close(false);
	switch_screen(audiobookmarklist_screen);
	if (from_player) switcher_set_player_return(audiobookmarklist_screen);
}

static void marked_book_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) return;
	lv_obj_t *button = lv_event_get_current_target(e);
	for (int i = 0; i < ROW_POOL; i++) if (marked_book_rows[i].button == button) open_marked_book(marked_book_rows[i].index, false);
}

static void mark_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) return;
	lv_obj_t *button = lv_event_get_current_target(e);
	for (int i = 0; i < ROW_POOL; i++) {
		if (mark_rows[i].button != button) continue;
		int index = mark_rows[i].index;
		if (index >= 0 && index < mark_count) audiobooks_play_book_at(marked_book_path, marks[index].file, marks[index].seconds);
		return;
	}
}

static void mark_delete_cb(lv_event_t *e) {
	lv_obj_t *button = lv_event_get_current_target(e);
	for (int i = 0; i < ROW_POOL; i++) {
		if (mark_rows[i].action != button) continue;
		int index = mark_rows[i].index;
		if (index >= 0 && index < mark_count && audiobookdb_bookmark_remove(marks[index].id)) {
			toast_plain("audiobook_bookmark_deleted");
			marks_reload();
		}
		return;
	}
}

static void marked_books_scroll_cb(lv_event_t *e) { (void)e; marked_books_window_update(); }
static void marks_scroll_cb(lv_event_t *e) { (void)e; marks_window_update(); }
static void marked_books_loaded_cb(lv_event_t *e) { (void)e; marked_books_reload(); }
static void marks_loaded_cb(lv_event_t *e) { (void)e; marks_reload(); }

void audiobookmarks_open(void) {
	marked_books_reload();
	switch_screen(audiobookmarks_screen);
}

static bool current_book(char *book, size_t size, device_state_t *state) {
	device_state_get(state);
	return state->current_file[0] && audiobookdb_book_for_file(state->current_file, book, size);
}

void audiobookmarks_open_current(void) {
	device_state_t state;
	char book[512];
	if (!current_book(book, sizeof(book), &state)) {
		gui_notify_popup("audiobook_no_book_playing");
		return;
	}
	char title[256];
	audiobookdb_book_details(book, title, sizeof(title), NULL, 0, NULL, 0, NULL, 0);
	snprintf(marked_book_path, sizeof(marked_book_path), "%s", book);
	snprintf(marked_book_title, sizeof(marked_book_title), "%s", title);
	marks_reload();
	player_sheet_close(false);
	switch_screen(audiobookmarklist_screen);
	switcher_set_player_return(audiobookmarklist_screen);
}

void audiobookmarks_add_current(void) {
	device_state_t state;
	char book[512];
	if (!current_book(book, sizeof(book), &state)) {
		gui_notify_popup("audiobook_no_book_playing");
		return;
	}
	char label[192] = "";
	int chapter = audiobook_chapter_at(state.progress_current_secs);
	if (chapter >= 0) audiobook_chapter(chapter, label, sizeof(label), NULL);
	if (!label[0]) {
		int part = audiobook_part_current();
		if (part >= 0) audiobook_part(part, label, sizeof(label), NULL, 0);
	}
	if (!label[0]) snprintf(label, sizeof(label), "%.*s", (int)sizeof(label) - 1, state.metadata.title);
	if (audiobookdb_bookmark_add(book, state.current_file, state.progress_current_secs, label)) {
		toast_glyph(&icon_bookmark_check, "audiobook_bookmark_added");
	} else {
		toast_error("audiobook_bookmark_limit");
	}
}

// ---------------------------------------------------------------------------
// Description / summary page
// ---------------------------------------------------------------------------

static lv_obj_t *summary_book;
static lv_obj_t *summary_byline;
static lv_obj_t *summary_text;
static char summary_buffer[8193];

void audiobooksummary_open(const char *book, bool from_player) {
	char title[256] = "";
	char author[256] = "";
	char series[256] = "";
	summary_buffer[0] = '\0';
	if (!audiobookdb_book_details(book, title, sizeof(title), author, sizeof(author), series, sizeof(series),
								  summary_buffer, sizeof(summary_buffer))) {
		return;
	}
	lv_label_set_text(summary_book, title);
	char byline[540];
	if (author[0] && series[0]) snprintf(byline, sizeof(byline), "%s\n%s", author, series);
	else snprintf(byline, sizeof(byline), "%s", author[0] ? author : series);
	lv_label_set_text(summary_byline, byline);
	lv_label_set_text(summary_text, summary_buffer[0] ? summary_buffer : tr("audiobook_summary_empty"));
	if (from_player) player_sheet_close(false);
	switch_screen(audiobooksummary_screen);
	if (from_player) switcher_set_player_return(audiobooksummary_screen);
}

void audiobooksummary_open_current(void) {
	device_state_t state;
	char book[512];
	if (!current_book(book, sizeof(book), &state)) {
		gui_notify_popup("audiobook_no_book_playing");
		return;
	}
	audiobooksummary_open(book, true);
}

// ---------------------------------------------------------------------------

static lv_obj_t *empty_label(lv_obj_t *parent, const char *key) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, tr(key));
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, g_cfg->screen_width - 4 * g_cfg->padding);
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(label, LV_ALIGN_CENTER, 0, -20);
	lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
	return label;
}

void audiobookextras_init(gui_config_t *cfg) {
	g_cfg = cfg;
	snprintf(audiobook_root, sizeof(audiobook_root), "%s/%s", cfg->sd_root_path ? cfg->sd_root_path : "", AUDIOBOOKDB_FOLDER);

	lv_obj_add_style(audiobookfolders_screen, &theme_style_screen, 0);
	folder_title = settingsrow_title(audiobookfolders_screen, cfg, "audiobook_folders");
	folder_list = make_list(audiobookfolders_screen, cfg, &folder_body, folder_scroll_cb);
	folder_empty = empty_label(audiobookfolders_screen, "audiobook_folders_empty");
	for (int i = 0; i < ROW_POOL; i++) build_row(&folder_rows[i], folder_body, cfg->screen_width - 2 * cfg->padding, folder_clicked_cb);
	switcher_set_back_guard(audiobookfolders_screen, folder_back);
	switcher_set_back_guard_peek(audiobookfolders_screen, folder_back_peek);
	switcher_attach_back_gesture(folder_list);
	player_sheet_attach_drag(audiobookfolders_screen, true);

	lv_obj_add_style(audiobookmarks_screen, &theme_style_screen, 0);
	settingsrow_title(audiobookmarks_screen, cfg, "audiobook_bookmarks");
	marked_books_list = make_list(audiobookmarks_screen, cfg, &marked_books_body, marked_books_scroll_cb);
	marked_books_empty = empty_label(audiobookmarks_screen, "audiobook_bookmarks_empty");
	for (int i = 0; i < ROW_POOL; i++) build_row(&marked_book_rows[i], marked_books_body, cfg->screen_width - 2 * cfg->padding, marked_book_clicked_cb);
	lv_obj_add_event_cb(audiobookmarks_screen, marked_books_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(marked_books_list);
	player_sheet_attach_drag(audiobookmarks_screen, true);

	lv_obj_add_style(audiobookmarklist_screen, &theme_style_screen, 0);
	marks_title = settingsrow_title(audiobookmarklist_screen, cfg, "audiobook_bookmarks");
	marks_list = make_list(audiobookmarklist_screen, cfg, &marks_body, marks_scroll_cb);
	marks_empty = empty_label(audiobookmarklist_screen, "audiobook_bookmarks_book_empty");
	for (int i = 0; i < ROW_POOL; i++) {
		build_row(&mark_rows[i], marks_body, cfg->screen_width - 2 * cfg->padding, mark_clicked_cb);
		mark_rows[i].action = lv_btn_create(mark_rows[i].button);
		lv_obj_set_size(mark_rows[i].action, 48, 48);
		lv_obj_set_style_bg_opa(mark_rows[i].action, 0, 0);
		lv_obj_set_style_shadow_width(mark_rows[i].action, 0, 0);
		lv_obj_add_event_cb(mark_rows[i].action, mark_delete_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_t *trash = lv_image_create(mark_rows[i].action);
		lv_image_set_src(trash, &icon_trash);
		lv_obj_add_style(trash, &theme_style_icon, 0);
		lv_obj_center(trash);
	}
	lv_obj_add_event_cb(audiobookmarklist_screen, marks_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(marks_list);

	lv_obj_add_style(audiobooksummary_screen, &theme_style_screen, 0);
	settingsrow_title(audiobooksummary_screen, cfg, "audiobook_summary");
	int top = settingsrow_content_top(cfg);
	lv_obj_t *summary_scroll = lv_obj_create(audiobooksummary_screen);
	lv_obj_set_size(summary_scroll, cfg->screen_width, cfg->screen_height - top);
	lv_obj_align(summary_scroll, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_bg_opa(summary_scroll, 0, 0);
	lv_obj_set_style_border_width(summary_scroll, 0, 0);
	lv_obj_set_style_radius(summary_scroll, 0, 0);
	lv_obj_set_style_pad_hor(summary_scroll, cfg->padding * 2, 0);
	lv_obj_set_style_pad_ver(summary_scroll, cfg->padding, 0);
	lv_obj_set_style_pad_row(summary_scroll, 12, 0);
	lv_obj_set_flex_flow(summary_scroll, LV_FLEX_FLOW_COLUMN);
	summary_book = lv_label_create(summary_scroll);
	lv_label_set_long_mode(summary_book, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(summary_book, lv_pct(100));
	lv_obj_add_style(summary_book, &theme_style_text, 0);
	lv_obj_set_style_text_font(summary_book, &font_ui_28, 0);
	summary_byline = lv_label_create(summary_scroll);
	lv_label_set_long_mode(summary_byline, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(summary_byline, lv_pct(100));
	lv_obj_add_style(summary_byline, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(summary_byline, &font_ui_20, 0);
	summary_text = lv_label_create(summary_scroll);
	lv_label_set_long_mode(summary_text, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(summary_text, lv_pct(100));
	lv_obj_add_style(summary_text, &theme_style_text, 0);
	lv_obj_set_style_text_font(summary_text, &font_ui_22, 0);
	switcher_attach_back_gesture(summary_scroll);
}
