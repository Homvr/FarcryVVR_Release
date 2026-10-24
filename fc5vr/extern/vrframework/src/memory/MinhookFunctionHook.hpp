#pragma once

#include <cstdint>
#include <utility/Address.hpp>
#include <windows.h>

class MinhookFunctionHook {
public:
    MinhookFunctionHook() = delete;
    MinhookFunctionHook(const MinhookFunctionHook& other) = delete;
    MinhookFunctionHook(MinhookFunctionHook&& other) = delete;
    MinhookFunctionHook(Address target, Address destination);
    virtual ~MinhookFunctionHook();

    bool create();

    // Called automatically by the destructor, but you can call it explicitly
    // if you need to remove the hook.
    bool remove();

    auto get_original() const { return m_original; }

    template <typename T> T* get_original() const { return (T*)m_original; }

    auto is_valid() const { return m_original != 0; }

    MinhookFunctionHook& operator=(const MinhookFunctionHook& other) = delete;
    MinhookFunctionHook& operator=(MinhookFunctionHook&& other) = delete;

private:
    uintptr_t m_target{0};
    uintptr_t m_destination{0};
    uintptr_t m_original{0};
};