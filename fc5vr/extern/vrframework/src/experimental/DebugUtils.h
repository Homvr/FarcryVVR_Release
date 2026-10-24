#pragma once
#include <glm/fwd.hpp>

namespace DebugUtils
{
    struct DebugConfig
    {
        bool      debugShaders{ false };
        bool      cameraShake{ false };
        bool      cameraSpin{ false };
        bool      cameraMotion{ true };
        bool      alternativeSymmetricProjection{ false };
    };
    extern DebugConfig config;
    glm::mat4 generateOffsetMatrix();
};
