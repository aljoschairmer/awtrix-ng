"""MCU extension build 14: the LED driver table handlers run in a RISC-V emulator (unicorn).

The firmware routines they call are stand-ins that record their arguments. With AWTRIX_MCU_BASE
set to the vendor original V1.0.17, the table reader 0x10004b06 runs as the real code instead; the
setter 0x10004adc uses a Bluetrum-specific indexed store that unicorn cannot run, so it stays a
stand-in. Skipped without unicorn; the release check is skipped without the LLVM tools.
"""
import hashlib
import lzma
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / 'extension'))
import build  # noqa: E402

try:
    from unicorn import Uc, UcError, UC_ARCH_RISCV, UC_MODE_RISCV32, UC_HOOK_CODE
    from unicorn.riscv_const import (UC_RISCV_REG_A0, UC_RISCV_REG_A1, UC_RISCV_REG_A2, UC_RISCV_REG_PC,
                                     UC_RISCV_REG_RA, UC_RISCV_REG_S0, UC_RISCV_REG_S1, UC_RISCV_REG_S2,
                                     UC_RISCV_REG_SP, UC_RISCV_REG_TP)
except ImportError:  # pragma: no cover - optional
    Uc = None

TABLE = 0x15794                # red; green +0x44, blue +0x88
FACTORY = {0x0c: (8, 24, 48), 0x14: (20, 34, 50), 0x16: (54, 50, 46)}
FLASH_SAVE = 0x10004868        # what 0x12 does and the table commands must not
SEND, CONSUME = 0x10003a14, 0x10003ba0
DRIVER_SEND, SETTINGS_PUT, SETTINGS_SAVE = 0x10003af4, 0x10005458, 0x100053da
TABLE_SET, TABLE_GET = 0x10004adc, 0x10004b06
FRAME, STACK = 0x12000, 0x13f00


def unpack(pkg):
    """The flash image inside a .fot/.pot package (as McuPatch.cpp unpack())."""
    word = lambda b, p: struct.unpack_from('<I', b, p)[0]
    data = bytearray(pkg[512:word(pkg, 0x78)])
    key0 = word(pkg, 0x90) ^ 0x474b5058
    for p in range(0, len(data), 512):
        key = key0 ^ ((p + 512) >> 9)
        for i in range(p, min(p + 512, len(data))):
            data[i] ^= key & 0xff
            for _ in range(8):
                key = (key >> 1) ^ (0xa3000000 if key & 1 else 0)
    filters = [{'id': lzma.FILTER_LZMA1, 'dict_size': 16384, 'lc': 0, 'lp': 0, 'pb': 2}]
    return lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filters).decompress(
        bytes(data[5:]), max_length=word(pkg, 0x94))


def vendor_base():
    path = os.environ.get('AWTRIX_MCU_BASE')
    if not path:
        return None
    pkg = Path(path).read_bytes()
    if hashlib.sha256(pkg).hexdigest() != build.json.loads((HERE / 'release.json').read_text())['base_sha256']:
        raise AssertionError('AWTRIX_MCU_BASE is not the original V1.0.17')
    return pkg


def vendor_code():
    """The decoded stock code region, or None."""
    pkg = vendor_base()
    return unpack(pkg)[0x2200:0x2200 + 0x7a00] if pkg else None


