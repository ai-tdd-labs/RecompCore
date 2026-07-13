// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/Interpreter/Interpreter.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/GekkoDisassembler.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/Debugger_SymbolMap.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/CPU.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/PowerPC/Interpreter/ExceptionUtils.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/OpcodeDecoding.h"

namespace
{
std::once_flag s_parity_event_init;
std::mutex s_parity_event_mutex;
FILE* s_parity_event_file = nullptr;
std::atomic<u64> s_parity_event_sequence{0};
std::atomic<bool> s_parity_fine_window_active{false};
std::unordered_map<std::string, u64> s_parity_kind_counts;
std::atomic<bool> s_parity_all_function_window_active{false};
u32 s_parity_pending_call_target = 0;
u64 s_parity_all_function_count = 0;

struct ParityJkrAllocation
{
  u32 heap = 0;
  u32 size = 0;
  u32 alignment = 0;
  u32 caller_lr = 0;
};

// Keyed by the caller stack pointer.  The wrapper reserves 16 bytes, so its
// common return PC can recover the exact request with sp + 16 even when the
// virtual allocator clobbered volatile registers.
std::unordered_map<u32, ParityJkrAllocation> s_parity_jkr_allocations;

int ParityCaptureLevel()
{
  static const int level = [] {
    const char* raw = std::getenv("DOLPHIN_PARITY_LEVEL");
    const int parsed = raw ? std::atoi(raw) : 0;
    return parsed >= 1 && parsed <= 5 ? parsed : 2;
  }();
  return level;
}

bool IsCompactParitySystemFamily(const char* family)
{
  if (!family)
    return false;
  static constexpr std::array<const char*, 20> families = {
      "thread", "scheduler", "queue", "timer", "interrupt", "time", "vi", "dvd",
      "aram", "input", "dsp", "audio", "exi", "rtc", "savecard", "bba", "loader",
      "allocation", "rng", "resource",
  };
  return std::any_of(families.begin(), families.end(), [family](const char* candidate) {
    return std::strcmp(family, candidate) == 0;
  });
}

struct ParityLockstepConfig
{
  bool enabled = false;
  bool started = false;
  u32 start_pc = 0;
  u32 end_pc = 0;
  u64 limit = 1000;
  u64 emitted = 0;
};

struct ParityRegisterConfig
{
  struct Target
  {
    u32 pc = 0;
    std::string function_name;
  };

  std::vector<Target> targets;
};

void AddParityRegisterTarget(ParityRegisterConfig* config, const char* raw_pc,
                             const std::string& function_name)
{
  if (!raw_pc || !raw_pc[0] || function_name.empty())
    return;
  const u32 pc = static_cast<u32>(std::strtoul(raw_pc, nullptr, 0));
  if (pc != 0)
    config->targets.push_back({pc, function_name});
}

const ParityRegisterConfig& SelectedParityRegisterConfig()
{
  static const ParityRegisterConfig config = [] {
    ParityRegisterConfig value;
    const char* raw_targets = std::getenv("DOLPHIN_PARITY_REGISTER_TARGETS");
    if (raw_targets && raw_targets[0])
    {
      std::string targets(raw_targets);
      size_t pos = 0;
      while (pos < targets.size())
      {
        const size_t comma = targets.find(',', pos);
        const std::string token = targets.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos);
        const size_t separator = token.find('=');
        if (separator != std::string::npos)
        {
          const std::string raw_pc = token.substr(0, separator);
          AddParityRegisterTarget(&value, raw_pc.c_str(), token.substr(separator + 1));
        }
        if (comma == std::string::npos)
          break;
        pos = comma + 1;
      }
      return value;
    }
    const char* raw_pc = std::getenv("DOLPHIN_PARITY_REGISTER_PC");
    const char* raw_name = std::getenv("DOLPHIN_PARITY_REGISTER_FUNC");
    AddParityRegisterTarget(&value, raw_pc, raw_name ? raw_name : "");
    return value;
  }();
  return config;
}

struct ParityAllFunctionConfig
{
  bool enabled = false;
  u32 start_pc = 0;
  u64 limit = 100000;
};

const ParityAllFunctionConfig& AllFunctionParityConfig()
{
  static const ParityAllFunctionConfig config = [] {
    ParityAllFunctionConfig value;
    const char* enabled = std::getenv("DOLPHIN_PARITY_TRACE_ALL_FUNCTIONS");
    value.enabled = enabled && enabled[0] && enabled[0] != '0';
    if (const char* raw_start = std::getenv("DOLPHIN_PARITY_ALL_FUNCTION_START_PC"))
      value.start_pc = static_cast<u32>(std::strtoul(raw_start, nullptr, 0));
    if (const char* raw_limit = std::getenv("DOLPHIN_PARITY_ALL_FUNCTION_LIMIT"))
    {
      const u64 parsed = std::strtoull(raw_limit, nullptr, 0);
      if (parsed != 0)
        value.limit = parsed;
    }
    return value;
  }();
  return config;
}

u32 ParityFunctionId(const char* name)
{
  u32 hash = 2166136261u;
  for (const unsigned char ch : std::string(name))
    hash = (hash ^ ch) * 16777619u;
  return hash;
}

void CloseParityEventFile()
{
  std::lock_guard lk(s_parity_event_mutex);
  if (!s_parity_event_file)
    return;
  std::fflush(s_parity_event_file);
  std::fclose(s_parity_event_file);
  s_parity_event_file = nullptr;
}

void InitParityEventFile()
{
  const char* path = std::getenv("DOLPHIN_PARITY_EVENT_FILE");
  if (!path || !path[0])
    return;
  s_parity_event_file = std::fopen(path, "w");
  if (s_parity_event_file)
    std::atexit(CloseParityEventFile);
}

