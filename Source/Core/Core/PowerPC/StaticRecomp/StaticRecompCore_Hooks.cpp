// RecompCore: StaticRecomp CPU core - Memory and instruction fallback HLE hooks.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/System.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/SystemTimers.h"
#include "Common/Logging/Log.h"

namespace
{
constexpr u32 LOCKED_CACHE_BASE = 0xE0000000u;
}

u64 StaticRecompCore::HookExternalRead(CPUState* cpu, u32 ea, u8 size)
{
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  core->PropagateGuestMSR();
  auto& mmu = core->m_system.GetMMU();
  u64 value;
  switch (size)
  {
  case 1:
    value = mmu.Read<u8>(ea);
    break;
  case 2:
    value = mmu.Read<u16>(ea);
    break;
  case 4:
    value = mmu.Read<u32>(ea);
    break;
  case 8:
    value = mmu.Read<u64>(ea);
    break;
  default:
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: external read of bad size {} at 0x{:08X}", size, ea);
    return 0;
  }
  if (core->m_lockstep_verifier->m_ls_journaling &&
      StaticRecompLockstep::LsHwAccessInScope(mmu, ea))
  {
    core->m_lockstep_verifier->m_journal.native_reads.push_back({ea, static_cast<u32>(value), size});
  }
  return value;
}

void StaticRecompCore::HookExternalWrite(CPUState* cpu, u32 ea, u64 value, u8 size)
{
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);

  // Gather-pipe fast path: stores to the write-gather pipe page at effective
  // 0xCC008000 go straight to GPFifo, mirroring the MMU's masked-write
  // special case without an MMU round trip. Keying on the effective page is
  // the same shortcut Dolphin's JITs take (optimizeGatherPipe). GPFifo
  // maintains ppc_state.gather_pipe_ptr internally.
  if ((ea & 0xFFFFF000) == 0xCC008000u)
  {
    if (core->m_lockstep_verifier->m_ls_journaling)
      core->m_lockstep_verifier->m_journal.native_mmio.push_back({ea, static_cast<u32>(value), size});
    auto& gpfifo = core->m_system.GetGPFifo();
    switch (size)
    {
    case 1:
      gpfifo.FastWrite8(static_cast<u8>(value));
      gpfifo.FastCheckGatherPipe();
      return;
    case 2:
      gpfifo.FastWrite16(static_cast<u16>(value));
      gpfifo.FastCheckGatherPipe();
      return;
    case 4:
      gpfifo.FastWrite32(static_cast<u32>(value));
      gpfifo.FastCheckGatherPipe();
      return;
    case 8:
      gpfifo.FastWrite64(value);
      gpfifo.FastCheckGatherPipe();
      return;
    default:
      for (u32 i = size * 8u; i > 0;)
      {
        i -= 8;
        gpfifo.FastWrite8(static_cast<u8>(value >> i));
      }
      gpfifo.FastCheckGatherPipe();
      return;
    }
  }

  core->PropagateGuestMSR();
  auto& mmu = core->m_system.GetMMU();
  if (core->m_lockstep_verifier->m_ls_journaling &&
      StaticRecompLockstep::LsHwAccessInScope(mmu, ea))
  {
    core->m_lockstep_verifier->m_journal.native_mmio.push_back({ea, static_cast<u32>(value), size});
  }
  switch (size)
  {
  case 1:
    mmu.Write<u8>(static_cast<u8>(value), ea);
    break;
  case 2:
    mmu.Write<u16>(static_cast<u16>(value), ea);
    break;
  case 4:
    mmu.Write<u32>(static_cast<u32>(value), ea);
    break;
  case 8:
    mmu.Write<u64>(value, ea);
    break;
  default:
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: external write of bad size {} at 0x{:08X}", size, ea);
    break;
  }
}