@unittest.skipIf(Uc is None, 'unicorn is not installed')
class Handlers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not all(shutil.which(t) for t in ('clang', 'ld.lld', 'llvm-objcopy', 'llvm-nm')):
            raise unittest.SkipTest('LLVM RISC-V tools are not installed')
        cls.extension, _ = build.build()
        cls.code = vendor_code()

    def setUp(self):
        mu = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
        mu.mem_map(0x0, 0x20000)
        mu.mem_map(0x10000000, 0x10000)
        if self.code:
            mu.mem_write(0x10000000, self.code)
        mu.mem_write(build.LOAD, self.extension)
        for reg in range(0x02, 0x21):
            values = FACTORY.get(reg, (0, 0, 0))
            for colour, value in enumerate(values):
                mu.mem_write(TABLE + colour * 0x44 + (reg - 2) * 2, struct.pack('<H', reg << 8 | value))
        # Stand-ins end in "ret"; the hook records their arguments and does their work.
        stubs = [SEND, DRIVER_SEND, SETTINGS_PUT, SETTINGS_SAVE, FLASH_SAVE, TABLE_SET]
        if not self.code:
            stubs.append(TABLE_GET)
        for address in stubs:
            mu.mem_write(address, b'\x82\x80')
        self.mu, self.calls, self.errors = mu, [], []
        mu.hook_add(UC_HOOK_CODE, self.hook)

    def regs(self, *ids):
        return [self.mu.reg_read(r) for r in ids]

    def hook(self, mu, address, size, _):
        a0, a1, a2 = self.regs(UC_RISCV_REG_A0, UC_RISCV_REG_A1, UC_RISCV_REG_A2)
        if address == SEND:
            self.calls.append(('send', a0, bytes(mu.mem_read(a1, a2)) if a2 else b''))
        elif address == DRIVER_SEND:
            self.calls.append(('drivers', a0, a1, a2))
        elif address == SETTINGS_PUT:
            self.calls.append(('settings', bytes(mu.mem_read(a0, a2)), a1))
        elif address == SETTINGS_SAVE:
            self.calls.append(('save',))
        elif address == FLASH_SAVE:
            self.errors.append('a table command wrote the override to flash')
        elif address == TABLE_SET or (address == TABLE_GET and not self.code):
            self.fake_table(address, a0, a1, a2)
        elif address < build.LOAD and not self.code:
            self.errors.append(f'unexpected call to {address:#x}')

    def fake_table(self, address, a0, a1, a2):
        """The two table routines of V1.0.17, for runs without the vendor original."""
        mu = self.mu
        if address == TABLE_SET:
            reg = ((a0 >> 8) - 2) & 0xff
            if reg <= 0x1e:
                for colour, word in enumerate((a0, a1, a2)):
                    mu.mem_write(TABLE + colour * 0x44 + reg * 2, struct.pack('<H', word & 0xffff))
            return
        reg = ((struct.unpack('<H', mu.mem_read(a0, 2))[0] >> 8) - 2) & 0xff
        if reg > 0x1e:
            mu.reg_write(UC_RISCV_REG_A0, 0)
            return
        for colour, pointer in enumerate((a0, a1, a2)):
            mu.mem_write(pointer, bytes(mu.mem_read(TABLE + colour * 0x44 + reg * 2, 2)))
        mu.reg_write(UC_RISCV_REG_A0, 1)

    def run_command(self, command, payload):
        frame = bytes([0xff, 0x55, command, len(payload)]) + bytes(payload) + b'\0\0'
        self.mu.mem_write(FRAME, frame)
        self.mu.reg_write(UC_RISCV_REG_S0, FRAME)
        self.mu.reg_write(UC_RISCV_REG_S2, len(payload))
        self.mu.reg_write(UC_RISCV_REG_S1, len(frame))
        self.mu.reg_write(UC_RISCV_REG_SP, STACK)
        self.mu.reg_write(UC_RISCV_REG_TP, 0x13000)
        self.mu.reg_write(UC_RISCV_REG_RA, 0)
        entry = {0x09: 'write_register', 0x0a: 'read_register', 0x0b: 'clear_override'}[command]
        try:
            self.mu.emu_start(build.ENTRIES[entry], CONSUME, count=5000)
        except UcError as error:
            self.fail(f'0x{command:02x} crashed at {self.mu.reg_read(UC_RISCV_REG_PC):#x}: {error}')
        self.assertEqual(self.errors, [])
        self.assertEqual(self.regs(UC_RISCV_REG_PC, UC_RISCV_REG_S1, UC_RISCV_REG_SP), [CONSUME, len(frame), STACK],
                         'ends at the dispatcher with the frame length and the stack intact')
        return self.calls

    def table(self, reg):
        return tuple(struct.unpack('<H', self.mu.mem_read(TABLE + c * 0x44 + (reg - 2) * 2, 2))[0] & 0xff
                     for c in range(3))

    def test_write_sets_table_and_drivers_without_flash(self):
        calls = self.run_command(0x09, [0x0c, 0x06, 0x0c, 0x12, 0x0c, 0x24])
        self.assertEqual(self.table(0x0c), (6, 18, 36))
        self.assertEqual(calls, [('drivers', 0x0c06, 0x0c12, 0x0c24),
                                 ('send', 0x09, bytes([0x0c, 0x06, 0x0c, 0x12, 0x0c, 0x24]))])

    def test_write_refuses_bad_requests(self):
        for payload in ([0x21, 0, 0x21, 0, 0x21, 0], [0x01, 0, 0x01, 0, 0x01, 0], [0x0c, 1, 0x14, 1, 0x0c, 1],
                        [0x0c, 1, 0x0c, 1]):
            self.setUp()
            calls = self.run_command(0x09, payload)
            self.assertEqual(calls, [('send', 0x09, b'')], payload)
            self.assertEqual(self.table(0x0c), FACTORY[0x0c])

    def test_read_returns_the_table(self):
        self.assertEqual(self.run_command(0x0a, [0x14, 0x14, 0x14]),
                         [('send', 0x0a, bytes([0x14, 20, 0x14, 34, 0x14, 50]))])

    def test_read_after_write(self):
        self.run_command(0x09, [0x16, 1, 0x16, 2, 0x16, 3])
        self.calls.clear()
        self.assertEqual(self.run_command(0x0a, [0x16, 0x16, 0x16]),
                         [('send', 0x0a, bytes([0x16, 1, 0x16, 2, 0x16, 3]))])

    def test_read_refuses_bad_requests(self):
        for payload in ([0x01, 0x01, 0x01], [0x0c, 0x0c, 0x14], [0x0c]):
            self.setUp()
            self.assertEqual(self.run_command(0x0a, payload), [('send', 0x0a, b'')], payload)

    def test_clear_override(self):
        self.assertEqual(self.run_command(0x0b, []),
                         [('settings', bytes(16), 0x300), ('save',), ('send', 0x0b, b'')])
        self.setUp()
        self.assertEqual(self.run_command(0x0b, [0]), [('send', 0x0b, b'')], 'a payload is refused')


