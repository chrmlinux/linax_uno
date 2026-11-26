#!/usr/bin/env python3
"""linaX(uno) M7 build: kernel + romfs with the shell and commands as
separate user programs. Pass 1 learns uwindow; programs are
linked at placeholder addresses for size, the romfs layout is
computed, programs are relinked at their final addresses (loop
until stable), the romfs is assembled, and the kernel is relinked
with it."""
import re, struct, subprocess, sys

CC = ["avr-gcc", "-mmcu=atmega328p", "-Os", "-Wall", "-Wextra"]
GATE = "-Wl,--section-start=.gate=0x2000"
ROMFS_SEC = "-Wl,--section-start=.romfs=0x4000"
ROMFS_BASE = 0x4000
HDR = 16
ENT = 24

PROGS = ["sh", "ls", "cat", "echo", "uname", "ps", "cc", "vm", "gpio", "i2c", "sleep"]
FILES = {
    "/etc/motd": b"Welcome to linaX(uno).\nType 'help' for commands.\n",
    "/etc/hostname": b"linaX(uno)\n",
    "/readme": b"linaX(uno) 0.7: a small Unix for ATmega328P.\n"
               b"Kernel, processes, exec, romfs - in 2K of SRAM.\n"
               b"Programs live in /bin, files in /etc.\n"
               b"Write C with echo > /tmp/a.c, cc /tmp/a.c,\n"
               b"then run /tmp/a.out (bytecode, run by /bin/vm).\n"
               b"gpio drives pins, i2c drives the I2C bus.\n"
               b"cc has wait and pin statements; sleep waits.\n"
               b"Ctrl+C stops the running program.\n",
}
DIRS = ["/", "/bin", "/etc", "/tmp"]

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("FAILED:", " ".join(cmd)); print(r.stdout); print(r.stderr)
        sys.exit(1)
    return r.stdout

def link_kernel(elf):
    run(CC + ["-o", elf, "kernel7_test.c", "switch.S", "gate.S", "romfs.S",
              GATE, ROMFS_SEC])

def nm_addr(elf, sym):
    for line in run(["avr-nm", elf]).splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == sym:
            return int(p[0], 16)
    raise SystemExit(f"symbol {sym} not in {elf}")

def sections(elf):
    secs = {}
    for line in run(["avr-objdump", "-h", elf]).splitlines():
        m = re.match(r"\s*\d+\s+(\.\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
        if m:
            secs[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16))
    return secs

def sym_addr(elf, sym):
    for line in run(["avr-objdump", "-t", elf]).splitlines():
        if line.strip().endswith(" " + sym) or ("\t" + sym) in line:
            return int(line.split()[0], 16)
    raise SystemExit(f"symbol {sym} not in {elf}")

ULD = """MEMORY {
  text (rx) : ORIGIN = %d, LENGTH = 0x3F00
  data (rw) : ORIGIN = %d, LENGTH = 256
}
SECTIONS {
  .text : { *(.text) *(.text.*) *(.rodata) *(.rodata.*) } > text
  .data : { __data_start = .; *(.data) *(.data.*) __data_end = .; } > data AT > text
  __data_load_start = LOADADDR(.data);
  .bss : { __bss_start = .; *(.bss) *(.bss.*) *(COMMON) __bss_end = .; } > data
}
"""

def build_prog(name, text_origin, window):
    open(f"{name}.ld", "w").write(ULD % (text_origin, window))
    run(["avr-gcc", "-mmcu=atmega328p", "-Os", "-Wall", "-Wextra",
         "-nostartfiles", "-o", f"{name}.elf",
         "user/ucrt.c", f"user/{name}.c", "-Wl,-T," + name + ".ld",
         f"-Wl,-Tdata,0x{window:x}"])
    subprocess.run(["avr-objcopy", "-O", "binary", f"{name}.elf", f"{name}.raw"],
                   check=True, capture_output=True)
    blob = open(f"{name}.raw", "rb").read()
    secs = sections(f"{name}.elf")
    dsize, dvma, dlma = secs.get(".data", (0, window, text_origin))
    bsize = secs.get(".bss", (0, 0, 0))[0]
    assert dvma == window, f"{name}: .data VMA 0x{dvma:x} != window 0x{window:x}"
    assert dsize + bsize <= 256, f"{name}: data+bss {dsize}+{bsize} > 256"
    entry_word = sym_addr(f"{name}.elf", "_start") // 2
    data_off = (dlma - text_origin) + HDR if dsize else HDR
    hdr = struct.pack("<HHHHHH4x", 0x5855, entry_word, data_off, dsize, bsize, HDR + len(blob))
    return hdr + blob

