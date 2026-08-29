#!/usr/bin/env python3
"""
serial_harness.py - smoke test driver (phase number via argv).

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
DEADLINE = 12.0          # seconds overall


def main():
    qemu = sys.argv[1]
    machine_args = sys.argv[2].split()
    kernel = sys.argv[3]
    logfile = sys.argv[4]
    phase = int(sys.argv[5]) if len(sys.argv) > 5 else 3
    marker = f"phase{phase}rx\r".encode()
    echo_tag = f"phase{phase}rx".encode()
    banner = f"[OK] mobile_phone_os phase {phase}".encode()

    # phase 15+: the compositor owns fb0/event0, so the guest needs
    # the virtio display + input devices even headless (the GPU
    # renders into an off-screen host surface under -display none)
    ui_devices = ["-device", "virtio-gpu-device",
                  "-device", "virtio-tablet-device",
                  "-device", "virtio-keyboard-device"] \
        if phase >= 15 else []

    # the UI battery waits for init -> compositor -> app chain
    deadline_s = 30.0 if phase >= 15 else DEADLINE

    for f in (SOCKET, logfile):
        try:
            os.unlink(f)
        except FileNotFoundError:
            pass

    cmd = ([qemu] + machine_args +
           ["-display", "none", "-monitor", "none",
            "-chardev", f"socket,id=ser0,path={SOCKET},server=on,wait=off",
            "-serial", "chardev:ser0",
            "-kernel", kernel] + ui_devices)
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
        ok = (banner in b and b"s uptime" in b and echo_tag in b)
        if phase >= 4:
            ok = ok and (b"ping: round" in b and b"pong: round" in b)
        if phase >= 5:
            ok = ok and (b"running at EL0" in b and
                         b"hello: exiting 42" in b and
                         b"exited with code 42" in b)
        if phase >= 15:
            ok = ok and (
                b"selftest: ui ok" in b and
                b"[uitest] protocol ok" in b and
                b"[ui] unlock ok" in b and
                b"[ui] launch dialer" in b and
                b"[dialer] ready" in b and
                b"[ui] banner: SMS" in b)
        if phase >= 16:
            ok = ok and (
                b"selftest: release ok" in b and
                b"[perf] boot" in b and
                b"watchdog: armed" in b and
                b"ab: automatic rollback fired" in b)
        return ok

    try:
        while time.time() - t0 < deadline_s:
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
                    and b"echo armed" in buf and echo_tag not in buf):
                sock.sendall(marker)
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
    print(f"  banner : {banner in buf}")
    print(f"  uptime : {b's uptime' in buf}")
    print(f"  rx echo: {echo_tag in buf}")
    if phase >= 4:
        print(f"  threads: {b'ping: round' in buf and b'pong: round' in buf}")
    if phase >= 5:
        print(f"  el0 run : {b'running at EL0' in buf}")
        print(f"  u exit  : {b'hello: exiting 42' in buf}")
        print(f"  k reaped: {b'exited with code 42' in buf}")
    if phase >= 15:
        print(f"  ui proto : {b'[uitest] protocol ok' in buf}")
        print(f"  ui unlock: {b'[ui] unlock ok' in buf}")
        print(f"  ui launch: {b'[ui] launch dialer' in buf}")
        print(f"  ui banner: {b'[ui] banner: SMS' in buf}")
        print(f"  ui selft : {b'selftest: ui ok' in buf}")
    if phase >= 16:
        print(f"  rel perf : {b'[perf] boot' in buf}")
        print(f"  rel wdt  : {b'watchdog: armed' in buf}")
        print(f"  rel ab   : {b'ab: automatic rollback fired' in buf}")
        print(f"  rel selft: {b'selftest: release ok' in buf}")
    with open(logfile, "rb") as f:
        tail = f.read()[-800:].decode(errors="replace")
    print("--- last serial output ---")
    print(tail)
    return 1


if __name__ == "__main__":
    sys.exit(main())
