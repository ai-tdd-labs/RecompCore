#!/usr/bin/env python3

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("gen_rel_module_tables.py")
SPEC = importlib.util.spec_from_file_location("gen_rel_module_tables", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class RelModuleTableTests(unittest.TestCase):
    def write_chunk(self, root: Path, module: str, name: str, count: int) -> None:
        chunks = root / "rels" / module / "chunks"
        chunks.mkdir(parents=True, exist_ok=True)
        cases = "".join(
            f"case 0x{0x80500100 + index * 4:08X}u: goto label_{index};\n"
            for index in range(count)
        )
        (chunks / name).write_text(cases, encoding="utf-8")

    def test_emits_sorted_modules_sections_and_chunk_functions(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_chunk(
                root, "actor_261", "chunk_0000_rel1_80500100.c", 2
            )
            self.write_chunk(
                root, "actor_261", "chunk_0001_rel1_80500108.c", 1
            )
            self.write_chunk(
                root, "other_7", "chunk_0000_rel2_80600200.c", 1
            )

            text = MODULE.emit(MODULE.discover(root))

            self.assertLess(text.index("{7u,"), text.index("{261u,"))
            self.assertIn("void func_80500100(CPUState*, u32, intptr_t);", text)
            self.assertIn("{0x80500100u, 0x80500108u}", text)
            self.assertIn("1u, 0u, 0x0000000Cu, 0x80500100u", text)
            self.assertIn("#define MODULE_REL_COUNT", text)

    def test_rejects_chunk_gaps(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_chunk(
                root, "actor_1", "chunk_0000_rel1_80500100.c", 1
            )
            self.write_chunk(
                root, "actor_1", "chunk_0001_rel1_80500108.c", 1
            )
            with self.assertRaisesRegex(ValueError, "gap/overlap"):
                MODULE.discover(root)

    def test_rejects_duplicate_global_chunk_symbols(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_chunk(
                root, "actor_1", "chunk_0000_rel1_80500100.c", 1
            )
            self.write_chunk(
                root, "other_2", "chunk_0000_rel1_80500100.c", 1
            )
            with self.assertRaisesRegex(ValueError, "duplicate REL chunk symbol"):
                MODULE.discover(root)

    def test_dol_only_catalog_is_empty(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            text = MODULE.emit(MODULE.discover(Path(directory)))
            self.assertIn("#define MODULE_REL_MODULES NULL", text)
            self.assertIn("#define MODULE_REL_COUNT 0u", text)


if __name__ == "__main__":
    unittest.main()
