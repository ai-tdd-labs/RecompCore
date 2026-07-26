// RecompCore: StaticRecomp CPU core - Main execution loop.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/System.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/ProcessorInterface.h"
#include "VideoCommon/PerformanceMetrics.h"

#include <cstdio>
#include <atomic>
#include <iterator>

namespace
{
constexpr u32 SYNC_EXCEPTION_MASK = ~static_cast<u32>(
    EXCEPTION_EXTERNAL_INT | EXCEPTION_DECREMENTER | EXCEPTION_PERFORMANCE_MONITOR);
}

bool StaticRecompCore::TryHandleNativeLowStub(u32 pc)
{
  // Dolphin's HLE boot installs this exact seven-instruction return-from-
  // exception stub in low memory before Nintendo OS installs its full vector.
  // Treat the complete, verified signature as one native chassis operation;
  // this is deliberately not a general low-memory instruction interpreter.
  static constexpr u32 BOOT_RFI_STUB[] = {
      0x7D30FAA6u,  // mfspr r9,HID0
      0x612A0008u,  // ori r10,r9,8
      0x7D50FBA6u,  // mtspr HID0,r10
      0x4C00012Cu,  // isync
      0x7C0004ACu,  // sync
      0x7D30FBA6u,  // mtspr HID0,r9
      0x4C000064u,  // rfi
  };
  if (pc >= 0x00002000u)
    return false;

  auto& mmu = m_system.GetMMU();
  for (u32 i = 0; i < std::size(BOOT_RFI_STUB); ++i)
  {
    if (mmu.Read<u32>(pc + i * 4u) != BOOT_RFI_STUB[i])
      return false;
  }

  SyncIn();
  auto& ppc = m_system.GetPPCState();
  m_guest.gpr[9] = ppc.spr[SPR_HID0];
  m_guest.gpr[10] = m_guest.gpr[9] | 8u;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  // Both HID0 writes leave the original architectural value restored.
  constexpr u32 RFI_MASK = 0x87C0FFFFu;
  m_guest.msr = (m_guest.msr & ~RFI_MASK) | (m_guest.srr1 & RFI_MASK);
  m_guest.msr &= 0xFFFBFFFFu;
  m_guest.pc = m_guest.srr0 & ~3u;
  m_guest.downcount -= 12;  // Dolphin PPCTables cost of the seven instructions.
  m_native_shim_instructions += std::size(BOOT_RFI_STUB);
  SyncOut();
  return true;
}

