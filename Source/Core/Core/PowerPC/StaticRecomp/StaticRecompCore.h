// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/DynamicLibrary.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompABI.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"

namespace Core
{
class System;
}

namespace PowerPC
{
struct PowerPCState;
}

namespace StaticRecompLockstep
{
class StaticRecompLockstepVerifier;
}

// Executes statically recompiled per-game native code when the PC is covered by
// a loaded module. Interpreter fallback is an explicit compatibility policy;
// strict runtimes stop on the first uncovered PC or unmodeled instruction.
class StaticRecompCore : public JitBase
{
public:
  struct TimelineSnapshot
  {
    u64 native_dispatches;
    u64 bursts;
    u64 charged_cycles;
    u64 idle_skips;
    u64 fallback_entries;
  };

  friend class StaticRecompLockstep::StaticRecompLockstepVerifier;

  explicit StaticRecompCore(Core::System& system, StaticRecompModuleSource module_source);
  StaticRecompCore(const StaticRecompCore&) = delete;
  StaticRecompCore(StaticRecompCore&&) = delete;
  StaticRecompCore& operator=(const StaticRecompCore&) = delete;
  StaticRecompCore& operator=(StaticRecompCore&&) = delete;
  ~StaticRecompCore() override;

  void Init() override;
  void Shutdown() override;

  void Run() override;
  void SingleStep() override;
  bool IsModuleActive() const;
  // Read on the CPU thread at a VI boundary. This deliberately avoids atomics
  // in the per-dispatch hot path while still giving the frame timeline exact
  // deltas.
  TimelineSnapshot GetTimelineSnapshot() const
  {
    return {m_native_dispatches, m_bursts, m_charged_cycles, m_idle_skips, m_fallback_entries};
  }
  bool HasNativeFallbackViolation() const { return m_native_fallback_violation; }
  const std::string& GetNativeFallbackViolation() const { return m_native_fallback_message; }
  bool DispatchableAt(u32 address);
  // This is the dispatcher back-edge hot path. Keep the complete lookup in
  // the header so release builds can fold it into Run() instead of paying a
  // function call and repeating the chunk-index helper on every native block.
  bool FastDispatchableAt(u32 address) const
  {
    if (!m_module_active || m_chunk_lookup_table.empty())
      return false;

    int lookup_index = -1;
    if (address >= 0x80000000u && address < 0x80000000u + m_lookup_ram_size)
      lookup_index = static_cast<int>((address - 0x80000000u) >> 2);
    else if (address >= 0x90000000u && address < 0x90000000u + m_lookup_exram_size)
      lookup_index =
          static_cast<int>((m_lookup_ram_size >> 2) + ((address - 0x90000000u) >> 2));

    if (lookup_index < 0 || lookup_index >= static_cast<int>(m_chunk_lookup_table.size()))
      return false;
    const int chunk_index = m_chunk_lookup_table[lookup_index];
    return chunk_index >= 0 && m_chunk_state[chunk_index] == CHUNK_VERIFIED;
  }

  void ClearCache() override;
  void Jit(u32 em_address) override {}
  bool HandleFault(uintptr_t access_address, SContext* ctx) override { return false; }

  JitBaseBlockCache* GetBlockCache() override { return &m_block_cache; }
  void EraseSingleBlock(const JitBlock& block) override {}
  std::vector<MemoryStats> GetMemoryStats() const override { return {}; }
  std::size_t DisassembleNearCode(const JitBlock& block, std::ostream& stream) const override
  {
    return 0;
  }
  std::size_t DisassembleFarCode(const JitBlock& block, std::ostream& stream) const override
  {
    return 0;
  }
  const CommonAsmRoutinesBase* GetAsmRoutines() override { return nullptr; }
  const char* GetName() const override { return "StaticRecomp"; }

private:
  // JitBaseBlockCache with no generated blocks; exists so generic
  // icache-invalidation plumbing in JitInterface has a real object to talk
  // to, and to feed every invalidation into the SMC demotion guard (D4).
  class EmptyBlockCache : public JitBaseBlockCache
  {
  public:
    explicit EmptyBlockCache(StaticRecompCore& core) : JitBaseBlockCache(core), m_core(core) {}
    void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) override {}

  protected:
    void InvalidateICacheInternal(u32 physical_address, u32 address, u32 length,
                                  bool forced) override
    {
      m_core.OnICacheInvalidate(address, length);
    }