void EmitParityEvent(Core::System& system, PowerPC::PowerPCState& state, PowerPC::MMU& mmu,
                     const char* family, const char* action, u32 subject,
                     u64 a, u64 b, u64 c, u64 d)
{
  (void)state;
  static const bool capture_boot_events = [] {
    const char* raw = std::getenv("DOLPHIN_PARITY_CAPTURE_BOOT_EVENTS");
    return raw && raw[0] && raw[0] != '0';
  }();
  // Level 2 is the compact always-on lane around the selected FIFO/frame
  // window. Emitting every OSGetTime/queue entry throughout interpreter boot
  // produced megabytes before the first presented frame and prevented the
  // requested window from being reached. Boot-wide events remain an explicit
  // opt-in for dedicated investigations.
  if (!capture_boot_events && !OpcodeDecoder::g_record_fifo_data)
    return;
  std::call_once(s_parity_event_init, InitParityEventFile);
  if (!s_parity_event_file)
    return;
  const u64 timebase = system.GetSystemTimers().GetFakeTimeBase();
  std::lock_guard lk(s_parity_event_mutex);
  if (ParityCaptureLevel() <= 2 || IsCompactParitySystemFamily(family))
  {
    static const u64 max_per_kind = [] {
      const char* raw = std::getenv("DOLPHIN_PARITY_LEVEL2_MAX_PER_KIND");
      const u64 parsed = raw ? std::strtoull(raw, nullptr, 0) : 0;
      return parsed ? parsed : 64u;
    }();
    std::string key(family);
    key.push_back('\0');
    key.append(action);
    const u64 occurrence = s_parity_kind_counts[key]++;
    if (occurrence >= max_per_kind)
      return;
  }
  const u64 sequence = s_parity_event_sequence.fetch_add(1);
  const u32 guest_thread = mmu.Read<u32>(0x800000E4u);
  const u64 draw = OpcodeDecoder::GetParityEventDrawAnchor();
  std::fprintf(s_parity_event_file,
               "{\"record\":\"event\",\"sequence\":%llu,\"family\":\"%s\","
               "\"action\":\"%s\",\"subject\":%u,\"anchor\":{"
               "\"timebase\":%llu,\"retrace\":null,\"copy_epoch\":null,\"draw\":%llu},"
               "\"data\":{\"a\":%llu,\"b\":%llu,\"c\":%llu,\"d\":%llu,"
               "\"thread\":%u}}\n",
               static_cast<unsigned long long>(sequence), family, action, subject,
               static_cast<unsigned long long>(timebase),
               static_cast<unsigned long long>(draw),
               static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
               static_cast<unsigned long long>(c), static_cast<unsigned long long>(d),
               guest_thread);
  // Boot milestones are used as process-stop boundaries by the headless
  // oracle.  Keeping them in stdio's buffer can make a successful IPL boot
  // look like a timeout until another 1024 events happen to flush the file.
  if (std::strcmp(family, "boot") == 0 || (sequence & 0x3ffu) == 0)
    std::fflush(s_parity_event_file);
}

bool ParityBranchCondition(const PowerPC::PowerPCState& state, UGeckoInstruction inst,
                           bool decrement_ctr)
{
  u32 ctr = state.spr[SPR_CTR];
  if (decrement_ctr && (inst.BO_2 & BO_DONT_DECREMENT_FLAG) == 0)
    --ctr;
  const u32 counter = ((inst.BO_2 >> 2) | ((ctr != 0) ^ (inst.BO_2 >> 1))) & 1;
  const u32 condition =
      ((inst.BO_2 >> 4) | (state.cr.GetBit(inst.BI_2) == ((inst.BO_2 >> 3) & 1))) & 1;
  return (counter & condition) != 0;
}

u32 ParityTakenCallTarget(const PowerPC::PowerPCState& state, UGeckoInstruction inst)
{
  if (inst.OPCD == 18 && inst.LK)
  {
    u32 target = u32(SignExt26(inst.LI << 2));
    return inst.AA ? target : target + state.pc;
  }
  if (inst.OPCD == 16 && inst.LK_2 && ParityBranchCondition(state, inst, true))
  {
    u32 target = u32(SignExt16(s16(inst.BD << 2)));
    return inst.AA_2 ? target : target + state.pc;
  }
  if (inst.OPCD != 19 || !inst.LK_3)
    return 0;
  if (inst.SUBOP10 == 16 && ParityBranchCondition(state, inst, true))
    return state.spr[SPR_LR] & ~3u;
  if (inst.SUBOP10 == 528 && ParityBranchCondition(state, inst, false))
    return state.spr[SPR_CTR] & ~3u;
  return 0;
}

void TraceParityAllFunctionEntry(Core::System& system, PowerPC::PowerPCState& state,
                                 PowerPC::MMU& mmu, PPCSymbolDB& symbol_db)
{
  const ParityAllFunctionConfig& config = AllFunctionParityConfig();
  if (!config.enabled || ParityCaptureLevel() < 3)
    return;

  if (!s_parity_all_function_window_active.load(std::memory_order_acquire) &&
      config.start_pc != 0 && state.pc == config.start_pc)
  {
    s_parity_all_function_window_active.store(true, std::memory_order_release);
    s_parity_all_function_count = 0;
    EmitParityEvent(system, state, mmu, "function", "enter", state.pc,
                    state.gpr[3], state.gpr[4], state.gpr[5], state.spr[SPR_LR]);
    ++s_parity_all_function_count;
  }
  if (!s_parity_all_function_window_active.load(std::memory_order_acquire))
    return;

  if (s_parity_pending_call_target != 0)
  {
    if (state.pc == s_parity_pending_call_target && s_parity_all_function_count < config.limit)
    {
      EmitParityEvent(system, state, mmu, "function", "enter", state.pc,
                      state.gpr[3], state.gpr[4], state.gpr[5], state.spr[SPR_LR]);
      ++s_parity_all_function_count;
    }
    s_parity_pending_call_target = 0;
  }
  if (s_parity_all_function_count >= config.limit)
  {
    s_parity_all_function_window_active.store(false, std::memory_order_release);
    return;
  }
  const UGeckoInstruction inst(mmu.Read_Opcode(state.pc));
  const u32 target = ParityTakenCallTarget(state, inst);
  const Common::Symbol* symbol = target != 0 ? symbol_db.GetSymbolFromAddr(target) : nullptr;
  // Linking branches are occasionally used as local PC/LR tricks. Generated
  // recomp prologues exist only at real function starts, so suppress a target
  // proven to be an interior label while retaining unknown/dynamic targets.
  s_parity_pending_call_target = symbol && symbol->address != target ? 0 : target;
}

void EmitParityRegisterCheckpoint(Core::System& system, PowerPC::PowerPCState& state,
                                  PowerPC::MMU& mmu, const char* function_name)
{
  if (ParityCaptureLevel() < 3)
    return;
  std::call_once(s_parity_event_init, InitParityEventFile);
  if (!s_parity_event_file)
    return;
  const u64 sequence = s_parity_event_sequence.fetch_add(1);
  const u64 timebase = system.GetSystemTimers().GetFakeTimeBase();
  const u32 function_id = ParityFunctionId(function_name);
  const u32 guest_thread = mmu.Read<u32>(0x800000E4u);
  const u64 draw = OpcodeDecoder::GetParityEventDrawAnchor();
  std::lock_guard lk(s_parity_event_mutex);
  std::fprintf(s_parity_event_file,
               "{\"record\":\"event\",\"sequence\":%llu,\"family\":\"register\","
               "\"action\":\"checkpoint\",\"subject\":%u,\"anchor\":{"
               "\"timebase\":%llu,\"retrace\":null,\"copy_epoch\":null,\"draw\":%llu},"
               "\"data\":{\"thread\":%u,\"gpr\":[",
               static_cast<unsigned long long>(sequence), function_id,
               static_cast<unsigned long long>(timebase),
               static_cast<unsigned long long>(draw), guest_thread);
  for (size_t index = 0; index < std::size(state.gpr); ++index)
    std::fprintf(s_parity_event_file, "%s%u", index ? "," : "", state.gpr[index]);
  std::fputs("],\"fpr\":[", s_parity_event_file);
  for (size_t index = 0; index < std::size(state.ps); ++index)
  {
    const auto& ps = state.ps[index];
    std::fprintf(s_parity_event_file, "%s[%llu,%llu]", index ? "," : "",
                 static_cast<unsigned long long>(ps.PS0AsU64()),
                 static_cast<unsigned long long>(ps.PS1AsU64()));
  }
  std::fprintf(s_parity_event_file,
               "],\"cr\":%u,\"lr\":%u,\"ctr\":%u,\"xer\":%u,\"fpscr\":%u,"
               "\"gqr\":[",
               state.cr.Get(), state.spr[SPR_LR], state.spr[SPR_CTR], state.spr[SPR_XER],
               state.fpscr.Hex);
  for (u32 index = 0; index < 8; ++index)
    std::fprintf(s_parity_event_file, "%s%u", index ? "," : "", state.spr[SPR_GQR0 + index]);
  std::fprintf(s_parity_event_file,
               "],\"spr\":{\"pvr\":%u,\"hid0\":%u,\"hid1\":%u,"
               "\"hid2\":%u,\"hid4\":%u,\"l2cr\":%u}}}\n",
               state.spr[SPR_PVR], state.spr[SPR_HID0], state.spr[SPR_HID1],
               state.spr[SPR_HID2], state.spr[SPR_HID4], state.spr[SPR_L2CR]);
  std::fflush(s_parity_event_file);
}

