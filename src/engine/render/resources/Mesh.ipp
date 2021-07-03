#pragma once
#include "Mesh.h"

template<typename VertexType>
Carrot::Mesh::Mesh(Carrot::VulkanDriver& driver, const vector<VertexType>& vertices, const vector<uint32_t>& indices): meshID(currentMeshID++), driver(driver) {
    const auto& queueFamilies = driver.getQueueFamilies();
    // create and allocate underlying buffer
    std::set<uint32_t> families = {
            queueFamilies.transferFamily.value(), queueFamilies.graphicsFamily.value()
    };

    // TODO: change default of 0x10 (used because of storage buffer alignments on my RTX 3070)
    const size_t alignment = 0x10;//sizeof(uint32_t);
    vertexStartOffset = sizeof(uint32_t) * indices.size();
    if(vertexStartOffset % alignment != 0) {
        // align on uint32 boundary
        vertexStartOffset += alignment - (vertexStartOffset % alignment);
    }
    indexCount = indices.size();
    vertexCount = vertices.size();
    vertexAndIndexBuffer = make_unique<Carrot::Buffer>(driver,
                                                       vertexStartOffset + sizeof(VertexType) * vertices.size(),
                                                       vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc /*TODO: tmp: used by skinning to copy to final buffer, instead of descriptor bind*/ | vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eStorageBuffer,
                                                       vk::MemoryPropertyFlagBits::eDeviceLocal,
                                                       families);

    // upload vertices
    vertexAndIndexBuffer->stageUploadWithOffsets(make_pair(vertexStartOffset, vertices), make_pair(static_cast<uint64_t>(0), indices));
}