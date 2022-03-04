# buttond - button handling daemon

simple evdev daemon that reads a /dev/input/eventX file and runs configured
action if configured key has been pressed for configured time

## Quick start

### Identify input device

Find what input device events come from. It is recommended to use
stable paths for devices, e.g. the `/dev/input/by-*` symlinks.  
For example, we have here an usb keyboard and an internal switch (gpio):
```
$ ls -l /dev/input/by-*/*
...
lrwxrwxrwx 1 root root 9 Mar  3 10:14 /dev/input/by-id/usb-0566_3029-event-kbd -> ../event2
lrwxrwxrwx 1 root root 9 Mar  3 10:09 /dev/input/by-path/platform-gpio-keys-event -> ../event1
```

If you do not recognize any name here just try them all
(three -v will print filenames associated to each event):
```
$ buttond -vvv $(printf -- "-i %s " /dev/input/event*)
```

### Identify key

Find what key you want. Running with -vv will display ignored keys.  
Run buttond with -vv and press key you want to bind
```
$ buttond -vv -i /dev/input/by-path/platform-gpio-keys-event
[11340.668] PROG1 (148) pressed: ignored
[11340.928] PROG1 (148) released: ignored
```

### Define actions/run service

Device what you want to do and do it: e.g. "restart service foo if key
pressed shortly, and poweroff if pressed longer than 10 seconds"
(default for long key is 5s):
```
$ buttond -i /dev/input/by-path/platform-gpio-keys-event \
	-s prog1 -a "rc-services foo restart" \
	-l prog1 -t 10000 -a "poweroff"
```


### Notes

 - Multiple actions for a key:
   - short keys are handled when released, if released within timeout
(default 1s). There can be only one.
   - the longest long key is handled as soon as timeout (default 5s)
elapses.  
For example, the above example will run poweroff as soon as
10s ellapsed, without waiting for key release.
   - if there are multiple long keys, time at release time is checked
and the longest action whose timeout is shorter than hold time is
executed, if any.  
For example, `-s x -a action0 -l x -a action1 -l x -t 10000 -a action2`
would:
      - keypress < 1s: run action0 on release
      - keypress >= 1s, <5s: do nothing
      - keypress >= 5s, <10s: run action1 on release
      - keypress >= 10s: run action2 10s after pushdown


 - Key source does not matter, if you have two devices which use the
same key code start buttond once for each device instead.

 - Key presses are debounced. Releasing the key for less than 10ms will
not trigger anything, and keep counting time from initial key press.  
Actions "on release" actually happen 10ms after release.

 - For devices that might disappear (e.g. usb keyboard), it's possible
to use -I instead of -i to use inotify to wait for it to come back
