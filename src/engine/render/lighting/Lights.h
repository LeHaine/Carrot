//
// Created by jglrxavpok on 06/03/2021.
//

#pragma once
#include "engine/vulkan/includes.h"
#include "engine/utils/WeakPool.hpp"
#include "engine/render/RenderContext.h"
#include <glm/glm.hpp>

namespace Carrot::Render {
    using bool32 = uint32_t;

    enum class LightType: std::uint32_t {
        Point,
        Directional,
// TODO:        Spot,
    };

    struct Light {
        alignas(16) glm::vec3 position{0.0f};
        float intensity = 1.0f;

        alignas(16) glm::vec3 direction{1.0f};
        LightType type = LightType::Point;

        alignas(16) glm::vec3 color{1.0f};
        bool32 enabled = false;
    };

    class Lighting;

    class LightHandle: public WeakPoolHandle {
    public:
        Light light;

        /*[[deprecated]] */explicit LightHandle(std::uint32_t index, std::function<void(WeakPoolHandle*)> destructor, Lighting& system);

    private:
        void updateHandle(const Carrot::Render::Context& renderContext);

        Lighting& lightingSystem;
        friend class Lighting;
    };

    class Lighting {
    public:
        explicit Lighting();

    public:
        glm::vec3& getAmbientLight() { return ambientColor; }

        std::shared_ptr<LightHandle> create();

        Buffer& getBuffer() const { return *lightBuffer; }

    public:
        void onFrame(const Carrot::Render::Context& renderContext);

    private:
        Light& getLightData(LightHandle& handle) {
            auto* lightPtr = reinterpret_cast<Light *>(data + offsetof(Data, lights));
            return lightPtr[handle.getSlot()];
        }

        void reallocateBuffer(std::uint32_t lightCount);

    private:
        constexpr static std::uint32_t DefaultLightBufferSize = 16;

        WeakPool<LightHandle> lightHandles;
        glm::vec3 ambientColor {1.0f};

        struct Data {
            alignas(16) glm::vec3 ambient {1.0f};
            alignas(16) std::uint32_t lightCount;
            alignas(16) Light lights[];
        };

        Data* data = nullptr;
        std::size_t lightBufferSize = 0; // in number of lights
        std::unique_ptr<Carrot::Buffer> lightBuffer = nullptr;


        friend class LightHandle;
    };
}