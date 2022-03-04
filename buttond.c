// SPDX-License-Identifier: MIT
/*
 * Handle evdev button press
 * Copyright (c) 2021 Atmark Techno,Inc.
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define xassert(cond, fmt, args...) \
	if (!(cond)) { \
		fprintf(stderr, "ERROR: "fmt"\n", ##args); \
		exit(EXIT_FAILURE); \
	}

/* debug:
 * -v (> 0/set): info message e.g. registered key presses
 * -vv (> 1): ignored keys also printed
 * -vvv (> 2): add non-keyboard events and file names
 * -vvvv (> 3): add timeout/wakeup related debugs
 */
static int debug = 0;
static int test_mode = 0;
#define DEFAULT_LONG_PRESS_MSECS 5000
#define DEFAULT_SHORT_PRESS_MSECS 1000
#define DEBOUNCE_MSECS 10

ssize_t read_safe(int fd, void *buf, ssize_t count) {
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

#define NSECS_IN_SEC  1000000000L
#define NSECS_IN_MSEC 1000000L
#define NSECS_IN_USEC 1000L
#define USECS_IN_SEC  1000000L
#define USECS_IN_MSEC 1000L

/* time difference in msecs */
static int64_t time_diff_ts(struct timespec *ts1, struct timespec *ts2) {
	return (ts1->tv_nsec - ts2->tv_nsec) / NSECS_IN_MSEC
		+ (ts1->tv_sec - ts2->tv_sec) * 1000;
}

static int64_t time_diff_tv(struct timeval *tv1, struct timeval *tv2) {
	return (tv1->tv_usec - tv2->tv_usec) / USECS_IN_MSEC
		+ (tv1->tv_sec - tv2->tv_sec) * 1000;
}

/* add number of msec to given timespec */
static void time_add_ts(struct timespec *ts, int msec) {
	ts->tv_nsec += msec * NSECS_IN_MSEC;
	if (ts->tv_nsec >= NSECS_IN_SEC) {
		ts->tv_sec += ts->tv_nsec / NSECS_IN_SEC;
		ts->tv_nsec = ts->tv_nsec / NSECS_IN_SEC;
	}
}

/* convert timeval to timespec and add offset msec */
static void time_tv2ts(struct timespec *ts, struct timeval *base, int msec) {
	ts->tv_nsec = base->tv_usec * NSECS_IN_USEC + msec * NSECS_IN_MSEC;
	ts->tv_sec = base->tv_sec + ts->tv_nsec / NSECS_IN_SEC;
	ts->tv_nsec %= NSECS_IN_SEC;
}
/* convert timespec to timeval and add offset msec */
static void time_ts2tv(struct timeval *tv, struct timespec *base, int msec) {
	tv->tv_usec = base->tv_nsec / NSECS_IN_USEC + msec * USECS_IN_MSEC;
	tv->tv_sec = base->tv_sec + tv->tv_usec / USECS_IN_SEC;
	tv->tv_usec %= USECS_IN_SEC;
}

#define OPT_TEST 257

static struct option long_options[] = {
	{"input",	required_argument,	0, 'i' },
	{"short",	required_argument,	0, 's' },
	{"long",	required_argument,	0, 'l' },
	{"action",	required_argument,	0, 'a' },
	{"time",	required_argument,	0, 't' },
	{"verbose",	no_argument,		0, 'v' },
	{"version",	no_argument,		0, 'V' },
	{"help",	no_argument,		0, 'h' },
	{"test_mode",	no_argument,		0, OPT_TEST },
	{0,		0,			0,  0  }
};

static void version(char *argv0) {
	printf("%s version 0.1\n", argv0);
}

static void help(char *argv0) {
	printf("Usage: %s [options]\n", argv0);
	printf("Options:\n");
	printf("  -i, --input <file>: file to get event from e.g. /dev/input/event2\n");
	printf("                      pass multiple times to monitor multiple files\n");
	printf("  -I <file>: same as -i, except if file disappears wait for it to come back\n");
	printf("  -s/--short <key>  [-t/--time <time ms>] -a/--action <command>: action on short key press\n");
	printf("  -l/--long <key> [-t/--time <time ms>] -a/--action <command>: action on long key press\n");
	printf("  -h, --help: show this help\n");
	printf("  -V, --version: show version\n");
	printf("  -v, --verbose: verbose (repeatable)\n\n");

	printf("<key> code can be found in uapi/linux/input-event-code.h or by running\n");
	printf("with -vv\n\n");

	printf("Semantics: a short press action happens on release, if and only if\n");
	printf("the button was released before <time> (default %d) milliseconds.\n",
	       DEFAULT_SHORT_PRESS_MSECS);
	printf("a long press action happens even if key is still pressed, if it has been\n");
	printf("held for at least <time> (default %d) milliseconds.\n\n",
	       DEFAULT_LONG_PRESS_MSECS);

	printf("Note some keyboards have repeat built in firmware so quick repetitions\n");
	printf("(<%dms) are handled as if key were pressed continuously\n",
	       DEBOUNCE_MSECS);
}

struct action {
	/* type of action (long/short press) */
	enum type {
		LONG_PRESS,
		SHORT_PRESS,
	} type;
	/* cutoff time for action */
	int trigger_time;
	/* command to run */
	char const *action;
};

struct key {
	/* key code */
	uint16_t code;

	/* whether ts_wakeup below is valid */
	bool has_wakeup;

	/* key actions */
	int action_count;
	struct action *actions;

	/* when key was pressed - valid for state == KEY_PRESSED or KEY_DEBOUNCE */
	struct timeval tv_pressed;
	/* valid when KEY_DEBOUNCE */
	struct timeval tv_released;
	/* when next to wakeup if has_wakeup is set */
	struct timespec ts_wakeup;

	/* state machine:
	 * - RELEASED/PRESSED state
	 * - DEBOUNCE: immediately after being released for DEBOUNCE_MSECS
	 * - HANDLED: long press already handled (ignore until release)
	 */
	enum state {
		KEY_RELEASED,
		KEY_PRESSED,
		KEY_DEBOUNCE,
		KEY_HANDLED,
	} state;
};

struct input_file {
	/* first is full path, second is path in directory */
	char *filename;
	char *dirent;
	int inotify_wd;
};


static uint16_t strtou16(const char *str) {
	char *endptr;
	long val;

	val = strtol(str, &endptr, 0);
	xassert(*endptr == 0,
		"Argument %s must be a full integer", str);
	xassert(val >= 0 && val <= 0xffff,
		"Argument %s must be a 16 bit integer", str);
	errno = 0;
	return val;
}

static uint32_t strtoint(const char *str) {
	char *endptr;
	long val;

	val = strtol(str, &endptr, 0);
	xassert(*endptr == 0,
		"Argument %s must be a full integer", str);
	xassert(val >= INT_MIN && val <= INT_MAX,
		"Argument %s must fit in a C int", str);
	errno = 0;
	return val;
}

static void print_key(struct input_event *event, const char *filename,
		      const char *message) {
	if (debug < 1)
		return;
	switch (event->type) {
	case 0:
		/* extra info pertaining previous event: don't print */
		return;
	case 1:
		printf("[%ld.%03ld] %s%s%d %s: %s\n",
		       event->time.tv_sec, event->time.tv_usec / 1000,
		       debug > 2 ? filename : "",
		       debug > 2 ? " " : "",
		       event->code, event->value ? "pressed" : "released",
		       message);
		break;
	default:
		printf("[%ld.%03ld] %s%s%d %d %d: %s\n",
		       event->time.tv_sec, event->time.tv_usec / 1000,
		       debug > 2 ? filename : "",
		       debug > 2 ? " " : "",
		       event->type, event->code, event->value,
		       message);
	}
}

static void handle_key(struct input_event *event, struct key *key) {
	switch (key->state) {
	case KEY_RELEASED:
	case KEY_DEBOUNCE:
		/* new key press -- can be a release if program started with key or handled long press */
		if (event->value == 0)
			break;

		/* don't reset timestamp/wakeup on debounce */
		if (key->state == KEY_RELEASED) {
			key->tv_pressed = event->time;
		}
		key->state = KEY_PRESSED;

		/* short action is always first, so if last action is not LONG there
		 * are none. We only set a timeout if we have one.*/
		struct action *action = &key->actions[key->action_count-1];
		if (action->type == LONG_PRESS) {
			key->has_wakeup = true;
			time_tv2ts(&key->ts_wakeup, &key->tv_pressed,
				   action->trigger_time);
		} else {
			/* ... but make sure we cancel any other remaining wakeup */
			key->has_wakeup = false;
		}
		break;
	case KEY_PRESSED:
		/* ignore repress */
		if (event->value != 0)
			break;
		/* mark key for debounce, we will handle event after timeout */
		key->state = KEY_DEBOUNCE;
		key->tv_released = event->time;
		key->has_wakeup = true;
		xassert(clock_gettime(CLOCK_MONOTONIC, &key->ts_wakeup) == 0,
			"Could not get time: %m");
		time_add_ts(&key->ts_wakeup, DEBOUNCE_MSECS);
		break;
	case KEY_HANDLED:
		/* ignore until key down */
		if (event->value != 0)
			break;
		key->state = KEY_RELEASED;
	}
}

static int compute_timeout(struct key *keys, int key_count) {
	int i;
	int timeout = -1;
	struct timespec ts;

	xassert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0,
		"Could not get time: %m");

	for (i = 0; i < key_count; i++) {
		if (keys[i].has_wakeup) {
			int64_t diff = time_diff_ts(&keys[i].ts_wakeup, &ts);
			if (diff < 0)
				timeout = 0;
			else if (timeout == -1 || diff < timeout)
				timeout = diff;
		}
	}
	if (debug > 3) {
		if (timeout >= 0) {
			printf("wakeup scheduled in %d\n", timeout);
		} else {
			printf("no wakeup scheduled\n");
		}
	}

	return timeout;
}

