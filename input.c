// SPDX-License-Identifier: MIT

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>

#include "buttond.h"

// XXX we probably also ought to watch IN_MOVE_TO (with create)
// and IN_MOVE_SELF (with delete self), but this seems to be enough
// for simple cases
#define INOTIFY_WATCH_FLAGS (IN_CREATE|IN_DELETE_SELF)

static void mkdir_one(char *path, char *rest) {
	if (access(path, F_OK) == 0) {
		return;
	}
	xassert(mkdir(path, 0777) == 0,
		"Could not create %s required for %s/%s watch: %m",
		path, path, rest);
}

static void mkdir_p(char *path) {
	char *slash = path;
	/* note: if path is non-canonical we also try to create subcomponents
	 * that aren't strictly needed, but it's non-trivial to canonicalize
	 * a path which does not exist without guessing (logical vs. physical
	 * parent directory) so just go with it
	 */
	while ((slash = strchr(slash+1, '/'))) {
		slash[0] = 0;
		mkdir_one(path, slash+1);
		slash[0]='/';
	}
	mkdir_one(path, "");
}

static void touch(char *dir, char *file) {
	char buf[PATH_MAX];
	xassert(snprintf(buf, PATH_MAX, "%s/%s", dir, file) < PATH_MAX,
		"path too long: %s/%s", dir, file);
	int fd = open(buf, O_CREAT, 0666);
	xassert(fd >= 0, "Could not open %s: %m", buf);
	xassert(close(fd) == 0, "Could not close newly-opened fd (%s): %m", buf);
}

/* return 1 if something was done */
static int inotify_watch(struct input_file *input_file,
			  struct pollfd *inotify) {
	/* already setup - nothing to do! */
	if (input_file->inotify_wd >= 0)
		return 0;

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
	bool retried = false;
again:
	input_file->inotify_wd =
		inotify_add_watch(inotify->fd, watch_dir, INOTIFY_WATCH_FLAGS);
	if (input_file->inotify_wd < 0 && errno == ENOENT && !retried) {
		/* directory didn't exist, try to create it and create a dummy file in there
		 * so udev doesn't delete it under us.
		 * XXX: if this fails, add a timerfd and retry to open periodically like tail -F ?
		 */
		mkdir_p(watch_dir);
		touch(watch_dir, ".buttond_watching");
		retried = true;
		goto again;
	}
	xassert(input_file->inotify_wd >= 0,
		"Failed to add watch for %s: %m",
		watch_dir);

	/* restore / if required, we'll need it for open... */
	if (input_file->dirent != input_file->filename) {
		input_file->dirent[-1] = '/';
	}

	return 1;
}

void reopen_input(struct state *state, int i) {
	struct input_file *input_file = &state->input_files[i];
	struct pollfd *pollfd = &state->pollfds[i];
	struct pollfd *inotify = &state->pollfds[state->input_count];
	if (pollfd->fd >= 0) {
		close(pollfd->fd);
		pollfd->fd = -1;
		pollfd->events = 0;
	}
	int fd = open(input_file->filename,
		      O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		xassert(errno == ENOENT,
			"Open %s failed: %m", input_file->filename);
		xassert(input_file->dirent,
			"%s: %m.\nInotify is not enabled, aborting.",
			input_file->filename);
		if (inotify_watch(input_file, inotify) == 0)
			return;
		/* this was racy: retry to open here, just in case. */
		fd = open(input_file->filename,
			  O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			return;
	}
	int clock = CLOCK_MONOTONIC;
	/* we use a pipe for testing which won't understand this */
	if (!test_mode && ioctl(fd, EVIOCSCLOCKID, &clock) != 0) {
		close(fd);
		fprintf(stderr,
			"Could not request clock monotonic timestamps from %s. Ignoring this file.\n",
			input_file->filename);
		if (input_file->dirent)
			inotify_watch(input_file, inotify);
		else if (debug < 2)
			xassert(input_file->dirent,
				"Inotify not enabled for this file: aborting");
		return;
	}

	pollfd->fd = fd;
	pollfd->events = POLLIN;
}

static void handle_inotify_event(struct state *state, struct inotify_event *event) {
	/* skip events we don't care about */
	if (!(event->mask & INOTIFY_WATCH_FLAGS))
		return;

	for (int i = 0; i < state->input_count; i++) {
		struct input_file *input_file = &state->input_files[i];
		/* find inputs concerned */
		if (event->wd != input_file->inotify_wd)
			continue;
		if (debug > 2) {
			printf("got inotify event for %s's directory (%s): %x\n",
			       input_file->filename, event->name, event->mask);
		}
		if ((event->mask & IN_DELETE_SELF)) {
			input_file->inotify_wd = -1;
			inotify_watch(input_file,
				      &state->pollfds[state->input_count]);
			/* we might have been raced there with yet another
			 * re-creation, so also try to reopen even if it likely
			 * won't work: continue here */
		} else if (strcmp(event->name, input_file->dirent))
			/* was it a filename we care about? */
			continue;

		if (debug) {
			printf("trying to reopen %s\n",
					input_file->filename);
		}
		reopen_input(state, i);
	}

}

void handle_inotify(struct state *state) {
	int fd = state->pollfds[state->input_count].fd;
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

			handle_inotify_event(state, event);
		}
		xassert((char*)event == buf + n,
			"libinput event we read had a weird size: %zd / %d",
			((char*)event) - buf, n);
	}
	xassert(n >= 0, "Did not read expected amount from inotify fd: %d", n);
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


static void handle_input_event(struct state *state,
			       struct input_event *event,
			       const char *filename) {
	/* ignore non-keyboard events */
	if (event->type != 1) {
		if (debug > 2)
			print_key(event, filename, "non-keyboard event ignored");
		return;
	}

	struct key *key = NULL;
	for (int i = 0; i < state->key_count; i++) {
		if (state->keys[i].code == event->code) {
			key = &state->keys[i];
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

	handle_key(state, event, key);
}


int handle_input(struct state *state, int i) {
	int fd = state->pollfds[i].fd;
	const char *filename = state->input_files[i].filename;
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
			handle_input_event(state, event, filename);
		}
	}
	if (n < 0) {
		fprintf(stderr, "read error: %d. Trying to reopen\n", -n);
		return -1;
	}
	return 0;
}
