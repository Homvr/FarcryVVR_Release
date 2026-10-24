#include <utility/Module.hpp>
#include <utility/RTTI.hpp>
#include <utility/Scan.hpp>

namespace memory {
    HMODULE g_mod    = utility::get_executable();
    size_t mod_size = utility::get_module_size(g_mod).value_or(0);
    size_t mod_end  = (uintptr_t)g_mod + mod_size - 0x100;

    uintptr_t VTable(const char* hook_name, const char* table, uintptr_t static_offset)
    {
        uintptr_t val = 0;
#if defined _DEBUG || defined SIGNATURE_SCAN
        auto ref = utility::rtti::find_vtable(g_mod, table);
        if (!ref) {
            spdlog::error("VTable pattern not found for id={}", hook_name );
            return 0;
        }
        val = ref.value();
        if(static_offset > 0) {
            auto offset = val - (uintptr_t)g_mod;
            if(offset != static_offset) {
                spdlog::info("FuncRelocation offset does not match static offset for id={} offset={:x} reference={:x}", hook_name, offset, static_offset);
            }
        }
#else
        spdlog::info("Using static offset for VTable id={}", hook_name);
        val =  static_offset + (uintptr_t)g_mod;
#endif
        return val;
    };

    uintptr_t FuncRelocation(const char* hook_name, const char* pattern, uintptr_t static_offset) {
#ifndef SIGNATURE_SCAN
        spdlog::warn("FuncRelocation called without SIGNATURE_SCAN enabled, using static offsets only. id={}", hook_name);
#endif
        uintptr_t val = 0;
#if defined _DEBUG || defined SIGNATURE_SCAN
        auto ref = utility::scan(g_mod, pattern);
        if (!ref) {
            spdlog::error("FuncRelocation pattern not found for id={}", hook_name );
            return 0;
        }
        val = ref.value();
        if(static_offset > 0) {
            auto offset = val - (uintptr_t)g_mod;
            if(offset != static_offset) {
                spdlog::info("FuncRelocation offset does not match static offset for id={} offset={:x} reference={:x}", hook_name, offset, static_offset);
            }
        }
#else
        spdlog::info("Using static offset for VTable id={}", hook_name);
        val =  static_offset + (uintptr_t)g_mod;
#endif
        return val;
    };
    uintptr_t InstructionRelocation(const char* hook_name, const char* pattern, UINT offset_begin, UINT instruction_size,  uintptr_t static_offset) {
        uintptr_t val = 0;
#if defined _DEBUG || defined SIGNATURE_SCAN
        auto ref = utility::scan(g_mod, pattern);
        if (!ref) {
            spdlog::error("AsmCodeRelocation pattern not found for id={}", hook_name);
            return 0;
        }
        auto addr = ref.value();
        val = addr + *(int32_t*)(addr + offset_begin) + instruction_size;
        if(static_offset > 0) {
            auto offset = val - (uintptr_t)g_mod;
            if(offset != static_offset) {
                spdlog::info("AsmCodeRelocation offset does not match static offset for id={} offset={:x} reference={:x}", hook_name, offset, static_offset);
            }
        }
#else
        spdlog::info("Using static offset for VTable id={}", hook_name);
        val = static_offset + (uintptr_t)g_mod;
#endif
        return val;
    };

    bool PatchMemory(uintptr_t address, const unsigned char* patch, size_t patchSize) {
        if (patchSize == 0) {
            return false;
        }

        DWORD oldProtect;
        if (!VirtualProtect((LPVOID)address, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            return false;
        }

        memcpy((LPVOID)address, patch, patchSize);
        DWORD tempProtect;
        VirtualProtect((LPVOID)address, patchSize, oldProtect, &tempProtect);

        return true;
    }


    bool PatchMemory(uintptr_t address, const std::vector<uint8_t>& patchBytes) {
        return PatchMemory(address, patchBytes.data(), patchBytes.size());
    }

}