#!/usr/bin/env python3
"""
serial_harness.py - phase 3 smoke test driver.

Spawns the kernel in QEMU with the console UART backed by a UNIX
socket chardev, then acts as the far end of the serial line:

  * captures everything the guest prints into a log file,
  * pushes "phase3rx\\r" into the guest once it is up, exercising
    the interrupt-driven UART RX path (echo must come back),
  * passes only if the phase banner, an uptime line and the echoed
    text all appear before the deadline.

Usage: serial_harness.py <qemu-binary> "<qemu-machine-args>" <kernel> <logfile>
"""

import os
import socket
import subprocess
import sys
import threading
import time

SOCKET = "build/ser.sock"
MARKER = b"phase3rx\r"
DEADLINE = 12.0          # seconds overall


def main():
    qemu = sys.argv[1]
    machine_args = sys.argv[2].split()
    kernel = sys.argv[3]
    logfile = sys.argv[4]

    for f in (SOCKET, logfile):
        try:
            os.unlink(f)
        except FileNotFoundError:
            pass

    cmd = ([qemu] + machine_args +
           ["-display", "none", "-monitor", "none",
            "-chardev", f"socket,id=ser0,path={SOCKET},server=on,wait=off",
            "-serial", "chardev:ser0",
            "-kernel", kernel])
    send_input = "--no-input" not in sys.argv
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)

    # wait for the chardev server to appear
    sock = None
    t0 = time.time()
    while time.time() - t0 < 5.0 and proc.poll() is None:
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(SOCKET)
            break
        except (FileNotFoundError, ConnectionRefusedError):
            sock.close() if sock else None
            sock = None
            time.sleep(0.05)
    if not sock:
        print("SMOKE TEST: FAIL (cannot reach QEMU console socket)")
        proc.kill()
        return 1

    log = open(logfile, "wb")
    buf = bytearray()
    got_input_sent = False
    last_send = 0.0
    passed = False

    def check(b):
        return (b"[OK] mobile_phone_os phase 3" in b and
                b"s uptime" in b and
                b"phase3rx" in b)

    try:
        while time.time() - t0 < DEADLINE:
            sock.settimeout(0.25)
            try:
                data = sock.recv(4096)
            except socket.timeout:
                data = b""
            if data:
                buf += data
                log.write(data)
                log.flush()

            # feed RX input only once the guest armed its RX path,
            # and keep retrying until the echo proves it landed
            if (send_input and
                    (not got_input_sent or time.time() - last_send > 2.0)
                    and b"echo armed" in buf and b"phase3rx" not in buf):
                sock.sendall(MARKER)
                if not got_input_sent:
                    print(f"harness: RX input sent at "
                          f"t={time.time()-t0:.2f}s", file=sys.stderr)
                got_input_sent = True
                last_send = time.time()

            if check(buf):
                passed = True
                break
    finally:
        sock.close()
        log.close()
        proc.kill()
        proc.wait()

    if passed:
        print("SMOKE TEST: PASS")
        return 0

    print("SMOKE TEST: FAIL")
    print(f"  banner : {b'[OK] mobile_phone_os phase 3' in buf}")
    print(f"  uptime : {b's uptime' in buf}")
    print(f"  rx echo: {b'phase3rx' in buf}")
    with open(logfile, "rb") as f:
        tail = f.read()[-800:].decode(errors="replace")
    print("--- last serial output ---")
    print(tail)
    return 1


if __name__ == "__main__":
    sys.exit(main())
