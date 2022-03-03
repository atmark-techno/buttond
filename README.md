# buttond - button handling daemon

simple evdev daemon that reads a /dev/input/eventX file and runs configured
action if configured key has been pressed for configured time

## Quick start

### Identify input device

Find what input device events come from. It is recommended to use
stable paths for devices, e.g. the /dev/input/by-* symlinks.
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
[2467.960] 148 pressed: ignored
[2468.040] 148 released: ignored
```

### Define actions/run service

Device what you want to do and do it: e.g. "restart service foo if key
pressed shortly, and poweroff if pressed longer than 10 seconds"
(default for long key is 5s):
```
$ buttond -i /dev/input/by-path/platform-gpio-keys-event \
	-s 148 -a "rc-services foo restart" \
	-l 148 -t 10000 -a "poweroff"
```


### Notes

 - it is not possible to define multiple with same mode, even if
long trigger time is different, because the action happens as soon
as time has passed and not when key is released.

 - key source does not matter, if you have two devices which use the
same key code start buttond once for each device instead.