bool StaticRecompCore::TryHandleNativeOSExceptionVector(u32 pc)
{
  static constexpr u32 VECTOR_ADDRESSES[] = {
      0x00000100u, 0x00000200u, 0x00000300u, 0x00000400u, 0x00000500u,
      0x00000600u, 0x00000700u, 0x00000800u, 0x00000900u, 0x00000C00u,
      0x00000D00u, 0x00000F00u, 0x00001300u, 0x00001400u, 0x00001700u,
  };
  u32 exception = std::size(VECTOR_ADDRESSES);
  for (u32 i = 0; i < std::size(VECTOR_ADDRESSES); ++i)
  {
    if (pc == VECTOR_ADDRESSES[i])
    {
      exception = i;
      break;
    }
  }
  if (exception == std::size(VECTOR_ADDRESSES))
    return false;

  auto& mmu = m_system.GetMMU();
  // Verify Nintendo OS's copied first-level vector and reject debugger-
  // commandeered variants. The complete handler is represented as one native
  // chassis operation, preserving its architectural state and memory writes.
  if (mmu.Read<u32>(pc) != 0x7C9043A6u ||       // mtsprg0 r4
      mmu.Read<u32>(pc + 4u) != 0x808000C0u ||  // lwz r4,0xC0(0)
      mmu.Read<u32>(pc + 8u) != 0x9064000Cu ||  // stw r3,12(r4)
      mmu.Read<u32>(pc + 0x58u) != 0x60000000u ||
      mmu.Read<u32>(pc + 0x94u) != 0x4C000064u)  // recoverable rfi
  {
    return false;
  }

  SyncIn();
  const u32 old_r3 = m_guest.gpr[3];
  const u32 old_r4 = m_guest.gpr[4];
  const u32 old_r5 = m_guest.gpr[5];
  const u32 old_cr = m_guest.cr;
  const u32 old_srr1 = m_guest.srr1;
  const u32 context_physical = mmu.Read<u32>(0x000000C0u);
  const u32 context_virtual = mmu.Read<u32>(0x000000D4u);
  if (context_physical == 0 || context_virtual == 0)
    return false;

  const bool recoverable = (old_srr1 & 2u) != 0;
  const u32 default_lis = mmu.Read<u32>(pc + 0x78u);
  const u32 default_addi = mmu.Read<u32>(pc + 0x7Cu);
  if (!recoverable && ((default_lis & 0xFFFF0000u) != 0x3CA00000u ||
                       (default_addi & 0xFFFF0000u) != 0x38A50000u))
  {
    return false;
  }
  const u32 default_handler =
      (static_cast<u32>(static_cast<s32>(static_cast<s16>(default_lis))) << 16) +
      static_cast<u32>(static_cast<s32>(static_cast<s16>(default_addi)));
  const u32 handler = recoverable ? mmu.Read<u32>(0x00003000u + exception * 4u) :
                                    default_handler;
  if (handler == 0)
    return false;

  // Validate every prerequisite before mutating the guest context. If this
  // signature ever stops matching, compatibility fallback must execute the
  // untouched vector rather than repeat half of its memory writes.
  mmu.Write<u32>(old_r3, context_physical + 12u);
  mmu.Write<u32>(old_r4, context_physical + 16u);
  mmu.Write<u32>(old_r5, context_physical + 20u);
  mmu.Write<u16>(static_cast<u16>(mmu.Read<u16>(context_physical + 418u) | 2u),
                 context_physical + 418u);
  mmu.Write<u32>(old_cr, context_physical + 128u);
  mmu.Write<u32>(m_guest.lr, context_physical + 132u);
  mmu.Write<u32>(m_guest.ctr, context_physical + 136u);
  mmu.Write<u32>(m_guest.xer, context_physical + 140u);
  mmu.Write<u32>(m_guest.srr0, context_physical + 408u);
  mmu.Write<u32>(old_srr1, context_physical + 412u);

  m_guest.gpr[3] = exception;
  m_guest.gpr[4] = context_virtual;
  m_guest.gpr[5] = handler;
  const u32 cr0 = (recoverable ? 0x4u : 0x2u) | ((m_guest.xer >> 31) & 1u);
  m_guest.cr = (m_guest.cr & 0x0FFFFFFFu) | (cr0 << 28);
  m_guest.srr0 = handler;
  m_guest.srr1 = m_guest.msr | 0x30u;
  constexpr u32 RFI_MASK = 0x87C0FFFFu;
  m_guest.msr = (m_guest.msr & ~RFI_MASK) | (m_guest.srr1 & RFI_MASK);
  m_guest.msr &= 0xFFFBFFFFu;
  m_guest.pc = m_guest.srr0 & ~3u;
  m_guest.downcount -= 35;
  m_native_shim_instructions += 34;
  SyncOut();
  return true;
}