u32 StaticRecompCore::HookExternalRead32(CPUState* cpu, u32 ea, u8 rid)
{
  // eciwx external-control read. EAR-enable and alignment were checked by the
  // generated helper; Dolphin's interpreter services the access as a plain
  // MMU read (the rid is carried in EAR only).
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  core->PropagateGuestMSR();
  auto& mmu = core->m_system.GetMMU();
  const u32 value = mmu.Read<u32>(ea);
  if (core->m_lockstep_verifier->m_ls_journaling &&
      StaticRecompLockstep::LsHwAccessInScope(mmu, ea))
  {
    core->m_lockstep_verifier->m_journal.native_reads.push_back({ea, value, 4});
  }
  return value;
}

void StaticRecompCore::HookExternalWrite32(CPUState* cpu, u32 ea, u32 value, u8 rid)
{
  // ecowx external-control write; see HookExternalRead32.
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  core->PropagateGuestMSR();
  auto& mmu = core->m_system.GetMMU();
  if (core->m_lockstep_verifier->m_ls_journaling &&
      StaticRecompLockstep::LsHwAccessInScope(mmu, ea))
  {
    core->m_lockstep_verifier->m_journal.native_mmio.push_back({ea, value, 4});
  }
  mmu.Write<u32>(value, ea);
}

void* StaticRecompCore::HookExternalPointer(CPUState* cpu, u32 ea, u32 size)
{
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  auto& memory = core->m_system.GetMemory();
  if (ea >= LOCKED_CACHE_BASE && size != 0 &&
      (ea - LOCKED_CACHE_BASE) + size <= memory.GetL1CacheSize())
  {
    return memory.GetL1Cache() + (ea - LOCKED_CACHE_BASE);
  }
  // Everything else stays on the per-access MMU hooks: this hook receives
  // *effective* addresses, and whether one maps to RAM depends on live
  // MSR/BAT state that only the MMU can answer. Handing out a raw pointer
  // here would bypass MMIO and translation. (Memory::GetPointerForRange was
  // considered and rejected for exactly that reason.)
  return nullptr;
}

