// RecompCore: StaticRecomp CPU core - decomp symbol-map tracing.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <string_view>

#include "Common/Config/Config.h"
#include "Core/Config/StaticRecompSettings.h"
#include "Core/CoreTiming.h"
#include "Core/Movie.h"
#include "Core/System.h"

#if defined(_MSC_VER)
#define STATICRECOMP_NOINLINE __declspec(noinline)
#else
#define STATICRECOMP_NOINLINE __attribute__((noinline, used, visibility("default")))
#endif

extern "C" STATICRECOMP_NOINLINE void staticrecomp_symbol_trace_probe(
    CPUState* state, u32 address, const char* name)
{
  // This deliberately observable no-op gives LLDB/GDB one stable host symbol
  // for every selected guest function, while the arguments preserve the
  // complete CPU state, guest address, and decomp name.
#if defined(_MSC_VER)
  (void)state;
  (void)address;
  (void)name;
#else
  asm volatile("" : : "r"(state), "r"(address), "r"(name) : "memory");
#endif
}

namespace
{
std::string_view Trim(std::string_view value)
{
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

bool NameMatches(std::string_view raw_name, std::string_view requested)
{
  if (requested.empty() || raw_name == requested)
    return true;
  return raw_name.size() > requested.size() + 2 && raw_name.starts_with(requested) &&
         raw_name.substr(requested.size(), 2) == "__";
}
}  // namespace

void StaticRecompCore::LoadFunctionSymbols()
{
  m_trace_all_functions = Config::Get(Config::MAIN_STATICRECOMP_TRACE_FUNCTIONS);
  m_trace_function = Config::Get(Config::MAIN_STATICRECOMP_TRACE_FUNCTION);
  if (!m_trace_all_functions && m_trace_function.empty())
    return;

  const std::string path = Config::Get(Config::MAIN_STATICRECOMP_SYMBOL_MAP);
  std::ifstream input(path);
  if (!input)
  {
    std::fprintf(stderr, "[staticrecomp:symbols] unable to open '%s'\n", path.c_str());
    return;
  }

  std::string line;
  while (std::getline(input, line))
  {
    if (line.find("type:function") == std::string::npos)
      continue;
    const size_t equals = line.find('=');
    const size_t address_prefix = line.find("0x", equals);
    const size_t semicolon = line.find(';', address_prefix);
    if (equals == std::string::npos || address_prefix == std::string::npos ||
        semicolon == std::string::npos)
      continue;

    const std::string_view name = Trim(std::string_view(line).substr(0, equals));
    const std::string_view address_text =
        std::string_view(line).substr(address_prefix + 2, semicolon - address_prefix - 2);
    u32 address = 0;
    const auto parsed =
        std::from_chars(address_text.data(), address_text.data() + address_text.size(), address, 16);
    if (name.empty() || parsed.ec != std::errc{})
      continue;
    m_function_symbols.try_emplace(address, name);
  }

  std::fprintf(stderr,
               "[staticrecomp:symbols] loaded=%zu map='%s' mode=%s filter='%s'\n",
               m_function_symbols.size(), path.c_str(),
               m_trace_all_functions ? "all" : "filtered", m_trace_function.c_str());
}

void StaticRecompCore::TraceFunctionEntry()
{
  if (m_function_symbols.empty())
    return;
  const auto symbol = m_function_symbols.find(m_guest.pc);
  if (symbol == m_function_symbols.end() ||
      (!m_trace_all_functions && !NameMatches(symbol->second, m_trace_function)))
    return;

  ++m_traced_function_entries;
  const auto& system = Core::System::GetInstance();
  const auto& movie = system.GetMovie();
  std::fprintf(stderr,
               "[staticrecomp:function] movie_frame=%llu movie_input=%llu core_tick=%llu "
               "pc=0x%08X lr=0x%08X name=%s r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X "
               "f1=%.9g f2=%.9g f3=%.9g f4=%.9g\n",
               static_cast<unsigned long long>(movie.GetCurrentFrame()),
               static_cast<unsigned long long>(movie.GetCurrentInputCount()),
               static_cast<unsigned long long>(system.GetCoreTiming().GetTicks()), m_guest.pc,
               m_guest.lr, symbol->second.c_str(), m_guest.gpr[3], m_guest.gpr[4], m_guest.gpr[5],
               m_guest.gpr[6], m_guest.fpr[1], m_guest.fpr[2], m_guest.fpr[3], m_guest.fpr[4]);
  staticrecomp_symbol_trace_probe(&m_guest, m_guest.pc, symbol->second.c_str());
}