void TraceParityInstruction(Core::System& system, PowerPC::PowerPCState& state,
                            PowerPC::MMU& mmu)
{
  static ParityLockstepConfig config = [] {
    ParityLockstepConfig value;
    const char* enabled = std::getenv("DOLPHIN_PARITY_LOCKSTEP");
    if (!enabled || !enabled[0] || enabled[0] == '0')
      return value;
    value.enabled = true;
    if (const char* start = std::getenv("DOLPHIN_PARITY_LOCKSTEP_START_PC"))
      value.start_pc = static_cast<u32>(std::strtoul(start, nullptr, 0));
    if (const char* end = std::getenv("DOLPHIN_PARITY_LOCKSTEP_END_PC"))
      value.end_pc = static_cast<u32>(std::strtoul(end, nullptr, 0));
    if (const char* limit = std::getenv("DOLPHIN_PARITY_LOCKSTEP_LIMIT"))
    {
      const u64 parsed = std::strtoull(limit, nullptr, 0);
      if (parsed != 0)
        value.limit = parsed;
    }
    value.started = value.start_pc == 0;
    return value;
  }();
  if (!config.enabled || config.emitted >= config.limit)
    return;
  if (!config.started)
  {
    if (state.pc != config.start_pc)
      return;
    config.started = true;
  }
  // A static-recompiler Level-5 slice instruments only the selected guest-PC
  // range.  Apply the same filter in Dolphin so calls into nested functions
  // do not look like a control-flow divergence; execution resumes at the
  // caller's next in-range instruction and remains directly comparable.
  if (config.end_pc != 0 &&
      (state.pc < config.start_pc || state.pc > config.end_pc))
    return;

  std::call_once(s_parity_event_init, InitParityEventFile);
  if (!s_parity_event_file)
    return;
  const u64 sequence = s_parity_event_sequence.fetch_add(1);
  const u64 timebase = system.GetSystemTimers().GetFakeTimeBase();
  const u32 opcode = mmu.Read<u32>(state.pc);
  const u64 draw = OpcodeDecoder::GetParityEventDrawAnchor();
  std::lock_guard lk(s_parity_event_mutex);
  std::fprintf(s_parity_event_file,
               "{\"record\":\"event\",\"sequence\":%llu,\"family\":\"instruction\","
               "\"action\":\"checkpoint\",\"subject\":%u,\"anchor\":{"
               "\"timebase\":%llu,\"retrace\":null,\"copy_epoch\":null,\"draw\":%llu},"
               "\"data\":{\"opcode\":%u,\"gpr\":[",
               static_cast<unsigned long long>(sequence), state.pc,
               static_cast<unsigned long long>(timebase),
               static_cast<unsigned long long>(draw), opcode);
  for (size_t index = 0; index < std::size(state.gpr); ++index)
    std::fprintf(s_parity_event_file, "%s%u", index ? "," : "", state.gpr[index]);
  std::fprintf(s_parity_event_file, "],\"fpr\":[");
  for (size_t index = 0; index < std::size(state.ps); ++index)
  {
    const auto& ps = state.ps[index];
    std::fprintf(s_parity_event_file, "%s[%llu,%llu]", index ? "," : "",
                 static_cast<unsigned long long>(ps.PS0AsU64()),
                 static_cast<unsigned long long>(ps.PS1AsU64()));
  }
  std::fprintf(s_parity_event_file,
               "],\"cr\":%u,\"lr\":%u,\"ctr\":%u,\"xer\":%u,\"fpscr\":%u,"
               "\"gqr\":[",
               state.cr.Get(), state.spr[SPR_LR], state.spr[SPR_CTR], state.spr[SPR_XER],
               state.fpscr.Hex);
  for (u32 index = 0; index < 8; ++index)
    std::fprintf(s_parity_event_file, "%s%u", index ? "," : "", state.spr[SPR_GQR0 + index]);
  std::fprintf(s_parity_event_file, "]}}\n");
  std::fflush(s_parity_event_file);
  ++config.emitted;
}

void TraceParityFloatingPoint(Core::System& system, PowerPC::PowerPCState& state,
                              PowerPC::MMU& mmu)
{
  if (ParityCaptureLevel() < 4)
    return;
  static const u32 start_pc = [] {
    const char* raw = std::getenv("DOLPHIN_PARITY_LOCKSTEP_START_PC");
    return raw ? static_cast<u32>(std::strtoul(raw, nullptr, 0)) : 0u;
  }();
  static const u64 limit = [] {
    const char* raw = std::getenv("DOLPHIN_PARITY_LOCKSTEP_LIMIT");
    const u64 parsed = raw ? std::strtoull(raw, nullptr, 0) : 0;
    return parsed ? parsed : 1000u;
  }();
  static bool started = start_pc == 0;
  static u64 observed = 0;
  if (!started)
  {
    if (state.pc != start_pc)
      return;
    started = true;
  }
  if (observed++ >= limit)
  {
    s_parity_fine_window_active.store(false, std::memory_order_release);
    return;
  }
  s_parity_fine_window_active.store(true, std::memory_order_release);
  const u32 opcode = mmu.Read<u32>(state.pc);
  const u32 primary = opcode >> 26;
  const bool paired = primary == 4;
  const bool floating = paired || primary == 56 || primary == 57 || primary == 59 ||
                        primary == 60 || primary == 61 || primary == 63;
  if (!floating)
    return;
  u64 fpr_hash = 1469598103934665603ull;
  const auto hash_u64 = [&](u64 value) {
    for (u32 shift = 0; shift < 64; shift += 8)
      fpr_hash = (fpr_hash ^ static_cast<u8>(value >> shift)) * 1099511628211ull;
  };
  for (const auto& ps : state.ps)
  {
    hash_u64(ps.PS0AsU64());
    if (paired)
      hash_u64(ps.PS1AsU64());
  }
  u64 gqr_hash = 1469598103934665603ull;
  for (u32 index = 0; index < 8; ++index)
  {
    const u32 value = state.spr[SPR_GQR0 + index];
    for (u32 shift = 0; shift < 32; shift += 8)
      gqr_hash = (gqr_hash ^ static_cast<u8>(value >> shift)) * 1099511628211ull;
  }
  EmitParityEvent(system, state, mmu, "floating_point", "checkpoint", state.pc,
                  opcode, fpr_hash, state.fpscr.Hex, gqr_hash);
}

