//
// Created by jglrxavpok on 21/11/2020.
//

#define GLM_FORCE_RADIANS
#include <filesystem>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/Engine.h"
#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <vector>
#include <map>
#include <set>
#include <core/io/IO.h>
#include <engine/render/CameraBufferObject.h>
#include <engine/render/shaders/ShaderStages.h>
#include "engine/constants.h"
#include "engine/render/resources/Buffer.h"
#include "engine/render/resources/Image.h"
#include "engine/render/resources/Mesh.h"
#include "engine/render/Model.h"
#include "engine/render/resources/Vertex.h"
#include "engine/render/resources/Pipeline.h"
#include "engine/render/InstanceData.h"
#include "engine/render/Camera.h"
#include "engine/render/raytracing/RayTracer.h"
#include "engine/render/raytracing/ASBuilder.h"
#include "engine/render/GBuffer.h"
#include "engine/render/resources/ResourceAllocator.h"
#include "engine/render/resources/Texture.h"
#include "engine/CarrotGame.h"
#include "stb_image_write.h"
#include "LoadingScreen.h"
#include "engine/console/RuntimeOption.hpp"
#include "engine/console/Console.h"
#include "engine/render/RenderGraph.h"
#include "engine/render/TextureRepository.h"
#include "core/Macros.h"
#include "engine/io/actions/ActionSet.h"
#include "core/io/Logging.hpp"
#include "engine/io/actions/ActionDebug.h"
#include "engine/render/Sprite.h"

#ifdef ENABLE_VR
#include "vr/VRInterface.h"
#endif

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

Carrot::Engine* Carrot::Engine::instance = nullptr;
static Carrot::RuntimeOption showFPS("Debug/Show FPS", false);
static Carrot::RuntimeOption showInputDebug("Debug/Show Inputs", false);
static Carrot::RuntimeOption showGBuffer("Debug/Show GBuffer", false);


static std::unordered_set<int> activeJoysticks{};

Carrot::Engine::Engine(Configuration config): window(WINDOW_WIDTH, WINDOW_HEIGHT, config),
instanceSetterHack(this),
#ifdef ENABLE_VR
vrInterface(std::make_unique<VR::Interface>(*this)),
#endif
vkDriver(window, config, this
#ifdef ENABLE_VR
    , *vrInterface
#endif
),
resourceAllocator(std::move(std::make_unique<ResourceAllocator>(vkDriver))),
renderer(vkDriver, config), screenQuad(std::make_unique<Mesh>(vkDriver,
                                                              std::vector<ScreenSpaceVertex> {
                                                                   { { -1, -1} },
                                                                   { { 1, -1} },
                                                                   { { 1, 1} },
                                                                   { { -1, 1} },
                                                              },
                                                              std::vector<uint32_t> {
                                                                   2,1,0,
                                                                   3,2,0,
                                                              })),
    config(config)
    {
    ZoneScoped;
    instance = this;

#ifndef ENABLE_VR
    if(config.runInVR) {
        //Carrot::crash("");
        throw std::runtime_error("Tried to launch engine in VR, but ENABLE_VR was not defined during compilation.");
    }
#else
    vrSession = vrInterface->createSession();
    vkDriver.getTextureRepository().setXRSession(vrSession.get());
#endif

    if(config.runInVR) {
        composers[Render::Eye::LeftEye] = std::make_unique<Render::Composer>(vkDriver);
        composers[Render::Eye::RightEye] = std::make_unique<Render::Composer>(vkDriver);
    } else {
        composers[Render::Eye::NoVR] = std::make_unique<Render::Composer>(vkDriver);
    }

    init();
}