static bool action_match(struct action *action, int time) {
	switch (action->type) {
	case LONG_PRESS:
		return time >= action->trigger_time;
	case SHORT_PRESS:
		return time < action->trigger_time;
	default:
		xassert(false, "invalid action!!");
	}
}

static struct action *find_key_action(struct key *key, int time) {
	/* check from the end to get the best match */
	for (int i = key->action_count - 1; i >= 0; i--) {
		if (action_match(&key->actions[i], time))
			return &key->actions[i];
	}
	return NULL;
}

static void handle_timeouts(struct key *keys, int key_count) {
	int i;
	struct timespec ts;

	xassert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0,
		"Could not get time: %m");

	for (i = 0; i < key_count; i++) {
		if (keys[i].has_wakeup
		    && (time_diff_ts(&keys[i].ts_wakeup, &ts) <= 0)) {
			if (debug > 3)
				printf("we are %ld ahead of timeout\n",
				       time_diff_ts(&keys[i].ts_wakeup, &ts));

			if (keys[i].state != KEY_DEBOUNCE) {
				/* key still pressed - set artifical release time */
				time_ts2tv(&keys[i].tv_released, &ts, 0);
			}

			int64_t diff = time_diff_tv(&keys[i].tv_released,
						    &keys[i].tv_pressed);
			struct action *action = find_key_action(&keys[i], diff);
			if (action) {
				if (debug)
					printf("running %s after %"PRId64" ms\n",
					       action->action, diff);
				system(action->action);
			} else if (debug) {
				printf("ignoring key %d released after %"PRId64" ms\n",
				       keys[i].code, diff);
			}

			keys[i].has_wakeup = false;
			if (keys[i].state == KEY_DEBOUNCE)
				keys[i].state = KEY_RELEASED;
			else
				keys[i].state = KEY_HANDLED;
		}
	}
}