void TraceParityMemoryPages(Core::System& system, PowerPC::PowerPCState& state,
                            PowerPC::MMU& mmu)
{
  struct Region
  {
    u32 address;
    u32 length;
  };
  struct Config
  {
    std::vector<Region> regions;
    u32 page_size = 0x10000u;
    u32 samples = 8;
  };
  static const Config config = [] {
    Config out;
    const char* raw = std::getenv("DOLPHIN_PARITY_MEMORY_PAGES");
    if (!raw || !raw[0])
      return out;
    std::string value(raw);
    size_t pos = 0;
    while (pos < value.size())
    {
      const size_t comma = value.find(',', pos);
      const std::string token = value.substr(
          pos, comma == std::string::npos ? std::string::npos : comma - pos);
      char* end = nullptr;
      const unsigned long address = std::strtoul(token.c_str(), &end, 0);
      unsigned long length = 0;
      if (end && (*end == ':' || *end == '+'))
        length = std::strtoul(end + 1, nullptr, 0);
      if (address && length)
        out.regions.push_back({static_cast<u32>(address), static_cast<u32>(length)});
      if (comma == std::string::npos)
        break;
      pos = comma + 1;
    }
    if (const char* page = std::getenv("DOLPHIN_PARITY_MEMORY_PAGE_SIZE"))
    {
      const unsigned long parsed = std::strtoul(page, nullptr, 0);
      if (parsed >= 0x100u && parsed <= 0x100000u)
        out.page_size = static_cast<u32>(parsed);
    }
    if (const char* count = std::getenv("DOLPHIN_PARITY_MEMORY_PAGE_SAMPLES"))
    {
      const unsigned long parsed = std::strtoul(count, nullptr, 0);
      if (parsed)
        out.samples = static_cast<u32>(parsed);
    }
    return out;
  }();
  static u32 sample = 0;
  if (config.regions.empty() || sample >= config.samples)
    return;
  const u32 checkpoint = ++sample;
  for (const Region& region : config.regions)
  {
    const u64 region_end = u64{region.address} + region.length;
    for (u64 address = region.address; address < region_end; address += config.page_size)
    {
      const u32 length = static_cast<u32>(
          std::min<u64>(config.page_size, region_end - address));
      u64 hash = 1469598103934665603ull;
      for (u32 index = 0; index < length; ++index)
        hash = (hash ^ mmu.Read<u8>(static_cast<u32>(address) + index)) * 1099511628211ull;
      EmitParityEvent(system, state, mmu, "memory", "page_hash",
                      static_cast<u32>(address), length, hash, checkpoint, config.page_size);
    }
  }
}