void Carrot::Engine::init() {
    initWindow();

    allocateGraphicsCommandBuffers();
    createTracyContexts();

    createViewport(); // main viewport

    // quickly render something on screen
    LoadingScreen screen{*this};
    initVulkan();

    auto fillGraphBuilder = [&](Render::GraphBuilder& mainGraph, bool shouldPresentToSwapchain, Render::Eye eye = Render::Eye::NoVR) {
         auto& gResolvePass = fillInDefaultPipeline(mainGraph, eye,
                                     [&](const Render::CompiledPass& pass, const Render::Context& frame, vk::CommandBuffer& cmds) {
                                         ZoneScopedN("CPU RenderGraph Opaque GPass");
                                         game->recordOpaqueGBufferPass(pass.getRenderPass(), frame, cmds);
                                         renderer.recordOpaqueGBufferPass(pass.getRenderPass(), frame, cmds);
                                     },
                                     [&](const Render::CompiledPass& pass, const Render::Context& frame, vk::CommandBuffer& cmds) {
                                         ZoneScopedN("CPU RenderGraph Opaque GPass");
                                         game->recordTransparentGBufferPass(pass.getRenderPass(), frame, cmds);
                                         renderer.recordTransparentGBufferPass(pass.getRenderPass(), frame, cmds);
                                    });

        composers[eye]->add(gResolvePass.getData().resolved);
        auto& composerPass = composers[eye]->appendPass(mainGraph);

        gResolvePassData = gResolvePass.getData();


        if(shouldPresentToSwapchain) {
//            auto& imguiPass = renderer.addImGuiPass(mainGraph, composerPass.getData().color);

            mainGraph.addPass<Carrot::Render::PassData::Present>("present",

                                                                 [prevPassData = composerPass.getData()](Render::GraphBuilder& builder, Render::Pass<Carrot::Render::PassData::Present>& pass, Carrot::Render::PassData::Present& data) {
                                                                     // pass.rasterized = false;
                                                                     data.input = builder.read(prevPassData.color, vk::ImageLayout::eShaderReadOnlyOptimal);
                                                                     data.output = builder.write(builder.getSwapchainImage(), vk::AttachmentLoadOp::eClear, vk::ImageLayout::eColorAttachmentOptimal, vk::ClearColorValue(std::array{0,0,0,0}));
                                                                     // uses ImGui, so no pre-record: pass.prerecordable = false;
                                                                     builder.present(data.output);
                                                                 },
                                                                 [](const Render::CompiledPass& pass, const Render::Context& frame, const Carrot::Render::PassData::Present& data, vk::CommandBuffer& cmds) {
                                                                     ZoneScopedN("CPU RenderGraph present");
                                                                     auto& inputTexture = pass.getGraph().getTexture(data.input, frame.swapchainIndex);
                                                                     auto& swapchainTexture = pass.getGraph().getTexture(data.output, frame.swapchainIndex);
                                                                     frame.renderer.fullscreenBlit(pass.getRenderPass(), frame, inputTexture, swapchainTexture, cmds);
                                                                     ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmds);

                                                                     //swapchainTexture.assumeLayout(vk::ImageLayout::eUndefined);
                                                                     //frame.renderer.blit(inputTexture, swapchainTexture, cmds);
                                                                     //swapchainTexture.transitionInline(cmds, vk::ImageLayout::ePresentSrcKHR);
                                                                 },
                                                                 [this](Render::CompiledPass& pass, Carrot::Render::PassData::Present& data) {
                                                                     renderer.initImGuiPass(pass.getRenderPass());
                                                                 }
            );
        }
        return composerPass;
    };

    if(config.runInVR) {
        Render::GraphBuilder leftEyeGraph(vkDriver);
        Render::GraphBuilder rightEyeGraph(vkDriver);
        Render::GraphBuilder mainGraph(vkDriver);
        Render::Composer companionComposer(vkDriver);

        auto leftEyeFinalPass = fillGraphBuilder(leftEyeGraph, false, Render::Eye::LeftEye);
        auto rightEyeFinalPass = fillGraphBuilder(rightEyeGraph, false, Render::Eye::RightEye);

        companionComposer.add(leftEyeFinalPass.getData().color, -1.0, 0.0);
        companionComposer.add(rightEyeFinalPass.getData().color, 0.0, 1.0);

#ifdef ENABLE_VR
        vrSession->setEyeTexturesToPresent(leftEyeFinalPass.getData().color, rightEyeFinalPass.getData().color);
#endif

        auto& composerPass = companionComposer.appendPass(mainGraph);

        leftEyeGlobalFrameGraph = std::move(leftEyeGraph.compile());
        rightEyeGlobalFrameGraph = std::move(rightEyeGraph.compile());

       // auto& imguiPass = renderer.addImGuiPass(mainGraph);

        mainGraph.addPass<Render::PassData::Present>("present",
                                           [prevPassData = composerPass.getData()](Render::GraphBuilder& builder, Render::Pass<Render::PassData::Present>& pass, Render::PassData::Present& data) {
                                               //pass.rasterized = false;
                                               data.input = builder.read(prevPassData.color, vk::ImageLayout::eShaderReadOnlyOptimal);
                                               data.output = builder.write(builder.getSwapchainImage(), vk::AttachmentLoadOp::eClear, vk::ImageLayout::eColorAttachmentOptimal, vk::ClearColorValue(std::array{0,0,0,0}));
                                               // uses ImGui, so no pre-record: pass.prerecordable = false;
                                               builder.present(data.output);
                                           },
                                           [](const Render::CompiledPass& pass, const Render::Context& frame, const Render::PassData::Present& data, vk::CommandBuffer& cmds) {
                                               ZoneScopedN("CPU RenderGraph present");
                                               auto& inputTexture = pass.getGraph().getTexture(data.input, frame.swapchainIndex);
                                               auto& swapchainTexture = pass.getGraph().getTexture(data.output, frame.swapchainIndex);
                                               frame.renderer.fullscreenBlit(pass.getRenderPass(), frame, inputTexture, swapchainTexture, cmds);
                                               ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmds);

                                               //swapchainTexture.assumeLayout(vk::ImageLayout::eUndefined);
                                               //frame.renderer.blit(inputTexture, swapchainTexture, cmds);
                                               //swapchainTexture.transitionInline(cmds, vk::ImageLayout::ePresentSrcKHR);
                                           },
                                           [this](Render::CompiledPass& pass, Render::PassData::Present& data) {
                                               renderer.initImGuiPass(pass.getRenderPass());
                                           }
        );

        globalFrameGraph = std::move(mainGraph.compile());
    } else {
        Render::GraphBuilder mainGraph(vkDriver);

        fillGraphBuilder(mainGraph, true);

        globalFrameGraph = std::move(mainGraph.compile());
    }
    updateImGuiTextures(getSwapchainImageCount());

    initConsole();
    initInputStructures();
}

void Carrot::Engine::initConsole() {
    Console::instance().registerCommands();
}

void Carrot::Engine::initInputStructures() {
    for (int joystickID = 0; joystickID <= GLFW_JOYSTICK_LAST; ++joystickID) {
        if(glfwJoystickPresent(joystickID) && glfwJoystickIsGamepad(joystickID)) {
            activeJoysticks.insert(joystickID);
        }
    }
}

void Carrot::Engine::pollGamepads() {
    gamepadStatePreviousFrame = gamepadStates;
    gamepadStates.clear();

    for(int joystickID : activeJoysticks) {
        if(glfwJoystickIsGamepad(joystickID)) {
            bool vec2ToUpdate[static_cast<std::size_t>(Carrot::IO::GameInputVectorType::Count)] = { false };
            bool valid = glfwGetGamepadState(joystickID, &gamepadStates[joystickID]);
            assert(valid);

            // Update button states
            auto& prevState = gamepadStatePreviousFrame[joystickID];
            auto& state = gamepadStates[joystickID];
            for(int buttonID = 0; buttonID <= GLFW_GAMEPAD_BUTTON_LAST; buttonID++) {
                if(state.buttons[buttonID] != prevState.buttons[buttonID]) {
                    onGamepadButtonChange(joystickID, buttonID, state.buttons[buttonID]);
                }
            }

            // Update axis states
            for(int axisID = 0; axisID <= GLFW_GAMEPAD_BUTTON_LAST; axisID++) {
                if(state.axes[axisID] != prevState.axes[axisID]) {
                    onGamepadAxisChange(joystickID, axisID, state.axes[axisID], prevState.axes[axisID]);

                    for(auto vec2Type = static_cast<std::size_t>(Carrot::IO::GameInputVectorType::First); vec2Type < static_cast<std::size_t>(Carrot::IO::GameInputVectorType::Count); vec2Type++) {
                        if(Carrot::IO::InputVectors[vec2Type].isAxisIn(axisID)) {
                            vec2ToUpdate[vec2Type] = true;
                        }
                    }
                }
            }

            // Update vec2 states
            for(auto vec2Type = static_cast<std::size_t>(Carrot::IO::GameInputVectorType::First); vec2Type < static_cast<std::size_t>(Carrot::IO::GameInputVectorType::Count); vec2Type++) {
                if(vec2ToUpdate[vec2Type]) {
                    auto& input = Carrot::IO::InputVectors[vec2Type];
                    glm::vec2 current = { state.axes[input.horizontalAxisID], state.axes[input.verticalAxisID] };
                    glm::vec2 previous = { prevState.axes[input.horizontalAxisID], prevState.axes[input.verticalAxisID] };
                    onGamepadVec2Change(joystickID, static_cast<Carrot::IO::GameInputVectorType>(vec2Type), current, previous);
                }
            }
        }
    }
}

