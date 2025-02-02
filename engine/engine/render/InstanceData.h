//
// Created by jglrxavpok on 05/12/2020.
//

#pragma once
#include <glm/glm.hpp>
#include <core/utils/UUID.h>

namespace Carrot {
    struct InstanceData {
        alignas(16) glm::vec4 color{1.0f};
        alignas(16) Carrot::UUID uuid = Carrot::UUID::null();
        alignas(16) glm::mat4 transform{1.0f};
        glm::mat4 lastFrameTransform{0.0f};
    };

    struct AnimatedInstanceData {
        alignas(16) glm::vec4 color{1.0f};
        alignas(16) Carrot::UUID uuid = Carrot::UUID::null();
        alignas(16) glm::mat4 transform{1.0f};
        glm::mat4 lastFrameTransform{1.0f};
        alignas(16) uint32_t animationIndex = 0;
        double animationTime = 0.0;
        bool raytraced = true;
    };
}