void TraceAnimalCrossingParityEvent(Core::System& system, PowerPC::PowerPCState& state,
                                    PowerPC::MMU& mmu, PPCSymbolDB& symbol_db)
{
  static u64 pad_reads = 0;
  TraceParityFloatingPoint(system, state, mmu);
  TraceParityInstruction(system, state, mmu);
  const u32 pc = state.pc;
  static const u32 boot_state_pc = [] {
    const char* raw = std::getenv("DOLPHIN_PARITY_BOOT_STATE_PC");
    return raw && raw[0] ? static_cast<u32>(std::strtoul(raw, nullptr, 0)) : 0u;
  }();
  static bool boot_state_emitted = false;
  if (!boot_state_emitted && boot_state_pc != 0 && pc == boot_state_pc)
  {
    boot_state_emitted = true;
    EmitParityEvent(system, state, mmu, "boot", "dol_handoff", pc, state.gpr[1],
                    state.gpr[2], state.gpr[13], state.msr.Hex);
    EmitParityRegisterCheckpoint(system, state, mmu, "boot.dol_handoff");
    const auto snapshot = system.GetVideoInterface().GetParityTimingSnapshot(
        system.GetCoreTiming().GetTicks());
    EmitParityEvent(system, state, mmu, "boot", "vi_state", snapshot.half_line,
                    snapshot.half_lines_per_frame, snapshot.ticks_per_half_line,
                    snapshot.ticks_until_interrupt, snapshot.ticks_per_field);
  }
  const ParityRegisterConfig& selected_registers = SelectedParityRegisterConfig();
  if (ParityCaptureLevel() >= 3)
  {
    for (const auto& selected_register : selected_registers.targets)
    {
      if (pc != selected_register.pc)
        continue;
      const u32 function_id = ParityFunctionId(selected_register.function_name.c_str());
      EmitParityEvent(system, state, mmu, "function", "enter", function_id,
                      state.gpr[3], state.gpr[4], state.gpr[5], state.spr[SPR_LR]);
      EmitParityRegisterCheckpoint(system, state, mmu,
                                   selected_register.function_name.c_str());
      break;
    }
  }
  TraceParityAllFunctionEntry(system, state, mmu, symbol_db);
  bool have_current_thread = false;
  u32 cached_current_thread = 0;
  const auto current_thread = [&]() {
    if (!have_current_thread)
    {
      cached_current_thread = mmu.Read<u32>(0x800000E4u);
      have_current_thread = true;
    }
    return cached_current_thread;
  };
  const u32 lr = state.spr[SPR_LR];
  const u32 sp = state.gpr[1];
  const auto emit_initial_menu_memory = [&](u32 function_id) {
    constexpr u32 address = 0x800E2680u;
    constexpr u32 size = 0x100u;
    u64 hash = 1469598103934665603ull;
    for (u32 offset = 0; offset < size; ++offset)
      hash = (hash ^ mmu.Read<u8>(address + offset)) * 1099511628211ull;
    EmitParityEvent(system, state, mmu, "memory", "checkpoint", address, size, hash,
                    function_id, lr);
  };
  if (ParityCaptureLevel() >= 4 &&
      (pc == 0x80000100u || pc == 0x80000200u || pc == 0x80000300u ||
       pc == 0x80000600u || pc == 0x80000700u || pc == 0x80000900u))
  {
    EmitParityEvent(system, state, mmu, "exception", "entry", pc,
                    SRR0(state), SRR1(state), state.spr[SPR_DAR], state.spr[SPR_DSISR]);
  }
  switch (pc)
  {
  case 0x80007CB0u:
    if (ParityCaptureLevel() >= 3)
    {
      EmitParityEvent(system, state, mmu, "function", "enter",
                      ParityFunctionId("__imp__initial_menu_init"), state.gpr[3], state.gpr[4],
                      state.gpr[5], lr);
      EmitParityRegisterCheckpoint(system, state, mmu, "__imp__initial_menu_init");
    }
    break;
  case 0x8005CF08u:
    if (ParityCaptureLevel() < 3)
      break;
    EmitParityEvent(system, state, mmu, "allocation", "alloc_request", state.gpr[3],
                    lr, sp, 32, 0);
    break;
  case 0x8005CF34u:
    if (ParityCaptureLevel() < 3)
      break;
    EmitParityEvent(system, state, mmu, "allocation", "free", state.gpr[3],
                    lr, sp, 0, 0);
    break;
  case 0x8005CCCCu:
  case 0x8005CCECu:
  case 0x8005CCF4u:
  {
    if (ParityCaptureLevel() < 3)
      break;
    const char* name = pc == 0x8005CCCCu ? "__imp__qrand" :
                       (pc == 0x8005CCECu ? "__imp__sqrand" : "__imp__fqrand");
    const u32 function_id = ParityFunctionId(name);
    EmitParityEvent(system, state, mmu, "function", "enter", function_id,
                    state.gpr[3], state.gpr[4], state.gpr[5], lr);
    const u32 seed_addr = state.gpr[13] - 32152u;
    EmitParityEvent(system, state, mmu, "rng", "seed", seed_addr,
                    mmu.Read<u32>(seed_addr), function_id, lr, 0);
    EmitParityRegisterCheckpoint(system, state, mmu, name);
    break;
  }
  case 0x80079720u:
    if (ParityCaptureLevel() < 3)
      break;
    EmitParityEvent(system, state, mmu, "allocation", "heap_alloc_request", state.gpr[3],
                    state.gpr[4], lr, sp, 0);
    break;
  case 0x80063A50u:
    if (ParityCaptureLevel() < 3)
      break;
    // JKRHeap::alloc(size, alignment, heap) is the allocation boundary used
    // by JKRThread for its stack and OSThread object.  Capturing the explicit
    // heap as the subject makes heap-stream alignment source-neutral while
    // retaining the caller and stack for bounded causal zoom.
    EmitParityEvent(system, state, mmu, "allocation", "jkr_alloc_request", state.gpr[5],
                    state.gpr[3], state.gpr[4], lr, sp);
    s_parity_jkr_allocations[sp] = {
        .heap = state.gpr[5],
        .size = state.gpr[3],
        .alignment = state.gpr[4],
        .caller_lr = lr,
    };
    EmitParityRegisterCheckpoint(system, state, mmu, "alloc__7JKRHeapFUliP7JKRHeap");
    break;
  case 0x80063AB8u:
  {
    if (ParityCaptureLevel() < 3)
      break;
    const auto request = s_parity_jkr_allocations.find(sp + 16u);
    if (request == s_parity_jkr_allocations.end())
      break;
    EmitParityEvent(system, state, mmu, "allocation", "jkr_alloc_result", state.gpr[3],
                    request->second.heap, request->second.size, request->second.alignment,
                    request->second.caller_lr);
    s_parity_jkr_allocations.erase(request);
    break;
  }
  case 0x80079B54u:
    if (ParityCaptureLevel() < 4)
      break;
    EmitParityEvent(system, state, mmu, "cache", "DCInvalidateRange", state.gpr[3],
                    state.gpr[4], lr, 0, 0);
    break;
  case 0x80079B84u:
    if (ParityCaptureLevel() < 4)
      break;
    EmitParityEvent(system, state, mmu, "cache", "DCFlushRange", state.gpr[3],
                    state.gpr[4], lr, 0, 0);
    break;
  case 0x80079BB8u:
    if (ParityCaptureLevel() < 4)
      break;
    EmitParityEvent(system, state, mmu, "cache", "DCStoreRange", state.gpr[3],
                    state.gpr[4], lr, 0, 0);
    break;
  case 0x80079BECu:
    if (ParityCaptureLevel() < 4)
      break;
    EmitParityEvent(system, state, mmu, "cache", "DCFlushRangeNoSync", state.gpr[3],
                    state.gpr[4], lr, 0, 0);
    break;
  case 0x80079C1Cu:
    if (ParityCaptureLevel() < 4)
      break;
    EmitParityEvent(system, state, mmu, "cache", "DCStoreRangeNoSync", state.gpr[3],
                    state.gpr[4], lr, 0, 0);
    break;
  case 0x80079CACu:
    if (ParityCaptureLevel() < 4)
      break;
    EmitParityEvent(system, state, mmu, "cache", "ICInvalidateRange", state.gpr[3],
                    state.gpr[4], lr, 0, 0);
    break;
  case 0x80007A20u:
  {
    const u32 thread = current_thread();
    const u32 priority = thread ? mmu.Read<u32>(thread + 0x2D0u) : 16u;
    EmitParityEvent(system, state, mmu, "scheduler", "dispatch", thread, pc,
                    state.gpr[3], sp, priority);
    if (ParityCaptureLevel() >= 3)
    {
      EmitParityEvent(system, state, mmu, "function", "enter", ParityFunctionId("__imp__proc"),
                      state.gpr[3], state.gpr[4], state.gpr[5], lr);
      emit_initial_menu_memory(ParityFunctionId("__imp__proc"));
      EmitParityRegisterCheckpoint(system, state, mmu, "__imp__proc");
    }
    break;
  }
  case 0x80007660u:
    if (ParityCaptureLevel() >= 3)
    {
      EmitParityEvent(system, state, mmu, "function", "enter", ParityFunctionId("__imp__keycheck"),
                      state.gpr[3], state.gpr[4], state.gpr[5], lr);
      emit_initial_menu_memory(ParityFunctionId("__imp__keycheck"));
      EmitParityRegisterCheckpoint(system, state, mmu, "__imp__keycheck");
    }
    break;
  case 0x8007E2BCu:
    EmitParityEvent(system, state, mmu, "thread", "create", state.gpr[3], state.gpr[4],
                    state.gpr[5], state.gpr[8], state.gpr[6]);
    s_parity_all_function_window_active.store(false, std::memory_order_release);
    s_parity_pending_call_target = 0;
    break;
  case 0x8007E85Cu:
    EmitParityEvent(system, state, mmu, "thread", "resume_request", state.gpr[3],
                    current_thread(), lr, sp, 0);
    break;
  case 0x8007BC80u:
    EmitParityEvent(system, state, mmu, "queue", "send_begin", state.gpr[3], state.gpr[4],
                    state.gpr[5], current_thread(), lr);
    break;
  case 0x8007BD48u:
    EmitParityEvent(system, state, mmu, "queue", "receive_begin", state.gpr[3], state.gpr[4],
                    state.gpr[5], current_thread(), lr);
    break;
  case 0x8007931Cu:
  {
    const bool shifted_ostime = (state.gpr[7] & 0x80000000u) != 0;
    const u64 tick = shifted_ostime ?
                         ((u64{state.gpr[5]} << 32) | state.gpr[6]) :
                         ((u64{state.gpr[4]} << 32) | state.gpr[5]);
    const u32 callback = shifted_ostime ? state.gpr[7] : state.gpr[6];
    EmitParityEvent(system, state, mmu, "timer", "set", state.gpr[3],
                    tick, callback, current_thread(), lr);
    break;
  }
  case 0x8007AC24u:
    EmitParityEvent(system, state, mmu, "interrupt", "disable", current_thread(), 0, 0, lr, 0);
    break;
  case 0x80078F04u:
    if (ParityCaptureLevel() < 4)
      break;
    EmitParityEvent(system, state, mmu, "exception", "set_handler", state.gpr[3],
                    state.gpr[4], lr, 0, 0);
    break;
  case 0x8007AC38u:
    EmitParityEvent(system, state, mmu, "interrupt", "enable", current_thread(), 0, lr, 0, 0);
    break;
  case 0x8007AC4Cu:
    EmitParityEvent(system, state, mmu, "interrupt", "restore", current_thread(),
                    state.gpr[3], 0, 0, lr);
    break;
  case 0x8007F6F8u:
    EmitParityEvent(system, state, mmu, "time", "get_time", current_thread(),
                    system.GetSystemTimers().GetFakeTimeBase(), lr, sp, 0);
    break;
  case 0x8007B718u:
  {
    const u32 module = state.gpr[3];
    const u32 module_id = module ? mmu.Read<u32>(module) : 0u;
    EmitParityEvent(system, state, mmu, "loader", "rel_link_begin", module,
                    module_id, state.gpr[4], lr, 0);
    break;
  }
  case 0x80088740u:
    EmitParityEvent(system, state, mmu, "vi", "wait_begin", current_thread(), 0, lr, sp, 0);
    TraceParityMemoryPages(system, state, mmu);
    break;
  case 0x80084A9Cu:
    EmitParityEvent(system, state, mmu, "dvd", "open", state.gpr[4], state.gpr[3],
                    current_thread(), lr, 0);
    break;
  case 0x80084DACu:
    EmitParityEvent(system, state, mmu, "dvd", "read_async", state.gpr[3], state.gpr[4],
                    state.gpr[5], state.gpr[6], lr);
    break;
  case 0x80086C50u:
    EmitParityEvent(system, state, mmu, "dvd", "read_abs_async", state.gpr[3], state.gpr[4],
                    state.gpr[5], state.gpr[6], lr);
    break;
  case 0x8008C274u:
    EmitParityEvent(system, state, mmu, "aram", "dma", state.gpr[3], state.gpr[4],
                    state.gpr[5], state.gpr[6], lr);
    break;
  case 0x8008D05Cu:
    EmitParityEvent(system, state, mmu, "aram", "request", state.gpr[3], state.gpr[4],
                    state.gpr[5], state.gpr[6], lr);
    break;
  case 0x8008A9A8u:
    EmitParityEvent(system, state, mmu, "input", "pad_read", state.gpr[3],
                    ++pad_reads, 0, 0, 0);
    break;
  case 0x8008B928u:
    EmitParityEvent(system, state, mmu, "audio", "register_dma_callback", state.gpr[3],
                    0, 0, 0, 0);
    break;
  case 0x8008B96Cu:
    EmitParityEvent(system, state, mmu, "audio", "init_dma", state.gpr[3],
                    state.gpr[4], 0, 0, 0);
    if (state.gpr[4] <= 0x100000u)
    {
      u64 hash = 1469598103934665603ull;
      for (u32 index = 0; index < state.gpr[4]; ++index)
        hash = (hash ^ mmu.Read<u8>(state.gpr[3] + index)) * 1099511628211ull;
      EmitParityEvent(system, state, mmu, "audio", "buffer_hash", state.gpr[3],
                      state.gpr[4], hash, 0, 0);
    }
    break;
  case 0x8008B9F4u:
    EmitParityEvent(system, state, mmu, "audio", "start_dma", current_thread(),
                    lr, sp, 0, 0);
    break;
  case 0x800907E0u:
    EmitParityEvent(system, state, mmu, "savecard", "unmount", state.gpr[3],
                    lr, 0, 0, 0);
    break;
  case 0x800914A4u:
    EmitParityEvent(system, state, mmu, "savecard", "open", state.gpr[3],
                    state.gpr[4], state.gpr[5], lr, 0);
    break;
  case 0x80091CF8u:
    EmitParityEvent(system, state, mmu, "savecard", "read", state.gpr[3],
                    state.gpr[4], state.gpr[5], state.gpr[6], state.gpr[7]);
    break;
  case 0x800917A8u:
    EmitParityEvent(system, state, mmu, "savecard", "create", state.gpr[3],
                    state.gpr[4], state.gpr[5], state.gpr[6], state.gpr[7]);
    break;
  case 0x800920A8u:
    EmitParityEvent(system, state, mmu, "savecard", "write", state.gpr[3],
                    state.gpr[4], state.gpr[5], state.gpr[6], state.gpr[7]);
    break;
  default:
    break;
  }
}

