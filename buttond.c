// SPDX-License-Identifier: MIT
/*
 * Handle evdev button press
 * Copyright (c) 2021 Atmark Techno,Inc.
 */


#include <ctype.h>
#include <getopt.h>
#include <linux/input.h>
#include <poll.h>
#include <string.h>

#include "buttond.h"
#include "keynames.h"

/* debug:
 * -v (> 0/set): info message e.g. registered key presses
 * -vv (> 1): ignored keys also printed
 * -vvv (> 2): add non-keyboard events and file names
 * -vvvv (> 3): add timeout/wakeup related debugs
 */
int debug = 0;
int test_mode = 0;
#define DEFAULT_LONG_PRESS_MSECS 5000
#define DEFAULT_SHORT_PRESS_MSECS 1000
int debounce_msecs = 10;

#define OPT_TEST 257
#define OPT_DEBOUNCE_TIME 258

static struct option long_options[] = {
	{"inotify",	required_argument,	0, 'i' },
	{"short",	required_argument,	0, 's' },
	{"long",	required_argument,	0, 'l' },
	{"action",	required_argument,	0, 'a' },
	{"time",	required_argument,	0, 't' },
	{"verbose",	no_argument,		0, 'v' },
	{"version",	no_argument,		0, 'V' },
	{"help",	no_argument,		0, 'h' },
	{"test_mode",	no_argument,		0, OPT_TEST },
	{"debounce-time", required_argument,	0, OPT_DEBOUNCE_TIME },
	{0,		0,			0,  0  }
};

static void version(char *argv0) {
	printf("%s version 0.1\n", argv0);
}

static void help(char *argv0) {
	printf("Usage: %s [options] [files]\n", argv0);
	printf("Options:\n");
	printf("  [files]: file(s) to get event from e.g. /dev/input/event2\n");
	printf("           pass as many as needed to monitor multiple files\n");
	printf("  -i <file>: same as non-option files, except if they disappear wait for them to come back\n");
	printf("  -s/--short <key>  [-t/--time <time ms>] -a/--action <command>: action on short key press\n");
	printf("  -l/--long <key> [-t/--time <time ms>] -a/--action <command>: action on long key press\n");
	printf("  -h, --help: show this help\n");
	printf("  -V, --version: show version\n");
	printf("  -v, --verbose: verbose (repeatable)\n\n");

	printf("<key> code should preferrably be a key name or its value, which can be found\n");
	printf("in uapi/linux/input-event-code.h or by running with -vv\n");
	printf("(note for single digits e.g. '1' the key name is used)\n\n");

	printf("Semantics: a short press action happens on release, if and only if\n");
	printf("the button was released before <time> (default %d) milliseconds.\n",
	       DEFAULT_SHORT_PRESS_MSECS);
	printf("a long press action happens even if key is still pressed, if it has been\n");
	printf("held for at least <time> (default %d) milliseconds.\n\n",
	       DEFAULT_LONG_PRESS_MSECS);

	printf("Note some keyboards have repeat built in firmware so quick repetitions\n");
	printf("(<%dms) are handled as if key were pressed continuously\n",
	       debounce_msecs);
}

static const char *keynames[KEY_MAX];

static void init_keynames(void) {
	size_t idx = 0;
	/* starts at 1... */
	for (int i = 1;
	     i < KEY_MAX && idx < sizeof(allkeynames);
	     i++, idx += strlen(&allkeynames[idx]) + 1) {
		keynames[i] = &allkeynames[idx];
	}
}

static uint16_t find_key_by_name(char *arg) {
	/* XXX if this is too slow try to optimize later, but list is not so big */
	for (int i = 0; arg[i]; i++) {
		/* We require ASCII name anyway: make it uppercase to match header.
		 * This doesn't bother pure digits for fallback. */
		arg[i] = toupper(arg[i]);
	}
	for (size_t i = 0; i < KEY_MAX; i++) {
		if (!keynames[i])
			continue;
		if (strcmp(keynames[i], arg) == 0) {
			return i;
		}
	}
	return 0;
}

#define keyname_by_code(code) \
	((code < KEY_MAX && keynames[code]) ? keynames[code] : "unknown")



static void print_key(struct input_event *event, const char *filename,
		      const char *message) {
	if (debug < 1)
		return;
	switch (event->type) {
	case 0:
		/* extra info pertaining previous event: don't print */
		return;
	case 1:
		printf("[%ld.%03ld] %s%s%s (%d) %s: %s\n",
		       event->input_event_sec, event->input_event_usec / 1000,
		       debug > 2 ? filename : "",
		       debug > 2 ? " " : "",
		       keyname_by_code(event->code), event->code,
		       event->value ? "pressed" : "released",
		       message);
		break;
	default:
		printf("[%ld.%03ld] %s%s%d %d %d: %s\n",
		       event->input_event_sec, event->input_event_usec / 1000,
		       debug > 2 ? filename : "",
		       debug > 2 ? " " : "",
		       event->type, event->code, event->value,
		       message);
	}
}