@unittest.skipUnless(os.environ.get('AWTRIX_MCU_BASE') and os.environ.get('AWTRIX_MCU_ASSEMBLE'),
                     'needs the private original and awtrix-tc002-mcu-assemble')
class Patcher(unittest.TestCase):
    def dispatch(self, extension):
        with tempfile.TemporaryDirectory() as work:
            path = Path(work) / 'extension.bin'
            path.write_bytes(extension)
            image = Path(work) / 'image.pot'
            subprocess.run([os.environ['AWTRIX_MCU_ASSEMBLE'], os.environ['AWTRIX_MCU_BASE'], str(path), str(image)],
                           check=True, capture_output=True)
            flash = unpack(image.read_bytes())
        return [struct.unpack_from('<I', flash, 0x5de4 + 4 * i)[0] for i in range(8)]

    def test_driver_table_entries_only_for_feature_0x10(self):
        pcm = [0x10007a00, 0x10007b00, 0x10007b80, 0x10007bc0]
        self.assertEqual(self.dispatch(build.BUILD13.read_bytes()), pcm + [0x10003ba0] * 4)
        self.assertEqual(self.dispatch((HERE / 'release' / 'extension.bin').read_bytes()),
                         pcm + [0x100087c0, 0x10008848, 0x100088e0, 0x10003ba0])


class Release(unittest.TestCase):
    def test_release_is_the_build(self):
        if not all(shutil.which(t) for t in ('clang', 'ld.lld', 'llvm-objcopy', 'llvm-nm')):
            self.skipTest('LLVM RISC-V tools are not installed')
        extension, tag = build.build()
        self.assertEqual((HERE / 'release' / 'extension.bin').read_bytes(), extension)
        manifest = build.json.loads((HERE / 'release' / 'manifest.json').read_text())
        self.assertEqual((manifest['version'], manifest['features'], manifest['build_tag'], manifest['bytes']),
                         (build.VERSION, build.FEATURES, tag, len(extension)))

    def test_entries_match_the_patcher(self):
        header = (HERE.parents[2] / 'src/platform/tc002/mcu/McuPatch.h').read_text()
        for name, address in (('kDriverTableWriteEntry', 'write_register'), ('kDriverTableReadEntry', 'read_register'),
                              ('kDriverClearOverrideEntry', 'clear_override')):
            self.assertIn(f'{name} = {build.ENTRIES[address]:#010x}', header)


if __name__ == '__main__':
    unittest.main()