void Carrot::Engine::onGamepadButtonChange(int gamepadID, int buttonID, bool pressed) {
    for(auto& [id, callback] : gamepadButtonCallbacks) {
        callback(gamepadID, buttonID, pressed);
    }
}

void Carrot::Engine::onGamepadAxisChange(int gamepadID, int axisID, float newValue, float oldValue) {
    for(auto& [id, callback] : gamepadAxisCallbacks) {
        callback(gamepadID, axisID, newValue, oldValue);
    }
}

void Carrot::Engine::onGamepadVec2Change(int gamepadID, Carrot::IO::GameInputVectorType vecID, glm::vec2 newValue, glm::vec2 oldValue) {
    for(auto& [id, callback] : gamepadVec2Callbacks) {
        callback(gamepadID, vecID, newValue, oldValue);
    }
}

void Carrot::Engine::onKeysVec2Change(Carrot::IO::GameInputVectorType vecID, glm::vec2 newValue, glm::vec2 oldValue) {
    for(auto& [id, callback] : keysVec2Callbacks) {
        callback(vecID, newValue, oldValue);
    }
}

void Carrot::Engine::pollKeysVec2() {
    // Update vec2 states
    for(auto vec2TypeIndex = static_cast<std::size_t>(Carrot::IO::GameInputVectorType::First); vec2TypeIndex < static_cast<std::size_t>(Carrot::IO::GameInputVectorType::Count); vec2TypeIndex++) {
        auto vec2Type = static_cast<Carrot::IO::GameInputVectorType>(vec2TypeIndex);
        auto& state = keysVec2States[vec2Type];
        auto& prevState = keysVec2StatesPreviousFrame[vec2Type];
        if(prevState != state) {
            onKeysVec2Change(vec2Type, state.asVec2(), prevState.asVec2());
        }
    }

    keysVec2StatesPreviousFrame = keysVec2States;
}

void Carrot::Engine::run() {
    size_t currentFrame = 0;


    auto previous = std::chrono::steady_clock::now();
    auto lag = std::chrono::duration<float>(0.0f);
    const auto timeBetweenUpdates = std::chrono::duration<float>(1.0f/60.0f); // 60 Hz
    bool ticked = false;
    while(running) {
        auto frameStartTime = std::chrono::steady_clock::now();
        std::chrono::duration<float> timeElapsed = frameStartTime-previous;
        currentFPS = 1.0f / timeElapsed.count();
        lag += timeElapsed;
        previous = frameStartTime;

        // Reset input actions based mouse dx/dy
        onMouseMove(mouseX, mouseY, true);
        Carrot::IO::ActionSet::updatePrePollAllSets(*this, ticked);
        glfwPollEvents();
        pollKeysVec2();
        pollGamepads();
#ifdef ENABLE_VR
        if(config.runInVR) {
            ZoneScopedN("VR poll events");
            vrInterface->pollEvents();
        }
#endif

        {
            ZoneScopedN("File watching");
            Carrot::removeIf(fileWatchers, [](auto p) { return p.expired(); });
            for(const auto& ref : fileWatchers) {
                if(auto ptr = ref.lock()) {
                    ptr->tick();
                }
            }
        }

        if(glfwWindowShouldClose(window.getGLFWPointer())) {
            if(game->onCloseButtonPressed()) {
                game->requestShutdown();
            } else {
                glfwSetWindowShouldClose(window.getGLFWPointer(), false);
            }
        }

        if(game->hasRequestedShutdown()) {
            running = false;
            break;
        }

        renderer.newFrame();
        ImGui::NewFrame();
        nextFrameAwaiter.resume_all();

        if(showInputDebug) {
            Carrot::IO::debugDrawActions();
        }

        {
            ZoneScopedN("Tick");
            TracyPlot("Tick lag", lag.count());
            TracyPlot("Estimated FPS", currentFPS);
            ticked = false;

            const std::uint32_t maxCatchupTicks = 10;
            std::uint32_t caughtUp = 0;
            while(lag >= timeBetweenUpdates && caughtUp++ < maxCatchupTicks) {
                ticked = true;
                tick(timeBetweenUpdates.count());
                lag -= timeBetweenUpdates;
            }
        }

        if(showFPS) {
            if(ImGui::Begin("FPS Counter", nullptr, ImGuiWindowFlags_::ImGuiWindowFlags_NoCollapse)) {
                ImGui::Text("%f FPS", currentFPS);
            }
            ImGui::End();
        }

        drawFrame(currentFrame);
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();

        Carrot::Log::flush();

        nextFrameAwaiter.cleanup();

        currentFrame = (currentFrame+1) % MAX_FRAMES_IN_FLIGHT;

        FrameMark;
    }

    glfwHideWindow(window.getGLFWPointer());

    getLogicalDevice().waitIdle();
}

void Carrot::Engine::stop() {
    running = false;
}

static void windowResize(GLFWwindow* window, int width, int height) {
    auto app = reinterpret_cast<Carrot::Engine*>(glfwGetWindowUserPointer(window));
    app->onWindowResize();
}

static void mouseMove(GLFWwindow* window, double xpos, double ypos) {
    auto app = reinterpret_cast<Carrot::Engine*>(glfwGetWindowUserPointer(window));
    app->onMouseMove(xpos, ypos, false);
}

static void mouseButton(GLFWwindow* window, int button, int action, int mods) {
    auto app = reinterpret_cast<Carrot::Engine*>(glfwGetWindowUserPointer(window));
    app->onMouseButton(button, action, mods);
}

static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    auto app = reinterpret_cast<Carrot::Engine*>(glfwGetWindowUserPointer(window));
    app->onKeyEvent(key, scancode, action, mods);

    ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
}

