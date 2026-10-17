#ifndef ABS_SYNC_H
#define ABS_SYNC_H

#include <stdbool.h>
#include <math.h>
#include <stdlib.h>

typedef struct {
	bool exists, finished;
	double seconds;
	long long updated_ms, server_now_ms;
} abs_remote_progress_t;

typedef enum { ABS_DECISION_HOLD, ABS_DECISION_EQUAL, ABS_DECISION_UPLOAD, ABS_DECISION_DOWNLOAD } abs_decision_t;

static inline bool abs_sync_clock_valid(long long updated_ms, const abs_remote_progress_t *remote, long long now_ms) {
	const long long earliest = 1577836800000LL;
	return remote->server_now_ms >= earliest && llabs(now_ms - remote->server_now_ms) <= 300000 &&
		updated_ms >= earliest && updated_ms <= remote->server_now_ms + 300000 &&
		(!remote->exists || (remote->updated_ms >= earliest && remote->updated_ms <= remote->server_now_ms + 300000));
}

// Event timestamps win, including a newer rewind. Unreliable clocks fall back
// to whole-book distance, never to the time the device finally reconnects.
static inline abs_decision_t abs_sync_decide(double local, bool finished, long long updated_ms,
	const abs_remote_progress_t *remote, long long now_ms) {
	if (remote->exists && finished == remote->finished && (finished || fabs(local - remote->seconds) <= 0.01))
		return ABS_DECISION_EQUAL;
	if (!remote->exists) return ABS_DECISION_UPLOAD;
	if (!abs_sync_clock_valid(updated_ms, remote, now_ms) || updated_ms == remote->updated_ms) {
		if (finished != remote->finished) return finished ? ABS_DECISION_UPLOAD : ABS_DECISION_DOWNLOAD;
		return local > remote->seconds ? ABS_DECISION_UPLOAD : ABS_DECISION_DOWNLOAD;
	}
	return updated_ms > remote->updated_ms ? ABS_DECISION_UPLOAD : ABS_DECISION_DOWNLOAD;
}

#endif
