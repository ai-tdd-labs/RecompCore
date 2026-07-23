// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include "Common/Config/Config.h"
#include "Common/DynamicLibrary.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/StaticRecompSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Host.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/System.h"

#ifdef _M_X86_64
#include "Core/PowerPC/Jit64/Jit.h"
#endif
#ifdef _M_ARM_64
#include "Core/PowerPC/JitArm64/Jit.h"
#endif


StaticRecompCore* g_static_recomp_core = nullptr;

namespace
{
bool RangesAreSorted(const StaticRecompRange* ranges, u32 count)
{
  if (!ranges || count == 0)
    return false;
  for (u32 i = 0; i < count; ++i)
  {
    if (ranges[i].start >= ranges[i].end ||
        (i != 0 && ranges[i - 1].end > ranges[i].start))
      return false;
  }
  return true;
}

bool AddressIsCovered(const StaticRecompRange* ranges, u32 count, u32 address)
{
  for (u32 i = 0; i < count; ++i)
  {
    if (address >= ranges[i].start && address < ranges[i].end)
      return true;
  }
  return false;
}

bool ChunksTileCode(const StaticRecompModuleDesc& desc)
{
  if (!RangesAreSorted(desc.chunk_ranges, desc.num_chunk_ranges) || !desc.chunk_hashes ||
      !desc.chunk_functions)
    return false;
  u32 chunk = 0;
  for (u32 code = 0; code < desc.num_code_ranges; ++code)
  {
    u32 cursor = desc.code_ranges[code].start;
    while (chunk < desc.num_chunk_ranges && desc.chunk_ranges[chunk].start < desc.code_ranges[code].end)
    {
      if (desc.chunk_ranges[chunk].start != cursor ||
          desc.chunk_ranges[chunk].end > desc.code_ranges[code].end ||
          desc.chunk_functions[chunk] == nullptr)
        return false;
      cursor = desc.chunk_ranges[chunk++].end;
    }
    if (cursor != desc.code_ranges[code].end)
      return false;
  }
  return chunk == desc.num_chunk_ranges;
}

bool RelCatalogIsValid(const StaticRecompModuleDesc& desc)
{
  if (desc.abi_version < STATICRECOMP_ABI_VERSION_V4)
    return true;
  if (desc.num_rel_modules == 0)
    return desc.rel_modules == nullptr;
  if (!desc.rel_modules)
    return false;

  for (u32 module_index = 0; module_index < desc.num_rel_modules; ++module_index)
  {
    const auto& module = desc.rel_modules[module_index];
    if (module.module_id == 0 || !module.executable_sections ||
        module.num_executable_sections == 0)
      return false;
    for (u32 previous = 0; previous < module_index; ++previous)
    {
      if (desc.rel_modules[previous].module_id == module.module_id)
        return false;
    }
    for (u32 section_index = 0; section_index < module.num_executable_sections;
         ++section_index)
    {
      const auto& section = module.executable_sections[section_index];
      const u64 canonical_end =
          static_cast<u64>(section.canonical_start) + section.size;
      if (section.size == 0 || !section.chunk_ranges || section.num_chunk_ranges == 0 ||
          !section.chunk_functions ||
          static_cast<u64>(section.offset) + section.size > UINT32_MAX ||
          canonical_end > UINT32_MAX)
        return false;
      if (section_index != 0)
      {
        const auto& previous = module.executable_sections[section_index - 1];
        if (previous.section_index > section.section_index ||
            (previous.section_index == section.section_index &&
             static_cast<u64>(previous.offset) + previous.size > section.offset))
          return false;
      }
      u32 cursor = section.canonical_start;
      for (u32 chunk = 0; chunk < section.num_chunk_ranges; ++chunk)
      {
        if (section.chunk_ranges[chunk].start != cursor ||
            section.chunk_ranges[chunk].start >= section.chunk_ranges[chunk].end ||
            section.chunk_ranges[chunk].end > canonical_end ||
            section.chunk_functions[chunk] == nullptr)
          return false;
        cursor = section.chunk_ranges[chunk].end;
      }
      if (cursor != canonical_end)
        return false;
    }
  }
  return true;
}
}  // namespace

