#include "devcontrol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/device/power.h"
#include "src/system/device/screenshot.h"

#define CONTROL_FIFO "/tmp/sonix-control"
#define STATUS_FILE "/tmp/sonix-control.status"
#define RX_CAPACITY 512
#define DEFAULT_TAP_MS 120
#define DEFAULT_SWIPE_MS 350
#define MAX_GESTURE_MS 10000
#define WAKE_SETTLE_MS 250

typedef struct {
	bool active;
	int from_x;
	int from_y;
	int to_x;
	int to_y;
	int last_x;
	int last_y;
	uint32_t starts_at;
	uint32_t duration_ms;
} virtual_gesture_t;

static int control_fd = -1;
static int panel_width;
static int panel_height;
static lv_display_t *control_display;
static lv_indev_t *virtual_pointer;
static virtual_gesture_t gesture;
static char rx_buffer[RX_CAPACITY];
static size_t rx_used;

static int clamp_coord(int value, int limit) {
	if (value < 0) {
		return 0;
	}
	if (value >= limit) {
		return limit - 1;
	}
	return value;
}

static uint32_t clamp_duration(int value, int fallback) {
	if (value <= 0) {
		value = fallback;
	}
	if (value > MAX_GESTURE_MS) {
		value = MAX_GESTURE_MS;
	}
	return (uint32_t)value;
}

static void write_status(const char *text) {
	FILE *f = fopen(STATUS_FILE ".new", "w");
	if (!f) {
		return;
	}
	fprintf(f, "%s\n", text);
	fclose(f);
	rename(STATUS_FILE ".new", STATUS_FILE);
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data) {
	(void)indev;

	int x = gesture.last_x;
	int y = gesture.last_y;
	data->state = LV_INDEV_STATE_RELEASED;

	if (gesture.active) {
		uint32_t now = lv_tick_get();
		int32_t since_start = (int32_t)(now - gesture.starts_at);
		if (since_start >= 0 && (uint32_t)since_start < gesture.duration_ms) {
			uint32_t elapsed = (uint32_t)since_start;
			x = gesture.from_x + (int)(((int64_t)(gesture.to_x - gesture.from_x) * elapsed) /
										  gesture.duration_ms);
			y = gesture.from_y + (int)(((int64_t)(gesture.to_y - gesture.from_y) * elapsed) /
										  gesture.duration_ms);
			data->state = LV_INDEV_STATE_PRESSED;
		} else if (since_start >= 0) {
			x = gesture.to_x;
			y = gesture.to_y;
			gesture.active = false;
		}
	}

	gesture.last_x = x;
	gesture.last_y = y;

	// Commands use what the framebuffer screenshot shows. LVGL itself stays in
	// its original coordinate system when the final framebuffer is rotated.
	if (display_get_rotated()) {
		x = panel_width - 1 - x;
		y = panel_height - 1 - y;
	}
	data->point.x = x;
	data->point.y = y;
}

static bool start_gesture(int x1, int y1, int x2, int y2, uint32_t duration_ms) {
	if (gesture.active) {
		write_status("error busy");
		return false;
	}

	x1 = clamp_coord(x1, panel_width);
	y1 = clamp_coord(y1, panel_height);
	x2 = clamp_coord(x2, panel_width);
	y2 = clamp_coord(y2, panel_height);

	uint32_t delay = 0;
	if (!power_screen_is_on()) {
		power_screen_on();
		delay = WAKE_SETTLE_MS;
	}
	power_notify_activity();
	if (control_display) {
		lv_display_trigger_activity(control_display);
	}

	gesture.from_x = x1;
	gesture.from_y = y1;
	gesture.to_x = x2;
	gesture.to_y = y2;
	gesture.last_x = x1;
	gesture.last_y = y1;
	gesture.starts_at = lv_tick_get() + delay;
	gesture.duration_ms = duration_ms;
	gesture.active = true;
	return true;
}

