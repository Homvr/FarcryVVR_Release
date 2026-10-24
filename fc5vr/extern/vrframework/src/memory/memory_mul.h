#pragma once

namespace memory
{
    extern uintptr_t InstructionRelocation(const char* hook_name, const char* pattern, UINT offset_begin, UINT instruction_size, uintptr_t static_offset);
    extern uintptr_t FuncRelocation(const char* hook_name, const char* pattern, uintptr_t static_offset);
    extern uintptr_t VTable(const char* hook_name, const char* table, uintptr_t static_offset);
    extern bool      PatchMemory(uintptr_t address, const std::vector<uint8_t>& patchBytes);
    extern bool      PatchMemory(uintptr_t address, const unsigned char* patch, size_t patchSize);

    extern HMODULE g_mod;
    extern size_t  mod_size;
    extern size_t  mod_end;
}
