#define _POSIX_C_SOURCE 199309L
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int debug = 0;

ssize_t read_full(int fd, void *buf, size_t count) {
	size_t total = 0;
	while (total < count) {
		ssize_t n;

		n = read(fd, (char*)buf + total, count - total);
		if (n < 0 && errno == EINTR)
			continue;
		else if (n < 0)
			return n;
		else if (n == 0)
			break;
		total += n;
	}
	return total;
}

#define NSECS_IN_SEC 1000000000L

/* time difference in nsecs */
static int64_t time_diff(struct timespec *tv1, struct timespec *tv2) {
	return tv1->tv_nsec - tv2->tv_nsec + (tv1->tv_sec - tv2->tv_sec) * NSECS_IN_SEC;
}

int main(int argc, char *argv[]) {
	// getopt command to run: long press; short press? double press?
	/* XXX config, look for gpio-keys in /sys/class/input/event*\/device/name ? */
	int fd = open("/dev/input/event1", O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Open /dev/input/event1 failed: %d\n", errno);
		exit(EXIT_FAILURE);
	}

	/* read events one at a time forever */
	struct input_event event;
	struct timespec ts_push = { 0 }, ts_release = { 0 };
	while (read_full(fd, &event, sizeof(event)) == sizeof(event)) {
		/* ignore non-keyboard events */
		if (event.type != 1)
			continue;
		/* ignore keys other than sw1 (KEY_1 in dtb) -- XXX config */
		if (event.code != KEY_1)
			continue;

		if (event.value == 1) {
			if (clock_gettime(CLOCK_MONOTONIC, &ts_push) < 0) {
				fprintf(stderr, "Could not get time: %d\n", errno);
				exit(EXIT_FAILURE);
			}
			if (debug)
				printf("sw1 pressed %ld.%03ld\n",
				       ts_push.tv_sec, ts_push.tv_nsec / 1000000);
		} else {
			if (clock_gettime(CLOCK_MONOTONIC, &ts_release) < 0) {
				fprintf(stderr, "Could not get time: %d\n", errno);
				exit(EXIT_FAILURE);
			}
			if (debug)
				printf("sw1 released %ld.%03ld\n",
				       ts_release.tv_sec, ts_release.tv_nsec / 1000000);
			if (time_diff(&ts_release, &ts_push) > 10 * NSECS_IN_SEC)
				printf("long press\n");
		}
	}
	fprintf(stderr, "read error or short read\n");
	exit(EXIT_FAILURE);
}
