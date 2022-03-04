// SPDX-License-Identifier: MIT

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <string.h>
#include <sys/inotify.h>

#include "buttond.h"


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

void reopen_input(struct input_file *input_file,
		  struct pollfd *pollfd,
		  struct pollfd *inotify) {
	if (pollfd->events) {
		close(pollfd->fd);
		pollfd->events = 0;
	}
	int fd = open(input_file->filename,
		      O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		xassert(errno == ENOENT,
			"Open %s failed: %m", input_file->filename);
		xassert(input_file->dirent,
			"Inotify not enabled for this file: aborting");
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
void handle_inotify(struct input_file *input_files, struct pollfd *pollfds,
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
