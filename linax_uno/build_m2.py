#!/usr/bin/env python3
"""unox M2 two-pass build.
Pass 1 links the kernel with dummy program images to learn the
uwindow data address. User programs are then linked against that
window and their slot addresses, given 16-byte headers, and pass 2
relinks the kernel with the real images."""
import re, struct, subprocess, sys

CC = ["avr-gcc", "-mmcu=atmega328p", "-Os", "-Wall", "-Wextra"]
SECTIONS = ["-Wl,--section-start=.gate=0x2000",
            "-Wl,--section-start=.prog1=0x4000",
            "-Wl,--section-start=.prog2=0x4800"]
SLOTS = {"prog1": 0x4000, "prog2": 0x4800}
HDR = 16

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("FAILED:", " ".join(cmd)); print(r.stdout); print(r.stderr)
        sys.exit(1)
    return r.stdout

def link_kernel(elf):
    run(CC + ["-o", elf, "kernel.c", "switch.S", "gate.S", "progimg.S"] + SECTIONS)

def nm_addr(elf, sym):
    out = run(["avr-nm", elf])
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == sym:
            return int(parts[0], 16)
    raise SystemExit(f"symbol {sym} not found in {elf}")

def sections(elf):
    out = run(["avr-objdump", "-h", elf])
    secs = {}
    for line in out.splitlines():
        m = re.match(r"\s*\d+\s+(\.\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
        if m:
            secs[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16))
    return secs

def sym_addr(elf, sym):
    out = run(["avr-objdump", "-t", elf])
    for line in out.splitlines():
        if line.strip().endswith(" " + sym) or ("\t" + sym) in line:
            return int(line.split()[0], 16)
    raise SystemExit(f"symbol {sym} not found in {elf}")

# dummy images for pass 1
for name in SLOTS:
    open(f"{name}.img", "wb").write(bytes(HDR))

link_kernel("m2_pass1.elf")
window = nm_addr("m2_pass1.elf", "uwindow")
print(f"uwindow data address: 0x{window:06x}")

uld = """MEMORY {
  text (rx) : ORIGIN = %d, LENGTH = 0x7F0
  data (rw) : ORIGIN = %d, LENGTH = 256
}
SECTIONS {
  .text : { *(.text) *(.text.*) *(.rodata) *(.rodata.*) } > text
  .data : { __data_start = .; *(.data) *(.data.*) __data_end = .; } > data AT > text
  __data_load_start = LOADADDR(.data);
  .bss : { __bss_start = .; *(.bss) *(.bss.*) *(COMMON) __bss_end = .; } > data
}
"""

for name, slot in SLOTS.items():
    text_origin = slot + HDR
    open(f"{name}.ld", "w").write(uld % (text_origin, window))
    run(["avr-gcc", "-mmcu=atmega328p", "-Os", "-Wall", "-Wextra",
         "-nostartfiles", "-o", f"{name}.elf",
         "user/ucrt.c", f"user/{name}.c", "-Wl,-T," + name + ".ld",
         f"-Wl,-Tdata,0x{window:x}"])
    raw = subprocess.run(["avr-objcopy", "-O", "binary", f"{name}.elf", f"{name}.raw"],
                         capture_output=True)
    assert raw.returncode == 0
    blob = open(f"{name}.raw", "rb").read()
    secs = sections(f"{name}.elf")
    dsize, dvma, dlma = secs.get(".data", (0, window, text_origin))
    bsize = secs.get(".bss", (0, 0, 0))[0]
    assert dvma == window, f"{name}: .data VMA 0x{dvma:x} != window 0x{window:x}"
    assert dsize + bsize <= 256, f"{name}: data+bss {dsize}+{bsize} > 256"
    entry_word = sym_addr(f"{name}.elf", "_start") // 2
    data_off = (dlma - text_origin) + HDR if dsize else HDR
    total = HDR + len(blob)
    assert total <= 0x800, f"{name}: image {total} > slot 0x800"
    hdr = struct.pack("<HHHHHH4x", 0x5855, entry_word, data_off, dsize, bsize, total)
    open(f"{name}.img", "wb").write(hdr + blob)
    print(f"{name}: entry word 0x{entry_word:x}, data {dsize} B @off {data_off}, bss {bsize} B, image {total} B")

link_kernel("m2_kernel.elf")
window2 = nm_addr("m2_kernel.elf", "uwindow")
assert window2 == window, f"uwindow moved: 0x{window:x} -> 0x{window2:x}"
run(["avr-objcopy", "-O", "binary", "m2_kernel.elf", "m2_kernel.bin"])
size = len(open("m2_kernel.bin", "rb").read())
gate = sections("m2_kernel.elf").get(".gate")
print(f"kernel bin: {size} bytes; .gate at {gate}")
assert gate and gate[1] == 0x2000, "gate not at 0x2000"
assert size <= 0x7E00, f"image {size} exceeds app area"
print("BUILD OK")
