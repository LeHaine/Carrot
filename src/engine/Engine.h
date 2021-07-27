//
// Created by jglrxavpok on 21/11/2020.
//
#pragma once
#include <map>
#include <memory>
#include <optional>
#include <vector>
#include <set>

namespace Carrot {
    class Engine;
}

#include <engine/vulkan/includes.h>
#include <engine/vulkan/SwapchainAware.h>
#include <GLFW/glfw3.h>
#include "engine/memory/NakedPtr.hpp"
#include "engine/render/IDTypes.h"
#include "engine/vulkan/CustomTracyVulkan.h"
#include "engine/render/DebugBufferObject.h"
#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_glfw.h"

#include "engine/vulkan/VulkanDriver.h"
#include "engine/render/VulkanRenderer.h"
#include "render/Skybox.hpp"
#include "render/Composer.h"
#include "engine/Configuration.h"
#include "engine/tasks/PresentThread.h"
#include "engine/io/actions/InputVectors.h"

using namespace std;

namespace Carrot {
    class CarrotGame;

    class Buffer;

    class BufferView;

    class Image;

    class Mesh;

    class Model;

    class Pipeline;

    class Material;

    class InstanceData;

    class Camera;

    class GBuffer;

    class RayTracer;

    class ResourceAllocator;

    namespace Render {
        class Texture;
        class Graph;
    };

#ifdef ENABLE_VR
    namespace VR {
        class Interface;
        class Session;
    }
#endif

    /// Base class interfacing with Vulkan
    class Engine: public SwapchainAware {
    public:
        vector<unique_ptr<TracyVulkanContext>> tracyCtx{};

        /// Init the engine with the given GLFW window. Will immediately load Vulkan resources
        explicit Engine(NakedPtr<GLFWwindow> window, Configuration config = {});

        /// Launch the engine loop
        void run();


    public: // GLFW event handling

        /// Called by GLFW when the window is resized
        void onWindowResize();

        void onMouseMove(double xpos, double ypos, bool updateOnlyDelta);

        void onKeyEvent(int key, int scancode, int action, int mods);

        void onMouseButton(int button, int action, int mods);

    public:

        /// Cleanup resources
        ~Engine();

        /// Vulkan logical device
        vk::Device& getLogicalDevice();

        /// Queue families used by the engine
        const Carrot::QueueFamilies& getQueueFamilies();

        /// Vulkan Allocator
        vk::Optional<const vk::AllocationCallbacks> getAllocator();

        /// Command pool for transfer operations
        vk::CommandPool& getTransferCommandPool();

        /// Command pool for graphics operations
        vk::CommandPool& getGraphicsCommandPool();

        /// Command pool for compute operations
        vk::CommandPool& getComputeCommandPool();

        /// Queue for transfer operations
        vk::Queue& getTransferQueue();

        /// Queue for graphics operations
        vk::Queue& getGraphicsQueue();

        vk::Queue& getPresentQueue();

        vk::Queue& getComputeQueue();

        // templates

        /// Performs a transfer operation on the transfer queue.
        /// \tparam CommandBufferConsumer function describing the operation. Takes a single vk::CommandBuffer& argument, and returns void.
        /// \param consumer function describing the operation
        template<typename CommandBufferConsumer>
        void performSingleTimeTransferCommands(CommandBufferConsumer&& consumer, bool waitFor = true, vk::Semaphore waitSemaphore = {}, vk::PipelineStageFlags waitDstFlags = static_cast<vk::PipelineStageFlagBits>(0));

        /// Performs a graphics operation on the graphics queue.
        /// \tparam CommandBufferConsumer function describing the operation. Takes a single vk::CommandBuffer& argument, and returns void.
        /// \param consumer function describing the operation
        template<typename CommandBufferConsumer>
        void performSingleTimeGraphicsCommands(CommandBufferConsumer&& consumer, bool waitFor = true, vk::Semaphore waitSemaphore = {}, vk::PipelineStageFlags waitDstFlags = static_cast<vk::PipelineStageFlagBits>(0));

        uint32_t getSwapchainImageCount();

        vector<shared_ptr<Buffer>>& getDebugUniformBuffers();

        shared_ptr<Material> getOrCreateMaterial(const string& name);

        /// Creates a set with the graphics and transfer family indices
        set<uint32_t> createGraphicsAndTransferFamiliesSet();