  private:
    StaticRecompCore& m_core;
  };

  void LoadModule();
  void LoadFunctionSymbols();
  void TraceFunctionEntry();
  bool TryHandleNativeLowStub(u32 pc);
  bool TryHandleNativeOSExceptionVector(u32 pc);
  void ReportNativeFallbackViolation(const char* kind, u32 pc, u32 raw = 0);

  // D4 SMC guard, verify-on-entry model. Every chunk starts Unverified; the
  // first native dispatch into it hashes its guest RAM against the module's
  // recorded hash of the original text. An icache invalidation touching a
  // chunk resets it to Unverified (Dolphin invalidates while *loading* code,
  // so invalidation alone must not retire coverage); a hash mismatch (real
  // SMC) marks it Failed, interpreter-only until the next invalidation.
  enum ChunkState : u8
  {
    CHUNK_UNVERIFIED = 0,
    CHUNK_VERIFIED = 1,
    CHUNK_FAILED = 2,
  };

  void OnICacheInvalidate(u32 address, u32 length);
  int ChunkIndexOf(u32 address) const;
  void VerifyChunk(u32 index);

  static void SetPPCStateFromGuestState(const CPUState& s, PowerPC::PowerPCState& ppc);

  // D1 state residency: registers live in m_guest while native code runs;
  // full sync at every native-burst boundary.
  void SyncIn();   // Dolphin PowerPCState -> m_guest
  void SyncOut();  // m_guest -> Dolphin PowerPCState

  // CPUState hooks (module -> chassis environment). `cpu->external_user_data`
  // is the StaticRecompCore*.
  static u64 HookExternalRead(CPUState* cpu, u32 ea, u8 size);
  static void HookExternalWrite(CPUState* cpu, u32 ea, u64 value, u8 size);
  static u32 HookExternalRead32(CPUState* cpu, u32 ea, u8 rid);
  static void HookExternalWrite32(CPUState* cpu, u32 ea, u32 value, u8 rid);
  static void* HookExternalPointer(CPUState* cpu, u32 ea, u32 size);
  static void HookInstructionFallback(CPUState* cpu, u32 raw, u32 cia);

  // Keep Dolphin's MSR-derived state (translation mode, feature flags) in step
  // with the guest MSR before any MMU access or exception delivery.
  void PropagateGuestMSR();

  std::unique_ptr<StaticRecompLockstep::StaticRecompLockstepVerifier> m_lockstep_verifier;

  EmptyBlockCache m_block_cache{*this};

  CPUState m_guest{};
  Common::DynamicLibrary m_library;
  StaticRecompModuleSource m_module_source;
  const StaticRecompModuleDesc* m_module = nullptr;
  bool m_module_active = false;
  bool m_allow_fallback = true;
  bool m_native_fallback_violation = false;
  std::string m_native_fallback_message;
  std::unique_ptr<JitBase> m_fallback_jit;

  u64 m_native_dispatches = 0;
  u64 m_fallback_steps = 0;
  u64 m_fallback_entries = 0;
  u64 m_native_reentries = 0;
  std::unordered_map<u32, u64> m_fallback_pc_counts;
  std::unordered_map<u32, u64> m_reentry_pc_counts;
  std::unordered_map<u32, u64> m_external_irq_counts;
  std::unordered_map<u32, u64> m_native_pc_samples;
  u32 m_first_fallback_pc = 0;
  u32 m_first_native_reentry_pc = 0;
  u64 m_native_exceptions = 0;
  u64 m_hook_fallback_instructions = 0;
  u64 m_native_shim_instructions = 0;
  u64 m_native_alias_entries = 0;
  u64 m_bursts = 0;          // SyncIn..SyncOut native runs (diagnostic)
  u64 m_charged_cycles = 0;  // cycles flushed from module charges (diagnostic)
  u64 m_idle_skips = 0;      // configured guest idle-loop skips (diagnostic)

  std::unordered_map<u32, std::string> m_function_symbols;
  std::string m_trace_function;
  bool m_trace_all_functions = false;
  u64 m_traced_function_entries = 0;

  // D4 guard state: parallel to m_module->chunk_ranges.
  std::vector<u8> m_chunk_state;
  u32 m_failed_chunks = 0;    // chunks currently failing verification (real SMC)
  u64 m_verifications = 0;    // chunk hash checks performed
  u64 m_reverify_events = 0;  // invalidations that reset a chunk to Unverified

  // Lookup table optimization for O(1) chunk searches
  std::vector<int> m_chunk_lookup_table;
  u32 m_lookup_ram_size = 0;
  u32 m_lookup_exram_size = 0;
  int GetAddressLookupIndex(u32 address) const;
  void InitLookupTable(u32 ram_size, u32 exram_size);

  // Dispatch locality: most control transfers stay inside one chunk, so the
  // last hit short-circuits the chunk binary search on the hot path.
  mutable u32 m_last_chunk_index = 0;

  u32 m_idle_pc = 0;
};

extern StaticRecompCore* g_static_recomp_core;