bool StaticRecompCore::IsModuleActive() const
{
  return m_module_active;
}

bool StaticRecompCore::ArmHostEvent(u32 event_id)
{
  if (event_id == 0 || !m_module || !m_take_host_event)
    return false;

  m_staged_host_event.store(0, std::memory_order_release);
  m_staged_host_event_guest_timebase.store(0, std::memory_order_relaxed);
  m_staged_host_event_core_ticks.store(0, std::memory_order_relaxed);
  m_armed_host_event.store(event_id, std::memory_order_release);
  return true;
}

bool StaticRecompCore::TakeHostEvent(StaticRecompHostEvent* event)
{
  if (!event)
    return false;

  const u32 id = m_staged_host_event.exchange(0, std::memory_order_acq_rel);
  if (id == 0)
    return false;

  event->id = id;
  event->reserved = 0;
  event->guest_timebase =
      m_staged_host_event_guest_timebase.load(std::memory_order_relaxed);
  event->core_ticks = m_staged_host_event_core_ticks.load(std::memory_order_relaxed);
  return true;
}

void StaticRecompCore::PollArmedHostEvent(u64 core_ticks)
{
  const u32 expected = m_armed_host_event.load(std::memory_order_acquire);
  if (expected == 0 || !m_take_host_event)
    return;

  StaticRecompHostEvent event{};
  if (!m_take_host_event(&event))
    return;

  if (event.id != expected)
  {
    std::fprintf(stderr,
                 "[staticrecomp] ignored module host event=0x%08X expected=0x%08X\n",
                 event.id, expected);
    return;
  }

  u32 armed = expected;
  if (!m_armed_host_event.compare_exchange_strong(armed, 0, std::memory_order_acq_rel))
    return;

  m_staged_host_event_guest_timebase.store(event.guest_timebase, std::memory_order_relaxed);
  m_staged_host_event_core_ticks.store(core_ticks, std::memory_order_relaxed);
  m_staged_host_event.store(event.id, std::memory_order_release);
}

StaticRecompCore::StaticRecompCore(Core::System& system, StaticRecompModuleSource module_source)
    : JitBase(system), m_module_source(std::move(module_source))
{
}

StaticRecompCore::~StaticRecompCore() = default;