void StaticRecompCore::HookInstructionFallback(CPUState* cpu, u32 raw, u32 cia)
{
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);

  auto& system = core->m_system;
  auto& ppc = system.GetPPCState();

  // Fast path for dcbf/dcbst/dcbi/icbi: streaming code flushes caches in
  // 32-byte loops (thousands per frame), and these ops read two GPRs and
  // change no CPU state, so they run straight off ctx without the full
  // SyncOut/interpreter/SyncIn round trip. This mirrors Dolphin's
  // interpreter with dcache emulation off: every one funnels into
  // InvalidateICacheLine (keeping the SMC guard exact). dcbi's PR!=0
  // privilege trap and dcache-on configs take the slow path.
  if ((raw >> 26) == 31u && !ppc.m_enable_dcache)
  {
    const u32 xo = (raw >> 1) & 0x3FFu;
    if (xo == 86u || xo == 54u || xo == 982u || (xo == 470u && (cpu->msr & 0x4000u) == 0))
    {
      const u32 ra = (raw >> 16) & 31u;
      const u32 rb = (raw >> 11) & 31u;
      const u32 ea = (ra ? cpu->gpr[ra] : 0u) + cpu->gpr[rb];
      if (xo == 982u)
      {
        // Match Interpreter::icbi so interpreter fallback and native
        // execution observe the same REL bytes after a load-address reuse.
        ppc.iCache.Invalidate(system.GetMemory(), system.GetJitInterface(), ea);
      }
      // These bypass SingleStepInner, so charge Dolphin's PPCTables cost
      // here (icbi 4, dcbf/dcbst/dcbi 5); their emitted block cost is zero.
      ppc.downcount -= (xo == 982u) ? 4 : 5;
      cpu->pc = cia + 4u;
      ++core->m_native_shim_instructions;
      return;
    }
  }

  // HID0 is CPU hardware state, not game code. Model its SDK-visible access
  // directly so cache initialization remains native. ICFI is self-clearing
  // and invalidates Dolphin's instruction-cache model exactly as its JIT does.
  if ((raw >> 26) == 31u && (cpu->msr & 0x4000u) == 0)
  {
    const u32 xo = (raw >> 1) & 0x3FFu;
    const u32 spr = ((raw >> 16) & 0x1Fu) | (((raw >> 11) & 0x1Fu) << 5);
    const u32 reg = (raw >> 21) & 0x1Fu;
    if (spr == SPR_HID0 && xo == 339u)
    {
      cpu->gpr[reg] = ppc.spr[SPR_HID0];
      ppc.downcount -= 1;
      cpu->pc = cia + 4u;
      ++core->m_native_shim_instructions;
      return;
    }
    if (spr == SPR_HID0 && xo == 467u)
    {
      ppc.spr[SPR_HID0] = cpu->gpr[reg];
      if (HID0(ppc).ICFI)
      {
        HID0(ppc).ICFI = 0;
        ppc.iCache.Reset(system.GetJitInterface());
      }
      ppc.downcount -= 2;
      cpu->pc = cia + 4u;
      ++core->m_native_shim_instructions;
      return;
    }
    if (spr == SPR_L2CR && xo == 339u)
    {
      cpu->gpr[reg] = ppc.spr[SPR_L2CR];
      ppc.downcount -= 1;
      cpu->pc = cia + 4u;
      ++core->m_native_shim_instructions;
      return;
    }
    if (spr == SPR_L2CR && xo == 467u)
    {
      ppc.spr[SPR_L2CR] = cpu->gpr[reg];
      ppc.downcount -= 2;
      cpu->pc = cia + 4u;
      ++core->m_native_shim_instructions;
      return;
    }
    if ((spr == SPR_MMCR0 || spr == SPR_MMCR1) && xo == 339u)
    {
      cpu->gpr[reg] = ppc.spr[spr];
      ppc.downcount -= 1;
      cpu->pc = cia + 4u;
      ++core->m_native_shim_instructions;
      return;
    }
    if ((spr == SPR_MMCR0 || spr == SPR_MMCR1) && xo == 467u)
    {
      ppc.spr[spr] = cpu->gpr[reg];
      PowerPC::MMCRUpdated(ppc);
      ppc.downcount -= 2;
      cpu->pc = cia + 4u;
      ++core->m_native_shim_instructions;
      return;
    }
    const bool is_pmc = spr == SPR_PMC1 || spr == SPR_PMC2 || spr == SPR_PMC3 ||
                        spr == SPR_PMC4;
    if (is_pmc && xo == 339u)
    {
      cpu->gpr[reg] = ppc.spr[spr];
      ppc.downcount -= 1;
      cpu->pc = cia + 4u;
      ++core->m_native_shim_instructions;
      return;
    }
    if (is_pmc && xo == 467u)
    {
      ppc.spr[spr] = cpu->gpr[reg];
      ppc.downcount -= 2;
      cpu->pc = cia + 4u;
      ++core->m_native_shim_instructions;
      return;
    }
    const bool is_ibat = (spr >= SPR_IBAT0U && spr <= SPR_IBAT3L) ||
                         (spr >= SPR_IBAT4U && spr <= SPR_IBAT7L);
    const bool is_dbat = (spr >= SPR_DBAT0U && spr <= SPR_DBAT3L) ||
                         (spr >= SPR_DBAT4U && spr <= SPR_DBAT7L);
    if ((is_ibat || is_dbat) && xo == 339u)
    {
      cpu->gpr[reg] = ppc.spr[spr];
      ppc.downcount -= 1;
      cpu->pc = cia + 4u;
      ++core->m_native_shim_instructions;
      return;
    }
    if ((is_ibat || is_dbat) && xo == 467u)
    {
      const u32 old_value = ppc.spr[spr];
      ppc.spr[spr] = cpu->gpr[reg];
      if (old_value != ppc.spr[spr])
      {
        if (is_ibat)
          system.GetMMU().IBATUpdated();
        else
          system.GetMMU().DBATUpdated();
      }
      ppc.downcount -= 2;
      cpu->pc = cia + 4u;
      ++core->m_native_shim_instructions;
      return;
    }
    if (spr == SPR_DEC && xo == 339u)
    {
      if ((ppc.spr[SPR_DEC] & 0x80000000u) == 0)
        ppc.spr[SPR_DEC] = system.GetSystemTimers().GetFakeDecrementer();
      cpu->gpr[reg] = ppc.spr[SPR_DEC];
      ppc.downcount -= 1;
      cpu->pc = cia + 4u;
      ++core->m_native_shim_instructions;
      return;
    }
    if (spr == SPR_DEC && xo == 467u)
    {
      const u32 old_value = ppc.spr[SPR_DEC];
      ppc.spr[SPR_DEC] = cpu->gpr[reg];
      if ((old_value & 0x80000000u) == 0 && (ppc.spr[SPR_DEC] & 0x80000000u) != 0)
        ppc.Exceptions |= EXCEPTION_DECREMENTER;
      system.GetSystemTimers().DecrementerSet();
      ppc.downcount -= 2;
      cpu->pc = cia + 4u;
      ++core->m_native_shim_instructions;
      return;
    }
    if (spr == SPR_WPAR && xo == 339u)
    {
      if (system.GetGPFifo().IsBNE())
        ppc.spr[SPR_WPAR] |= 1u;
      else
        ppc.spr[SPR_WPAR] &= ~1u;
      cpu->gpr[reg] = ppc.spr[SPR_WPAR];
      ppc.downcount -= 1;
      cpu->pc = cia + 4u;
      ++core->m_native_shim_instructions;
      return;
    }
    if (spr == SPR_WPAR && xo == 467u)
    {
      ppc.spr[SPR_WPAR] = cpu->gpr[reg];
      system.GetGPFifo().ResetGatherPipe();
      ppc.downcount -= 2;
      cpu->pc = cia + 4u;
      ++core->m_native_shim_instructions;
      return;
    }
    if ((spr == SPR_DMAU || spr == SPR_DMAL) && xo == 339u)
    {
      cpu->gpr[reg] = ppc.spr[spr];
      ppc.downcount -= 1;
      cpu->pc = cia + 4u;
      ++core->m_native_shim_instructions;
      return;
    }
    if ((spr == SPR_DMAU || spr == SPR_DMAL) && xo == 467u)
    {
      ppc.spr[spr] = cpu->gpr[reg];
      if (spr == SPR_DMAL)
      {
        if (DMAL(ppc).DMA_T)
        {
          const u32 mem_address = DMAU(ppc).MEM_ADDR << 5;
          const u32 cache_address = DMAL(ppc).LC_ADDR << 5;
          u32 length = (DMAU(ppc).DMA_LEN_U << 2) | DMAL(ppc).DMA_LEN_L;
          if (length == 0)
            length = 128;
          if (DMAL(ppc).DMA_LD)
            system.GetMMU().DMA_MemoryToLC(cache_address, mem_address, length);
          else
            system.GetMMU().DMA_LCToMemory(mem_address, cache_address, length);
        }
        DMAL(ppc).DMA_T = 0;
      }
      ppc.downcount -= 2;
      cpu->pc = cia + 4u;
      ++core->m_native_shim_instructions;
      return;
    }
  }

  if (!core->m_allow_fallback)
  {
    cpu->pc = cia;
    core->ReportNativeFallbackViolation("unmodeled instruction", cia, raw);
    return;
  }

  ++core->m_hook_fallback_instructions;

  // Lockstep: a block that fell back to the interpreter for an unmodeled
  // instruction performed side effects not captured by the RAM journal /
  // MMIO hooks, so re-running it on the shadow would double-issue them.
  if (core->m_lockstep_verifier->m_ls_journaling)
    core->m_lockstep_verifier->m_ls_fallback_seen = true;

  // The recompiled segment resumes via the dispatcher at the PC this leaves
  // behind, so this must execute exactly the instruction at cia via
  // Dolphin's interpreter and hand the register state back.
  core->SyncOut();
  ppc.pc = cia;
  ppc.npc = cia + 4;
  ppc.downcount -= system.GetInterpreter().SingleStepInner();
  core->SyncIn();
}
