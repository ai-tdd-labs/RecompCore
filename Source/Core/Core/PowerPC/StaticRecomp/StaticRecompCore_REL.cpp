// RecompCore: dynamic REL discovery and native section binding.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"

#include <algorithm>
#include <cstdio>
#include <unordered_set>

#include "Core/PowerPC/MMU.h"
#include "Core/System.h"

namespace
{
constexpr u32 OS_MODULE_QUEUE = 0x800030C8u;
constexpr u32 OS_MODULE_INFO_SIZE = 0x20u;
constexpr u32 OS_SECTION_INFO_SIZE = 8u;
constexpr u32 OS_SECTION_EXEC = 1u;
constexpr u32 MAX_LINKED_MODULES = 4096u;
constexpr u32 MAX_REL_SECTIONS = 4096u;
}

const StaticRecompRelModuleDesc* StaticRecompCore::FindRelModule(u32 module_id) const
{
  if (!m_module || m_module->abi_version < STATICRECOMP_ABI_VERSION_V4)
    return nullptr;

  for (u32 index = 0; index < m_module->num_rel_modules; ++index)
  {
    if (m_module->rel_modules[index].module_id == module_id)
      return &m_module->rel_modules[index];
  }
  return nullptr;
}

bool StaticRecompCore::RefreshRelBindings()
{
  m_rel_bindings_valid = false;
  ++m_rel_binding_refreshes;
  if (!m_module || m_module->abi_version < STATICRECOMP_ABI_VERSION_V4 ||
      !m_module->rel_modules || m_module->num_rel_modules == 0)
  {
    m_rel_unlink_generations += m_rel_bindings.size();
    m_rel_bindings.clear();
    m_rel_bindings_valid = true;
    return true;
  }

  auto& mmu = m_system.GetMMU();
  auto& memory = m_system.GetMemory();
  const u32 ram_size = memory.GetRamSizeReal();
  const u32 exram_size = memory.GetExRamSizeReal();
  const auto range_is_readable = [ram_size, exram_size](u32 address, u32 size) {
    const u64 end = static_cast<u64>(address) + size;
    if (address >= 0x80000000u && end <= 0x80000000ull + ram_size)
      return true;
    return address >= 0x90000000u && end <= 0x90000000ull + exram_size;
  };
  if (!range_is_readable(OS_MODULE_QUEUE, 8))
    return false;

  std::vector<RelBinding> next;
  std::unordered_set<u32> visited_addresses;
  std::unordered_set<u32> visited_ids;
  u32 guest_module = mmu.Read<u32>(OS_MODULE_QUEUE);
  for (u32 count = 0; guest_module != 0 && count < MAX_LINKED_MODULES; ++count)
  {
    if (!range_is_readable(guest_module, OS_MODULE_INFO_SIZE) ||
        !visited_addresses.insert(guest_module).second)
    {
      return false;
    }

    const u32 module_id = mmu.Read<u32>(guest_module);
    const u32 next_module = mmu.Read<u32>(guest_module + 4);
    const u32 num_sections = mmu.Read<u32>(guest_module + 0x0C);
    const u32 section_table = mmu.Read<u32>(guest_module + 0x10);
    const StaticRecompRelModuleDesc* catalog = FindRelModule(module_id);
    if (catalog)
    {
      if (!visited_ids.insert(module_id).second || num_sections == 0 ||
          num_sections > MAX_REL_SECTIONS ||
          !range_is_readable(section_table, num_sections * OS_SECTION_INFO_SIZE))
      {
        return false;
      }

      RelBinding binding;
      binding.module = catalog;
      binding.guest_module = guest_module;
      binding.section_bases.assign(num_sections, 0);
      bool valid = true;
      for (u32 section_index = 0; section_index < catalog->num_executable_sections;
           ++section_index)
      {
        const auto& section = catalog->executable_sections[section_index];
        if (section.section_index >= num_sections)
        {
          valid = false;
          break;
        }
        const u32 entry = section_table + section.section_index * OS_SECTION_INFO_SIZE;
        const u32 runtime_offset = mmu.Read<u32>(entry);
        const u32 runtime_size = mmu.Read<u32>(entry + 4);
        const u32 runtime_base = runtime_offset & ~OS_SECTION_EXEC;
        const u64 actual_start = static_cast<u64>(runtime_base) + section.offset;
        if ((runtime_offset & OS_SECTION_EXEC) == 0 || runtime_base == 0 ||
            section.offset > runtime_size || section.size > runtime_size - section.offset ||
            actual_start > UINT32_MAX ||
            !range_is_readable(static_cast<u32>(actual_start), section.size))
        {
          valid = false;
          break;
        }
        binding.section_bases[section.section_index] = runtime_base;
      }
      if (valid)
      {
        const auto old = std::find_if(
            m_rel_bindings.begin(), m_rel_bindings.end(),
            [module_id](const RelBinding& candidate) {
              return candidate.module && candidate.module->module_id == module_id;
            });
        if (old != m_rel_bindings.end() && old->guest_module == binding.guest_module &&
            old->section_bases == binding.section_bases)
        {
          binding.generation = old->generation;
        }
        else
        {
          binding.generation = ++m_rel_link_generations;
          std::fprintf(stderr,
                       "[staticrecomp] REL link: module=%u generation=%llu guest=0x%08X\n",
                       module_id, static_cast<unsigned long long>(binding.generation),
                       guest_module);
        }
        next.push_back(std::move(binding));
      }
    }
    guest_module = next_module;
  }
  if (guest_module != 0)
    return false;

  for (const RelBinding& old : m_rel_bindings)
  {
    const auto still_live = std::find_if(
        next.begin(), next.end(), [&old](const RelBinding& candidate) {
          return old.module && candidate.module &&
                 old.module->module_id == candidate.module->module_id &&
                 old.generation == candidate.generation;
        });
    if (still_live == next.end())
    {
      ++m_rel_unlink_generations;
      std::fprintf(stderr, "[staticrecomp] REL unlink: module=%u generation=%llu\n",
                   old.module ? old.module->module_id : 0,
                   static_cast<unsigned long long>(old.generation));
    }
  }
  m_rel_bindings = std::move(next);
  m_rel_bindings_valid = true;
  return true;
}