void StaticRecompCore::Init()
{
  g_static_recomp_core = this;
  m_armed_host_event.store(0, std::memory_order_relaxed);
  m_staged_host_event.store(0, std::memory_order_relaxed);
  m_staged_host_event_guest_timebase.store(0, std::memory_order_relaxed);
  m_staged_host_event_core_ticks.store(0, std::memory_order_relaxed);
  RefreshConfig();
  jo.enableBlocklink = false;
  jo.fastmem = false;
  jo.fastmem_arena = false;

  m_block_cache.Init();

  m_guest = CPUState{};
  m_guest.external_read = HookExternalRead;
  m_guest.external_write = HookExternalWrite;
  m_guest.external_read32 = HookExternalRead32;
  m_guest.external_write32 = HookExternalWrite32;
  m_guest.external_pointer = HookExternalPointer;
  m_guest.instruction_fallback = HookInstructionFallback;
  m_guest.host_call = nullptr;
  m_guest.external_user_data = this;

  std::fprintf(stderr, "[staticrecomp] core init\n");

  LoadModule();
  LoadFunctionSymbols();
  m_allow_fallback = Config::Get(Config::MAIN_STATICRECOMP_ALLOW_FALLBACK);
  m_idle_pc = Config::Get(Config::MAIN_STATICRECOMP_IDLE_PC);
  const char* module_dispatch = std::getenv("STATICRECOMP_MODULE_DISPATCH");
  m_use_generic_module_dispatch =
      module_dispatch != nullptr && std::strcmp(module_dispatch, "generic") == 0;
  const char* native_wall = std::getenv("STATICRECOMP_TIMELINE_NATIVE_WALL");
  m_measure_native_wall =
      native_wall != nullptr && native_wall[0] != '\0' && native_wall[0] != '0';
  const char* native_pc_sampling = std::getenv("STATICRECOMP_NATIVE_PC_SAMPLING");
  m_sample_native_pcs = native_pc_sampling != nullptr && native_pc_sampling[0] != '\0' &&
                        native_pc_sampling[0] != '0';
  std::fprintf(stderr, "[staticrecomp] module dispatch=%s\n",
               m_use_generic_module_dispatch ? "generic" : "direct-chunk");
  std::fprintf(stderr, "[staticrecomp] detailed observers: native_wall=%u native_pc_samples=%u\n",
               m_measure_native_wall ? 1u : 0u, m_sample_native_pcs ? 1u : 0u);
  m_lockstep_verifier = std::make_unique<StaticRecompLockstep::StaticRecompLockstepVerifier>(*this);
  m_lockstep_verifier->Init();
  if (m_lockstep_verifier->IsOpcodeFuzzRequested())
  {
    m_opcode_fuzz_ran = true;
    (void)RunOpcodeFuzz();
  }

  // Strict-native runs never execute the compatibility JIT. Constructing it
  // anyway reserves a large code cache, and Dolphin cache invalidations then
  // spend time poisoning that unused memory on the CPU/GPU thread. Keep the
  // JIT only for the explicit --allow-fallback diagnostic mode.
  if (m_allow_fallback)
  {
#ifdef _M_ARM_64
    m_fallback_jit = std::make_unique<JitArm64>(m_system);
#elif defined(_M_X86_64)
    m_fallback_jit = std::make_unique<Jit64>(m_system);
#endif
  }
  if (m_fallback_jit)
    m_fallback_jit->Init();
}

void StaticRecompCore::ReportNativeFallbackViolation(const char* kind, u32 pc, u32 raw)
{
  if (m_native_fallback_violation)
    return;

  m_native_fallback_violation = true;
  if (raw != 0)
    m_native_fallback_message =
        fmt::format("strict native execution stopped: {} at PC 0x{:08X} (raw 0x{:08X})", kind,
                    pc, raw);
  else
    m_native_fallback_message =
        fmt::format("strict native execution stopped: {} at PC 0x{:08X}", kind, pc);

  std::fprintf(stderr, "[staticrecomp] NATIVE COVERAGE VIOLATION: %s\n",
               m_native_fallback_message.c_str());
  ERROR_LOG_FMT(POWERPC, "StaticRecomp: {}", m_native_fallback_message);
  Host_Message(HostMessageID::WMUserStop);
  m_system.GetCPU().Break();
}

