//
// Created by jglrxavpok on 28/12/2021.
//

#pragma once
#include <memory>
#include <engine/vulkan/includes.h>
#include <engine/vulkan/SwapchainAware.h>
#include <engine/render/resources/BufferView.h>

namespace Carrot {
    /// Allocates resources that will be used for a single frame
    /// Resources are automatically cleared at the beginning of the frame, with a ring-buffer like structure
    /// (to account for frames in flight)
    /// The buffer size given as parameter will multiplied by the swapchain length, as the memory is kept alive
    /// during frames in flight
    class SingleFrameStackGPUAllocator: public SwapchainAware {
    private:
        class RingBuffer: public SwapchainAware {
        public:
            explicit RingBuffer(vk::DeviceSize bufferSize, vk::BufferUsageFlags usages, vk::MemoryPropertyFlags memoryProperties);

            void clear(std::size_t index);
            void resize(std::size_t length);

            Carrot::BufferView allocateAligned(std::size_t index, vk::DeviceSize size, vk::DeviceSize align = 1);

        private:
            vk::DeviceSize bufferSize;
            vk::BufferUsageFlags usages;
            vk::MemoryPropertyFlags memoryProperties;

            std::vector<std::unique_ptr<Carrot::Buffer>> stacks;
            std::vector<vk::DeviceSize> stackPointers;
        };

    public:
        explicit SingleFrameStackGPUAllocator(vk::DeviceSize instancingBufferSize);

        /// Starts a new frame and clears the memory to use for this frame
        void newFrame(std::size_t frameIndex);

    public: // allocations
        Carrot::BufferView getInstanceBuffer(std::size_t size);


    public: // SwapchainAware
        virtual void onSwapchainImageCountChange(size_t newCount) override;
        virtual void onSwapchainSizeChange(int newWidth, int newHeight) override;

    private:
        std::size_t currentFrame = 0;
        RingBuffer instanceBuffers;
    };
}