// Determines whether or not the given instruction is one where its execution
// validity is determined by whether or not HID2's LSQE bit is set.
// In other words, if the instruction is psq_l, psq_lu, psq_st, or psq_stu
bool IsPairedSingleQuantizedNonIndexedInstruction(UGeckoInstruction inst)
{
  const u32 opcode = inst.OPCD;
  return opcode == 0x38 || opcode == 0x39 || opcode == 0x3C || opcode == 0x3D;
}

bool IsPairedSingleInstruction(UGeckoInstruction inst)
{
  return inst.OPCD == 4 || IsPairedSingleQuantizedNonIndexedInstruction(inst);
}
}  // namespace

namespace
{
void EmitRawParityEvent(Core::System& system, const char* family, const char* action,
                        u32 subject, u64 a, u64 b, u64 c, u64 d)
{
  std::call_once(s_parity_event_init, InitParityEventFile);
  if (!s_parity_event_file)
    return;
  const u64 sequence = s_parity_event_sequence.fetch_add(1);
  const u64 timebase = system.GetSystemTimers().GetFakeTimeBase();
  const u64 draw = OpcodeDecoder::GetParityEventDrawAnchor();
  std::lock_guard lk(s_parity_event_mutex);
  std::fprintf(s_parity_event_file,
               "{\"record\":\"event\",\"sequence\":%llu,\"family\":\"%s\","
               "\"action\":\"%s\",\"subject\":%u,\"anchor\":{"
               "\"timebase\":%llu,\"retrace\":null,\"copy_epoch\":null,\"draw\":%llu},"
               "\"data\":{\"a\":%llu,\"b\":%llu,\"c\":%llu,\"d\":%llu}}\n",
               static_cast<unsigned long long>(sequence), family, action, subject,
               static_cast<unsigned long long>(timebase),
               static_cast<unsigned long long>(draw),
               static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
               static_cast<unsigned long long>(c), static_cast<unsigned long long>(d));
  std::fflush(s_parity_event_file);
}
}  // namespace

