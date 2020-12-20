//
// Created by jglrxavpok on 26/11/2020.
//

#pragma once

#include <glm/glm.hpp>
#include "vulkan/includes.h"

namespace Carrot {
    /// Definition of a vertex for this engine
    struct Vertex {
        /// World position of the vertex
        alignas(16) glm::vec3 pos;

        /// RGB color
        alignas(16) glm::vec3 color;

        /// UV coordinates
        alignas(16) glm::vec2 uv;

        /// Creates a binding description for this Vertex struct
        static std::vector<vk::VertexInputBindingDescription> getBindingDescription();

        /// Creates an array describing the different attributes of a Vertex
        static std::vector<vk::VertexInputAttributeDescription> getAttributeDescriptions();
    };

    struct SkinnedVertex {
        /// World position of the vertex
        alignas(16) glm::vec3 pos;

        /// RGB color
        alignas(16) glm::vec3 color;

        /// UV coordinates
        alignas(16) glm::vec2 uv;

        /// Skinning information
        alignas(16) glm::ivec4 boneIDs{-1,-1,-1,-1};
        alignas(16) glm::vec4 boneWeights{0,0,0,0};

        void addBoneInformation(uint32_t boneID, float weight);

        /// Creates a binding description for this Vertex struct
        static std::vector<vk::VertexInputBindingDescription> getBindingDescription();

        /// Creates an array describing the different attributes of a Vertex
        static std::vector<vk::VertexInputAttributeDescription> getAttributeDescriptions();
    };
}


