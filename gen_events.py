#!/usr/bin/env python3

import struct
import sys
from time import clock_gettime_ns, CLOCK_MONOTONIC, sleep

def gen_event(key, state):
    ts = clock_gettime_ns(CLOCK_MONOTONIC)
    sys.stdout.buffer.write(struct.pack('LLHHI',
            int(ts / 1000000000), (int(ts/1000) % 1000000),
            1, key, state))
    sys.stdout.buffer.flush()


def main():
    # wait some for buttond init
    sleep(1)
    for command in sys.argv[1:]:
        [key, state, time] = command.split(',')
        gen_event(int(key), int(state))
        sleep(int(time)/1000)
    # ... and some more for debouncing
    sleep(1)

if __name__ == '__main__':
    main()