static void handle_input_event(struct input_event *event,
			       struct key *keys, int key_count,
			       const char *filename) {
	/* ignore non-keyboard events */
	if (event->type != 1) {
		if (debug > 2)
			print_key(event, filename, "non-keyboard event ignored");
		return;
	}

	struct key *key = NULL;
	for (int i = 0; i < key_count; i++) {
		if (keys[i].code == event->code) {
			key = &keys[i];
			break;
		}
	}
	/* ignore unconfigured key */
	if (!key) {
		if (debug > 1)
			print_key(event, filename, "ignored");
		return;
	}
	print_key(event, filename, "processing");

	handle_key(event, key);
}


static int handle_input(int fd, struct key *keys, int key_count,
			 const char *filename) {
	struct input_event *event;
	char buf[4096]
		__attribute__ ((aligned(__alignof__(*event))));
	int n = 0;

	while ((n = read_safe(fd, &buf, sizeof(buf))) > 0) {
		if (n % sizeof(*event) != 0) {
			fprintf(stderr,
				"Read something that is not a multiple of event size (%d / %zd) !? Trying to reopen\n",
				n, sizeof(*event));
			return -1;
		}
		for (event = (struct input_event*)buf;
		     (char*)event + sizeof(event) <= buf + n;
		     event++) {
			handle_input_event(event, keys, key_count, filename);
		}
	}
	if (n < 0) {
		fprintf(stderr, "read error: %d. Trying to reopen\n", -n);
		return -1;
	}
	return 0;
}