static void joystickCallback(int joystickID, int event) {
    if(event == GLFW_CONNECTED) {
        activeJoysticks.insert(joystickID);
    } else if(event == GLFW_DISCONNECTED) {
        activeJoysticks.erase(joystickID);
    }
}

void Carrot::Engine::initWindow() {
    glfwSetWindowUserPointer(window.getGLFWPointer(), this);
    glfwSetFramebufferSizeCallback(window.getGLFWPointer(), windowResize);

    glfwSetCursorPosCallback(window.getGLFWPointer(), mouseMove);
    glfwSetMouseButtonCallback(window.getGLFWPointer(), mouseButton);
    glfwSetKeyCallback(window.getGLFWPointer(), keyCallback);
    glfwSetJoystickCallback(joystickCallback);
}

void Carrot::Engine::initVulkan() {
    createCameras();

    initGame();

    createSynchronizationObjects();
}

Carrot::Engine::~Engine() {
    Carrot::Render::Sprite::cleanup();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    tracyCtx.clear();
/*    for(size_t i = 0; i < getSwapchainImageCount(); i++) {
        TracyVkDestroy(tracyCtx[i]);
    }*/
}

std::unique_ptr<Carrot::CarrotGame>& Carrot::Engine::getGame() {
    return game;
}

void Carrot::Engine::recordMainCommandBuffer(size_t i) {
    // main command buffer
    vk::CommandBufferBeginInfo beginInfo{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
            .pInheritanceInfo = nullptr,
    };

    {
        ZoneScopedN("ImGui Render");
        Console::instance().renderToImGui(*this);
        ImGui::Render();
    }

    {
        ZoneScopedN("mainCommandBuffers[i].begin(beginInfo)");
        mainCommandBuffers[i].begin(beginInfo);
    }
    {
        ZoneScopedN("PrepareVulkanTracy");

        PrepareVulkanTracy(tracyCtx[i], mainCommandBuffers[i]);
    }

    if(config.runInVR) {
        {
            ZoneScopedN("VR Left eye render");
            leftEyeGlobalFrameGraph->execute(newRenderContext(i, getMainViewport(), Render::Eye::LeftEye), mainCommandBuffers[i]);
        }
        {
            ZoneScopedN("VR Right eye render");
            rightEyeGlobalFrameGraph->execute(newRenderContext(i, getMainViewport(), Render::Eye::RightEye), mainCommandBuffers[i]);
        }
    }

    {
        ZoneScopedN("Render complete frame");
        globalFrameGraph->execute(newRenderContext(i, getMainViewport()), mainCommandBuffers[i]);
    }

    mainCommandBuffers[i].end();
}

void Carrot::Engine::allocateGraphicsCommandBuffers() {
    vk::CommandBufferAllocateInfo allocInfo{
            .commandPool = getGraphicsCommandPool(),
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = static_cast<uint32_t>(getSwapchainImageCount()),
    };

    this->mainCommandBuffers = this->getLogicalDevice().allocateCommandBuffers(allocInfo);

    vk::CommandBufferAllocateInfo gAllocInfo {
            .commandPool = getGraphicsCommandPool(),
            .level = vk::CommandBufferLevel::eSecondary,
            .commandBufferCount = static_cast<uint32_t>(getSwapchainImageCount()),
    };
    this->gBufferCommandBuffers = getLogicalDevice().allocateCommandBuffers(gAllocInfo);
    this->gResolveCommandBuffers = getLogicalDevice().allocateCommandBuffers(gAllocInfo);
    this->skyboxCommandBuffers = getLogicalDevice().allocateCommandBuffers(gAllocInfo);
}

