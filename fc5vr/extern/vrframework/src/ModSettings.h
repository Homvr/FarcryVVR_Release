#pragma once

namespace ModSettings
{
    struct InternalSettings
    {
        bool      debugShaders{ false };
        bool      cameraShake{ false };
        bool      showQuadDisplay{ false };
        bool      forceFlatScreen{false};
        int       toneMapAlg{ 0 }; // None, Saturate, Reinhard, ACESFilmic
        float     toneMapExposure{ 0.0f };
    };

    extern InternalSettings g_internalSettings;

    bool showFlatScreenDisplay();

} // namespace ModSettings