namespace PowerPC
{
void DolphinParityTraceBootContext(Core::System& system, const char* action, u32 pc)
{
  auto& state = system.GetPPCState();
  auto& mmu = system.GetMMU();
  EmitParityEvent(system, state, mmu, "boot", action, pc, state.gpr[3], state.gpr[4],
                  state.gpr[5], state.msr.Hex);
  const std::string checkpoint_name = fmt::format("boot.{}", action);
  EmitParityRegisterCheckpoint(system, state, mmu, checkpoint_name.c_str());
}

void DolphinParityTraceHardwareEvent(Core::System& system, const char* family,
                                     const char* action, u32 subject, u64 a,
                                     u64 b, u64 c, u64 d)
{
  if (ParityCaptureLevel() < 2)
    return;
  EmitRawParityEvent(system, family, action, subject, a, b, c, d);
}
}  // namespace PowerPC

namespace MMIO
{
void DolphinParityTraceMMIO(Core::System& system, bool write, u32 addr,
                            u32 size, u64 value)
{
  if (ParityCaptureLevel() < 4 ||
      !s_parity_fine_window_active.load(std::memory_order_acquire))
    return;
  const u32 guest_addr = addr < 0x10000000u ? addr | 0xC0000000u : addr;
  EmitRawParityEvent(system, "mmio", write ? "store" : "load", guest_addr,
                     size, value, system.GetPPCState().pc, 0);
}
}  // namespace MMIO

namespace PowerPC
{
void DolphinParityTraceMemoryWrite(Core::System& system, u32 pc, u32 addr,
                                   u32 size, u32 value)
{
  struct Range
  {
    bool enabled = false;
    u32 start = 0;
    u32 end = 0;
  };
  static const Range range = [] {
    Range out;
    const char* raw = std::getenv("DOLPHIN_PARITY_MEMORY_WRITE_RANGE");
    if (!raw || !raw[0])
      return out;
    char* end = nullptr;
    const unsigned long start = std::strtoul(raw, &end, 0);
    unsigned long finish = start;
    if (end && (*end == ':' || *end == '-' || *end == '+'))
    {
      const char separator = *end++;
      const unsigned long rhs = std::strtoul(end, nullptr, 0);
      finish = separator == '+' ? start + rhs : rhs;
    }
    out.enabled = true;
    out.start = static_cast<u32>(start);
    out.end = static_cast<u32>(std::max(start, finish));
    return out;
  }();
  if (ParityCaptureLevel() < 4 || !range.enabled ||
      !s_parity_fine_window_active.load(std::memory_order_acquire))
    return;
  const u32 guest_addr = addr < 0x10000000u ? addr | 0x80000000u : addr;
  const u64 last = u64{guest_addr} + (size ? size - 1u : 0u);
  if (last < range.start || guest_addr > range.end)
    return;
  EmitRawParityEvent(system, "memory", "write", guest_addr,
                     (u64{pc} << 32) | size, value, 0, 0);
}
}  // namespace PowerPC

// Checks if a given instruction would be illegal to execute if it's a paired single instruction.
//
// Paired single instructions are illegal to execute if HID2.PSE is not set.
// It's also illegal to execute psq_l, psq_lu, psq_st, and psq_stu if HID2.PSE is enabled,
// but HID2.LSQE is not set.
bool Interpreter::IsInvalidPairedSingleExecution(UGeckoInstruction inst)
{
  if (!HID2(m_ppc_state).PSE && IsPairedSingleInstruction(inst))
    return true;

  return HID2(m_ppc_state).PSE && !HID2(m_ppc_state).LSQE &&
         IsPairedSingleQuantizedNonIndexedInstruction(inst);
}

void Interpreter::UpdatePC()
{
  m_last_pc = m_ppc_state.pc;
  m_ppc_state.pc = m_ppc_state.npc;
}

Interpreter::Interpreter(Core::System& system, PowerPC::PowerPCState& ppc_state, PowerPC::MMU& mmu,
                         Core::BranchWatch& branch_watch, PPCSymbolDB& ppc_symbol_db)
    : m_system(system), m_ppc_state(ppc_state), m_mmu(mmu), m_branch_watch(branch_watch),
      m_ppc_symbol_db(ppc_symbol_db)
{
}

Interpreter::~Interpreter() = default;

void Interpreter::Init()
{
  m_end_block = false;
  // Create the sidecar as soon as the selected CPU core starts. An empty file
  // then means "no configured checkpoint was reached" instead of being
  // indistinguishable from a missing/disabled interpreter probe.
  std::call_once(s_parity_event_init, InitParityEventFile);
}

void Interpreter::Shutdown()
{
}

void Interpreter::Trace(const UGeckoInstruction& inst)
{
  std::string regs;
  for (size_t i = 0; i < std::size(m_ppc_state.gpr); i++)
  {
    regs += fmt::format("r{:02d}: {:08x} ", i, m_ppc_state.gpr[i]);
  }

  std::string fregs;
  for (size_t i = 0; i < std::size(m_ppc_state.ps); i++)
  {
    const auto& ps = m_ppc_state.ps[i];
    fregs += fmt::format("f{:02d}: {:08x} {:08x} ", i, ps.PS0AsU64(), ps.PS1AsU64());
  }

  const std::string ppc_inst = Common::GekkoDisassembler::Disassemble(inst.hex, m_ppc_state.pc);
  DEBUG_LOG_FMT(POWERPC,
                "INTER PC: {:08x} SRR0: {:08x} SRR1: {:08x} CRval: {:016x} "
                "FPSCR: {:08x} MSR: {:08x} LR: {:08x} {} {:08x} {}",
                m_ppc_state.pc, SRR0(m_ppc_state), SRR1(m_ppc_state), m_ppc_state.cr.fields[0],
                m_ppc_state.fpscr.Hex, m_ppc_state.msr.Hex, m_ppc_state.spr[SPR_LR], regs, inst.hex,
                ppc_inst);
}

bool Interpreter::HandleFunctionHooking(u32 address)
{
  const auto result =
      HLE::TryReplaceFunction(m_ppc_symbol_db, address, PowerPC::CoreMode::Interpreter);
  if (!result)
    return false;

  HLEFunction(*this, result.hook_index);

  return result.type != HLE::HookType::Start;
}