        Camera& getCamera(Carrot::Render::Eye eye);
        Camera& getCamera();

        vk::PhysicalDevice& getPhysicalDevice();

        ASBuilder& getASBuilder();

        bool isGrabbingCursor() const { return grabbingCursor; };

        RayTracer& getRayTracer() { return renderer.getRayTracer(); };

        ResourceAllocator& getResourceAllocator() { return *resourceAllocator; };

        VulkanDriver& getVulkanDriver() { return vkDriver; };

        VulkanRenderer& getRenderer() { return renderer; };

        GBuffer& getGBuffer() { return renderer.getGBuffer(); };

        void setSkybox(Skybox::Type type);

        void onSwapchainSizeChange(int newWidth, int newHeight) override;

        void onSwapchainImageCountChange(size_t newCount) override;

        void toggleCursorGrab() {
            if(grabbingCursor) {
                ungrabCursor();
            } else {
                grabCursor();
            }
        }

        void grabCursor() {
            grabbingCursor = true;
            glfwSetInputMode(window.get(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }

        void ungrabCursor() {
            grabbingCursor = false;
            glfwSetInputMode(window.get(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }

        bool hasPreviousFrame() const {
            return frames > 0;
        }

        uint32_t getPreviousFrameIndex() const {
            return lastFrameIndex;
        }

        Render::Composer& getMainComposer(Render::Eye eye = Render::Eye::NoVR) { return *composers[eye]; }

        Render::Context newRenderContext(std::size_t swapchainFrameIndex, Render::Eye eye = Render::Eye::NoVR);

        std::uint32_t getSwapchainImageIndexRightNow() { return swapchainImageIndexRightNow; }

#ifdef ENABLE_VR
        VR::Session& getVRSession() { return *vrSession; }
#endif

    public: // inputs

        using KeyCallback = std::function<void(int key, int scancode, int action, int mods)>;
        using GamepadButtonCallback = std::function<void(int joystickID, int button, bool isPressed)>;
        using GamepadAxisCallback = std::function<void(int joystickID, int axisID, float newValue, float oldValue)>;
        using GamepadVec2Callback = std::function<void(int joystickID, IO::GameInputVectorType vecID, glm::vec2 newValue, glm::vec2 oldValue)>;
        using MouseButtonCallback = std::function<void(int button, bool isPressed, int mods)>;
        using MousePositionCallback = std::function<void(double xpos, double ypos)>;
        using MouseDeltaCallback = std::function<void(double dx, double dy)>;

        void addGLFWKeyCallback(KeyCallback keyCallback) {
            keyCallbacks.push_back(keyCallback);
        }

        void addGLFWMouseButtonCallback(MouseButtonCallback callback) {
            mouseButtonCallbacks.push_back(callback);
        }

        void addGLFWGamepadButtonCallback(GamepadButtonCallback callback) {
            gamepadButtonCallbacks.push_back(callback);
        }

        void addGLFWGamepadAxisCallback(GamepadAxisCallback callback) {
            gamepadAxisCallbacks.push_back(callback);
        }

        void addGLFWGamepadVec2Callback(GamepadVec2Callback callback) {
            gamepadVec2Callbacks.push_back(callback);
        }

        void addGLFWMousePositionCallback(MousePositionCallback callback) {
            mousePositionCallbacks.push_back(callback);
        }

        void addGLFWMouseDeltaCallback(MouseDeltaCallback callback) {
            mouseDeltaCallbacks.push_back(callback);
        }

        void addGLFWMouseDeltaGrabbedCallback(MouseDeltaCallback callback) {
            mouseDeltaGrabbedCallbacks.push_back(callback);
        }

    public:
        const Configuration& getConfiguration() const { return config; }

    private:
        Configuration config;
        double mouseX = 0.0;
        double mouseY = 0.0;
        float currentFPS = 0.0f;
        bool running = true;
        bool grabbingCursor = false;
        NakedPtr<GLFWwindow> window = nullptr;

#ifdef ENABLE_VR
        std::unique_ptr<VR::Interface> vrInterface = nullptr;
        std::unique_ptr<VR::Session> vrSession = nullptr;
#endif

        VulkanDriver vkDriver;
        PresentThread presentThread;
        VulkanRenderer renderer;
        uint32_t lastFrameIndex = 0;
        uint32_t frames = 0;
        std::uint32_t swapchainImageIndexRightNow = 0;

        unique_ptr<ResourceAllocator> resourceAllocator;

        vk::UniqueCommandPool tracyCommandPool{};
        vector<vk::CommandBuffer> tracyCommandBuffers{};

        vector<vk::CommandBuffer> mainCommandBuffers{};
        vector<vk::CommandBuffer> gBufferCommandBuffers{};
        vector<vk::CommandBuffer> gResolveCommandBuffers{};
        vector<vk::CommandBuffer> skyboxCommandBuffers{};
        vector<vk::UniqueSemaphore> imageAvailableSemaphore{};
        vector<vk::UniqueSemaphore> renderFinishedSemaphore{};
        vector<vk::UniqueFence> inFlightFences{};
        vector<vk::UniqueFence> imagesInFlight{};

        map<string, shared_ptr<Material>> materials{};

        unordered_map<Render::Eye, unique_ptr<Camera>> cameras{};
        unique_ptr<Carrot::CarrotGame> game = nullptr;

        bool framebufferResized = false;

        Skybox::Type currentSkybox = Skybox::Type::None;
        unique_ptr<Render::Texture> loadedSkyboxTexture = nullptr;
        unique_ptr<Mesh> skyboxMesh = nullptr;

        unique_ptr<Mesh> screenQuad = nullptr;

        struct ImGuiTextures {
            Render::Texture* allChannels = nullptr;
            Render::Texture* albedo = nullptr;
            Render::Texture* position = nullptr;
            Render::Texture* normal = nullptr;
            Render::Texture* depth = nullptr;
            Render::Texture* raytracing = nullptr;
            Render::Texture* ui = nullptr;
            Render::Texture* intProperties = nullptr;
        };

        std::vector<ImGuiTextures> imguiTextures;

        Carrot::Render::PassData::GResolve gResolvePassData;
        std::unique_ptr<Render::Graph> globalFrameGraph = nullptr;
        std::unique_ptr<Render::Graph> leftEyeGlobalFrameGraph = nullptr;
        std::unique_ptr<Render::Graph> rightEyeGlobalFrameGraph = nullptr;

        unordered_map<Render::Eye, std::unique_ptr<Render::Composer>> composers;

        /// Init engine
        void init();

        /// Init window
        void initWindow();

        /// Init Vulkan for rendering
        void initVulkan();

        /// Init ingame console
        void initConsole();

        /// Create the primary command buffers for rendering
        void recordMainCommandBuffer(size_t frameIndex);

        /// Acquires a swapchain image, prepares UBOs, submit command buffer, and present to screen
        void drawFrame(size_t currentFrame);

        /// Update the game systems
        void tick(double deltaTime);

        void takeScreenshot();

        /// Create fences and semaphores used for rendering
        void createSynchronizationObjects();

        void recreateSwapchain();

        /// Update the uniform buffer at index 'imageIndex'
        void updateUniformBuffer(int imageIndex);

        void initGame();

        void createCameras();

        void createTracyContexts();

        void allocateGraphicsCommandBuffers();

        void updateImGuiTextures(size_t swapchainLength);

    private:
        std::unordered_map<int, GLFWgamepadstate> gamepadStates;
        std::unordered_map<int, GLFWgamepadstate> gamepadStatePreviousFrame;
        std::vector<KeyCallback> keyCallbacks;
        std::vector<MouseButtonCallback> mouseButtonCallbacks;
        std::vector<GamepadButtonCallback> gamepadButtonCallbacks;
        std::vector<GamepadAxisCallback> gamepadAxisCallbacks;
        std::vector<GamepadVec2Callback> gamepadVec2Callbacks;
        std::vector<MousePositionCallback> mousePositionCallbacks;
        std::vector<MouseDeltaCallback> mouseDeltaCallbacks;
        std::vector<MouseDeltaCallback> mouseDeltaGrabbedCallbacks;

        /// Poll state of gamepads, GLFW does not (yet) post events for gamepad inputs
        void pollGamepads();

        void initInputStructures();

        void onGamepadButtonChange(int gamepadID, int buttonID, bool pressed);
        void onGamepadAxisChange(int gamepadID, int buttonID, float newValue, float oldValue);
        void onGamepadVec2Change(int gamepadID, IO::GameInputVectorType vecID, glm::vec2 newValue, glm::vec2 oldValue);
    };
}

#include "Engine.ipp"