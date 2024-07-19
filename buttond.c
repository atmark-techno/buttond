// SPDX-License-Identifier: MIT
/*
 * Handle evdev button press
 * Copyright (c) 2021 Atmark Techno,Inc.
 */

#include <getopt.h>
#include <poll.h>
#include <string.h>
#include <sys/stat.h>

#include "buttond.h"
#include "version.h"

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
#define DEFAULT_DEBOUNCE_MSECS 10

#define OPT_TEST 257
#define OPT_DEBOUNCE_TIME 258
#define OPT_EXIT_AFTER 259

static struct option long_options[] = {
	{"inotify",	required_argument,	0, 'i' },
	{"short",	required_argument,	0, 's' },
	{"long",	required_argument,	0, 'l' },
	{"action",	required_argument,	0, 'a' },
	{"exit-after",	no_argument,		0, OPT_EXIT_AFTER },
	{"time",	required_argument,	0, 't' },
	{"exit-timeout",required_argument,	0, 'E' },
	{"verbose",	no_argument,		0, 'v' },
	{"version",	no_argument,		0, 'V' },
	{"help",	no_argument,		0, 'h' },
	{"test_mode",	no_argument,		0, OPT_TEST },
	{"debounce-time", required_argument,	0, OPT_DEBOUNCE_TIME },
	{0,		0,			0,  0  }
};

static void version(void) {
	printf("buttond version %s\n", BUTTOND_VERSION);
}

static void help(char *argv0) {
	printf("Usage: %s [options] [files]\n", argv0);
	printf("Options:\n");
	printf("  [files]: file(s) to get event from e.g. /dev/input/event2\n");
	printf("           pass as many as needed to monitor multiple files\n");
	printf("  -i <file>: same as non-option files, except if they disappear wait for them to come back\n");
	printf("  -s/--short <key>  [-t/--time <time ms>] [--exit-after] -a/--action <command>:\n");
	printf("             action on short key press\n");
	printf("  -l/--long <key> [-t/--time <time ms>] [--exit-after] -a/--action <command>:\n");
	printf("             action on long key press\n");
	printf("  -E/--exit-timeout <time ms>: exit after <time> milliseconds\n");
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
	       DEFAULT_DEBOUNCE_MSECS);
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

static void add_input(char *path, struct state *state, bool inotify) {
	/* skip directories */
	struct stat sb;
	if (stat(path, &sb) == 0) {
		if ((sb.st_mode & S_IFMT) == S_IFDIR) {
			fprintf(stderr, "Skipping directory %s\n", path);
			return;
		}
	} else {
		xassert(errno == ENOENT,
			"Could not stat %s: %m", path);
		xassert(inotify,
			"File %s does not exist and we are not in inotify mode",
			path);
	}

	state->input_files = xreallocarray(state->input_files,
			state->input_count + 1,
			sizeof(*state->input_files));
	struct input_file *input_file = &state->input_files[state->input_count];
	state->input_count++;
	memset(input_file, 0, sizeof(*input_file));
	input_file->filename = path;
	if (inotify) {
		input_file->inotify_wd = -1;
		input_file->dirent = strrchr(path, '/');
		if (input_file->dirent) {
			input_file->dirent++;
		} else {
			input_file->dirent = path;
		}
		xassert(input_file->dirent[0] != 0,
				"Invalid filename %s", path);
	}
}

struct action *add_action(char option, char *key, char *exit_timeout,
		struct state *state) {
	/* try to find key by name first, then by code if it failed */
	uint16_t code = key ? find_key_by_name(key) : 0;
	if (key && !code) {
		code = strtou16(key);
	}
	xassert(!key || code,
		"key code (%s) should be a key name or its keycode",
		key);

	struct key *cur_key = NULL;
	for (int i = 0; i < state->key_count; i++) {
		if (state->keys[i].code == code) {
			cur_key = &state->keys[i];
			break;
		}
	}
	if (!cur_key) {
		state->keys = xreallocarray(state->keys,
					    state->key_count + 1,
					    sizeof(*state->keys));
		cur_key = &state->keys[state->key_count];
		state->key_count++;
		memset(cur_key, 0, sizeof(*cur_key));
		cur_key->code = code;
		cur_key->state = KEY_RELEASED;
	}
	cur_key->actions = xreallocarray(cur_key->actions,
			cur_key->action_count + 1,
			sizeof(*cur_key->actions));

	/* insert at the end, we'll sort later */
	struct action *action = &cur_key->actions[cur_key->action_count];
	cur_key->action_count++;
	memset(action, 0, sizeof(*action));
	action->trigger_time = DEFAULT_SHORT_PRESS_MSECS;
	switch (option) {
	case 's':
		action->type = SHORT_PRESS;
		break;
	case 'l':
		action->type = LONG_PRESS;
		break;
	case 'E':
		action->type = LONG_PRESS;
		action->trigger_time = strtoint(exit_timeout);
		action->exit_after = true;
		xassert(action->trigger_time,
			"Could not parse trigger time (%s): %m",
			exit_timeout);
		arm_key_press(cur_key, true);
		break;
	default:
		xassert(false, "add_action should never be called with %c", option);
	}
	return action;
}

