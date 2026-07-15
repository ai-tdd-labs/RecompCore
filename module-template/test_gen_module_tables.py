#!/usr/bin/env python3

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class ModuleTableGeneratorTest(unittest.TestCase):
    def test_chunk_functions_follow_sorted_chunk_ranges(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            generated = root / "generated.h"
            generated.write_text(
                "void func_80001010(CPUState* ctx);\n"
                "void func_80001000(CPUState* ctx);\n"
                "if (address >= 0x80001000u && address < 0x80001020u) {}\n"
            )
            smc = root / "generated_smc.txt"
            smc.write_text("")

            # One minimal DOL text section covering the generated code range.
            dol = bytearray(0x120)
            dol[0x00:0x04] = (0x100).to_bytes(4, "big")
            dol[0x48:0x4C] = (0x80001000).to_bytes(4, "big")
            dol[0x90:0x94] = (0x20).to_bytes(4, "big")
            dol[0x100:0x120] = bytes(range(0x20))
            dol_path = root / "main.dol"
            dol_path.write_bytes(dol)

            output = root / "module_tables.inc"
            script = Path(__file__).with_name("gen_module_tables.py")
            subprocess.run(
                [sys.executable, script, generated, smc, dol_path, output],
                check=True,
                capture_output=True,
                text=True,
            )

            text = output.read_text()
            self.assertIn("{0x80001000u, 0x80001010u}", text)
            self.assertIn("{0x80001010u, 0x80001020u}", text)
            self.assertLess(text.index("func_80001000"), text.index("func_80001010"))


if __name__ == "__main__":
    unittest.main()