void Carrot::Engine::drawFrame(size_t currentFrame) {
    ZoneScoped;

    vk::Result result;
    uint32_t imageIndex;
    {
        ZoneNamedN(__acquire, "Acquire image", true);

        {
            ZoneNamedN(__fences, "Wait fences", true);
            static_cast<void>(getLogicalDevice().waitForFences((*inFlightFences[currentFrame]), true, UINT64_MAX));
            getLogicalDevice().resetFences((*inFlightFences[currentFrame]));
        }

        TracyVulkanCollect(tracyCtx[lastFrameIndex]);

        {
            ZoneScopedN("acquireNextImageKHR");
            auto nextImage = getLogicalDevice().acquireNextImageKHR(vkDriver.getSwapchain(), UINT64_MAX,
                                                                    *imageAvailableSemaphore[currentFrame], nullptr);
            result = nextImage.result;

            if (result == vk::Result::eErrorOutOfDateKHR) {
                recreateSwapchain();
                return;
            } else if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
                throw std::runtime_error("Failed to acquire swap chain image");
            }
            imageIndex = nextImage.value;
            swapchainImageIndexRightNow = imageIndex;
        }
    }

    vkDriver.newFrame();

    static DebugBufferObject debug{};
    static int32_t gIndex = -1;
    if(hasPreviousFrame() && showGBuffer) {
        Render::Texture* textureToDisplay = nullptr;
        if(ImGui::Begin("GBuffer View")) {
            ImGui::RadioButton("All channels", &gIndex, -1);
            ImGui::RadioButton("Albedo", &gIndex, 0);
            ImGui::RadioButton("Position", &gIndex, 1);
            ImGui::RadioButton("Normals", &gIndex, 2);
            ImGui::RadioButton("Depth", &gIndex, 3);
            ImGui::RadioButton("UI", &gIndex, 4);
            ImGui::RadioButton("Int Properties", &gIndex, 5);
            ImGui::RadioButton("Transparent", &gIndex, 6);

            vk::Format format = vk::Format::eR32G32B32A32Sfloat;
            if(gIndex == -1) {
                textureToDisplay = imguiTextures[lastFrameIndex].allChannels;
                format = vk::Format::eR8G8B8A8Unorm;
            }
            if(gIndex == 0) {
                textureToDisplay = imguiTextures[lastFrameIndex].albedo;
                format = vk::Format::eR8G8B8A8Unorm;
            }
            if(gIndex == 1) {
                textureToDisplay = imguiTextures[lastFrameIndex].position;
            }
            if(gIndex == 2) {
                textureToDisplay = imguiTextures[lastFrameIndex].normal;
            }
            if(gIndex == 3) {
                textureToDisplay = imguiTextures[lastFrameIndex].depth;
                format = vkDriver.getDepthFormat();
            }
            if(gIndex == 4) {
                textureToDisplay = imguiTextures[lastFrameIndex].ui;
                format = vk::Format::eR8G8B8A8Unorm;
            }
            if(gIndex == 5) {
                textureToDisplay = imguiTextures[lastFrameIndex].intProperties;
                format = vk::Format::eR32Sfloat;
            }
            if(gIndex == 6) {
                textureToDisplay = imguiTextures[lastFrameIndex].transparent;
                format = vk::Format::eR8G8B8A8Unorm;
            }
            if(textureToDisplay) {
                static vk::ImageLayout layout = vk::ImageLayout::eUndefined;
                auto size = ImGui::GetWindowSize();
/*                renderer.beforeFrameCommand([&](vk::CommandBuffer& cmds) {
                    layout = textureToDisplay->getCurrentImageLayout();
                    textureToDisplay->transitionInline(cmds, vk::ImageLayout::eShaderReadOnlyOptimal);
                });
                renderer.afterFrameCommand([&](vk::CommandBuffer& cmds) {
                    textureToDisplay->transitionInline(cmds, layout);
                });*/
                vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor;
                if(textureToDisplay == imguiTextures[lastFrameIndex].depth) {
                    aspect = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
                }
                ImGui::Image(textureToDisplay->getImguiID(format, aspect), ImVec2(size.x, size.y - ImGui::GetCursorPosY()));
            }
        }
        ImGui::End();
    }

    {
        ZoneScopedN("Prepare frame");

#ifdef ENABLE_VR
        if(config.runInVR) {
            ZoneScopedN("VR start frame");
            vrSession->startFrame();
        }
#endif

        getDebugUniformBuffers()[imageIndex]->directUpload(&debug, sizeof(debug));

        renderer.beginFrame(newRenderContext(imageIndex, getMainViewport()));
        for(auto& v : viewports) {
            Carrot::Render::Context renderContext = newRenderContext(imageIndex, v);
            v.onFrame(renderContext);
            getRayTracer().onFrame(renderContext);
            game->onFrame(renderContext);
            renderer.onFrame(renderContext);
        }
        renderer.endFrame(newRenderContext(imageIndex, getMainViewport()));
    }
    {
        ZoneScopedN("Record main command buffer");
        recordMainCommandBuffer(imageIndex);
    }

    {
        ZoneScopedN("Present");

        std::vector<vk::Semaphore> waitSemaphores = {*imageAvailableSemaphore[currentFrame]};
        std::vector<vk::PipelineStageFlags> waitStages = {vk::PipelineStageFlagBits::eColorAttachmentOutput};
        vk::Semaphore signalSemaphores[] = {*renderFinishedSemaphore[currentFrame]};

        game->changeGraphicsWaitSemaphores(imageIndex, waitSemaphores, waitStages);

        vk::SubmitInfo submitInfo{
                .waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size()),
                .pWaitSemaphores = waitSemaphores.data(),

                .pWaitDstStageMask = waitStages.data(),

                .commandBufferCount = 1,
                .pCommandBuffers = &mainCommandBuffers[imageIndex],

                .signalSemaphoreCount = 1,
                .pSignalSemaphores = signalSemaphores,
        };

        {
            ZoneScopedN("Renderer Pre-Frame actions");
            renderer.preFrame();
        }

        {
            ZoneScopedN("Reset in flight fences");
            getLogicalDevice().resetFences(*inFlightFences[currentFrame]);
        }
        {
            ZoneScopedN("Submit to graphics queue");
            //presentThread.present(currentFrame, signalSemaphores[0], submitInfo, *inFlightFences[currentFrame]);

            waitForFrameTasks();

            getGraphicsQueue().submit(submitInfo, *inFlightFences[currentFrame]);
            vk::SwapchainKHR swapchains[] = { vkDriver.getSwapchain() };

            vk::PresentInfoKHR presentInfo{
                    .waitSemaphoreCount = 1,
                    .pWaitSemaphores = signalSemaphores,

                    .swapchainCount = 1,
                    .pSwapchains = swapchains,
                    .pImageIndices = &imageIndex,
                    .pResults = nullptr,
            };

            {
                ZoneScopedN("PresentKHR");
                DISCARD(vkDriver.getPresentQueue().presentKHR(&presentInfo));
            }
        }

#ifdef ENABLE_VR
        if(config.runInVR) {
            {
                ZoneScopedN("VR render");
                vrSession->present(newRenderContext(imageIndex));
            }
        }
#endif

        {
            ZoneScopedN("Renderer Post-Frame actions");
            renderer.postFrame();
        }
    }

    lastFrameIndex = imageIndex;

    if(framebufferResized) {
        recreateSwapchain();
    }
    frames++;
}

void Carrot::Engine::createSynchronizationObjects() {
    imageAvailableSemaphore.resize(MAX_FRAMES_IN_FLIGHT);
    renderFinishedSemaphore.resize(MAX_FRAMES_IN_FLIGHT);
    inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);
    imagesInFlight.resize(getSwapchainImageCount());

    vk::SemaphoreCreateInfo semaphoreInfo{};

    vk::FenceCreateInfo fenceInfo{
            .flags = vk::FenceCreateFlagBits::eSignaled,
    };

    for(size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        imageAvailableSemaphore[i] = getLogicalDevice().createSemaphoreUnique(semaphoreInfo, vkDriver.getAllocationCallbacks());
        renderFinishedSemaphore[i] = getLogicalDevice().createSemaphoreUnique(semaphoreInfo, vkDriver.getAllocationCallbacks());
        inFlightFences[i] = getLogicalDevice().createFenceUnique(fenceInfo, vkDriver.getAllocationCallbacks());
    }
}

void Carrot::Engine::recreateSwapchain() {
    // TODO: debug only, remove
    std::cout << "========== RESIZE ==========" << std::endl;
    vkDriver.fetchNewFramebufferSize();

    framebufferResized = false;

    getLogicalDevice().waitIdle();

    std::size_t previousImageCount = getSwapchainImageCount();
    vkDriver.cleanupSwapchain();
    vkDriver.createSwapChain();

    // TODO: only recreate if necessary
    if(previousImageCount != vkDriver.getSwapchainImageCount()) {
        onSwapchainImageCountChange(vkDriver.getSwapchainImageCount());
    }
    onSwapchainSizeChange(vkDriver.getFinalRenderSize().width, vkDriver.getFinalRenderSize().height);
}

