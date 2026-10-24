#include "DebugUtils.h"
#include <glm/ext/matrix_float4x4.hpp>
#include <mods/VR.hpp>

namespace DebugUtils {
    DebugConfig config{};
    glm::mat4 generateOffsetMatrix() {
        static auto vr = VR::get();
        auto offset = glm::mat4{1.0f};
        if(config.cameraMotion) {
            offset[3].x = 0.034f * (vr->m_engine_frame_count % 2 == 0 ? 1.0f : -1.0f);
        }
        auto offset2 = glm::mat4{1.0f};
        auto modulino = vr->m_engine_frame_count % 1400;
        if(modulino > 700) {
            modulino = 1400 - modulino;
        }
        float angle = ((float)modulino / 700.0f) * glm::pi<float>() / 2.0f;
        if(config.cameraSpin) {
            offset2 *= glm::rotate(angle, glm::vec3(0.0f, 1.0f, 0.0f));
            offset2 *= glm::rotate(angle * 0.7f, glm::vec3(1.0f, 0.0f, 0.0f));
            offset2 *= glm::rotate(angle * 0.5f, glm::vec3(0.0f, 0.0f, 1.0f));
        }
        return offset2 * offset;
    }
}
