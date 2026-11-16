#!/usr/bin/env python3
"""Generate synthetic NSO-format modules for the Asahi host's loader tests.

These are not Nintendo modules and contain no game data. They exercise:
  - the NSO memory image layout (text at 0, MOD0 pointer at +4),
  - MOD0 / dynamic section parsing and symbol lookup,
  - DT_INIT and the bootstrap symbols main.c calls,
  - the Linux pthread TSD slot discovery and key page,
  - native AArch64 entry into `nnMain`.

`--vulkan` additionally imports `vkCreateInstance` through a JUMP_SLOT
relocation and calls it from `nnMain`, exercising the host's import table,
generated Vulkan forwarding stubs and real Mesa instance creation.

It deliberately has no SVC/gate instructions: the SVC path is covered by the
real rewriter and by per-game runs.
"""
import argparse
import os
import struct

TEXT_END = 0x1000
RO_OFF = 0x1000
DATA_OFF = 0x2000
KEY_PAGE = 0x4000
PATCH_OFF = 0x10000
IMAGE_SIZE = 0x14000


def adrp(rd, pc, target):
    delta = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12
    return 0x90000000 | ((delta & 3) << 29) | (((delta >> 2) & 0x7FFFF) << 5) | rd


def add_imm(rd, rn, imm):
    return 0x91000000 | ((imm & 0xFFF) << 10) | (rn << 5) | rd