void Carrot::Engine::onWindowResize() {
    framebufferResized = true;
}

const Carrot::QueueFamilies& Carrot::Engine::getQueueFamilies() {
    return vkDriver.getQueueFamilies();
}

vk::Device& Carrot::Engine::getLogicalDevice() {
    return vkDriver.getLogicalDevice();
}

vk::Optional<const vk::AllocationCallbacks> Carrot::Engine::getAllocator() {
    return vkDriver.getAllocationCallbacks();
}

vk::CommandPool& Carrot::Engine::getTransferCommandPool() {
    return vkDriver.getThreadTransferCommandPool();
}

vk::CommandPool& Carrot::Engine::getGraphicsCommandPool() {
    return vkDriver.getThreadGraphicsCommandPool();
}

vk::CommandPool& Carrot::Engine::getComputeCommandPool() {
    return vkDriver.getThreadComputeCommandPool();
}

vk::Queue& Carrot::Engine::getTransferQueue() {
    return vkDriver.getTransferQueue();
}

vk::Queue& Carrot::Engine::getGraphicsQueue() {
    return vkDriver.getGraphicsQueue();
}

vk::Queue& Carrot::Engine::getPresentQueue() {
    return vkDriver.getPresentQueue();
}

std::set<std::uint32_t> Carrot::Engine::createGraphicsAndTransferFamiliesSet() {
    return vkDriver.createGraphicsAndTransferFamiliesSet();
}

std::uint32_t Carrot::Engine::getSwapchainImageCount() {
    return vkDriver.getSwapchainImageCount();
}

std::vector<std::shared_ptr<Carrot::Buffer>>& Carrot::Engine::getDebugUniformBuffers() {
    return vkDriver.getDebugUniformBuffers();
}

void Carrot::Engine::createCameras() {
    auto center = glm::vec3(5*0.5f, 5*0.5f, 0);

    if(config.runInVR) {
        getMainViewport().getCamera(Render::Eye::LeftEye) = Camera(glm::mat4{1.0f}, glm::mat4{1.0f});
        getMainViewport().getCamera(Render::Eye::RightEye) = Camera(glm::mat4{1.0f}, glm::mat4{1.0f});
    } else {
        auto camera = Camera(45.0f, vkDriver.getWindowFramebufferExtent().width / (float) vkDriver.getWindowFramebufferExtent().height, 0.1f, 1000.0f);
        camera.getPositionRef() = glm::vec3(center.x, center.y + 1, 5.0f);
        camera.getTargetRef() = center;
        getMainViewport().getCamera(Render::Eye::NoVR) = std::move(camera);
    }
}

void Carrot::Engine::onMouseMove(double xpos, double ypos, bool updateOnlyDelta) {
    double dx = xpos-mouseX;
    double dy = ypos-mouseY;
    for(auto& [id, callback] : mouseDeltaCallbacks) {
        callback(dx, dy);
    }
    if(grabbingCursor) {
        for(auto& [id, callback] : mouseDeltaGrabbedCallbacks) {
            callback(dx, dy);
        }
    }
    if(!updateOnlyDelta) {
        for(auto& [id, callback] : mousePositionCallbacks) {
            callback(xpos, ypos);
        }
        if(game) {
            game->onMouseMove(dx, dy);
        }
        mouseX = xpos;
        mouseY = ypos;
    }
}

Carrot::Camera& Carrot::Engine::getCamera() {
    return getMainViewportCamera(Carrot::Render::Eye::NoVR);
}

Carrot::Camera& Carrot::Engine::getMainViewportCamera(Carrot::Render::Eye eye) {
    return getMainViewport().getCamera(eye);
}

void Carrot::Engine::onMouseButton(int button, int action, int mods) {
    for(auto& [id, callback] : mouseButtonCallbacks) {
        callback(button, action == GLFW_PRESS || action == GLFW_REPEAT, mods);
    }
}

void Carrot::Engine::onKeyEvent(int key, int scancode, int action, int mods) {
    if(key == GLFW_KEY_GRAVE_ACCENT && action == GLFW_RELEASE) {
        Console::instance().toggleVisibility();
    }

    if(key == GLFW_KEY_F2 && action == GLFW_PRESS) {
        takeScreenshot();
    }

    for(auto& [id, callback] : keyCallbacks) {
        callback(key, scancode, action, mods);
    }

    if(action == GLFW_REPEAT)
        return;
    bool pressed = action == GLFW_PRESS;
    for(auto vec2TypeIndex = static_cast<std::size_t>(Carrot::IO::GameInputVectorType::First); vec2TypeIndex < static_cast<std::size_t>(Carrot::IO::GameInputVectorType::Count); vec2TypeIndex++) {
        auto& input = Carrot::IO::InputVectors[vec2TypeIndex];
        if(input.isButtonIn(key)) {
            auto vec2Type = static_cast<Carrot::IO::GameInputVectorType>(vec2TypeIndex);
            auto& state = keysVec2States[vec2Type];
            if(input.upKey == key)
                state.up = pressed;
            if(input.leftKey == key)
                state.left = pressed;
            if(input.rightKey == key)
                state.right = pressed;
            if(input.downKey == key)
                state.down = pressed;
        }
    }

            /*
            if(key == GLFW_KEY_G && action == GLFW_PRESS) {
                grabCursor = !grabCursor;
                glfwSetInputMode(window.get(), GLFW_CURSOR, grabCursor ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            }*/



    // TODO: pass input to game
}

void Carrot::Engine::createTracyContexts() {
    for(size_t i = 0; i < getSwapchainImageCount(); i++) {
        tracyCtx.emplace_back(std::move(std::make_unique<TracyVulkanContext>(vkDriver.getPhysicalDevice(), getLogicalDevice(), getGraphicsQueue(), getQueueFamilies().graphicsFamily.value())));
    }
}

vk::Queue& Carrot::Engine::getComputeQueue() {
    return vkDriver.getComputeQueue();
}

Carrot::ASBuilder& Carrot::Engine::getASBuilder() {
    return renderer.getASBuilder();
}

void Carrot::Engine::tick(double deltaTime) {
    ZoneScoped;
    game->tick(deltaTime);
}