static void tv_from_event(struct timeval *tv, struct input_event *event) {
	/* input_event has a timeval struct on 64bit systems,
	 * but it is not guaranteed so copy manually
	 */
	tv->tv_sec = event->input_event_sec;
	tv->tv_usec = event->input_event_usec;
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
			tv_from_event(&key->tv_pressed, event);
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
		tv_from_event(&key->tv_released, event);
		key->has_wakeup = true;
		time_gettime(&key->ts_wakeup);
		time_add_ts(&key->ts_wakeup, debounce_msecs);
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
	time_gettime(&ts);

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
	time_gettime(&ts);

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
			} else if (keys[i].state != KEY_DEBOUNCE) {
				fprintf(stderr,
					"Woke up for key %s (%d) without any associated action, this shuld not happen!\n",
					keyname_by_code(keys[i].code),
					keys[i].code);
			} else if (debug) {
				printf("ignoring key %s (%d) released after %"PRId64" ms\n",
				       keyname_by_code(keys[i].code),
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
			"duplicate short key for key %s (%d), aborting.",
			keyname_by_code(key->code), key->code);
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

static void add_input(char *path, struct input_file **p_input_files,
		      int *p_input_count, bool inotify) {
	int input_count = *p_input_count;
	struct input_file *input_files = *p_input_files;
	input_files = xreallocarray(input_files, input_count + 1,
			sizeof(*input_files));
	input_files[input_count].filename = path;
	if (inotify) {
		input_files[input_count].inotify_wd = -1;
		input_files[input_count].dirent = strrchr(path, '/');
		if (input_files[input_count].dirent) {
			input_files[input_count].dirent++;
		} else {
			input_files[input_count].dirent = path;
		}
		xassert(input_files[input_count].dirent[0] != 0,
				"Invalid filename %s", path);
	} else {
		input_files[input_count].dirent = NULL;
	}
	*p_input_count = input_count + 1;
	*p_input_files = input_files;
}

int main(int argc, char *argv[]) {
	struct input_file *input_files = NULL;
	struct key *keys = NULL;
	struct action *cur_action = NULL;
	bool inotify_enabled = false;
	int input_count = 0;
	int key_count = 0;

	init_keynames();

	int c;
	while ((c = getopt_long(argc, argv, "i:s:l:a:t:vVh", long_options, NULL)) >= 0) {
		switch (c) {
		case 'i':
			add_input(optarg, &input_files, &input_count, true);
			inotify_enabled = true;
			break;
		case 's':
		case 'l':
			xassert(!cur_action || cur_action->action != NULL,
				"Must set action before specifying next key!");
			/* try to find key by name first, then by code if it failed */
			uint16_t code = find_key_by_name(optarg);
			if (!code) {
				code = strtou16(optarg);
			}
			xassert(code,
				"key code (%s) should be a key name or its keycode",
				optarg);

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
				cur_key->code = code;
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
			xassert(cur_action->trigger_time,
				"Could not parse trigger time (%s): %m",
				optarg);
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
		case OPT_DEBOUNCE_TIME:
			debounce_msecs = strtoint(optarg);
			break;
		default:
			help(argv[0]);
			exit(EXIT_FAILURE);
		}
	}
	for (int i = optind; i < argc; i++) {
		add_input(argv[i], &input_files, &input_count, false);
	}
	xassert(input_count > 0,
		"No input have been given, exiting");
	xassert(key_count > 0 || debug > 1,
		"No action given, exiting");
	xassert(!cur_action || cur_action->action != NULL,
		"Last key press was defined without action");
	for (int i = 0; i < key_count; i++) {
		sort_actions(&keys[i]);
		for (int j = 1; j < keys[i].action_count; j++) {
			xassert(keys[i].actions[j-1].type == SHORT_PRESS
					|| keys[i].actions[j-1].trigger_time != keys[i].actions[j].trigger_time,
				"Key %s was defined twice with %d ms action",
				keyname_by_code(keys[i].code),
				keys[i].actions[j].trigger_time)
		}
	}

	struct pollfd *pollfd = xcalloc(input_count + inotify_enabled,
					sizeof(*pollfd));
	for (int i = 0; i < input_count; i++) {
		pollfd[i].fd = -1;
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