void StaticRecompCore::Shutdown()
{
  g_static_recomp_core = nullptr;
  std::fprintf(stderr,
               "[staticrecomp] shutdown: native=%llu fallback=%llu native_exc=%llu hook_fb=%llu "
               "native_shims=%llu native_aliases=%llu native_rel=%llu "
               "rel_links=%llu rel_unlinks=%llu rel_refreshes=%llu "
               "fallback_entries=%llu native_reentries=%llu first_fallback=0x%08X "
               "first_reentry=0x%08X smc_failed=%u verifications=%llu reverify_events=%llu "
               "bursts=%llu charged_cycles=%llu idle_skips=%llu traced_functions=%llu\n",
               (unsigned long long)m_native_dispatches, (unsigned long long)m_fallback_steps,
               (unsigned long long)m_native_exceptions,
               (unsigned long long)m_hook_fallback_instructions,
               (unsigned long long)m_native_shim_instructions,
               (unsigned long long)m_native_alias_entries,
               (unsigned long long)m_native_rel_dispatches,
               (unsigned long long)m_rel_link_generations,
               (unsigned long long)m_rel_unlink_generations,
               (unsigned long long)m_rel_binding_refreshes,
               (unsigned long long)m_fallback_entries,
               (unsigned long long)m_native_reentries, m_first_fallback_pc,
               m_first_native_reentry_pc, m_failed_chunks,
               (unsigned long long)m_verifications, (unsigned long long)m_reverify_events,
               (unsigned long long)m_bursts, (unsigned long long)m_charged_cycles,
               (unsigned long long)m_idle_skips,
               (unsigned long long)m_traced_function_entries);
  const auto print_top_pcs = [](const char* label, const std::unordered_map<u32, u64>& counts) {
    std::vector<std::pair<u32, u64>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
      return a.second > b.second;
    });
    std::fprintf(stderr, "[staticrecomp] %s:", label);
    for (size_t i = 0; i < std::min<size_t>(8, sorted.size()); ++i)
      std::fprintf(stderr, " 0x%08X=%llu", sorted[i].first,
                   (unsigned long long)sorted[i].second);
    std::fprintf(stderr, "\n");
  };
  print_top_pcs("fallback-pcs", m_fallback_pc_counts);
  print_top_pcs("reentry-pcs", m_reentry_pc_counts);
  print_top_pcs("external-irqs(cause&mask)", m_external_irq_counts);
  print_top_pcs("native-pc-samples(1/1024)", m_native_pc_samples);
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: shutdown. native_dispatches={} fallback_steps={} "
                 "native_exceptions={} hook_fallback_instructions={} smc_failed_chunks={} "
                 "verifications={} reverify_events={}",
                 m_native_dispatches, m_fallback_steps, m_native_exceptions,
                 m_hook_fallback_instructions, m_failed_chunks, m_verifications,
                 m_reverify_events);
  m_lockstep_verifier.reset();
  m_block_cache.Shutdown();
  m_module = nullptr;
  m_rel_bindings.clear();
  m_rel_bindings_valid = false;
  m_take_host_event = nullptr;
  if (m_library.IsOpen())
    m_library.Close();

  if (m_fallback_jit)
  {
    m_fallback_jit->Shutdown();
    m_fallback_jit.reset();
  }
}