int Interpreter::SingleStepInner()
{
  TraceAnimalCrossingParityEvent(m_system, m_ppc_state, m_mmu, m_ppc_symbol_db);
  if (HandleFunctionHooking(m_ppc_state.pc))
  {
    UpdatePC();
    // TODO: Does it make sense to use m_prev_inst here?
    // It seems like we should use the num_cycles for the instruction at PC instead
    // (m_prev_inst has not yet been updated)
    return PPCTables::GetOpInfo(m_prev_inst, m_ppc_state.pc)->num_cycles;
  }

  m_ppc_state.npc = m_ppc_state.pc + sizeof(UGeckoInstruction);
  m_prev_inst.hex = m_mmu.Read_Opcode(m_ppc_state.pc);

  const GekkoOPInfo* opinfo = PPCTables::GetOpInfo(m_prev_inst, m_ppc_state.pc);

  // Uncomment to trace the interpreter
  // if ((m_ppc_state.pc & 0x00FFFFFF) >= 0x000AB54C &&
  //     (m_ppc_state.pc & 0x00FFFFFF) <= 0x000AB624)
  // {
  //   m_start_trace = true;
  // }
  // else
  // {
  //   m_start_trace = false;
  // }

  if (m_start_trace)
  {
    Trace(m_prev_inst);
  }

  if (m_prev_inst.hex != 0)
  {
    if (IsInvalidPairedSingleExecution(m_prev_inst))
    {
      GenerateProgramException(m_ppc_state, ProgramExceptionCause::IllegalInstruction);
      CheckExceptions();
    }
    else if (m_ppc_state.msr.FP)
    {
      RunInterpreterOp(*this, m_prev_inst);
      if ((m_ppc_state.Exceptions & EXCEPTION_DSI) != 0)
      {
        CheckExceptions();
      }
    }
    else
    {
      // check if we have to generate a FPU unavailable exception or a program exception.
      if ((opinfo->flags & FL_USE_FPU) != 0)
      {
        m_ppc_state.Exceptions |= EXCEPTION_FPU_UNAVAILABLE;
        CheckExceptions();
      }
      else
      {
        RunInterpreterOp(*this, m_prev_inst);
        if ((m_ppc_state.Exceptions & EXCEPTION_DSI) != 0)
        {
          CheckExceptions();
        }
      }
    }
  }
  else
  {
    // Memory exception on instruction fetch
    CheckExceptions();
  }

  UpdatePC();

  PowerPC::UpdatePerformanceMonitor(opinfo->num_cycles, (opinfo->flags & FL_LOADSTORE) != 0,
                                    (opinfo->flags & FL_USE_FPU) != 0, m_ppc_state);
  return opinfo->num_cycles;
}

void Interpreter::SingleStep()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& core_timing_globals = core_timing.GetGlobals();

  // Declare start of new slice
  core_timing.Advance();

  SingleStepInner();

  // The interpreter ignores instruction timing information outside the 'fast runloop'.
  core_timing_globals.slice_length = 1;
  m_ppc_state.downcount = 0;

  if (m_ppc_state.Exceptions != 0)
  {
    m_system.GetPowerPC().CheckExceptions();
    m_ppc_state.pc = m_ppc_state.npc;
  }
}

// #define SHOW_HISTORY
#ifdef SHOW_HISTORY
static std::vector<u32> s_pc_vec;
static std::vector<u32> s_pc_block_vec;
constexpr u32 s_show_blocks = 30;
constexpr u32 s_show_steps = 300;
#endif

// FastRun - inspired by GCemu (to imitate the JIT so that they can be compared).
void Interpreter::Run()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& cpu = m_system.GetCPU();
  auto& power_pc = m_system.GetPowerPC();
  while (cpu.GetState() == CPU::State::Running)
  {
    // CoreTiming Advance() ends the previous slice and declares the start of the next
    // one so it must always be called at the start. At boot, we are in slice -1 and must
    // advance into slice 0 to get a correct slice length before executing any cycles.
    core_timing.Advance();

    // we have to check exceptions at branches apparently (or maybe just rfi?)
    if (Config::IsDebuggingEnabled())
    {
#ifdef SHOW_HISTORY
      s_pc_block_vec.push_back(m_ppc_state.pc);
      if (s_pc_block_vec.size() > s_show_blocks)
        s_pc_block_vec.erase(s_pc_block_vec.begin());
#endif

      // Debugging friendly version of inner loop. Tries to do the timing as similarly to the
      // JIT as possible. Does not take into account that some instructions take multiple cycles.
      while (m_ppc_state.downcount > 0)
      {
        m_end_block = false;
        int cycles = 0;
        while (!m_end_block)
        {
#ifdef SHOW_HISTORY
          s_pc_vec.push_back(m_ppc_state.pc);
          if (s_pc_vec.size() > s_show_steps)
            s_pc_vec.erase(s_pc_vec.begin());
#endif

          if (power_pc.CheckAndHandleBreakPoints())
            return;
          cycles += SingleStepInner();
        }
        m_ppc_state.downcount -= cycles;
      }
    }
    else
    {
      // "fast" version of inner loop. well, it's not so fast.
      while (m_ppc_state.downcount > 0)
      {
        m_end_block = false;

        int cycles = 0;
        while (!m_end_block)
        {
          cycles += SingleStepInner();
        }
        m_ppc_state.downcount -= cycles;
      }
    }
  }
}

void Interpreter::unknown_instruction(Interpreter& interpreter, UGeckoInstruction inst)
{
  ASSERT(Core::IsCPUThread());
  auto& system = interpreter.m_system;
  Core::CPUThreadGuard guard(system);

  const u32 last_pc = interpreter.m_last_pc;
  const u32 opcode = PowerPC::MMU::HostRead<u32>(guard, last_pc);
  const std::string disasm = Common::GekkoDisassembler::Disassemble(opcode, last_pc);
  NOTICE_LOG_FMT(POWERPC, "Last PC = {:08x} : {}", last_pc, disasm);
  Dolphin_Debugger::PrintCallstack(guard, Common::Log::LogType::POWERPC,
                                   Common::Log::LogLevel::LNOTICE);

  const auto& ppc_state = interpreter.m_ppc_state;
  NOTICE_LOG_FMT(
      POWERPC,
      "\nIntCPU: Unknown instruction {:08x} at PC = {:08x}  last_PC = {:08x}  LR = {:08x}\n",
      inst.hex, ppc_state.pc, last_pc, LR(ppc_state));
  for (int i = 0; i < 32; i += 4)
  {
    NOTICE_LOG_FMT(POWERPC, "r{}: {:#010x} r{}: {:#010x} r{}: {:#010x} r{}: {:#010x}", i,
                   ppc_state.gpr[i], i + 1, ppc_state.gpr[i + 1], i + 2, ppc_state.gpr[i + 2],
                   i + 3, ppc_state.gpr[i + 3]);
  }
  ASSERT_MSG(POWERPC, 0,
             "\nIntCPU: Unknown instruction {:08x} at PC = {:08x}  last_PC = {:08x}  LR = {:08x}\n",
             inst.hex, ppc_state.pc, last_pc, LR(ppc_state));
  if (system.IsPauseOnPanicMode())
    system.GetCPU().Break();
}

void Interpreter::ClearCache()
{
  // Do nothing.
}

void Interpreter::CheckExceptions()
{
  m_system.GetPowerPC().CheckExceptions();
  m_end_block = true;
}

const char* Interpreter::GetName() const
{
#ifdef _ARCH_64
  return "Interpreter64";
#else
  return "Interpreter32";
#endif
}
