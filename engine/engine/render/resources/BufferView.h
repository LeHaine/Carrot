//
// Created by jglrxavpok on 10/03/2021.
//

#pragma once

#include "Buffer.h"
#include <memory>
#include <span>

namespace Carrot {
    class ResourceAllocator;

    class BufferView: public DeviceAddressable {
    public:
        explicit BufferView(Carrot::ResourceAllocator* allocator, Carrot::Buffer& buffer, vk::DeviceSize start, vk::DeviceSize size);
        explicit BufferView(): buffer(nullptr), start(0), size(0), allocator(nullptr) {};
        BufferView(const BufferView&) = default;

        bool operator==(const BufferView&) const;
        BufferView& operator=(const BufferView&) = default;

        [[nodiscard]] vk::DeviceSize getStart() const { return start; };
        [[nodiscard]] vk::DeviceSize getSize() const { return size; };

        [[nodiscard]] Buffer& getBuffer() { return *buffer; };
        [[nodiscard]] const Buffer& getBuffer() const { return *buffer; };
        [[nodiscard]] const vk::Buffer& getVulkanBuffer() const { return getBuffer().getVulkanBuffer(); };

        /// Mmaps the buffer memory into the application memory space, and copies the data from 'data'. Unmaps the memory when finished.
        /// Only use for host-visible and host-coherent memory
        void directUpload(const void* data, vk::DeviceSize length);

        /// Upload to device-local memory
        void stageUpload(const void* data, vk::DeviceSize length);

        template<typename T>
        void directUpload(const std::span<const T>& data) {
            directUpload(data.data(), static_cast<vk::DeviceSize>(data.size() * sizeof(T)));
        }

        template<typename T>
        void stageUpload(const std::span<const T>& data) {
            stageUpload(data.data(), static_cast<vk::DeviceSize>(data.size() * sizeof(T)));
        }

        /// Copies the contents of this buffer to the given memory
        void download(const std::span<std::uint8_t>& data, std::uint32_t offset = 0) const;

        [[nodiscard]] vk::DescriptorBufferInfo asBufferInfo() const;

        explicit operator bool() const {
            return !!buffer;
        }

        void flushMappedMemory();

        template<typename T>
        T* map();

        void unmap();

        vk::DeviceAddress getDeviceAddress() const override;

        ~BufferView();

    private:
        Carrot::ResourceAllocator* allocator = nullptr; // TODO: remove allocator ref, maybe make BufferRef somewhere. Need to do a pass on all usages of BufferView/Buffer
        Carrot::Buffer* buffer;
        vk::DeviceSize start;
        vk::DeviceSize size;
    };

}

#include "BufferView.ipp"