static struct action *add_short_action(struct key *key) {
	/* can only have one short key */
	for (int i = 0; i < key->action_count; i++) {
		xassert(key->actions[i].type != SHORT_PRESS,
			"duplicate short key for key %d, aborting.",
			key->code);
	}
	struct action *action = &key->actions[key->action_count];
	action->type = SHORT_PRESS;
	action->trigger_time = DEFAULT_SHORT_PRESS_MSECS;
	return action;
}

static struct action *add_long_action(struct key *key) {
	/* insert at the end, we'll move it when setting time */
	struct action *action = &key->actions[key->action_count];
	action->type = LONG_PRESS;
	action->trigger_time = DEFAULT_LONG_PRESS_MSECS;
	return action;
}

static int sort_actions_compare(const void *v1, const void *v2) {
	const struct action *a1 = (const struct action*)v1;
	const struct action *a2 = (const struct action*)v2;
	if (a1->type == SHORT_PRESS)
		return -1;
	if (a2->type == SHORT_PRESS)
		return 1;
	if (a1->trigger_time < a2->trigger_time)
		return -1;
	if (a1->trigger_time > a2->trigger_time)
		return 1;
	return 0;
}
static void sort_actions(struct key *key) {
	qsort(key->actions, key->action_count,
		sizeof(key->actions[0]), sort_actions_compare);
}

static void *xcalloc(size_t nmemb, size_t size) {
	void *ptr = calloc(nmemb, size);
	xassert(ptr, "Allocation failure");
	return ptr;
}

static void *xreallocarray(void *ptr, size_t nmemb, size_t size) {
	ptr = realloc(ptr, nmemb * size);
	xassert(ptr, "Allocation failure");
	return ptr;
}

static void inotify_watch(struct input_file *input_file,
			  struct pollfd *inotify) {
	/* already setup - nothing to do! */
	if (input_file->inotify_wd >= 0)
		return;

	/* setup inotify if not done yet */
	if (!inotify->events) {
		inotify->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		xassert(inotify->fd >= 0,
			"Inotify init failed: %m");
		inotify->events = POLLIN;
	}

	fprintf(stderr, "setting up inotify watch for %s\n",
		input_file->filename);

	/* add filename's parent directory.
	 * We either have:
	 * - a relative path without any dir,
	 *   we have dirent == filename
	 * - a full path or relative path with slashes,
	 *   dirent will point just after last /
	 */
	char *watch_dir;
	if (input_file->dirent == input_file->filename) {
		watch_dir = ".";
	} else {
		watch_dir = input_file->filename;
		xassert(input_file->dirent[-1] == '/',
			"input path changed under us?");
		input_file->dirent[-1] = 0;
	}
	input_file->inotify_wd =
		inotify_add_watch(inotify->fd, watch_dir, IN_CREATE);
	xassert(input_file->inotify_wd >= 0,
		"Failed to add watch for %s: %m",
		watch_dir);

	/* restore / if required, we'll need it for open... */
	if (input_file->dirent != input_file->filename) {
		input_file->dirent[-1] = '/';
	}
}

