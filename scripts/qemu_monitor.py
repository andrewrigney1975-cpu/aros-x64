#!/usr/bin/env python3
"""Sends one or more QEMU HMP monitor commands over the TCP monitor
socket (see scripts/run-qemu.sh's --snapshot mode, which opens
127.0.0.1:4444) and prints the response. Usage:

    python scripts/qemu_monitor.py "stop" "info registers" "cont"

Each argument is one monitor command, sent in order with a short pause
between them so QEMU has time to reply before the next one goes out.
"""
import socket
import sys
import time

HOST = "127.0.0.1"
PORT = 4444


def main():
    if len(sys.argv) < 2:
        print("usage: qemu_monitor.py <cmd> [cmd2] [cmd3] ...")
        sys.exit(1)

    with socket.create_connection((HOST, PORT), timeout=5) as s:
        s.settimeout(2)
        # Drain the initial banner/prompt.
        try:
            print(s.recv(65536).decode(errors="replace"), end="")
        except socket.timeout:
            pass

        for cmd in sys.argv[1:]:
            s.sendall((cmd + "\n").encode())
            time.sleep(0.3)
            try:
                while True:
                    data = s.recv(65536)
                    if not data:
                        break
                    print(data.decode(errors="replace"), end="")
            except socket.timeout:
                pass


if __name__ == "__main__":
    main()
