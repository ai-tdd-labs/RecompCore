// RecompCore: StaticRecomp CPU core - decomp symbol-map tracing.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <string_view>
#include <utility>
#include <vector>

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

void WriteJsonString(std::ostream& output, std::string_view value)
{
  output << '"';
  for (const unsigned char character : value)
  {
    switch (character)
    {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (character < 0x20)
      {
        constexpr char hex[] = "0123456789ABCDEF";
        output << "\\u00" << hex[character >> 4] << hex[character & 0x0F];
      }
      else
      {
        output << static_cast<char>(character);
      }
      break;
    }
  }
  output << '"';
}
}  // namespace

void StaticRecompCore::LoadFunctionSymbols()
{
  m_trace_all_functions = Config::Get(Config::MAIN_STATICRECOMP_TRACE_FUNCTIONS);
  m_trace_function = Config::Get(Config::MAIN_STATICRECOMP_TRACE_FUNCTION);
  m_function_profile_path = Config::Get(Config::MAIN_STATICRECOMP_FUNCTION_PROFILE);
  m_function_profile_start_frame =
      Config::Get(Config::MAIN_STATICRECOMP_FUNCTION_PROFILE_START_FRAME);
  m_function_profile_end_frame = Config::Get(Config::MAIN_STATICRECOMP_FUNCTION_PROFILE_END_FRAME);
  if (!m_trace_all_functions && m_trace_function.empty() && m_function_profile_path.empty())
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
    if (m_function_symbols.try_emplace(address, name).second)
      m_function_symbol_addresses.push_back(address);
  }
  std::sort(m_function_symbol_addresses.begin(), m_function_symbol_addresses.end());

  std::fprintf(stderr,
               "[staticrecomp:symbols] loaded=%zu map='%s' mode=%s filter='%s' profile='%s' "
               "profile_frames=%llu-%llu\n",
               m_function_symbols.size(), path.c_str(),
               m_trace_all_functions ? "all" : "filtered", m_trace_function.c_str(),
               m_function_profile_path.c_str(),
               static_cast<unsigned long long>(m_function_profile_start_frame),
               static_cast<unsigned long long>(m_function_profile_end_frame));
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

void StaticRecompCore::SampleFunction(u32 address, u64 movie_frame)
{
  // The profiler samples one finished native dispatch in 1,024. It therefore
  // ranks time hot spots without paying a symbol-map lookup on every dispatch.
  const auto next = std::upper_bound(m_function_symbol_addresses.begin(),
                                     m_function_symbol_addresses.end(), address);
  if (next == m_function_symbol_addresses.begin())
    return;
  ++m_profiled_function_samples[*std::prev(next)];
  if (m_profile_first_movie_frame == 0)
    m_profile_first_movie_frame = movie_frame;
  m_profile_last_movie_frame = movie_frame;
}

void StaticRecompCore::WriteFunctionProfile()
{
  if (m_function_profile_path.empty())
    return;

  std::vector<std::pair<u32, u64>> samples(m_profiled_function_samples.begin(),
                                            m_profiled_function_samples.end());
  std::sort(samples.begin(), samples.end(), [](const auto& left, const auto& right) {
    return left.second != right.second ? left.second > right.second : left.first < right.first;
  });

  const std::filesystem::path path(m_function_profile_path);
  std::error_code error;
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream output(path, std::ios::trunc);
  if (!output)
  {
    std::fprintf(stderr, "[staticrecomp:function-profile] unable to write '%s'\n",
                 m_function_profile_path.c_str());
    return;
  }

  u64 total_samples = 0;
  for (const auto& sample : samples)
    total_samples += sample.second;

  output << "{\n  \"schema\": \"moderngekko.function-profile.v1\",\n"
         << "  \"native_dispatches\": " << m_native_dispatches << ",\n"
         << "  \"sample_period_dispatches\": 1024,\n"
         << "  \"movie_frame_start\": " << m_function_profile_start_frame << ",\n"
         << "  \"movie_frame_end\": " << m_function_profile_end_frame << ",\n"
         << "  \"sampled_movie_frame_start\": " << m_profile_first_movie_frame << ",\n"
         << "  \"sampled_movie_frame_end\": " << m_profile_last_movie_frame << ",\n"
         << "  \"function_samples\": " << total_samples << ",\n"
         << "  \"unique_functions\": " << samples.size() << ",\n"
         << "  \"functions\": [\n";
  for (size_t index = 0; index < samples.size(); ++index)
  {
    const auto [address, count] = samples[index];
    output << "    {\"address\": \"0x";
    char address_text[9]{};
    std::snprintf(address_text, sizeof(address_text), "%08X", address);
    output << address_text << "\", \"name\": ";
    WriteJsonString(output, m_function_symbols.at(address));
    output << ", \"samples\": " << count << "}";
    output << (index + 1 == samples.size() ? "\n" : ",\n");
  }
  output << "  ]\n}\n";
  output.close();

  std::fprintf(stderr,
               "[staticrecomp:function-profile] wrote=%zu samples=%llu native=%llu path='%s'\n",
               samples.size(), static_cast<unsigned long long>(total_samples),
               static_cast<unsigned long long>(m_native_dispatches), m_function_profile_path.c_str());
  std::fprintf(stderr, "[staticrecomp:function-profile] top:");
  for (size_t index = 0; index < std::min<size_t>(8, samples.size()); ++index)
  {
    const auto [address, count] = samples[index];
    std::fprintf(stderr, " %s@0x%08X=%llu", m_function_symbols.at(address).c_str(), address,
                 static_cast<unsigned long long>(count));
  }
  std::fprintf(stderr, "\n");
}
