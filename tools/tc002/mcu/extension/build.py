#!/usr/bin/env python3
"""Build the AWTRIX MCU extension, build 14, from build 13 and registers.S.

Build 13 (build13.bin) is the released PCM and stream extension; its source is not in this
repository. Build 14 is build 13 byte for byte, with the LED driver table commands of registers.S
linked right after it and the AWMC identity raised to version 14 with feature 0x10:

    build.py                      build and compare with ../release/extension.bin (exit 1 if not equal)
    build.py --write              write ../release/extension.bin and its manifest.json fields
    build.py --write --base BASE.fot --assemble TOOL
                                  also image_bytes and image_sha256 (TOOL: awtrix-tc002-mcu-assemble)
                                  and the pins in ../release.json

Needs clang, ld.lld, llvm-objcopy and llvm-nm with RISC-V support. build13.bin and the release are
under the MIT licence in ../release/LICENSE.txt.
"""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
MCU = HERE.parent
RELEASE = MCU / 'release'
BUILD13 = HERE / 'build13.bin'
BUILD13_SHA256 = 'f49bea97f5686ed289b30504f6241683901b3fcadf6c925fcd7842c1a71288ac'
BUILD13_BYTES = 3490
LOAD = 0x10007a00              # where the stock firmware sees the extension
IDENTITY = 0xd92               # AWMC, abi u8, features u8, family u16, version u32, tag u32
HANDLERS = 0xdc0               # registers.S starts here (0x100087c0)
VERSION = 14
FEATURES = 0x0b | 0x10         # build 13's bits and the driver table
# Must equal kDriverTable*Entry in src/platform/tc002/mcu/McuPatch.h.
ENTRIES = {'write_register': 0x100087c0, 'read_register': 0x10008848, 'clear_override': 0x100088e0}
MAX_BYTES = 4096


def tool(name):
    path = shutil.which(name)
    if not path:
        raise SystemExit(f'{name} not found: the build needs clang, ld.lld, llvm-objcopy and llvm-nm')
    return path


def link_handlers(work):
    obj, elf, raw = work / 'registers.o', work / 'registers.elf', work / 'registers.bin'
    subprocess.run([tool('clang'), '--target=riscv32', '-march=rv32imc', '-mabi=ilp32', '-c',
                    str(HERE / 'registers.S'), '-o', str(obj)], check=True)
    subprocess.run([tool('ld.lld'), '-m', 'elf32lriscv', '--no-relax', f'-Ttext={LOAD + HANDLERS:#x}',
                    '-e', 'handlers', '-o', str(elf), str(obj)], check=True)
    subprocess.run([tool('llvm-objcopy'), '-O', 'binary', str(elf), str(raw)], check=True)
    symbols = {}
    listing = subprocess.run([tool('llvm-nm'), str(elf)], check=True, capture_output=True, text=True).stdout
    for line in listing.splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    for name, address in ENTRIES.items():
        if symbols.get(name) != address:
            raise SystemExit(f'{name} links at {symbols.get(name, 0):#x}, McuPatch expects {address:#x}: '
                             'update ENTRIES here and kDriverTable*Entry in McuPatch.h together')
    return raw.read_bytes()


def build():
    base = BUILD13.read_bytes()
    if len(base) != BUILD13_BYTES or hashlib.sha256(base).hexdigest() != BUILD13_SHA256:
        raise SystemExit(f'{BUILD13} is not build 13')
    if base[IDENTITY:IDENTITY + 4] != b'AWMC' or struct.unpack_from('<I', base, IDENTITY + 8)[0] != 13:
        raise SystemExit('build 13 identity not found where expected')
    with tempfile.TemporaryDirectory() as work:
        code = link_handlers(Path(work))
    out = bytearray(base)
    out += bytes(HANDLERS - len(out))
    out += code
    out[IDENTITY + 5] = FEATURES
    struct.pack_into('<I', out, IDENTITY + 8, VERSION)
    struct.pack_into('<I', out, IDENTITY + 12, 0)
    tag = zlib.crc32(bytes(out)) or 1
    struct.pack_into('<I', out, IDENTITY + 12, tag)
    if len(out) > MAX_BYTES:
        raise SystemExit(f'extension is {len(out)} bytes, more than {MAX_BYTES}')
    if bytes(out).count(b'AWMC') != 1:
        raise SystemExit('the extension must contain AWMC exactly once')
    return bytes(out), tag


def write_json(path, data):
    text = json.dumps(data, indent=2) + '\n'
    path.write_text(text, encoding='utf-8')
    return hashlib.sha256(text.encode('utf-8')).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--write', action='store_true', help='write the release instead of comparing')
    parser.add_argument('--base', type=Path, help='vendor original V1.0.17 (.fot), never committed')
    parser.add_argument('--assemble', type=Path, help='awtrix-tc002-mcu-assemble')
    args = parser.parse_args()

    extension, tag = build()
    target = RELEASE / 'extension.bin'
    if not args.write:
        if target.read_bytes() != extension:
            print(f'{target} differs from the build ({len(extension)} bytes, tag {tag:#x})', file=sys.stderr)
            return 1
        print(f'{target}: build {VERSION}, {len(extension)} bytes, tag {tag:#x}, matches the build')
        return 0

    target.write_bytes(extension)
    digest = hashlib.sha256(extension).hexdigest()
    manifest_path = RELEASE / 'manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    manifest.update({'features': FEATURES, 'version': VERSION, 'build_tag': tag, 'bytes': len(extension),
                     'sha256': digest})
    if args.base and args.assemble:
        result = subprocess.run([str(args.assemble), str(args.base), str(target)], check=True,
                                capture_output=True, text=True).stdout.split()
        manifest.update({'image_bytes': int(result[0]), 'image_sha256': result[1]})
    manifest_sha = write_json(manifest_path, manifest)
    pin_path = MCU / 'release.json'
    pin = json.loads(pin_path.read_text(encoding='utf-8'))
    pin.update({'version': VERSION, 'sha256': digest, 'image_sha256': manifest['image_sha256'],
                'manifest_sha256': manifest_sha})
    write_json(pin_path, pin)
    print(f'build {VERSION}: {len(extension)} bytes, SHA-256 {digest}, tag {tag:#x}, '
          f'image {manifest["image_bytes"]} bytes {manifest["image_sha256"]}')
    if not (args.base and args.assemble):
        print('image_bytes and image_sha256 were left as they were: pass --base and --assemble', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
