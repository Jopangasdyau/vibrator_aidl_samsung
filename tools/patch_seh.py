#!/usr/bin/env python3
# Patch Samsung libvibratorservice64.so so the two Seh capability gates return
# false instead of dereferencing a null getSehHal() result (which bootloops
# system_server on a non-Samsung vibrator HAL).
#
# Each gate:  ... bl getSehHal ; ldr x0,[sp] ; ldr x8,[x0] (CRASH) ...
# Patched to: ... mov w20,#0   ; b <ok_path>  -> builds HalResult<bool>::ok(false)
import sys, struct

SRC = sys.argv[1] if len(sys.argv) > 1 else "libvibratorservice64.so"
DST = sys.argv[2] if len(sys.argv) > 2 else "libvibratorservice64.so.patched"

# (name, bl_off, ldr_off)  -- ldr is replaced with branch to bl_off-0x24+0x6c (the ok path)
GATES = [
    ("supportsHapticEngine",                0x175b8, 0x175bc, 0x17600),
    ("supportsEnhancedSamsungHapticPattern", 0x17770, 0x17774, 0x177b8),
]

# original encodings we expect (sanity)
EXP_BL_HE  = 0x9400275e   # bl getSehHal @ 0x175b8
EXP_BL_EH  = 0x940026f0   # bl getSehHal @ 0x17770
EXP_LDR    = 0xf94003e0   # ldr x0,[sp]

MOV_W20_0  = 0x52800014   # movz w20, #0

def b_insn(src, dst):
    off = dst - src
    assert off % 4 == 0
    imm26 = (off >> 2) & 0x03ffffff
    return 0x14000000 | imm26

data = bytearray(open(SRC, "rb").read())

def rd(off): return struct.unpack_from("<I", data, off)[0]
def wr(off, val): struct.pack_into("<I", data, off, val)

for name, bl_off, ldr_off, ok_off in GATES:
    bl = rd(bl_off); ldr = rd(ldr_off)
    print(f"[{name}] bl@{bl_off:#x}={bl:#010x}  ldr@{ldr_off:#x}={ldr:#010x}")
    assert bl in (EXP_BL_HE, EXP_BL_EH), f"unexpected bl encoding {bl:#x} (lib version mismatch?)"
    assert ldr == EXP_LDR, f"unexpected ldr encoding {ldr:#x}"
    wr(bl_off, MOV_W20_0)
    br = b_insn(ldr_off, ok_off)
    wr(ldr_off, br)
    print(f"    -> mov w20,#0 ({MOV_W20_0:#010x}) ; b {ok_off:#x} ({br:#010x})")

open(DST, "wb").write(data)
print(f"\nWrote {DST} ({len(data)} bytes)")
