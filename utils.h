// SPDX-License-Identifier: MIT

#ifndef BUTTOND_UTILS_H
#define BUTTOND_UTILS_H

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define xassert(cond, fmt, args...) \
	do if (!(cond)) { \
		fprintf(stderr, "ERROR: " fmt "\n", ##args); \
		exit(EXIT_FAILURE); \
	} while (0)

static inline void *xcalloc(size_t nmemb, size_t size) {
	void *ptr = calloc(nmemb, size);
	xassert(ptr, "Allocation failure");
	return ptr;
}

static inline void *xreallocarray(void *ptr, size_t nmemb, size_t size) {
	ptr = realloc(ptr, nmemb * size);
	xassert(ptr, "Allocation failure");
	return ptr;
}

static inline ssize_t read_safe(int fd, void *buf, ssize_t count) {
	ssize_t total = 0;

	while (total < count) {
		ssize_t n;

		n = read(fd, (char*)buf + total, count - total);
		if (n < 0 && errno == EINTR)
			continue;
		else if (n < 0)
			return errno == EAGAIN ? total : -errno;
		else if (n == 0)
			break;
		total += n;
	}
	return total;
}

static inline uint16_t strtou16(const char *str) {
	char *endptr;
	long val;

	val = strtol(str, &endptr, 0);
	if (*endptr != 0) {
		errno = EINVAL;
		return 0;
	}
	if (val < 0 || val > 0xffff) {
		errno = ERANGE;
		return 0;
	}
	errno = 0;
	return val;
}

static inline uint32_t strtoint(const char *str) {
	char *endptr;
	long long val;

	val = strtoll(str, &endptr, 0);
	if (*endptr != 0) {
		errno = EINVAL;
		return 0;
	}
	if (val < 0 || val > UINT_MAX) {
		errno = ERANGE;
		return 0;
	}
	errno = 0;
	return val;
}

#endif
