#ifdef POLYHOOK2_FUNCTION_HOOK
#include "PolyHook2FunctionHook.h"

bool PolyHook2FunctionHook::create()
{
    if (m_target == 0 || m_destination == 0) {
        spdlog::error("PolyHook2FunctionHook not initialized");
        return false;
    }
    if(!hook->hook()) {
        spdlog::error("Failed to hook {:x}->{:x}", m_target, m_destination);
        m_original = 0;
        m_destination = 0;
        m_target = 0;
        return false;
    }
    spdlog::info("Hooked {:x}->{:x}", m_target, m_destination);
    return true;
}

PolyHook2FunctionHook::PolyHook2FunctionHook(Address a_target, Address a_detour):
    m_target{a_target},
    m_destination{a_detour},
    m_original{0}
{
    hook = std::make_unique<PLH::x64Detour>(m_target, m_destination, &m_original);
}

PolyHook2FunctionHook::~PolyHook2FunctionHook()
{
    remove();
}

bool PolyHook2FunctionHook::remove()
{
    if (m_original == 0) {
        return true;
    }
    if(!hook->unHook()) {
        return false;
    }
    m_original = 0;
    m_destination = 0;
    m_target = 0;
    return true;
}
#endif