#!/usr/bin/env python3
"""linaX(uno) M8 build: kernel + romfs. The shell is its own
program; the ten commands are applets of one multicall
(busybox) image, with one romfs entry per applet name all
pointing at that image. Pass 1 learns uwindow; programs are
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

PROGS = ["sh", "busybox"]
APPLETS = ["ls", "cat", "echo", "uname", "ps", "cc", "vm",
           "gpio", "i2c", "sleep"]
FILES = {
    "/etc/motd": b"Welcome to linaX(uno).\nType 'help' for commands.\n",
    "/etc/hostname": b"linaX(uno)\n",
    "/readme": b"linaX(uno) 0.8: a small Unix for ATmega328P.\n"
               b"Kernel, processes, exec, romfs - in 2K of SRAM.\n"
               b"Programs live in /bin, files in /etc.\n"
               b"Write C with echo > /tmp/a.c, cc /tmp/a.c,\n"
               b"then run /tmp/a.out (bytecode, run by /bin/vm).\n"
               b"gpio drives pins, i2c drives the I2C bus.\n"
               b"cc has wait and pin statements; sleep waits.\n"
               b"Ctrl+C stops the running program.\n"
               b"Commands are applets of one busybox image.\n",
}
DIRS = ["/", "/bin", "/etc", "/tmp"]

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("FAILED:", " ".join(cmd)); print(r.stdout); print(r.stderr)
        sys.exit(1)
    return r.stdout

def link_kernel(elf):
    run(CC + ["-o", elf, "kernel8_test.c", "switch.S", "gate.S", "romfs.S",
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
link_kernel("m8t_pass1.elf")
window = nm_addr("m8t_pass1.elf", "uwindow")
print(f"uwindow: 0x{window:06x}")

# entries in fixed order; applet entries share the busybox image
entries = [(d, 2, None) for d in DIRS]
entries += [("/bin/sh", 3, "sh")]
entries += [(f"/bin/{a}", 3, "busybox") for a in APPLETS]
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
    key_layout = {}
    for path, typ, key in entries:
        if typ == 3:
            if key in key_layout:
                o0, sz0 = key_layout[key]
                layout.append((path, typ, key, o0, sz0))
                continue
            img = build_prog(key, ROMFS_BASE + off + HDR, window) \
                if origins[key] == ROMFS_BASE + off + HDR else None
            if img is None:
                # placeholder size from previous build
                img = build_prog(key, origins[key], window)
            images[key] = img
            layout.append((path, typ, key, off, len(img)))
            key_layout[key] = (off, len(img))
            new_origins[key] = ROMFS_BASE + off + HDR
            off += (len(img) + 1) & ~1
        elif typ == 1:
            data = FILES[key]
            layout.append((path, typ, key, off, len(data)))
            off += (len(data) + 1) & ~1
        else:
            layout.append((path, typ, key, 0, 0))
    if new_origins == origins:
        break
    origins = new_origins
else:
    raise SystemExit("layout did not stabilize")

# final program images at their true origins
done = set()
for path, typ, key, off, size in layout:
    if typ == 3 and key not in done:
        images[key] = build_prog(key, ROMFS_BASE + off + HDR, window)
        assert len(images[key]) == size, f"{key}: size changed after final link"
        done.add(key)

# assemble romfs
img = bytearray(b"\xff" * off)
img[0:4] = struct.pack("<HH", 0x5255, n_ent)
for i, (path, typ, key, o, size) in enumerate(layout):
    e = bytearray(ENT)
    nb = path.encode()
    assert len(nb) < 16, path
    e[0:len(nb)] = nb
    e[16] = typ
    struct.pack_into("<HHH", e, 18, o if typ != 2 else 0, size, 0)
    img[4 + i * ENT:4 + (i + 1) * ENT] = e
    if typ == 3:
        img[o:o + size] = images[key]
    elif typ == 1:
        img[o:o + size] = FILES[key]
open("romfs.img", "wb").write(bytes(img))
print(f"romfs: {len(img)} bytes, {n_ent} entries")

link_kernel("m8t_kernel.elf")
assert nm_addr("m8t_kernel.elf", "uwindow") == window, "uwindow moved"
run(["avr-objcopy", "-O", "binary", "m8t_kernel.elf", "m8t_kernel.bin"])
size = len(open("m8t_kernel.bin", "rb").read())
ksecs = sections("m8t_kernel.elf")
kram = ksecs[".data"][0] + ksecs[".bss"][0]
print(f"kernel RAM: data+bss = {kram} bytes (of 2048)")
secs = sections("m8t_kernel.elf")
assert secs[".gate"][1] == 0x2000 and secs[".romfs"][1] == ROMFS_BASE, secs
print(f"kernel bin: {size} bytes")
assert size <= 0x7E00, f"image {size} exceeds app area"
print("BUILD OK")
