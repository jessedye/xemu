#!/usr/bin/env python3
"""Restore a snapshot through xemu's monitor once the machine is up.

Loading at launch with -loadvm cannot work when a controller is bound: xemu
hot-plugs it after the machine is built, so the saved USB topology does not
exist yet and the restore fails on an unknown usb-hub section.
"""
import json
import socket
import sys
import time


def main():
    sock_path, name = sys.argv[1], sys.argv[2]

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for _ in range(25):
        try:
            s.connect(sock_path)
            break
        except OSError:
            time.sleep(1)
    else:
        print("bench: monitor unreachable; continuing without the snapshot")
        return 0

    f = s.makefile("rw", encoding="utf-8", newline="\n")

    def command(**payload):
        f.write(json.dumps(payload) + "\n")
        f.flush()
        while True:
            line = f.readline()
            if not line:
                return {}
            reply = json.loads(line)
            if "event" not in reply:
                return reply

    f.readline()
    command(execute="qmp_capabilities")
    reply = command(execute="human-monitor-command",
                    arguments={"command-line": "loadvm %s" % name})
    out = (reply.get("return") or "").strip()
    print("bench: loadvm %s -> %s" % (name, out if out else "ok"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
