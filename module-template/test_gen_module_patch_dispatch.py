#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path

from gen_module_patch_dispatch import generate, parse_addresses


class PatchManifestTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.header = self.root / "generated.h"
        self.header.write_text(
            "if (address >= 0x80001000u && address < 0x80002000u) {}\n"
            "if (address >= 0x80100000u && address < 0x80110000u) {}\n"
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    def manifest(self, text: str) -> Path:
        path = self.root / "patch-addresses.txt"
        path.write_text(text)
        return path

    def test_generates_one_shared_callback_body_for_sorted_targets(self) -> None:
        output = self.root / "module_patch_points.h"
        generate(
            self.manifest("# projection and HUD\n0x80001000\n0x80100004\n"),
            self.header,
            output,
        )
        generated = output.read_text()
        self.assertIn("case 0x80001000u:", generated)
        self.assertIn("case 0x80100004u:", generated)
        self.assertIn("#define DOLRECOMP_PATCH_PC(ctx, address)", generated)
        self.assertIn("moderngekko_module_patch_dispatch((ctx), (address))", generated)
        self.assertIn("return;", generated)
        self.assertEqual(generated.count("moderngekko_module_patch_dispatch"), 2)

    def test_rejects_unaligned_duplicate_unsorted_and_uncovered_targets(self) -> None:
        bad_manifests = (
            "0x80001002\n",
            "0x80001000\n0x80001000\n",
            "0x80100000\n0x80001000\n",
        )
        for text in bad_manifests:
            with self.subTest(text=text), self.assertRaises(ValueError):
                parse_addresses(self.manifest(text))

        with self.assertRaisesRegex(ValueError, "outside"):
            generate(
                self.manifest("0x80200000\n"),
                self.header,
                self.root / "dispatch.inc",
            )


class TemplateDispatchTest(unittest.TestCase):
    def test_staticrecomp_template_uses_the_public_standalone_dispatcher(self) -> None:
        template = (Path(__file__).parent / "module_export.c").read_text()
        self.assertEqual(template.count("dolrecomp_call(ctx, address)"), 1)
        self.assertNotIn("dolrecomp_dispatch(ctx, address)", template)
        self.assertNotIn("module_patch_dispatch.inc", template)

    def test_host_event_is_optional_and_carries_guest_timebase(self) -> None:
        template = (Path(__file__).parent / "module_export.c").read_text()
        self.assertIn("moderngekko_module_signal_host_event", template)
        self.assertIn("ctx->timebase", template)
        self.assertIn("event->core_ticks = 0", template)
        self.assertIn("staticrecomp_take_host_event", template)
        dispatch = template[template.index("static int chassis_dispatch") :]
        dispatch = dispatch[: dispatch.index("static void chassis_on_state_loaded")]
        self.assertNotIn("host_event", dispatch)

    def test_host_event_is_translated_on_cpu_burst_not_presenter_thread(self) -> None:
        root = Path(__file__).parents[1]
        core = (
            root / "Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore.cpp"
        ).read_text()
        run = (
            root / "Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore_Run.cpp"
        ).read_text()
        take = core[core.index("bool StaticRecompCore::TakeHostEvent") :]
        take = take[: take.index("StaticRecompCore::StaticRecompCore")]
        self.assertIn("m_staged_host_event.exchange", take)
        self.assertNotIn("m_take_host_event(event)", take)
        self.assertIn("SyncOut();", run)
        self.assertIn("PollArmedHostEvent(core_timing.GetTicks())", run)

    def test_generated_chunks_offer_a_zero_cost_default_patch_point(self) -> None:
        emitter = (
            Path(__file__).parents[1] / "DolRecomp" / "src" / "backend" / "emitter.c"
        ).read_text()
        self.assertIn("#ifndef DOLRECOMP_PATCH_PC", emitter)
        self.assertIn("DOLRECOMP_PATCH_PC(ctx, 0x%08Xu);", emitter)

    def test_dolrecomp_dispatch_checks_host_replacements_before_original_code(self) -> None:
        emitter = (
            Path(__file__).parents[1]
            / "DolRecomp"
            / "src"
            / "backend"
            / "dispatch.c"
        ).read_text()
        host_call = (
            'if (ctx->host_call && ppc_host_call(ctx, address)) return 1;'
        )
        original_call = (
            'if (dolrecomp_call_original(ctx, address)) return 1;'
        )
        self.assertIn('static inline int dolrecomp_call(CPUState* ctx, u32 address)', emitter)
        self.assertIn(host_call, emitter)
        self.assertIn(original_call, emitter)
        self.assertLess(emitter.index(host_call), emitter.index(original_call))


if __name__ == "__main__":
    unittest.main()