def ldr_x(rt, rn, imm=0):
    return 0xF9400000 | (((imm // 8) & 0xFFF) << 10) | (rn << 5) | rt


def movz_x(rd, imm):
    return 0xD2800000 | ((imm & 0xFFFF) << 5) | rd


def blr(rn):
    return 0xD63F0000 | (rn << 5)


RET = 0xD65F03C0


def build(path, vulkan=False, nvn=False):
    img = bytearray(IMAGE_SIZE)
    # The host reads the MOD0 offset from image+4, as the NSO image layout does.
    struct.pack_into("<II", img, 0, 0xD503201F, RO_OFF)  # nop; MOD0 offset
    for off in range(0x100, 0x140, 4):
        struct.pack_into("<I", img, off, RET)

    img[RO_OFF:RO_OFF + 4] = b"MOD0"
    struct.pack_into("<i", img, RO_OFF + 4, 0x20)  # dynamic section, relative

    names = [
        "_init_libc0", "nnosInitialize", "_init_libc1",
        "nninitInitializeSdkModule", "nninitStartup", "_init_libc2",
        "nnMain", "_init",
        "_ZN2nn2os11SetHostArgcEi", "_ZN2nn2os11SetHostArgvEPPc",
    ]
    if vulkan:
        names.append("vkCreateInstance")
    if nvn:
        names.append("nvnBootstrapLoader")
    symtab = 0x2000
    nsym = len(names) + 1
    strtab = symtab + nsym * 24

    strings = bytearray(b"\0")
    for name in names:
        strings += name.encode() + b"\0"
    while len(strings) % 8:
        strings += b"\0"

    syms = bytearray(b"\0" * 24)
    for i, name in enumerate(names):
        st_name = 1 + sum(len(x) + 1 for x in names[:i])
        if (vulkan and name == "vkCreateInstance") or (nvn and name == "nvnBootstrapLoader"):
            syms += struct.pack("<IBBHQQ", st_name, 0x12, 0, 0, 0, 0)  # undefined, resolved by HLE
            if name == "vkCreateInstance": vk_sym_index = i + 1
        else:
            value = 0x108 if (vulkan and name == "nnMain") else 0x110 if name == "nnMain" else 0x100
            syms += struct.pack("<IBBHQQ", st_name, 0x12, 0, 1, value, 4)
    img[symtab:symtab + len(syms)] = syms
    img[strtab:strtab + len(strings)] = strings

    dyn = []
    def tag(t, v):
        dyn.append(struct.pack("<qQ", t, v))
    tag(6, symtab)          # DT_SYMTAB
    tag(5, strtab)          # DT_STRTAB
    tag(12, 0x100)          # DT_INIT
    tag(25, 0)              # DT_INIT_ARRAY
    tag(27, 0)              # DT_INIT_ARRAYSZ

    if vulkan:
        got = 0x8000
        lit_ci = 0x8010
        lit_out = 0x8018
        lit_got = 0x8020
        ci = 0x8100
        out = 0x8200
        rela = 0x8300
        jmprel = 0x8400
        struct.pack_into("<I", img, ci, 1)  # VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
        # nnMain: call vkCreateInstance(ci, NULL, &out), then return.
        code = [
            (0x108, 0xA9BF7BFD),                    # stp x29, x30, [sp, #-16]!
            (0x10C, 0x910003FD),                    # mov x29, sp
            (0x110, adrp(9, 0x110, lit_ci)), (0x114, add_imm(9, 9, lit_ci & 0xFFF)),
            (0x118, ldr_x(0, 9)), (0x11C, adrp(9, 0x11C, lit_out)),
            (0x120, add_imm(9, 9, lit_out & 0xFFF)), (0x124, ldr_x(2, 9)),
            (0x128, movz_x(1, 0)), (0x12C, adrp(9, 0x12C, lit_got)),
            (0x130, add_imm(9, 9, lit_got & 0xFFF)), (0x134, ldr_x(9, 9)),
            (0x138, ldr_x(9, 9)), (0x13C, blr(9)),
            (0x140, 0xA8C17BFD),                    # ldp x29, x30, [sp], #16
            (0x144, RET),
        ]
        for off, word in code:
            struct.pack_into("<I", img, off, word)
        # Literals hold absolute runtime addresses; the loader applies RELATIVE.
        relas = [
            (lit_ci, 1027, ci), (lit_out, 1027, out), (lit_got, 1027, got),
        ]
        for off, typ, addend in relas:
            struct.pack_into("<QQq", img, rela, off, typ, addend)
            rela += 24
        # JUMP_SLOT for the imported function.
        struct.pack_into("<QQq", img, jmprel, got, (vk_sym_index << 32) | 1026, 0)
        tag(7, 0x8300)      # DT_RELA
        tag(8, 3 * 24)      # DT_RELASZ
        tag(23, 0x8400)     # DT_JMPREL
        tag(2, 24)          # DT_PLTRELSZ

    tag(0, 0)               # DT_NULL
    img[RO_OFF + 0x20:RO_OFF + 0x20 + len(b"".join(dyn))] = b"".join(dyn)

    os.makedirs(path, exist_ok=True)
    with open(os.path.join(path, "main.patched.bin"), "wb") as f:
        f.write(img)
    with open(os.path.join(path, "main.fixups.tsv"), "w") as f:
        f.write(f"""# PATCH_OFF\t{PATCH_OFF:#x}
# KEY_PAGE_OFF\t{KEY_PAGE:#x}
# KEY_PAGE_SIZE\t0x4000
# TEXT_END\t{TEXT_END:#x}
# RO_OFF\t{RO_OFF:#x}
# DATA_OFF\t{DATA_OFF:#x}
# KEY_K18_IDX\t0
# KEY_KTLS_IDX\t1
# KEY_KTLR_IDX\t2
# KEY_KGATE_IDX\t3
# BUILD_ID\t00112233445566778899aabbccddeeff
""")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("directory", help="output directory for main.patched.bin and main.fixups.tsv")
    ap.add_argument("--vulkan", action="store_true", help="import and call vkCreateInstance")
    ap.add_argument("--nvn", action="store_true", help="import nvnBootstrapLoader (for profile/build detection)")
    args = ap.parse_args()
    build(args.directory, args.vulkan, args.nvn)
    print(f"synthetic module written to {args.directory}" + (" (Vulkan import)" if args.vulkan else "") + (" (NVN import)" if args.nvn else ""))


if __name__ == "__main__":
    main()
