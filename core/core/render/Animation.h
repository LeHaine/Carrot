//
// Created by jglrxavpok on 17/12/2020.
//

#pragma once

#include <cstdint>
#include <algorithm>
#include <glm/glm.hpp>

namespace Carrot {
    static constexpr int MAX_KEYFRAMES_PER_ANIMATION = 140;
    static constexpr int MAX_BONES_PER_MESH = 40;

    struct Keyframe {
        glm::mat4 boneTransforms[MAX_BONES_PER_MESH]{};
        alignas(16) float timestamp = 0.0f;
        glm::vec3 _padding{0.0f};

        explicit Keyframe(float timestamp = 0.0f): timestamp(timestamp) {
            std::fill_n(boneTransforms, MAX_BONES_PER_MESH, glm::mat4{1.0f});
        }
    };

    /**
     * Sent as-is to GPU
     */
    struct Animation {
        alignas(4) std::int32_t keyframeCount = 0;
        alignas(4) float duration = 1.0f;
        alignas(16) Keyframe keyframes[MAX_KEYFRAMES_PER_ANIMATION];
        explicit Animation() = default;
    };

    /**
     * CPU-visible metadata, kept after loading animation
     */
    struct AnimationMetadata {
        float duration = 1.0f;
        std::size_t index = 0; //< index in animation list of a given model
    };
}
