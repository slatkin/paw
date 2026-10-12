#!/usr/bin/env python3

"""Check the kitty graphics protocol support of the terminal.

Writes one graphics command (given as an argument) to the tty, followed
by a primary device attributes query (CSI c). Prints every reply it
reads, up to and including the DA1 reply.

Run inside a paw (or foot) window.
"""

import os
import re
import select
import sys
import termios
import tty

DA1 = re.compile(rb'\x1b\[[?>]?[0-9;]*c')
TIMEOUT = 2.0


def read_replies(tty_fd: int) -> bytes:
    reply = b''
    while True:
        ready, _, _ = select.select([tty_fd], [], [], TIMEOUT)
        if not ready:
            break

        data = os.read(tty_fd, 1)
        if not data:
            break

        reply += data
        if DA1.search(reply):
            break
    return reply


def main() -> None:
    if len(sys.argv) != 2:
        print(f'usage: {sys.argv[0]} <graphics-command>', file=sys.stderr)
        sys.exit(2)

    command = sys.argv[1].encode()

    tty_fd = os.open('/dev/tty', os.O_RDWR)
    try:
        attrs = termios.tcgetattr(tty_fd)
        try:
            tty.setraw(tty_fd)
            os.write(tty_fd, b'\x1b_G' + command + b'\x1b\\' + b'\x1b[c')
            sys.stdout.buffer.write(read_replies(tty_fd))
            sys.stdout.buffer.flush()
        finally:
            termios.tcsetattr(tty_fd, termios.TCSADRAIN, attrs)
    finally:
        os.close(tty_fd)


if __name__ == '__main__':
    main()
