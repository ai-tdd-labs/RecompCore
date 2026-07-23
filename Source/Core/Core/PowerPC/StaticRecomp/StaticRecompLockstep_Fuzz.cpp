// RecompCore: deterministic DolRecomp opcode differential driver.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace StaticRecompLockstep
{
namespace
{
// Use the physical MEM1 alias: this bank runs before a normal title has
// installed its virtual-memory layout.
constexpr u32 SCRATCH_ADDRESS = 0x00200000u;
constexpr u32 SCRATCH_SIZE = 0x1000u;
constexpr u32 STACK_ADDRESS = 0x80310000u;
constexpr u32 RETURN_SENTINEL = 0x81234560u;
constexpr u64 DEFAULT_SEED = 0x9E3779B97F4A7C15ull;

struct FuzzBlock
{
  u32 address = 0;
  u32 raw = 0;
  std::string name;
};

class FuzzRandom
{
public:
  explicit FuzzRandom(u64 seed) : m_state(seed != 0 ? seed : DEFAULT_SEED) {}

  u64 Next64()
  {
    // xorshift64*: deliberately tiny and fully reproducible across hosts.
    m_state ^= m_state >> 12;
    m_state ^= m_state << 25;
    m_state ^= m_state >> 27;
    return m_state * 0x2545F4914F6CDD1Dull;
  }

  u32 Next32() { return static_cast<u32>(Next64() >> 32); }
  u8 Next8() { return static_cast<u8>(Next64() >> 56); }

private:
  u64 m_state;
};

bool ParseManifest(const char* path, std::vector<FuzzBlock>* blocks)
{
  std::ifstream input(path);
  if (!input)
  {
    std::fprintf(stderr, "[opfuzz] cannot open manifest: %s\n", path);
    return false;
  }

  std::string line;
  size_t line_number = 0;
  while (std::getline(input, line))
  {
    ++line_number;
    if (line.empty() || line[0] == '#')
      continue;

    std::istringstream fields(line);
    std::string address_text;
    std::string raw_text;
    FuzzBlock block;
    if (!(fields >> address_text >> block.name >> raw_text))
    {
      std::fprintf(stderr, "[opfuzz] malformed manifest line %zu: %s\n", line_number,
                   line.c_str());
      return false;
    }

    char* end = nullptr;
    block.address = static_cast<u32>(std::strtoull(address_text.c_str(), &end, 0));
    if (end == address_text.c_str() || *end != '\0')
      return false;
    end = nullptr;
    block.raw = static_cast<u32>(std::strtoull(raw_text.c_str(), &end, 0));
    if (end == raw_text.c_str() || *end != '\0')
      return false;
    blocks->push_back(std::move(block));
  }

  if (blocks->empty())
  {
    std::fprintf(stderr, "[opfuzz] manifest contains no blocks: %s\n", path);
    return false;
  }
  return true;
}

u32 ReadBe32(const u8* bytes)
{
  return (static_cast<u32>(bytes[0]) << 24) | (static_cast<u32>(bytes[1]) << 16) |
         (static_cast<u32>(bytes[2]) << 8) | static_cast<u32>(bytes[3]);
}

bool LoadSyntheticDol(const char* path, CPUState* guest)
{
  std::ifstream input(path, std::ios::binary);
  if (!input)
  {
    std::fprintf(stderr, "[opfuzz] cannot open DOL: %s\n", path);
    return false;
  }
  input.seekg(0, std::ios::end);
  const std::streamoff length = input.tellg();
  if (length < 0x100)
  {
    std::fprintf(stderr, "[opfuzz] malformed DOL: %s\n", path);
    return false;
  }
  input.seekg(0);
  std::vector<u8> dol(static_cast<size_t>(length));
  if (!input.read(reinterpret_cast<char*>(dol.data()), length))
  {
    std::fprintf(stderr, "[opfuzz] cannot read DOL: %s\n", path);
    return false;
  }

  // DOL has seven text and eleven data sections. Load both: the generated
  // bank presently uses only text, but the loader remains correct when a
  // future sequence corpus embeds tables.
  for (u32 section = 0; section < 18; ++section)
  {
    const u32 file_offset = ReadBe32(dol.data() + section * 4u);
    const u32 address = ReadBe32(dol.data() + 0x48u + section * 4u);
    const u32 size = ReadBe32(dol.data() + 0x90u + section * 4u);
    if (size == 0)
      continue;
    if (file_offset > dol.size() || size > dol.size() - file_offset || address < 0x80000000u ||
        address - 0x80000000u > guest->ram_size ||
        size > guest->ram_size - (address - 0x80000000u))
    {
      std::fprintf(stderr, "[opfuzz] unsupported DOL section=%u\n", section);
      return false;
    }
    std::memcpy(guest->ram + (address - 0x80000000u), dol.data() + file_offset, size);
  }
  return true;
}

u32 ReadGuestInstruction(const CPUState& guest, u32 address)
{
  if (address < 0x80000000u)
    return 0;
  const u32 offset = address - 0x80000000u;
  if (offset + 4u > guest.ram_size)
    return 0;
  const u8* p = guest.ram + offset;
  return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
         (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
}

double DoubleFromBits(u64 bits)
{
  double value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

double RandomFloatValue(FuzzRandom& random, u32 slot, u32 seed_index)
{
  // Repeat a compact edge corpus between random finite values. Quiet NaNs are
  // included because their sign/payload and FPSCR classification exposed real
  // paired-single emitter bugs in the older bank.
  constexpr std::array<u64, 12> edges = {
      0x0000000000000000ull, 0x8000000000000000ull, 0x3ff0000000000000ull,
      0xbff0000000000000ull, 0x0010000000000000ull, 0x0000000000000001ull,
      0x7fefffffffffffffull, 0xffefffffffffffffull, 0x7ff0000000000000ull,
      0xfff0000000000000ull, 0x7ff8000000000001ull, 0xfff8000000000042ull,
  };
  if (((slot + seed_index) % 3u) != 0u)
  {
    u64 bits = edges[(slot * 5u + seed_index) % edges.size()];
    // FPSCR.NI flushes denormal results.  The direct-register fuzzer must not
    // inject a denormal value after it has declared NI active: on host FPUs
    // that turns the harness setup, rather than the guest instruction, into
    // the source of the result. Dedicated load/store tests cover transitions
    // into NI mode separately.
    if ((seed_index & 4u) != 0u && (bits & (0x7ffull << 52)) == 0 &&
        (bits & ((1ull << 52) - 1)) != 0)
      bits &= 1ull << 63;
    return DoubleFromBits(bits);
  }

  // Force a finite exponent while retaining a broad mantissa/sign spread.
  u64 bits = random.Next64();
  bits &= ~(0x7ffull << 52);
  bits |= (static_cast<u64>((random.Next32() % 0x7feu) + 1u) << 52);
  return DoubleFromBits(bits);
}

void FillState(CPUState* guest, FuzzRandom& random, u32 seed_index)
{
  for (u32 i = 0; i < 32; ++i)
    guest->gpr[i] = random.Next32();
  for (u32 i = 0; i < 32; ++i)
  {
    guest->fpr[i] = RandomFloatValue(random, i, seed_index);
    guest->ps1[i] = RandomFloatValue(random, i + 32u, seed_index);
  }

  guest->gpr[1] = STACK_ADDRESS;
  guest->gpr[29] = SCRATCH_ADDRESS + 0x400u;
  guest->gpr[30] = SCRATCH_ADDRESS + 0x800u;
  guest->lr = RETURN_SENTINEL;
  guest->ctr = seed_index < 3u ? seed_index : random.Next32();
  guest->cr = random.Next32();
  guest->xer = random.Next32() & 0xE000007Fu;
  guest->fpscr = seed_index & 7u;  // RN plus NI; exception/result bits start clean.
  guest->msr = 0x00002000u;  // FP enabled; IR and DR disabled.
  guest->srr0 = 0;
  guest->srr1 = 0;
  guest->dar = 0;
  guest->dsisr = 0;
  guest->ear = 0;
  guest->hid2 = PPC_HID2_PSE | PPC_HID2_LSQE;
  guest->timebase = (static_cast<u64>(seed_index) << 32) | random.Next32();
  std::fill(std::begin(guest->sr), std::end(guest->sr), 0u);
  guest->gqr[0] = 0;
  for (u32 i = 1; i < 8; ++i)
  {
    const u32 type = 4u + ((i - 1u) & 3u);  // u8, u16, s8, s16
    const u32 scale = (seed_index * 7u + i * 5u) & 0x3fu;
    const u32 half = type | (scale << 8);
    guest->gqr[i] = half | (half << 16);
  }
  guest->exception = 0;
  guest->program_exception = 0;
  guest->reserve_addr = 0;
  guest->reserve_valid = false;
  guest->downcount = 0;
  guest->host_fp_control_cache = ~0u;
}

void FillScratch(CPUState* guest, FuzzRandom& random)
{
  const u32 offset = SCRATCH_ADDRESS;
  if (offset + SCRATCH_SIZE > guest->ram_size)
    return;
  for (u32 i = 0; i < SCRATCH_SIZE; ++i)
    guest->ram[offset + i] = random.Next8();
}
}  // namespace

bool StaticRecompLockstepVerifier::IsOpcodeFuzzRequested() const
{
  const char* seeds = std::getenv("STATICRECOMP_OPFUZZ");
  return seeds && seeds[0] != '\0' && seeds[0] != '0';
}

bool StaticRecompLockstepVerifier::RunOpcodeFuzz()
{
  const char* manifest_path = std::getenv("STATICRECOMP_OPFUZZ_MANIFEST");
  if (!m_lockstep || !m_set_mem_journal)
  {
    std::fprintf(stderr,
                 "[opfuzz] ERROR: STATICRECOMP_LOCKSTEP=1 is required for the Dolphin oracle\n");
    return false;
  }
  if (!manifest_path || manifest_path[0] == '\0')
  {
    std::fprintf(stderr, "[opfuzz] ERROR: STATICRECOMP_OPFUZZ_MANIFEST is required\n");
    return false;
  }
  const char* dol_path = std::getenv("STATICRECOMP_OPFUZZ_DOL");
  if (!dol_path || dol_path[0] == '\0' || !LoadSyntheticDol(dol_path, &m_core.m_guest))
  {
    if (!dol_path || dol_path[0] == '\0')
      std::fprintf(stderr, "[opfuzz] ERROR: STATICRECOMP_OPFUZZ_DOL is required\n");
    return false;
  }
  if (!m_core.m_module || !m_core.m_module_active)
  {
    std::fprintf(stderr, "[opfuzz] ERROR: generated native module is not active\n");
    return false;
  }

  std::vector<FuzzBlock> blocks;
  if (!ParseManifest(manifest_path, &blocks))
    return false;

  u32 seeds = static_cast<u32>(std::strtoul(std::getenv("STATICRECOMP_OPFUZZ"), nullptr, 0));
  seeds = std::clamp(seeds, 1u, 100000u);
  u64 random_seed = DEFAULT_SEED;
  if (const char* value = std::getenv("STATICRECOMP_OPFUZZ_SEED"))
    random_seed = std::strtoull(value, nullptr, 0);

  u64 rejected_blocks = 0;
  for (const FuzzBlock& block : blocks)
  {
    const u32 loaded_raw = ReadGuestInstruction(m_core.m_guest, block.address);
    if (loaded_raw != block.raw || !m_core.DispatchableAt(block.address))
    {
      ++rejected_blocks;
      std::fprintf(stderr,
                   "[opfuzz] REJECT name=%s address=0x%08X manifest=0x%08X loaded=0x%08X\n",
                   block.name.c_str(), block.address, block.raw, loaded_raw);
    }
  }
  if (rejected_blocks != 0)
  {
    std::fprintf(stderr,
                 "[opfuzz] RESULT blocks=%zu seeds=%u dispatches=0 divergent_blocks=0 "
                 "reports=0 fallback_skips=0 zero_charges=0 rejected_blocks=%llu seed=0x%016llX\n",
                 blocks.size(), seeds, static_cast<unsigned long long>(rejected_blocks),
                 static_cast<unsigned long long>(random_seed));
    return false;
  }

  std::fprintf(stderr, "[opfuzz] START blocks=%zu seeds=%u seed=0x%016llX\n", blocks.size(),
               seeds, static_cast<unsigned long long>(random_seed));

  const u64 reports_before = m_ls_reports;
  const u64 fallback_before = m_ls_skipped_fallback;
  const u64 zero_before = m_ls_skipped_zero;
  u64 dispatches = 0;
  u64 divergent_blocks = 0;
  FuzzRandom random(random_seed);

  for (const FuzzBlock& block : blocks)
  {
    bool block_diverged = false;
    for (u32 seed_index = 0; seed_index < seeds; ++seed_index)
    {
      FillState(&m_core.m_guest, random, seed_index);
      FillScratch(&m_core.m_guest, random);
      m_core.m_guest.pc = block.address;
      if (m_core.m_module->on_state_loaded)
        m_core.m_module->on_state_loaded(&m_core.m_guest);

      m_ls_checked.erase(block.address);
      const u64 block_reports_before = m_ls_reports;
      Prepare(m_core.m_guest);
      const int dispatched = m_core.m_module->dispatch(&m_core.m_guest, block.address);
      ++dispatches;
      ++m_core.m_native_dispatches;
      if (dispatched == 0)
      {
        m_ls_fallback_seen = true;
        std::fprintf(stderr, "[opfuzz] DISPATCH-REJECT name=%s address=0x%08X seed=%u\n",
                     block.name.c_str(), block.address, seed_index);
      }
      Verify(m_core.m_guest);

      if (m_ls_reports != block_reports_before)
      {
        block_diverged = true;
        std::fprintf(stderr, "[opfuzz] DIVERGENT name=%s address=0x%08X seed=%u\n",
                     block.name.c_str(), block.address, seed_index);
        // One counterexample identifies the emitter family. Continue with the
        // next block so a single bug cannot flood a run with duplicate noise.
        break;
      }
    }
    divergent_blocks += block_diverged ? 1u : 0u;
  }

  const u64 reports = m_ls_reports - reports_before;
  const u64 fallback_skips = m_ls_skipped_fallback - fallback_before;
  const u64 zero_charges = m_ls_skipped_zero - zero_before;
  std::fprintf(stderr,
               "[opfuzz] RESULT blocks=%zu seeds=%u dispatches=%llu divergent_blocks=%llu "
               "reports=%llu fallback_skips=%llu zero_charges=%llu rejected_blocks=0 "
               "seed=0x%016llX\n",
               blocks.size(), seeds, static_cast<unsigned long long>(dispatches),
               static_cast<unsigned long long>(divergent_blocks),
               static_cast<unsigned long long>(reports),
               static_cast<unsigned long long>(fallback_skips),
               static_cast<unsigned long long>(zero_charges),
               static_cast<unsigned long long>(random_seed));
  return divergent_blocks == 0 && fallback_skips == 0 && zero_charges == 0;
}

}  // namespace StaticRecompLockstep
