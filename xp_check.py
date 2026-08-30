#!/usr/bin/env python3
"""Compare kernel's printed page-table chain (serial) against QEMU's
physical RAM view (HMP xp) in the SAME boot."""
import socket, re, sys, time

serial_log = sys.argv[1] if len(sys.argv) > 1 else "run_serial.log"
mon_port = int(sys.argv[2]) if len(sys.argv) > 2 else 4447

text = open(serial_log, errors="replace").read()

# find the first fault-debug walk: TTBR0 line + walk lines
m = re.search(r"\[dbg\] TTBR0=([0-9a-f]+) TCR=([0-9a-f]+)\n"
              r"\[dbg\] walk va=([0-9a-f]+) root=([0-9a-f]+)\n"
              r"((?:\[dbg\]  L\d\[\d+\] = [0-9a-f]+ \(table [0-9a-f]+\)\n)+)"
              r"(?:\[dbg\]  -> leaf pa=([0-9a-f]+)\n)?", text)
if not m:
    print("no [dbg] walk found in serial log")
    sys.exit(1)

ttbr, tcr, va, root = (int(m.group(i), 16) for i in (1, 2, 3, 4))
chain = re.findall(r"L(\d)\[(\d+)\] = ([0-9a-f]+) \(table ([0-9a-f]+)\)",
                   m.group(5))
leaf_pa = int(m.group(6), 16) if m.group(6) else None

print(f"kernel view: TTBR0={ttbr:x} TCR={tcr:x} root={root:x} va={va:x}")
# which table PA was each descriptor read from, and the level-3 entry addr
# chain: L0 read at root+idx*8; next table = value & 0x0000fffffffff000
addr = root
for lvl, idx, val, tbl in chain:
    val, tbl = int(val, 16), int(tbl, 16)
    off = int(idx) * 8
    print(f"  L{lvl}[{idx}] @ {addr:x}+{off:x} = {val:016x} (kernel says table {tbl:x})")
    addr = tbl
if leaf_pa is not None:
    print(f"  leaf pa (kernel) = {leaf_pa:x}")

# talk to the QEMU monitor
s = socket.create_connection(("127.0.0.1", mon_port), timeout=10)
s.settimeout(3)
def mon(cmd, wait=0.3):
    s.sendall((cmd + "\n").encode())
    time.sleep(wait)
    out = b""
    s.settimeout(0.6)
    try:
        while True:
            b = s.recv(65536)
            if not b:
                break
            out += b
    except socket.timeout:
        pass
    return out.decode(errors="replace")

banner = mon("")  # drain banner
print("--- monitor connected ---")

def xp(addr, fmt):
    out = mon(f"xp /1gx {addr:#x}" if fmt == "g" else f"xp /4xb {addr:#x}")
    # HMP echoes the command then result line(s)
    lines = [l for l in out.splitlines() if l.strip() and not l.startswith("(qemu)")]
    return lines[-1] if lines else "??"

# physical truth at each descriptor address
addr = root
for lvl, idx, val, tbl in chain:
    val, tbl = int(val, 16), int(tbl, 16)
    pa = addr + int(idx) * 8
    got = xp(pa, "g")
    print(f"PHYS {pa:#x}: {got}")
    addr = tbl
if leaf_pa is not None:
    print(f"PHYS leaf {leaf_pa:#x}: {xp(leaf_pa, 'b')}")
mon("quit")