void Carrot::Engine::takeScreenshot() {
    namespace fs = std::filesystem;

    auto currentTime = std::chrono::system_clock::now().time_since_epoch().count();
    auto screenshotFolder = fs::current_path() / "screenshots";
    if(!fs::exists(screenshotFolder)) {
        if(!fs::create_directories(screenshotFolder)) {
            throw std::runtime_error("Could not create screenshot folder");
        }
    }
    auto screenshotPath = screenshotFolder / (std::to_string(currentTime) + ".png");

    auto& lastImage = vkDriver.getSwapchainTextures()[lastFrameIndex];

    auto& swapchainExtent = vkDriver.getFinalRenderSize();
    auto screenshotImage = Image(vkDriver,
                                 {swapchainExtent.width, swapchainExtent.height, 1},
                                 vk::ImageUsageFlagBits::eTransferDst,
                                 vk::Format::eR8G8B8A8Unorm
                                 );

    vk::DeviceSize bufferSize = 4*swapchainExtent.width*swapchainExtent.height * sizeof(uint32_t); // TODO
    auto screenshotBuffer = getResourceAllocator().allocateBuffer(
                                   bufferSize,
                                   vk::BufferUsageFlagBits::eTransferDst,
                                   vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
                                   );

    auto offsetMin = vk::Offset3D {
            .x = 0,
            .y = 0,
            .z = 0,
    };
    auto offsetMax = vk::Offset3D {
            .x = static_cast<int32_t>(swapchainExtent.width),
            .y = static_cast<int32_t>(swapchainExtent.height),
            .z = 1,
    };
    performSingleTimeGraphicsCommands([&](vk::CommandBuffer& commands) {
        lastImage->assumeLayout(vk::ImageLayout::ePresentSrcKHR);
        lastImage->transitionInline(commands, vk::ImageLayout::eTransferSrcOptimal);
        screenshotImage.transitionLayoutInline(commands, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);

        commands.blitImage(lastImage->getImage().getVulkanImage(), vk::ImageLayout::eTransferSrcOptimal, screenshotImage.getVulkanImage(), vk::ImageLayout::eTransferDstOptimal, vk::ImageBlit {
                .srcSubresource = {
                        .aspectMask = vk::ImageAspectFlagBits::eColor,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                },
                .srcOffsets = std::array<vk::Offset3D, 2> {
                        offsetMin,
                        offsetMax,
                },
                .dstSubresource = {
                        .aspectMask = vk::ImageAspectFlagBits::eColor,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                },
                .dstOffsets = std::array<vk::Offset3D, 2> {
                        offsetMin,
                        offsetMax,
                },
        }, vk::Filter::eNearest);

        commands.copyImageToBuffer(screenshotImage.getVulkanImage(), vk::ImageLayout::eGeneral, screenshotBuffer.getVulkanBuffer(), vk::BufferImageCopy {
            // tightly packed
            .bufferRowLength = 0,
            .bufferImageHeight = 0,

            .imageSubresource = {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
            },

            .imageExtent = {
                    .width = swapchainExtent.width,
                    .height = swapchainExtent.height,
                    .depth = 1,
            },
        });
    });

    void* pData = screenshotBuffer.map<void>();
    stbi_write_png(screenshotPath.generic_string().c_str(), swapchainExtent.width, swapchainExtent.height, 4, pData, 4 * swapchainExtent.width);

    screenshotBuffer.unmap();
}

Carrot::Skybox::Type Carrot::Engine::getSkybox() const {
    return currentSkybox;
}

void Carrot::Engine::setSkybox(Carrot::Skybox::Type type) {
    static std::vector<SimpleVertex> skyboxVertices = {
            { { 1.0f, -1.0f, -1.0f } },
            { { 1.0f, -1.0f, 1.0f } },
            { { -1.0f, -1.0f, -1.0f } },
            { { -1.0f, -1.0f, 1.0f } },
            { { 1.0f, 1.0f, -1.0f } },
            { { 1.0f, 1.0f, 1.0f } },
            { { -1.0f, 1.0f, -1.0f } },
            { { -1.0f, 1.0f, 1.0f } },
    };
    static std::vector<std::uint32_t> skyboxIndices = {
            1, 2, 0,
            3, 6, 2,
            7, 4, 6,
            5, 0, 4,
            6, 0, 2,
            3, 5, 7,
            1, 3, 2,
            3, 7, 6,
            7, 5, 4,
            5, 1, 0,
            6, 4, 0,
            3, 1, 5,
    };
    currentSkybox = type;
    if(type != Carrot::Skybox::Type::None) {
        ZoneScopedN("Prepare skybox texture & mesh");
        {
            ZoneScopedN("Load skybox cubemap");
            loadedSkyboxTexture = std::make_unique<Render::Texture>(Image::cubemapFromFiles(vkDriver, [type](Skybox::Direction dir) {
                return Skybox::getTexturePath(type, dir);
            }));
            loadedSkyboxTexture->name("Current loaded skybox");
        }

        {
            ZoneScopedN("Create skybox mesh");
            skyboxMesh = make_unique<Mesh>(vkDriver, skyboxVertices, skyboxIndices);
        }
    }
}

void Carrot::Engine::onSwapchainImageCountChange(size_t newCount) {
    vkDriver.onSwapchainImageCountChange(newCount);

    // TODO: rebuild graphs
    // TODO: multi-threading (command pools are threadlocal)
    vkDriver.getLogicalDevice().resetCommandPool(getGraphicsCommandPool());
    allocateGraphicsCommandBuffers();

    renderer.onSwapchainImageCountChange(newCount);

    if(config.runInVR) {
        leftEyeGlobalFrameGraph->onSwapchainImageCountChange(newCount);
        rightEyeGlobalFrameGraph->onSwapchainImageCountChange(newCount);
    }
    globalFrameGraph->onSwapchainImageCountChange(newCount);

    createSynchronizationObjects();

    game->onSwapchainImageCountChange(newCount);

    updateImGuiTextures(newCount);
}

void Carrot::Engine::onSwapchainSizeChange(int newWidth, int newHeight) {
    vkDriver.onSwapchainSizeChange(newWidth, newHeight);

    renderer.onSwapchainSizeChange(newWidth, newHeight);

    if(config.runInVR) {
        leftEyeGlobalFrameGraph->onSwapchainSizeChange(newWidth, newHeight);
        rightEyeGlobalFrameGraph->onSwapchainSizeChange(newWidth, newHeight);
    }

    globalFrameGraph->onSwapchainSizeChange(newWidth, newHeight);

    game->onSwapchainSizeChange(newWidth, newHeight);

    updateImGuiTextures(getSwapchainImageCount());
}

