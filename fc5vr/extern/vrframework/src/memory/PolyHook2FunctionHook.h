#ifdef POLYHOOK2_FUNCTION_HOOK
#pragma once
#include <utility/Address.hpp>
#include <polyhook2/Detour/x64Detour.hpp>
#include <memory>

class PolyHook2FunctionHook
{
public:
    PolyHook2FunctionHook() = delete;
    PolyHook2FunctionHook(const PolyHook2FunctionHook&) = delete;
    PolyHook2FunctionHook(PolyHook2FunctionHook&) = delete;
    PolyHook2FunctionHook(Address a_target, Address a_detour);
    virtual ~PolyHook2FunctionHook();

    bool create();
    bool remove();

    auto get_original() const { return m_original; }

    template <typename T> T* get_original() const { return (T*)m_original; }

    auto is_valid() const { return m_original != 0; }

    PolyHook2FunctionHook& operator=(const PolyHook2FunctionHook&) = delete;
    PolyHook2FunctionHook& operator=(PolyHook2FunctionHook&) = delete;
private:
    uintptr_t m_target{0};
    uintptr_t m_destination{0};
    uintptr_t m_original{0};
    std::unique_ptr<PLH::x64Detour> hook;
};
#endif