void StaticRecompCore::LoadModule()
{
  m_take_host_event = nullptr;
  if (m_module_source.kind == StaticRecompModuleSource::Kind::None)
  {
    NOTICE_LOG_FMT(POWERPC, "StaticRecomp: no explicit module source; interpreter-only.");
    return;
  }

  const std::string game_id = SConfig::GetInstance().GetGameID();
  std::string path = m_module_source.path;
  const StaticRecompModuleDesc* desc = nullptr;
  if (m_module_source.kind == StaticRecompModuleSource::Kind::AttachedDescriptor)
  {
    desc = m_module_source.descriptor;
  }
  else
  {
    if (path.empty() || !File::Exists(path) || !m_library.Open(path.c_str()))
    {
      ERROR_LOG_FMT(POWERPC, "StaticRecomp: failed to open explicit module '{}'.", path);
      return;
    }
    const auto get_module = reinterpret_cast<StaticRecompGetModuleFn>(
        m_library.GetSymbolAddress(STATICRECOMP_GET_MODULE_SYMBOL));
    desc = get_module ? get_module() : nullptr;
  }

  const auto reject = [&](const std::string& why) {
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: rejecting module '{}': {}. Interpreter-only.", path, why);
    m_module = nullptr;
    m_take_host_event = nullptr;
    if (m_library.IsOpen())
      m_library.Close();
  };

  if (!desc)
    return reject("missing or null " STATICRECOMP_GET_MODULE_SYMBOL);
  if (desc->abi_version != STATICRECOMP_ABI_VERSION &&
      desc->abi_version != STATICRECOMP_ABI_VERSION_V3)
    return reject(fmt::format("unsupported abi_version {}", desc->abi_version));
  if (desc->cpu_abi_version != GXRUNTIME_CPU_ABI_VERSION)
    return reject(fmt::format("cpu_abi_version {} != {}", desc->cpu_abi_version,
                              GXRUNTIME_CPU_ABI_VERSION));
  if (desc->cpu_state_size != sizeof(CPUState))
    return reject(fmt::format("cpu_state_size {} != sizeof(CPUState) {}", desc->cpu_state_size,
                              sizeof(CPUState)));
  if (!desc->dispatch || !desc->code_ranges || desc->num_code_ranges == 0)
    return reject("no dispatch entry or empty code ranges");
  if (!std::memchr(desc->game_id, '\0', sizeof(desc->game_id)) || desc->game_id[0] == '\0')
    return reject("invalid game_id");
  if (!RangesAreSorted(desc->code_ranges, desc->num_code_ranges))
    return reject("malformed or overlapping code ranges");
  if (desc->num_smc_ranges != 0 && !RangesAreSorted(desc->smc_ranges, desc->num_smc_ranges))
    return reject("malformed or overlapping SMC ranges");
  if (!desc->chunk_ranges || desc->num_chunk_ranges == 0 || !desc->chunk_hashes ||
      !desc->chunk_functions)
    return reject("no chunk ranges/hashes/functions (required for direct verified dispatch)");
  if (!ChunksTileCode(*desc))
    return reject("chunk ranges do not exactly tile code ranges");
  if (!RelCatalogIsValid(*desc))
    return reject("malformed REL catalog");
  if (!AddressIsCovered(desc->code_ranges, desc->num_code_ranges, desc->entry_point))
    return reject("entry point is not covered by the module");
  if (!game_id.empty() && game_id != desc->game_id)
    return reject(fmt::format("module game_id '{}' != running game '{}'", desc->game_id, game_id));

  m_module = desc;
  m_take_host_event =
      m_library.IsOpen() ? reinterpret_cast<StaticRecompTakeHostEventFn>(
                               m_library.GetSymbolAddress(STATICRECOMP_TAKE_HOST_EVENT_SYMBOL)) :
                           nullptr;
  m_module_active = (desc != nullptr);
  m_chunk_state.assign(desc->num_chunk_ranges, CHUNK_UNVERIFIED);
  m_failed_chunks = 0;
  m_lookup_ram_size = 0;
  m_lookup_exram_size = 0;
  m_chunk_lookup_table.clear();
  m_rel_bindings.clear();
  m_rel_bindings_valid = false;

  // Generated native code currently treats guest instruction-cache
  // operations as coherence notifications rather than modelling the Gekko
  // cache contents. Keep interpreter fallback on that same model. Games such
  // as Wind Waker repeatedly load different RELs at the same guest addresses;
  // leaving Dolphin's interpreter cache enabled can otherwise execute bytes
  // from the previously loaded REL after native code has installed a new one.
  m_system.GetPPCState().iCache.m_disable_icache = true;
  Config::SetCurrent(Config::MAIN_DISABLE_ICACHE, true);

  std::fprintf(stderr, "[staticrecomp] module loaded: %s entry=0x%08X\n", path.c_str(),
               desc->entry_point);
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: loaded module '{}' (game_id={} entry=0x{:08X} "
                 "code_ranges={} smc_ranges={})",
                 path, desc->game_id, desc->entry_point, desc->num_code_ranges,
                 desc->num_smc_ranges);
}

void StaticRecompCore::ClearCache()
{
  if (m_fallback_jit)
    m_fallback_jit->ClearCache();

  if (!m_module)
    return;
  std::fill(m_chunk_state.begin(), m_chunk_state.end(), u8{CHUNK_UNVERIFIED});
  m_failed_chunks = 0;
  m_rel_bindings_valid = false;
  ++m_reverify_events;
}
