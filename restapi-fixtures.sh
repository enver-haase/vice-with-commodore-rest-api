#!/bin/sh
# Generates the fixtures restapi-regress.sh needs into $1 (default: ./fixtures).
#
#   test.prg  a BASIC program that prints "REST OK"
#   test.d64  a disk holding it, named HELLO
#   test.crt  an 8K cartridge that cold-starts and prints "CRT OK"
#
# Needs python3 and the c1541 built in $VICE_TREE/src (default: ./vice).
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/fixtures}
V=${VICE_TREE:-$here/vice}
mkdir -p "$out"
cd "$out"

python3 - <<'EOF'
import struct

# 10 PRINT "REST OK"
line = bytes([0x99]) + b'"REST OK"' + bytes([0])
link = struct.pack("<H", 0x801 + 4 + len(line)) + struct.pack("<H", 10)
open("test.prg", "wb").write(struct.pack("<H", 0x801) + link + line + b"\x00\x00")

# Cold-start vector at $8000; the code initialises the KERNAL just enough
# to print through CHROUT, then parks the CPU in a loop.
#   $8009  SEI
#          JSR IOINIT / RAMTAS / RESTOR / CINIT
#          CLI
#          LDX #0
#   $8019  LDA msg,X ; BEQ done ; JSR CHROUT ; INX ; BNE $8019
#   $8024  JMP $8024
#   $8027  msg "CRT OK",0
code = bytes.fromhex(
    "0980 0980 c3c2cd3830"
    "78 2084ff 2087ff 208aff 2081ff 58 a200"
    "bd2780 f006 20d2ff e8 d0f5 4c2480".replace(" ", "")) + b"CRT OK\x00"
assert code[0x27:0x2d] == b"CRT OK"
rom = code.ljust(0x2000, b"\xff")
# CRT header: generic cartridge, EXROM low, GAME high = 8K at $8000
header = (b"C64 CARTRIDGE   " + struct.pack(">IHHBB", 0x40, 0x100, 0, 0, 1)
          + bytes(6) + b"RESTTEST".ljust(32, b"\0"))
chip = b"CHIP" + struct.pack(">IHHHH", 0x2010, 0, 0, 0x8000, 0x2000) + rom
open("test.crt", "wb").write(header + chip)
EOF

rm -f test.d64
"$V/src/c1541" -format "restdisk,01" d64 test.d64 -write test.prg hello
