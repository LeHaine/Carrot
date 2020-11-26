//
// Created by jglrxavpok on 21/11/2020.
//
#pragma once
#include <memory>
#include <optional>
#include <vector>

#include <vulkan/includes.h>
#include <GLFW/glfw3.h>
#include "memory/NakedPtr.hpp"

using namespace std;

struct QueueFamilies {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete();
};

struct SwapChainSupportDetails {
    vk::SurfaceCapabilitiesKHR capabilities;
    std::vector<vk::SurfaceFormatKHR> formats;
    std::vector<vk::PresentModeKHR> presentModes;
};

class CarrotEngine {
public:
    explicit CarrotEngine(NakedPtr<GLFWwindow> window);

    void run();

    void onWindowResize();

    /// Cleanup resources
    ~CarrotEngine();

private:
    bool running = true;
    NakedPtr<GLFWwindow> window = nullptr;
    const vk::AllocationCallbacks* allocator = nullptr;

    vk::UniqueInstance instance;
    vk::UniqueDebugUtilsMessengerEXT callback{};
    vk::PhysicalDevice physicalDevice{};
    vk::UniqueDevice device{};
    vk::Queue graphicsQueue{};
    vk::Queue presentQueue{};
    vk::SurfaceKHR surface{};
    vk::UniqueSwapchainKHR swapchain{};
    std::vector<vk::Image> swapchainImages{}; // not unique because deleted with swapchain
    vk::Format swapchainImageFormat = vk::Format::eUndefined;
    vk::Extent2D swapchainExtent{};
    std::vector<vk::UniqueImageView> swapchainImageViews{};
    vk::UniqueRenderPass renderPass{};
    vk::UniquePipelineLayout pipelineLayout{};
    vk::UniquePipeline graphicsPipeline{};
    std::vector<vk::UniqueFramebuffer> swapchainFramebuffers{};
    vk::UniqueCommandPool commandPool{};
    std::vector<vk::CommandBuffer> commandBuffers{};
    std::vector<vk::UniqueSemaphore> imageAvailableSemaphore{};
    std::vector<vk::UniqueSemaphore> renderFinishedSemaphore{};
    std::vector<vk::UniqueFence> inFlightFences{};
    std::vector<vk::UniqueFence> imagesInFlight{};

    // TODO: abstraction over vertex buffers
    vk::UniqueBuffer vertexBuffer{};
    vk::UniqueDeviceMemory vertexBufferMemory{};
    bool framebufferResized = false;

    /// Init engine
    void init();

    /// Init window
    void initWindow();

    /// Init Vulkan for rendering
    void initVulkan();

    /// Create Vulkan instance
    void createInstance();

    /// Check validation layers are supported (if NO_DEBUG disabled)
    bool checkValidationLayerSupport();

    /// Generate a vector with the required extensions for this application
    std::vector<const char *> getRequiredExtensions();

    /// Setups debug messages
    void setupDebugMessenger();

    /// Prepares a debug messenger
    void setupMessenger(vk::DebugUtilsMessengerCreateInfoEXT &ext);

    /// Select a GPU
    void pickPhysicalDevice();

    /// Rank physical device based on their capabilities
    int ratePhysicalDevice(const vk::PhysicalDevice& device);

    /// Gets the queue indices of a given physical device
    QueueFamilies findQueueFamilies(const vk::PhysicalDevice& device);

    /// Create the logical device to interface with Vulkan
    void createLogicalDevice();

    /// Create the rendering surface for Vulkan
    void createSurface();

    /// Check the given device supports the extensions inside VULKAN_DEVICE_EXTENSIONS (constants.h)
    bool checkDeviceExtensionSupport(const vk::PhysicalDevice &logicalDevice);

    /// Queries the format and present modes from a given physical device
    SwapChainSupportDetails querySwapChainSupport(const vk::PhysicalDevice& device);

    /// Choose best surface format from the list of given formats
    /// \param formats
    /// \return
    vk::SurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& formats);

    /// Choose best present mode from the list of given modes
    /// \param presentModes
    /// \return
    vk::PresentModeKHR chooseSwapPresentMode(const std::vector<vk::PresentModeKHR>& presentModes);

    /// Choose resolution of swap chain images
    vk::Extent2D chooseSwapExtent(const vk::SurfaceCapabilitiesKHR& capabilities);

    void createSwapChain();

    void createSwapChainImageViews();

    [[nodiscard]] vk::UniqueImageView createImageView(const vk::Image& image, vk::Format imageFormat, vk::ImageAspectFlagBits aspectMask = vk::ImageAspectFlagBits::eColor) const;

    void createGraphicsPipeline();

    vk::UniqueShaderModule createShaderModule(const std::vector<char>& bytecode);

    void createRenderPass();

    void createFramebuffers();

    void createCommandPool();

    void createCommandBuffers();

    void drawFrame(size_t currentFrame);

    void createSynchronizationObjects();

    void recreateSwapchain();

    void cleanupSwapchain();

    void createVertexBuffer();

    uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties);

    // Templates

    template<typename T>
    vk::UniqueDeviceMemory allocateUploadBuffer(vk::Device& device, vk::Buffer& buffer, const vector<T>& data);
};

// template implementation
#include "CarrotEngine.ipp"