void Carrot::Engine::updateImGuiTextures(std::size_t swapchainLength) {
    imguiTextures.resize(swapchainLength);
    for (int i = 0; i < swapchainLength; ++i) {
        auto& textures = imguiTextures[i];
        textures.allChannels = &globalFrameGraph->getTexture(gResolvePassData.resolved, i);

        textures.albedo = &globalFrameGraph->getTexture(gResolvePassData.albedo, i);

        textures.position = &globalFrameGraph->getTexture(gResolvePassData.positions, i);

        textures.normal = &globalFrameGraph->getTexture(gResolvePassData.normals, i);

        textures.depth = &globalFrameGraph->getTexture(gResolvePassData.depthStencil, i);

        textures.intProperties = &globalFrameGraph->getTexture(gResolvePassData.flags, i);

        textures.transparent = &globalFrameGraph->getTexture(gResolvePassData.transparent, i);
    }
}

Carrot::Render::Context Carrot::Engine::newRenderContext(std::size_t swapchainFrameIndex, Carrot::Render::Viewport& viewport, Carrot::Render::Eye eye) {
    return Carrot::Render::Context {
            .renderer = renderer,
            .viewport = viewport,
            .eye = eye,
            .frameCount = frames,
            .swapchainIndex = swapchainFrameIndex,
            .lastSwapchainIndex = lastFrameIndex,
    };
}

Carrot::Async::Task<> Carrot::Engine::cowaitNextFrame() {
    co_await nextFrameAwaiter;
}

void Carrot::Engine::addFrameTask(FrameTask&& task) {
    frameTaskFutures.emplace_back(std::move(std::async(std::launch::async, task)));
}

void Carrot::Engine::waitForFrameTasks() {
    for(auto& f : frameTaskFutures) {
        f.wait();
    }
    frameTaskFutures.clear();
}

Carrot::Render::Viewport& Carrot::Engine::getMainViewport() {
    return viewports.front();
}

Carrot::Render::Viewport& Carrot::Engine::createViewport() {
    viewports.emplace_back(renderer);
    return viewports.back();
}

Carrot::Render::Pass<Carrot::Render::PassData::GResolve>& Carrot::Engine::fillInDefaultPipeline(Carrot::Render::GraphBuilder& mainGraph, Carrot::Render::Eye eye,
                                           std::function<void(const Carrot::Render::CompiledPass&,
                                                              const Carrot::Render::Context&,
                                                              vk::CommandBuffer&)> opaqueCallback,
                                           std::function<void(const Carrot::Render::CompiledPass&,
                                                              const Carrot::Render::Context&,
                                                              vk::CommandBuffer&)> transparentCallback) {
    auto testTexture = renderer.getOrCreateTexture("default.png");

    auto& skyboxPass = mainGraph.addPass<Carrot::Render::PassData::Skybox>("skybox",
                                                                           [this](Render::GraphBuilder& builder, Render::Pass<Carrot::Render::PassData::Skybox>& pass, Carrot::Render::PassData::Skybox& data) {
                                                                               data.output = builder.createRenderTarget(vk::Format::eR8G8B8A8Unorm,
                                                                                                                        {},
                                                                                                                        vk::AttachmentLoadOp::eClear,
                                                                                                                        vk::ClearColorValue(std::array{0,0,0,0})
                                                                               );
                                                                           },
                                                                           [this](const Render::CompiledPass& pass, const Render::Context& frame, const Carrot::Render::PassData::Skybox& data, vk::CommandBuffer& buffer) {
                                                                               ZoneScopedN("CPU RenderGraph skybox");
                                                                               auto skyboxPipeline = renderer.getOrCreateRenderPassSpecificPipeline("skybox", pass.getRenderPass());
                                                                               renderer.bindCameraSet(vk::PipelineBindPoint::eGraphics, skyboxPipeline->getPipelineLayout(), frame,
                                                                                                      buffer);
                                                                               renderer.bindTexture(*skyboxPipeline, frame, *loadedSkyboxTexture, 0, 0, vk::ImageAspectFlagBits::eColor, vk::ImageViewType::eCube);
                                                                               skyboxPipeline->bind(pass.getRenderPass(), frame, buffer);
                                                                               skyboxMesh->bind(buffer);
                                                                               skyboxMesh->draw(buffer);
                                                                           }
    );
    skyboxPass.setCondition([this](const Render::CompiledPass& pass, const Render::Context& frame, const Carrot::Render::PassData::Skybox& data) {
        return currentSkybox != Skybox::Type::None;
    });

    auto& opaqueGBufferPass = getGBuffer().addGBufferPass(mainGraph, [opaqueCallback](const Render::CompiledPass& pass, const Render::Context& frame, vk::CommandBuffer& cmds) {
        ZoneScopedN("CPU RenderGraph Opaque GPass");
        opaqueCallback(pass, frame, cmds);
    });
    auto& transparentGBufferPass = getGBuffer().addTransparentGBufferPass(mainGraph, opaqueGBufferPass.getData(), [transparentCallback](const Render::CompiledPass& pass, const Render::Context& frame, vk::CommandBuffer& cmds) {
        ZoneScopedN("CPU RenderGraph Opaque GPass");
        transparentCallback(pass, frame, cmds);
    });
    auto& gresolvePass = getGBuffer().addGResolvePass(opaqueGBufferPass.getData(), transparentGBufferPass.getData(), skyboxPass.getData().output, mainGraph);

    return gresolvePass;
}

std::shared_ptr<Carrot::IO::FileWatcher> Carrot::Engine::createFileWatcher(const Carrot::IO::FileWatcher::Action& action, const std::vector<std::filesystem::path>& filesToWatch) {
    auto watcher = std::make_shared<Carrot::IO::FileWatcher>(action, filesToWatch);
    fileWatchers.emplace_back(watcher);
    return watcher;
}


#ifdef TRACY_ENABLE
void* operator new(std::size_t count) {
    auto ptr = malloc(count);
    TracyAllocS(ptr, count, 20);
    return ptr;
}

void operator delete(void* ptr) noexcept{
    TracyFreeS(ptr, 20);
    free(ptr);
}
#endif