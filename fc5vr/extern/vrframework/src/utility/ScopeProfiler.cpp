#include "ScopeProfiler.h"
#include "mods/VR.hpp"

std::string ScopeProfiler::get_counters()
{

    static auto vr = VR::get();
    return fmt::format("[e:{}r:{}p:{}]", vr->m_engine_frame_count, vr->m_render_frame_count, vr->m_presenter_frame_count);
}
