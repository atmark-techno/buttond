#include <ctype.h>
#include <string.h>

#include "buttond.h"
#include "keynames.h"

static const char *keynames[KEY_MAX];

void init_keynames(void) {
	size_t idx = 0;
	/* starts at 1... */
	for (int i = 1;
	     i < KEY_MAX && idx < sizeof(allkeynames);
	     i++, idx += strlen(&allkeynames[idx]) + 1) {
		keynames[i] = &allkeynames[idx];
	}
}

uint16_t find_key_by_name(char *arg) {
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

const char *keyname_by_code(uint16_t code) {
	if (code > KEY_MAX || !keynames[code])
		return "unknown";
	return keynames[code];
}

static void tv_from_event(struct timeval *tv, struct input_event *event) {
	/* input_event has a timeval struct on 64bit systems,
	 * but it is not guaranteed so copy manually
	 */
	tv->tv_sec = event->input_event_sec;
	tv->tv_usec = event->input_event_usec;
}

void arm_key_press(struct key *key, bool reset_pressed) {
	key->state = KEY_PRESSED;

	/* short action is always first, so if last action is not LONG there
	 * are none. We only set a timeout if we have one.*/
	struct action *action = &key->actions[key->action_count-1];
	if (action->type != LONG_PRESS) {
		key->has_wakeup = false;
		return;
	}
	key->has_wakeup = true;
	if (reset_pressed) {
		time_gettime(&key->ts_wakeup);
		time_ts2tv(&key->tv_pressed, &key->ts_wakeup, 0);
                time_add_ts(&key->ts_wakeup, action->trigger_time);
	} else {
		time_tv2ts(&key->ts_wakeup, &key->tv_pressed,
			   action->trigger_time);
	}
}

void handle_key(struct state *state, struct input_event *event,
		struct key *key) {
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
		arm_key_press(key, false);
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
		time_add_ts(&key->ts_wakeup, state->debounce_msecs);
		break;
	case KEY_HANDLED:
		/* ignore until key down */
		if (event->value != 0)
			break;
		key->state = KEY_RELEASED;
	}
}

int compute_timeout(struct key *keys, int key_count) {
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
	/* check short keys in growing order, then long keys in
	 * decreasing order to get the best match */
	for (int i = 0; i < key->action_count; i++) {
		if (key->actions[i].type != SHORT_PRESS)
			break;
		if (action_match(&key->actions[i], time))
			return &key->actions[i];
	}
	for (int i = key->action_count - 1; i >= 0; i--) {
		if (key->actions[i].type != LONG_PRESS)
			break;
		if (action_match(&key->actions[i], time))
			return &key->actions[i];
	}
	return NULL;
}

void handle_timeouts(struct key *keys, int key_count) {
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
				/* special keys can have no action */
				if (action->action && action->action[0]) {
					if (debug)
						printf("running %s after %"PRId64" ms\n",
						       action->action, diff);
					system(action->action);
				}
				if (action->exit_after) {
					if (debug && keys[i].code)
						printf("Exiting after processing key %s (%d)\n",
						       keyname_by_code(keys[i].code),
						       keys[i].code);
					else if (debug)
						printf("Exiting after stop timeout\n");
					exit(0);
				}
			} else if (keys[i].state != KEY_DEBOUNCE) {
				fprintf(stderr,
					"Woke up for key %s (%d) after %"PRId64" ms without any associated action, this should not happen!\n",
					keyname_by_code(keys[i].code),
					keys[i].code, diff);
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
