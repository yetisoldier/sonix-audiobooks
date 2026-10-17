#define _GNU_SOURCE
#include "src/system/core/logging.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int reads, mode;

int __wrap_open64(const char *path, int flags, ...) {
	assert(strcmp(path, "/dev/kmsg") == 0);
	assert(flags & O_NONBLOCK);
	return 123;
}

off_t __wrap_lseek64(int fd, off_t offset, int whence) {
	assert(fd == 123 && offset == 0 && whence == SEEK_END);
	return 0;
}

ssize_t __wrap_read(int fd, void *buf, size_t size) {
	assert(fd == 123);
	assert(++reads <= 64);
	if (mode == EINTR || mode == EPIPE || mode == EAGAIN) {
		errno = mode;
		return -1;
	}
	if (mode == 0) return 0;
	const char *line = mode == 1 ? "4,123,4000,-;test warning\n" :
		mode == 2 ? "6,123,4000,-;filtered message\n" : "malformed record";
	assert(strlen(line) < size);
	memcpy(buf, line, strlen(line));
	return (ssize_t)strlen(line);
}

ssize_t __wrap___read_chk(int fd, void *buf, size_t size, size_t capacity) {
	assert(size <= capacity);
	return __wrap_read(fd, buf, size);
}

int main(void) {
	const int cases[] = {1, 2, 3, EINTR, EPIPE, EAGAIN, 0};
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		mode = cases[i];
		reads = 0;
		logging_follow_kernel();
		assert(reads == ((mode == 0 || mode == EAGAIN) ? 1 : 64));
	}
	puts("Kernel log tests passed: warning/info/malformed floods, EINTR, EPIPE, EAGAIN and EOF");
	return 0;
}