void StaticRecompCore::Run()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  auto& interpreter = m_system.GetInterpreter();
  auto& memory = m_system.GetMemory();
  const CPU::State* state_ptr = m_system.GetCPU().GetStatePtr();

  m_guest.ram = memory.GetRAM();
  m_guest.ram_size = memory.GetRamSizeReal();
  m_guest.exram = memory.GetEXRAM();
  m_guest.exram_size = memory.GetExRamSizeReal();
  InitLookupTable(m_guest.ram_size, m_guest.exram_size);

  const std::string initial_game_id = SConfig::GetInstance().GetGameID();
  m_module_active = m_module && (initial_game_id.empty() || initial_game_id == m_module->game_id);
  if (m_module_active)
    RefreshRelBindings();

  if (!m_module_active)
  {
    if (!m_allow_fallback)
    {
      ReportNativeFallbackViolation("native module inactive", ppc.pc);
      return;
    }
    if (m_fallback_jit)
    {
      m_fallback_jit->Run();
      return;
    }
  }

  // Oracle features are immutable for a run. Do not call their cold helpers
  // once per native dispatch when no trace or lockstep session was requested.
  const bool trace_function_entries = m_trace_all_functions || !m_trace_function.empty();
  const bool profile_functions = !m_function_profile_path.empty();
  const bool lockstep_enabled = m_lockstep_verifier->IsEnabled();
  const bool timeline_enabled = m_system.GetPerfMetrics().IsTimelineEnabled();
  // Exact native-wall timing brackets every native burst (roughly 1,400 per
  // MKDD frame), so it is an explicit profiler observer rather than part of
  // the low-overhead acceptance timeline. CPU-thread time remains sampled at
  // each VI boundary and is the authoritative standard-run CPU measurement.
  const bool measure_native_wall = timeline_enabled && m_measure_native_wall;
  const bool sample_native_pcs = m_sample_native_pcs;

  // Opcode fuzzing owns this process: it feeds deterministic synthetic CPU
  // states directly into the generated module and reuses the lockstep shadow
  // as the independent Dolphin-interpreter oracle. Do this before the DOL's
  // intentionally stationary entry point enters the normal run loop.
  if (m_lockstep_verifier->IsOpcodeFuzzRequested())
  {
    if (!m_opcode_fuzz_ran)
    {
      m_opcode_fuzz_ran = true;
      RunOpcodeFuzz();
    }
    m_system.GetCPU().Break();
    return;
  }

  while (*state_ptr == CPU::State::Running)
  {
    core_timing.Advance();
    // CoreTiming may have asserted VI, PI, or decrementer state while ending
    // the previous slice. JIT cores sample those pending external exceptions
    // before executing the next block; without the same boundary here, the
    // Nintendo OS idle thread can enable MSR.EE and spin forever without ever
    // entering its interrupt vector.
    power_pc.CheckExternalExceptions();
    const std::string current_game_id = SConfig::GetInstance().GetGameID();
    m_module_active = m_module && (current_game_id.empty() || current_game_id == m_module->game_id);
    if (!m_module_active && !m_allow_fallback)
    {
      ReportNativeFallbackViolation("native module inactive", ppc.pc);
      return;
    }
    int native_chunk_index = -1;
    RelChunkDispatch rel_chunk{};
    bool native_is_rel = false;
    const auto resolve_native = [&](u32 address) {
      native_chunk_index = FastDispatchableChunkAt(address);
      if (native_chunk_index < 0)
        native_chunk_index = DispatchableChunkAt(address);
      native_is_rel = false;
      if (native_chunk_index >= 0)
        return true;
      const bool can_be_rel =
          address >= 0x80000000u && m_module &&
          m_module->abi_version >= STATICRECOMP_ABI_VERSION_V4 &&
          m_module->rel_modules && m_module->num_rel_modules != 0;
      // OSLink/OSUnlink invalidate the instruction cache after changing the
      // live module queue. EmptyBlockCache routes that hardware event through
      // OnICacheInvalidate(), which marks these bindings stale before newly
      // installed code can execute. Re-scan once on the first subsequent REL
      // lookup, after startup, or after a savestate cache reset.
      if (can_be_rel && (m_rel_bindings_valid || RefreshRelBindings()) &&
          LookupRelChunk(address, &rel_chunk))
      {
        native_is_rel = true;
        return true;
      }
      return false;
    };

    do
    {
      // MSR.FP needs no gate here: generated FPU instructions raise the
      // FP-unavailable exception themselves (ppc_fp_available).
      if (m_module_active && resolve_native(ppc.pc))
      {
        const TimePoint native_started = measure_native_wall ? Clock::now() : TimePoint{};
        SyncIn();
        // Host code between native bursts may use its own FP mode. Invalidate
        // the generated helpers' cache so the first FP instruction re-arms
        // GameCube FPSCR.RN/NI, while later instructions in this burst avoid
        // duplicate ARM FPCR reads and writes.
        m_guest.host_fp_control_cache = ~0u;
        ++m_bursts;
        do
        {
          if (trace_function_entries)
            TraceFunctionEntry();
          const bool do_ls = lockstep_enabled && m_lockstep_verifier->ShouldCheck(m_guest.pc);
          if (do_ls)
          {
            m_lockstep_verifier->Prepare(m_guest);
          }

          // Generated local backedges may remain inside their native chunk
          // until this exact Dolphin timing-slice budget is consumed. This
          // removes dispatcher traffic from tight guest polling loops without
          // postponing CoreTiming's next scheduled event.
          const bool at_configured_idle_loop = m_idle_pc != 0 && m_guest.pc == m_idle_pc;
          m_guest.dispatch_cycle_budget =
              (trace_function_entries || lockstep_enabled || at_configured_idle_loop) ?
                  0 :
                  std::max<s64>(1, ppc.downcount);

          // The chassis lookup already resolved and SMC-verified this chunk.
          // Enter it directly instead of asking the module dispatcher to map
          // the same guest address a second time.
          if (native_is_rel)
          {
            rel_chunk.function(&m_guest, rel_chunk.canonical_pc, rel_chunk.section_delta);
            ++m_native_rel_dispatches;
          }
          else if (m_use_generic_module_dispatch)
            m_module->dispatch(&m_guest, m_guest.pc);
          else
            m_module->chunk_functions[native_chunk_index](&m_guest);
          ++m_native_dispatches;
          if (sample_native_pcs && (m_native_dispatches & 0x3FFu) == 0u)
            ++m_native_pc_samples[m_guest.pc];
          if (profile_functions && (m_native_dispatches & 0x3FFu) == 0u)
            SampleFunction(m_guest.pc);

          if (do_ls)
          {
            m_lockstep_verifier->Verify(m_guest);
          }

          // Flush the module's per-block cycle charges into Dolphin's
          // downcount. A dispatch that charged nothing (PC-switch default,
          // pure embedded data) still costs 1 so the burst always makes
          // downcount progress; this per-dispatch flush is also the
          // dispatcher back-edge timing check — CoreTiming regains control
          // with at least CachedInterpreter's per-block frequency, so
          // external-interrupt latency matches stock.
          const s64 charge = -m_guest.downcount;
          m_guest.downcount = 0;
          ppc.downcount -= static_cast<int>(charge > 0 ? charge : 1);
          m_charged_cycles += static_cast<u64>(charge > 0 ? charge : 1);
          m_guest.timebase += static_cast<u64>(charge > 0 ? charge : 1);

          // A generated request is a conservative compile-time match of
          // Dolphin PPCAnalyzer::IsBusyWaitLoop. Explicit --idle-pc remains
          // available for diagnosed loops outside that safe subset.
          const bool generated_idle_loop = m_guest.idle_loop_requested != 0;
          m_guest.idle_loop_requested = 0;
          if (generated_idle_loop || (m_guest.pc == m_idle_pc && m_idle_pc != 0))
          {
            ++m_idle_skips;
            m_system.GetCoreTiming().Idle();
          }

          // ctx->timebase is refreshed at burst start (SyncIn), and here we
          // incrementally advance it by the exact block cycle charges to
          // prevent guest busy-wait loops from spinning on a stale timebase.
          if (m_guest.exception)
          {
            // DolRecomp's runtime already redirected pc/msr/srr to the guest
            // exception vector; the flag only signals that it happened.
            m_guest.exception = 0;
            m_guest.program_exception = 0;
            ++m_native_exceptions;
          }
          if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
            break;  // Hook-raised synchronous exception: deliver via Dolphin below.
        } while (m_module_active && resolve_native(m_guest.pc) &&
                 ppc.downcount > 0 && *state_ptr == CPU::State::Running);
        SyncOut();
        if (measure_native_wall)
        {
          const auto elapsed =
              std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - native_started);
          m_native_wall_ns += static_cast<u64>(std::max<s64>(0, elapsed.count()));
        }
        // Probe-only event translation. The module's guest timebase includes
        // the emulated wall-clock epoch, whereas XFB timestamps use
        // CoreTiming ticks. Consume only while explicitly armed and attach a
        // same-domain timestamp at this CPU burst boundary.
        if (m_armed_host_event.load(std::memory_order_relaxed) != 0)
          PollArmedHostEvent(core_timing.GetTicks());
        if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
          power_pc.CheckExceptions();
      }
      else
      {
        // SingleStepInner delivers synchronous exceptions itself; external
        // interrupts are delivered at slice start, as in Interpreter::Run.
        // A transient branch outside the native module (most commonly an
        // exception vector) must not hand control to JitBase::Run(): that
        // runloop only returns when emulation stops, so StaticRecomp would
        // never get a chance to reclaim execution after the guest returns to
        // compiled code.  Step the interpreter until the PC is dispatchable
        // again.  Keep the full JIT runloop only for a genuinely inactive
        // module, where there is no native code to return to.
        // With translation disabled, the CPU may execute a DOL page through
        // its physical MEM1 address. The static module names the same bytes by
        // their cached 0x80000000 alias; canonicalize only when that exact
        // alias is covered and verified by the module's SMC guard.
        if (m_module_active && ppc.pc < m_guest.ram_size)
        {
          const u32 cached_alias = ppc.pc | 0x80000000u;
          if (DispatchableAt(cached_alias))
          {
            ppc.pc = cached_alias;
            ppc.npc = cached_alias;
            ++m_native_alias_entries;
            continue;
          }
        }
        if (m_module_active && TryHandleNativeLowStub(ppc.pc))
        {
          continue;
        }
        if (m_module_active && TryHandleNativeOSExceptionVector(ppc.pc))
        {
          continue;
        }
        if (!m_module_active && m_fallback_jit)
        {
          if (!m_allow_fallback)
          {
            ReportNativeFallbackViolation("native module inactive", ppc.pc);
            return;
          }
          m_fallback_jit->Run();
        }
        else
        {
          if (m_module_active)
          {
            ++m_fallback_entries;
            ++m_fallback_pc_counts[ppc.pc];
            if (ppc.pc == 0x00000500u)
            {
              const auto& pi = m_system.GetProcessorInterface();
              ++m_external_irq_counts[pi.GetCause() & pi.GetMask()];
            }
            if (m_first_fallback_pc == 0)
            {
              m_first_fallback_pc = ppc.pc;
              std::fprintf(stderr, "[staticrecomp] first fallback: pc=0x%08X\n", ppc.pc);
            }
            if (!m_allow_fallback)
            {
              const u32 raw = m_system.GetMMU().Read<u32>(ppc.pc);
              if (ppc.pc < 0x00002000u)
              {
                std::fprintf(stderr, "[staticrecomp] uncovered-low-code:");
                for (u32 offset = 0; offset < 32; offset += 4)
                  std::fprintf(stderr, " %08X", m_system.GetMMU().Read<u32>(ppc.pc + offset));
                std::fprintf(stderr, "\n");
              }
              ReportNativeFallbackViolation("uncovered PC", ppc.pc, raw);
              return;
            }
          }
          do
          {
            ppc.downcount -= interpreter.SingleStepInner();
            ++m_fallback_steps;
          } while (!(m_module_active && resolve_native(ppc.pc)) && ppc.downcount > 0 &&
                   *state_ptr == CPU::State::Running);
          if (m_module_active && resolve_native(ppc.pc))
          {
            ++m_native_reentries;
            ++m_reentry_pc_counts[ppc.pc];
            if (m_first_native_reentry_pc == 0)
            {
              m_first_native_reentry_pc = ppc.pc;
              std::fprintf(stderr, "[staticrecomp] first native re-entry: pc=0x%08X\n", ppc.pc);
            }
          }
        }
      }
    } while (ppc.downcount > 0 && *state_ptr == CPU::State::Running);
  }
}

bool StaticRecompCore::RunOpcodeFuzz()
{
  auto& memory = m_system.GetMemory();
  m_guest.ram = memory.GetRAM();
  m_guest.ram_size = memory.GetRamSizeReal();
  m_guest.exram = memory.GetEXRAM();
  m_guest.exram_size = memory.GetExRamSizeReal();
  InitLookupTable(m_guest.ram_size, m_guest.exram_size);

  const std::string game_id = SConfig::GetInstance().GetGameID();
  m_module_active = m_module && (game_id.empty() || game_id == m_module->game_id);
  return m_lockstep_verifier->RunOpcodeFuzz();
}

void StaticRecompCore::SingleStep()
{
  // Debugger stepping runs through the interpreter; state outside Run() lives
  // in PowerPCState, so no sync is needed.
  auto& system = m_system;
  system.GetCoreTiming().Advance();
  system.GetPPCState().downcount -= system.GetInterpreter().SingleStepInner();
}