# pass 1: dummy romfs -> uwindow
open("romfs.img", "wb").write(struct.pack("<HH", 0x5255, 0))
link_kernel("m7t_pass1.elf")
window = nm_addr("m7t_pass1.elf", "uwindow")
print(f"uwindow: 0x{window:06x}")

# entries in fixed order
entries = [(d, 2, None) for d in DIRS]
entries += [(f"/bin/{p}", 3, p) for p in PROGS]
entries += [(f, 1, f) for f in FILES]
n_ent = len(entries)
content_start = 4 + n_ent * ENT

# layout + link loop until program origins are stable
origins = {p: 0x6000 for p in PROGS}
for it in range(4):
    images = {}
    off = content_start
    new_origins = {}
    layout = []
    for path, typ, key in entries:
        if typ == 3:
            img = build_prog(key, ROMFS_BASE + off + HDR, window) \
                if origins[key] == ROMFS_BASE + off + HDR else None
            if img is None:
                # placeholder size from previous build
                img = build_prog(key, origins[key], window)
            images[key] = img
            layout.append((path, typ, off, len(img)))
            new_origins[key] = ROMFS_BASE + off + HDR
            off += (len(img) + 1) & ~1
        elif typ == 1:
            data = FILES[key]
            layout.append((path, typ, off, len(data)))
            off += (len(data) + 1) & ~1
        else:
            layout.append((path, typ, 0, 0))
    if new_origins == origins:
        break
    origins = new_origins
else:
    raise SystemExit("layout did not stabilize")

# final program images at their true origins
for path, typ, off, size in layout:
    if typ == 3:
        key = path.rsplit("/", 1)[1]
        images[key] = build_prog(key, ROMFS_BASE + off + HDR, window)
        assert len(images[key]) == size, f"{key}: size changed after final link"

# assemble romfs
img = bytearray(b"\xff" * off)
img[0:4] = struct.pack("<HH", 0x5255, n_ent)
for i, (path, typ, o, size) in enumerate(layout):
    e = bytearray(ENT)
    nb = path.encode()
    assert len(nb) < 16, path
    e[0:len(nb)] = nb
    e[16] = typ
    struct.pack_into("<HHH", e, 18, o if typ != 2 else 0, size, 0)
    img[4 + i * ENT:4 + (i + 1) * ENT] = e
    if typ == 3:
        img[o:o + size] = images[path.rsplit("/", 1)[1]]
    elif typ == 1:
        img[o:o + size] = FILES[path]
open("romfs.img", "wb").write(bytes(img))
print(f"romfs: {len(img)} bytes, {n_ent} entries")

link_kernel("m7t_kernel.elf")
assert nm_addr("m7t_kernel.elf", "uwindow") == window, "uwindow moved"
run(["avr-objcopy", "-O", "binary", "m7t_kernel.elf", "m7t_kernel.bin"])
size = len(open("m7t_kernel.bin", "rb").read())
ksecs = sections("m7t_kernel.elf")
kram = ksecs[".data"][0] + ksecs[".bss"][0]
print(f"kernel RAM: data+bss = {kram} bytes (of 2048)")
secs = sections("m7t_kernel.elf")
assert secs[".gate"][1] == 0x2000 and secs[".romfs"][1] == ROMFS_BASE, secs
print(f"kernel bin: {size} bytes")
assert size <= 0x7E00, f"image {size} exceeds app area"
print("BUILD OK")