int main(int argc, char *argv[]) {
	struct state state = {
		.debounce_msecs = DEFAULT_DEBOUNCE_MSECS,
	};
	struct action *cur_action = NULL;
	bool inotify_enabled = false;

	init_keynames();

	int c;
	while ((c = getopt_long(argc, argv, "i:s:l:a:t:E:vVh", long_options, NULL)) >= 0) {
		switch (c) {
		case 'i':
			add_input(optarg, &state, true);
			inotify_enabled = true;
			break;
		case 's':
		case 'l':
			xassert(!cur_action || cur_action->action != NULL,
				"Must set action before specifying next key!");
			cur_action = add_action(c, optarg, NULL, &state);
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
		case OPT_EXIT_AFTER:
			xassert(cur_action,
				"--exit-after can only be set after setting key code");
			cur_action->exit_after = true;
			break;
		case 'E':
			xassert(!cur_action || cur_action->action != NULL,
				"Cannot set stop timeout in the middle of defining a key");
			/* add fake key with code 0 */
			add_action('E', NULL, optarg, &state);
			break;
		case 'v':
			debug++;
			break;
		case 'V':
			version();
			exit(EXIT_SUCCESS);
		case 'h':
			help(argv[0]);
			exit(EXIT_SUCCESS);
		case OPT_TEST:
			test_mode = true;
			break;
		case OPT_DEBOUNCE_TIME:
			state.debounce_msecs = strtoint(optarg);
			break;
		default:
			help(argv[0]);
			exit(EXIT_FAILURE);
		}
	}
	for (int i = optind; i < argc; i++) {
		add_input(argv[i], &state, false);
	}
	xassert(state.input_count > 0,
		"No input have been given, exiting");
	xassert(state.key_count > 0 || debug > 1,
		"No action given, exiting");
	xassert(!cur_action || cur_action->action != NULL,
		"Last key press was defined without action");
	for (int i = 0; i < state.key_count; i++) {
		struct key *key = &state.keys[i];
		sort_actions(key);
		for (int j = 1; j < key->action_count; j++) {
			struct action *a1, *a2;
			a1 = &key->actions[j-1];
			a2 = &key->actions[j];
			xassert(a1->type == a2->type || a1->trigger_time <= a2->trigger_time,
				"Key %s had a short key (%d) longer than its shortest long key (%d)",
				keyname_by_code(key->code),
				a1->trigger_time, a2->trigger_time);
			xassert(a1->type != a2->type || a1->trigger_time != a2->trigger_time,
				"Key %s was defined twice with %d ms %s action",
				keyname_by_code(key->code),
				a1->trigger_time,
				a1->type == SHORT_PRESS ? "short" : "long");
		}
	}

	state.pollfds = xcalloc(state.input_count + inotify_enabled, sizeof(*state.pollfds));
	for (int i = 0; i < state.input_count; i++) {
		state.pollfds[i].fd = -1;
		reopen_input(&state, i);
	}

	if (debug > 1)
		printf("Waiting for input, press a key to display it\n");

	while (1) {
		int timeout = compute_timeout(state.keys, state.key_count);
		int n = poll(state.pollfds, state.input_count + inotify_enabled, timeout);
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		xassert(n >= 0, "Poll failure: %m");

		handle_timeouts(state.keys, state.key_count);
		if (n == 0)
			continue;
		for (int i = 0; i < state.input_count; i++) {
			if (state.pollfds[i].revents == 0)
				continue;
			if (!(state.pollfds[i].revents & POLLIN)) {
				if (test_mode)
					exit(0);
				fprintf(stderr, "got HUP/ERR on %s. Trying to reopen.\n",
					state.input_files[i].filename);
				reopen_input(&state, i);
			}
			if (handle_input(&state, i)) {
				reopen_input(&state, i);
			}
		}
		if (inotify_enabled && state.pollfds[state.input_count].revents) {
			xassert(state.pollfds[state.input_count].revents & POLLIN,
				"inotify fd went bad");
			handle_inotify(&state);
		}
	}

	/* unreachable */
}
