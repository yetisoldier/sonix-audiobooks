#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

static void stop(int signal_number) {
	(void)signal_number;
	running = 0;
}

static int set_bit(int fd, unsigned long request, int bit, const char *name) {
	if (ioctl(fd, request, bit) == 0) {
		return 0;
	}
	fprintf(stderr, "%s failed: %s\n", name, strerror(errno));
	return -1;
}

int main(void) {
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "open /dev/uinput failed: %s\n", strerror(errno));
		return 1;
	}

	if (set_bit(fd, UI_SET_EVBIT, EV_ABS, "UI_SET_EVBIT") < 0 ||
		set_bit(fd, UI_SET_ABSBIT, ABS_X, "UI_SET_ABSBIT ABS_X") < 0 ||
		set_bit(fd, UI_SET_ABSBIT, ABS_Y, "UI_SET_ABSBIT ABS_Y") < 0) {
		close(fd);
		return 1;
	}

	struct uinput_user_dev device;
	memset(&device, 0, sizeof(device));
	snprintf(device.name, UINPUT_MAX_NAME_SIZE, "sonix-abs-test-holder");
	device.id.bustype = BUS_VIRTUAL;
	device.id.vendor = 0x534f;
	device.id.product = 0x4e58;
	device.id.version = 1;
	device.absmin[ABS_X] = 0;
	device.absmax[ABS_X] = 479;
	device.absmin[ABS_Y] = 0;
	device.absmax[ABS_Y] = 799;

	if (write(fd, &device, sizeof(device)) != (ssize_t)sizeof(device) ||
		ioctl(fd, UI_DEV_CREATE) < 0) {
		fprintf(stderr, "creating uinput device failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	signal(SIGINT, stop);
	signal(SIGTERM, stop);
	printf("absolute non-direct input device active\n");
	fflush(stdout);
	while (running) {
		pause();
	}

	ioctl(fd, UI_DEV_DESTROY);
	close(fd);
	return 0;
}
