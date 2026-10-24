#include "ModSettings.h"

#include "mods/VR.hpp"

namespace ModSettings {
    InternalSettings g_internalSettings;

    bool showFlatScreenDisplay()
    {
        static auto vr = VR::get();
        //TODO fix for openvr
        if (vr->is_hmd_active() && vr->get_runtime()->is_openvr()) {
            return false;
        }
        return g_internalSettings.forceFlatScreen || g_internalSettings.showQuadDisplay;
    }
}
