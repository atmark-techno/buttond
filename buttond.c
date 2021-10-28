#define _POSIX_C_SOURCE 199309L
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int debug = 0;
#define MAX_ACTIONS 64
#define LONG_PRESS_MSECS 5000
#define SHORT_PRESS_MSECS 1000
#define DEBOUNCE_MSECS 10

ssize_t read_safe(int fd, void *buf, size_t count, size_t partial_read) {
	size_t total = partial_read % count;

	while (total < count) {
		ssize_t n;

		n = read(fd, (char*)buf + total, count - total);
		if (n < 0 && errno == EINTR)
			continue;
		else if (n < 0)
			return errno == EAGAIN ? 0 : -errno;
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

static struct option long_options[] = {
	{"input",	required_argument,	0, 'i' },
	{"short",	required_argument,	0, 's' },
	{"long",	required_argument,	0, 'l' },
	{"action",	required_argument,	0, 'a' },
	{"verbose",	no_argument,		0, 'v' },
	{"version",	no_argument,		0, 'V' },
	{"help",	no_argument,		0, 'h' },
	{0,		0,			0,  0  }
};

static void version(char *argv0) {
	printf("%s version 0.1\n", argv0);
}

static void help(char *argv0) {
	printf("Usage: %s [options]\n", argv0);
	printf("Options:\n");
	printf("  -i, --input <file>: file to get event from e.g. /dev/input/event2\n");
	printf("  -s, --short <key> --action <command>: action on short key press\n");
	printf("  -l, --long <key> --action <command>: action on long key press\n");
	printf("  -h, --help: show this help\n");
	printf("  -V, --version: show version\n");
	printf("  -v, --verbose: verbose (repeatable)\n\n");

	printf("<key> code can be found in uapi/linux/input-event-code.h or by running\n");
	printf("with -vv\n\n");

	printf("Semantics: a short press action happens on release, if and only if\n");
	printf("the button was released before %d milliseconds.\n", SHORT_PRESS_MSECS);
	printf("a long press action happens even if key is still pressed, if it has been\n");
	printf("held for at least %d milliseconds.\n\n", LONG_PRESS_MSECS);

	printf("Note some keyboards have repeat built in firmware so quick repetitions\n");
	printf("(<%dms) are handled as if key were pressed continuously\n", DEBOUNCE_MSECS);
}

struct action {
	/* key code from linux uapi/linux/input-event-code.h */
	uint16_t code;
	/* type of action (long/short press) */
	enum type {
		LONG_PRESS,
		SHORT_PRESS,
	} type;
	/* command to run */
	char const *action;
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
	/* when key was pressed - valid for state == KEY_PRESSED or KEY_DEBOUNCE */
	struct timeval tv_pressed;
	/* valid when KEY_DEBOUNCE */
	struct timeval tv_released;
	/* when next to wakeup - valid for
	 * state == KEY_DEBOUNCE || (state == KEY_PRESSED && type == LONG_PRESS) */
	struct timespec ts_wakeup;
};

static uint16_t strtou16(const char *str) {
	char *endptr;
	long val;

	val = strtol(str, &endptr, 0);
	if (*endptr != 0) {
		fprintf(stderr, "Key code must passed as integer");
		errno = EINVAL;
		return 0xffff;
	}
	if (val < 0 || val > 0xffff) {
		fprintf(stderr, "Key code must be a 16 bit integer");
		errno = ERANGE;
		return 0xffff;
	}
	errno = 0;
	return val;
}

static void handle_key(struct input_event *event, struct action *action) {
	if (debug > 1)
		printf("%d %s %ld.%03ld\n", event->code,
		       event->value ? "pressed" : "released",
		       event->time.tv_sec, event->time.tv_usec / 1000);

	switch (action->state) {
	case KEY_RELEASED:
	case KEY_DEBOUNCE:
		/* new key press -- can be a release if program started with key or handled long press */
		if (event->value == 0)
			break;
		/* don't reset timestamp on debounce */
		if (action->state == KEY_RELEASED) {
			action->tv_pressed = event->time;
		}
		action->state = KEY_PRESSED;

		if (action->type == LONG_PRESS) {
			time_tv2ts(&action->ts_wakeup, &action->tv_pressed,
				 LONG_PRESS_MSECS);
		}
		break;
	case KEY_PRESSED:
		/* ignore repress */
		if (event->value != 0)
			break;
		/* mark key for debounce, we will handle event after timeout */
		action->state = KEY_DEBOUNCE;
		action->tv_released = event->time;
		time_tv2ts(&action->ts_wakeup, &action->tv_pressed,
			 DEBOUNCE_MSECS);
		break;
	case KEY_HANDLED:
		/* ignore until key down */
		if (event->value != 0)
			break;
		action->state = KEY_RELEASED;
	}
}

static bool has_wakeup(struct action *action) {
	return action->state == KEY_DEBOUNCE
		|| (action->state == KEY_PRESSED && action->type == LONG_PRESS);
}

static int compute_timeout(struct action *actions, int action_count) {
	int i;
	int timeout = -1;
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
		fprintf(stderr, "Could not get time: %d\n", errno);
		exit(EXIT_FAILURE);
	}

	for (i = 0; i <= action_count; i++) {
		if (has_wakeup(&actions[i])) {
			int diff = time_diff_ts(&actions[i].ts_wakeup, &ts);
			if (diff < 0)
				timeout = 0;
			else if (timeout == -1 || diff < timeout)
				timeout = diff;
		}
	}
	return timeout;
}

static void handle_timeouts(struct action *actions, int action_count) {
	int i;
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
		fprintf(stderr, "Could not get time: %d\n", errno);
		exit(EXIT_FAILURE);
	}

	for (i = 0; i <= action_count; i++) {
		if (has_wakeup(&actions[i])
		    && (time_diff_ts(&actions[i].ts_wakeup, &ts) <= 0)) {

			if (actions[i].state != KEY_DEBOUNCE) {
				/* key still pressed - set artifical release time */
				time_ts2tv(&actions[i].tv_released, &ts, 0);
			}

			int diff = time_diff_tv(&actions[i].tv_released, &actions[i].tv_pressed);
			if ((actions[i].type == LONG_PRESS && diff >= LONG_PRESS_MSECS)
			    || (actions[i].type == SHORT_PRESS && diff < SHORT_PRESS_MSECS)) {
				if (debug)
					printf("running %s\n", actions[i].action);
				system(actions[i].action);
			} else if (debug) {
				printf("ignoring key %d released after %d ms\n",
				       actions[i].code, diff);
			}

			if (actions[i].state == KEY_DEBOUNCE)
				actions[i].state = KEY_RELEASED;
			else
				actions[i].state = KEY_HANDLED;
		}
	}
}