static void reopen_input(struct input_file *input_file,
			struct pollfd *pollfd,
			struct pollfd *inotify) {
	if (pollfd->events) {
		close(pollfd->fd);
		pollfd->events = 0;
	}
	int fd = open(input_file->filename,
		      O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		fd = errno;
		fprintf(stderr, "Open %s failed: %m\n", input_file->filename);
		xassert(input_file->dirent,
			"Inotify not enabled for this file: aborting");
		xassert(fd == ENOENT,
			"Cannot rely on inotify when error is not ENOENT: aborting");
		inotify_watch(input_file, inotify);
		return;
	}
	int clock = CLOCK_MONOTONIC;
	/* we use a pipe for testing which won't understand this */
	if (!test_mode && ioctl(fd, EVIOCSCLOCKID, &clock) != 0) {
		close(fd);
		fprintf(stderr,
			"Could not request clock monotonic timestamps from %s. Ignoring this file.\n",
			input_file->filename);
		xassert(input_file->dirent,
			"Inotify not enabled for this file: aborting");
		inotify_watch(input_file, inotify);
		return;
	}

	pollfd->fd = fd;
	pollfd->events = POLLIN;
}

static void handle_inotify_event(struct inotify_event *event,
				 struct input_file *input_files,
				 struct pollfd *pollfds,
				 int input_count) {
	/* skip events we don't care about */
	if (!(event->mask & IN_CREATE))
		return;

	for (int i = 0; i < input_count; i++) {
		/* find inputs concerned */
		if (event->wd != input_files[i].inotify_wd)
			continue;
		if (debug > 2) {
			printf("got inotify event for %s's directory, %s\n",
					input_files[i].filename, event->name);
		}
		/* is it filename we care about? */
		if (strcmp(event->name, input_files[i].dirent))
			continue;
		if (debug) {
			printf("reopening %s\n",
					input_files[i].filename);
		}
		reopen_input(&input_files[i], &pollfds[i],
				&pollfds[input_count]);
	}

}
static void handle_inotify(struct input_file *input_files, struct pollfd *pollfds,
			   int input_count) {
	int fd = pollfds[input_count].fd;
	struct inotify_event *event;
	/* read more at a time. Align because man page example does... */
	char buf[4096]
		__attribute__ ((aligned(__alignof__(*event))));
	int n = 0;

	while ((n = read_safe(fd, &buf, sizeof(buf))) > 0) {
		for (event = (struct inotify_event *)buf;
		     (char*)event + sizeof(*event) <= buf + n;
		     event = (struct inotify_event*)(((char*)event) + sizeof(*event) + event->len)) {
			xassert((char*)event < buf + n + event->len,
				"inotify event read has a weird size");

			handle_inotify_event(event, input_files,
					     pollfds, input_count);
		}
		xassert((char*)event == buf + n,
			"libinput event we read had a weird size: %zd / %d",
			((char*)event) - buf, n);
	}
	xassert(n >= 0, "Did not read expected amount from inotify fd: %d", n);
}

