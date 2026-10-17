#define _GNU_SOURCE
#include "src/system/core/logging.h"

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Upstream 1.3.0's kernel follower caps what a watchdog pass PRINTS
// (KMSG_LINES_PER_CALL, 20) and counts the rest in a summary line; the read
// loop itself drains the ring until EAGAIN or EOF, and retries through EINTR
// and EPIPE. The old fork's separate 64-read cap was dropped with the rebase
// (REBASE_PLAN.md §1 item 11), so this asserts upstream's own flood defense:
// at most 20 lines copied per call, one summary for the rest, and prompt exits
// on EAGAIN and EOF.
static int reads, mode;

#define RING_LINES 100
#define PRINT_CAP 20

// Only the kernel follower's /dev/kmsg is answered from the mock; the
// test's own plumbing (mkstemp's file, the stderr sink) reaches the real
// filesystem through the real open64 underneath the wrapper.
int __wrap_open64(const char *path, int flags, ...) {
	if (strcmp(path, "/dev/kmsg") != 0) {
		static int (*real_open64)(const char *, int, ...);
		if (!real_open64) {
			real_open64 = dlsym(RTLD_NEXT, "open64");
			assert(real_open64);
		}
		if (flags & O_CREAT) {
			va_list args;
			va_start(args, flags);
			int mode_arg = va_arg(args, int);
			va_end(args);
			return real_open64(path, flags, mode_arg);
		}
		return real_open64(path, flags);
	}
	assert(flags & O_NONBLOCK);
	return 123;
}

off_t __wrap_lseek64(int fd, off_t offset, int whence) {
	if (fd != 123) {
		static off_t (*real_lseek64)(int, off_t, int);
		if (!real_lseek64) {
			real_lseek64 = dlsym(RTLD_NEXT, "lseek64");
			assert(real_lseek64);
		}
		return real_lseek64(fd, offset, whence);
	}
	assert(offset == 0 && whence == SEEK_END);
	return 0;
}

ssize_t __wrap_read(int fd, void *buf, size_t size) {
	if (fd != 123) {
		static ssize_t (*real_read)(int, void *, size_t);
		if (!real_read) {
			real_read = dlsym(RTLD_NEXT, "read");
			assert(real_read);
		}
		return real_read(fd, buf, size);
	}
	assert(++reads <= 2 * RING_LINES + 8); // the mock's ring is never infinite
	if (mode == EINTR || mode == EPIPE) {
		if (reads <= 2) {
			errno = mode; // the overwrite/interrupt happens a couple of times...
			return -1;
		}
		errno = EAGAIN; // ...then the ring runs dry
		return -1;
	}
	if (mode == EAGAIN) {
		errno = EAGAIN;
		return -1;
	}
	if (mode == 0) {
		return 0;
	}
	const char *line = mode == 1 ? "4,123,4000,-;test warning\n" :
		mode == 2 ? "6,123,4000,-;filtered message\n" : "malformed record";
	assert(strlen(line) < size);
	memcpy(buf, line, strlen(line));
	if (reads > RING_LINES) { // the mock's ring is drained
		errno = EAGAIN;
		return -1;
	}
	return (ssize_t)strlen(line);
}

ssize_t __wrap___read_chk(int fd, void *buf, size_t size, size_t capacity) {
	assert(size <= capacity);
	return __wrap_read(fd, buf, size);
}

// Counts the "kernel: " lines one follow pass printed, through a redirected
// stderr: the read loop is bounded, but only the print cap bounds the output.
static int count_kernel_lines(void) {
	char path[] = "/tmp/sonix-kmsg-test.XXXXXX";
	int mark = mkstemp(path);
	assert(mark >= 0);
	close(mark);
	int saved = dup(2);
	assert(saved >= 0);
	int sink = open(path, O_WRONLY | O_TRUNC, 0644);
	assert(sink >= 0);
	assert(lseek(sink, 0, SEEK_SET) == 0);
	dup2(sink, 2);
	logging_follow_kernel();
	fflush(stderr);
	dup2(saved, 2);
	close(saved);
	close(sink);

	FILE *f = fopen(path, "r");
	assert(f);
	int lines = 0;
	char row[512];
	while (fgets(row, sizeof(row), f)) {
		if (strncmp(row, "kernel: ", 8) == 0) {
			lines++;
		}
	}
	fclose(f);
	unlink(path);
	return lines;
}

int main(void) {
	// A flood of warnings: the whole ring is drained, but only PRINT_CAP lines
	// are copied and one summary reports the rest.
	mode = 1;
	reads = 0;
	int lines = count_kernel_lines();
	assert(reads == RING_LINES + 1); // 100 records, then the EAGAIN that ends it
	assert(lines == PRINT_CAP + 1);  // 20 copies + "80 more lines not copied"

	// Informational records are filtered by level, so none of them print.
	mode = 2;
	reads = 0;
	lines = count_kernel_lines();
	assert(reads == RING_LINES + 1);
	assert(lines == 0);

	// Malformed records are skipped the same way.
	mode = 3;
	reads = 0;
	lines = count_kernel_lines();
	assert(reads == RING_LINES + 1);
	assert(lines == 0);

	// EINTR and EPIPE are retried, and the pass ends at the first EAGAIN.
	const int retry_cases[] = {EINTR, EPIPE};
	for (unsigned i = 0; i < sizeof(retry_cases) / sizeof(retry_cases[0]); i++) {
		mode = retry_cases[i];
		reads = 0;
		lines = count_kernel_lines();
		assert(reads == 3); // interrupted twice, then the EAGAIN
		assert(lines == 0);
	}

	// An empty ring answers EAGAIN at once.
	mode = EAGAIN;
	reads = 0;
	lines = count_kernel_lines();
	assert(reads == 1);
	assert(lines == 0);

	// EOF ends the pass the same way.
	mode = 0;
	reads = 0;
	lines = count_kernel_lines();
	assert(reads == 1);
	assert(lines == 0);

	puts("Kernel log tests passed: print-capped warning flood, filtered info, malformed records, EINTR, EPIPE, EAGAIN and EOF");
	return 0;
}