bool StaticRecompCore::LookupRelChunk(u32 address, RelChunkDispatch* dispatch) const
{
  if (!dispatch)
    return false;

  for (const RelBinding& binding : m_rel_bindings)
  {
    for (u32 section_index = 0;
         section_index < binding.module->num_executable_sections; ++section_index)
    {
      const auto& section = binding.module->executable_sections[section_index];
      if (section.section_index >= binding.section_bases.size())
        continue;
      const u32 section_base = binding.section_bases[section.section_index];
      const u64 actual_start = static_cast<u64>(section_base) + section.offset;
      const u64 actual_end = actual_start + section.size;
      if (address < actual_start || address >= actual_end)
        continue;

      const intptr_t delta =
          static_cast<intptr_t>(static_cast<s64>(section.canonical_start) -
                                static_cast<s64>(actual_start));
      const u32 canonical_pc =
          static_cast<u32>(static_cast<intptr_t>(address) + delta);
      u32 lo = 0;
      u32 hi = section.num_chunk_ranges;
      while (lo < hi)
      {
        const u32 mid = lo + (hi - lo) / 2;
        if (section.chunk_ranges[mid].end <= canonical_pc)
          lo = mid + 1;
        else
          hi = mid;
      }
      if (lo >= section.num_chunk_ranges ||
          canonical_pc < section.chunk_ranges[lo].start ||
          canonical_pc >= section.chunk_ranges[lo].end)
      {
        return false;
      }

      dispatch->function = section.chunk_functions[lo];
      dispatch->canonical_pc = canonical_pc;
      dispatch->section_delta = delta;
      dispatch->module_id = binding.module->module_id;
      dispatch->section_index = section.section_index;
      dispatch->generation = binding.generation;
      return dispatch->function != nullptr;
    }
  }
  return false;
}