static void apply_command(char *line) {
	while (*line == ' ' || *line == '\t') {
		line++;
	}
	char *end = line + strlen(line);
	while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
		*--end = '\0';
	}

	int x = 0, y = 0, hold = DEFAULT_TAP_MS;
	int x2 = 0, y2 = 0, duration = DEFAULT_SWIPE_MS;
	char extra = '\0';
	char status[160];

	int fields = sscanf(line, "tap %d %d %d %c", &x, &y, &hold, &extra);
	if (fields >= 2 && fields <= 3) {
		uint32_t ms = clamp_duration(fields == 3 ? hold : DEFAULT_TAP_MS, DEFAULT_TAP_MS);
		if (start_gesture(x, y, x, y, ms)) {
			snprintf(status, sizeof(status), "ok tap %d %d %u", clamp_coord(x, panel_width),
					 clamp_coord(y, panel_height), ms);
			write_status(status);
		}
		return;
	}

	fields = sscanf(line, "swipe %d %d %d %d %d %c", &x, &y, &x2, &y2, &duration, &extra);
	if (fields >= 4 && fields <= 5) {
		uint32_t ms = clamp_duration(fields == 5 ? duration : DEFAULT_SWIPE_MS, DEFAULT_SWIPE_MS);
		if (start_gesture(x, y, x2, y2, ms)) {
			snprintf(status, sizeof(status), "ok swipe %d %d %d %d %u", clamp_coord(x, panel_width),
					 clamp_coord(y, panel_height), clamp_coord(x2, panel_width),
					 clamp_coord(y2, panel_height), ms);
			write_status(status);
		}
		return;
	}

	if (strcmp(line, "wake") == 0) {
		power_screen_on();
		power_notify_activity();
		if (control_display) {
			lv_display_trigger_activity(control_display);
		}
		write_status("ok wake");
		return;
	}

	if (strcmp(line, "screenshot") == 0 || strcmp(line, "shot") == 0) {
		screenshot_request();
		write_status("ok screenshot");
		return;
	}

	if (strcmp(line, "status") == 0 || strcmp(line, "ping") == 0) {
		snprintf(status, sizeof(status), "ok ready %dx%d screen=%s gesture=%s", panel_width, panel_height,
				 power_screen_is_on() ? "on" : "off", gesture.active ? "active" : "idle");
		write_status(status);
		return;
	}

	write_status("error usage: tap x y [ms] | swipe x1 y1 x2 y2 [ms] | wake | screenshot | status");
}

void devcontrol_init(lv_display_t *display, int width, int height) {
	control_display = display;
	panel_width = width > 0 ? width : 480;
	panel_height = height > 0 ? height : 800;
	gesture.last_x = panel_width / 2;
	gesture.last_y = panel_height / 2;

	virtual_pointer = lv_indev_create();
	if (!virtual_pointer) {
		fprintf(stderr, "devcontrol: cannot create virtual pointer\n");
		return;
	}
	lv_indev_set_type(virtual_pointer, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(virtual_pointer, pointer_read);
	lv_indev_set_display(virtual_pointer, display);

	unlink(CONTROL_FIFO);
	if (mkfifo(CONTROL_FIFO, 0600) != 0 && errno != EEXIST) {
		perror("devcontrol: mkfifo");
		return;
	}
	chmod(CONTROL_FIFO, 0600);
	control_fd = open(CONTROL_FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (control_fd < 0) {
		perror("devcontrol: open");
		return;
	}

	char status[96];
	snprintf(status, sizeof(status), "ok ready %dx%d screen=%s gesture=idle", panel_width, panel_height,
			 power_screen_is_on() ? "on" : "off");
	write_status(status);
	printf("devcontrol: %s ready for ADB UI automation\n", CONTROL_FIFO);
}

int devcontrol_fd(void) { return control_fd; }

void devcontrol_service(void) {
	if (control_fd < 0) {
		return;
	}

	for (;;) {
		ssize_t got = read(control_fd, rx_buffer + rx_used, sizeof(rx_buffer) - 1 - rx_used);
		if (got > 0) {
			rx_used += (size_t)got;
			rx_buffer[rx_used] = '\0';
		} else if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
			perror("devcontrol: read");
			return;
		} else {
			break;
		}

		if (rx_used == sizeof(rx_buffer) - 1 && !strchr(rx_buffer, '\n')) {
			rx_used = 0;
			write_status("error command too long");
		}
	}

	char *line = rx_buffer;
	char *newline = NULL;
	while ((newline = strchr(line, '\n')) != NULL) {
		*newline = '\0';
		if (line[0]) {
			apply_command(line);
		}
		line = newline + 1;
	}

	size_t remaining = rx_used - (size_t)(line - rx_buffer);
	memmove(rx_buffer, line, remaining);
	rx_used = remaining;
	rx_buffer[rx_used] = '\0';
}