int main(int argc, char *argv[]) {
	struct input_file *input_files = NULL;
	struct key *keys = NULL;
	struct action *cur_action = NULL;
	int inotify_enabled = 0;
	int input_count = 0;
	int key_count = 0;

	int c;
	/* and real argument parsing now! */
	while ((c = getopt_long(argc, argv, "I:i:s:l:a:t:vVh", long_options, NULL)) >= 0) {
		switch (c) {
		case 'i':
		case 'I':
			input_files = xreallocarray(input_files, input_count + 1,
					             sizeof(*input_files));
			input_files[input_count].filename = optarg;
			if (c == 'I') {
				inotify_enabled = 1;
				input_files[input_count].inotify_wd = -1;
				input_files[input_count].dirent = strrchr(optarg, '/');
				if (input_files[input_count].dirent) {
					input_files[input_count].dirent++;
				} else {
					input_files[input_count].dirent = optarg;
				}
				xassert(input_files[input_count].dirent[0] != 0,
					"Invalid filename %s", optarg);
			} else {
				input_files[input_count].dirent = NULL;
			}
			input_count++;
			break;
		case 's':
		case 'l':
			xassert(!cur_action || cur_action->action != NULL,
				"Must set action before specifying next key!");
			uint16_t code = strtou16(optarg);
			struct key *cur_key = NULL;
			for (int i = 0; i < key_count; i++) {
				if (keys[i].code == code) {
					cur_key = &keys[i];
					break;
				}
			}
			if (!cur_key) {
				keys = xreallocarray(keys, key_count + 1,
						     sizeof(*keys));
				cur_key = &keys[key_count];
				key_count++;
				memset(cur_key, 0, sizeof(*cur_key));
				cur_key->code = strtou16(optarg);
				cur_key->state = KEY_RELEASED;
			}
			cur_key->actions = xreallocarray(cur_key->actions,
							 cur_key->action_count + 1,
							 sizeof(*cur_key->actions));
			if (c == 's') {
				cur_action = add_short_action(cur_key);
			} else {
				cur_action = add_long_action(cur_key);
			}
			cur_action->action = NULL;
			cur_key->action_count++;
			break;
		case 'a':
			xassert(cur_action,
				"Action can only be provided after setting key code");
			cur_action->action = optarg;
			break;
		case 't':
			xassert(cur_action,
				"Action timeout can only be set after setting key code");
			cur_action->trigger_time = strtoint(optarg);
			break;
		case 'v':
			debug++;
			break;
		case 'V':
			version(argv[0]);
			exit(EXIT_SUCCESS);
		case 'h':
			help(argv[0]);
			exit(EXIT_SUCCESS);
		case OPT_TEST:
			test_mode = true;
			break;
		default:
			help(argv[0]);
			exit(EXIT_FAILURE);
		}
	}
	xassert(optind >= argc,
		"Non-option argument: %s. Did you forget to quote action?",
		optind >= 0 ? argv[optind] : "???");
	xassert(input_count > 0,
		"No input have been given, exiting");
	xassert(key_count > 0 || debug > 1,
		"No action given, exiting");
	xassert(!cur_action || cur_action->action != NULL,
		"Last key press was defined without action");
	for (int i = 0; i < key_count; i++) {
		sort_actions(&keys[i]);
	}

	struct pollfd *pollfd = xcalloc(input_count + inotify_enabled,
					sizeof(*pollfd));
	for (int i = 0; i < input_count; i++) {
		reopen_input(&input_files[i], &pollfd[i],
				     &pollfd[input_count]);
	}

	while (1) {
		int timeout = compute_timeout(keys, key_count);
		int n = poll(pollfd, input_count + inotify_enabled, timeout);
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		xassert(n >= 0, "Poll failure: %m");

		handle_timeouts(keys, key_count);
		if (n == 0)
			continue;
		for (int i = 0; i < input_count; i++) {
			if (pollfd[i].revents == 0)
				continue;
			if (!(pollfd[i].revents & POLLIN)) {
				if (test_mode)
					exit(0);
				fprintf(stderr, "got HUP/ERR on %s. Trying to reopen.\n",
					input_files[i].filename);
				reopen_input(&input_files[i], &pollfd[i],
					     &pollfd[input_count]);
			}
			if (handle_input(pollfd[i].fd, keys, key_count,
					 input_files[i].filename)) {
				reopen_input(&input_files[i], &pollfd[i],
					     &pollfd[input_count]);
			}
		}
		if (inotify_enabled && pollfd[input_count].revents) {
			xassert(pollfd[input_count].revents & POLLIN,
				"inotify fd went bad");
			handle_inotify(input_files, pollfd, input_count);
		}
	}

	/* unreachable */
}