int main(int argc, char *argv[]) {
	char const *event_input = "/dev/input/event0";
	struct action actions[MAX_ACTIONS];
	int action_count = -1;

	int c;
	while ((c = getopt_long(argc, argv, "i:s:l:a:vVh", long_options, NULL)) >= 0) {
		switch (c) {
		case 'i':
			event_input = optarg;
			break;
		case 's':
		case 'l':
			if (action_count >= 0 && actions[action_count].action == NULL) {
				fprintf(stderr, "Must set action before specifying next key!\n");
				exit(EXIT_FAILURE);
			}
			if (action_count >= MAX_ACTIONS - 1) {
				fprintf(stderr, "Only support up to %d bindings\n", MAX_ACTIONS);
				exit(EXIT_FAILURE);
			}
			action_count++;
			actions[action_count].type = (c == 's' ? SHORT_PRESS : LONG_PRESS);
			actions[action_count].code = strtou16(optarg);
			actions[action_count].action = NULL;
			actions[action_count].state = KEY_RELEASED;
			if (actions[action_count].code == 0xffff && errno != 0)
				exit(EXIT_FAILURE);
			break;
		case 'a':
			if (action_count < 0) {
				fprintf(stderr, "Action can only be provided after setting key code\n");
				exit(EXIT_FAILURE);
			}
			if (actions[action_count].action != NULL) {
				fprintf(stderr, "Action was already set for this key\n");
				exit(EXIT_FAILURE);
			}
			actions[action_count].action = optarg;
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
		default:
			help(argv[0]);
			exit(EXIT_FAILURE);
		}
	}
	if (action_count < 0 && debug <= 1) {
		/* allow no action with full debug to configure new keys the first time */
		fprintf(stderr, "No action defined, exiting\n");
		exit(EXIT_FAILURE);
	}
	if (action_count >= 0 && actions[action_count].action == NULL) {
		fprintf(stderr, "Last key press was defined without action\n");
		exit(EXIT_FAILURE);
	}

	int fd = open(event_input, O_RDONLY|O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "Open %s failed: %d\n", event_input, errno);
		exit(EXIT_FAILURE);
	}

	c = CLOCK_MONOTONIC;
	if (ioctl(fd, EVIOCSCLOCKID, &c)) {
		fprintf(stderr, "Could not request clock monotonic timestamps from events\n");
		exit(EXIT_FAILURE);
	}

	struct pollfd pollfd = {
		.fd = fd,
		.events = POLLIN,
	};
	struct input_event event;
	int partial_read = 0;

	while (1) {
		int timeout = compute_timeout(actions, action_count);
		int n = poll(&pollfd, 1, timeout);

		handle_timeouts(actions, action_count);
		if (n == 0)
			continue;

		while ((partial_read = read_safe(fd, &event, sizeof(event), partial_read))
				== sizeof(event)) {
			int key;

			/* ignore non-keyboard events */
			if (event.type != 1)
				continue;
			for (key = 0; key <= action_count; key++) {
				if (actions[key].code == event.code) {
					break;
				}
			}
			/* ignore unconfigured key */
			if (key > action_count) {
				if (debug > 1)
					printf("%d %s: ignore\n", event.code,
						event.value ? "pressed" : "released");
				continue;
			}

			handle_key(&event, &actions[key]);
		}
		if (partial_read < 0) {
			fprintf(stderr, "read error: %d\n", -partial_read);
			exit(EXIT_FAILURE);
		}
	}

	/* unreachable */
}
