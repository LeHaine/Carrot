//
// Created by jglrxavpok on 30/12/2020.
//

#pragma once
#include <core/SparseArray.hpp>

#include "engine/Engine.h"
#include "AccelerationStructure.h"
#include <glm/matrix.hpp>
#include <core/utils/WeakPool.hpp>
#include <core/async/Locks.h>
#include "SceneElement.h"

namespace Carrot {
    struct GeometryInput {
        std::vector<vk::AccelerationStructureGeometryKHR> geometries{};
        std::vector<vk::AccelerationStructureBuildRangeInfoKHR> buildRanges{};

        std::unique_ptr<AccelerationStructure> as{};

        // cached structures for rebuilding
        std::unique_ptr<Buffer> scratchBuffer{};
        std::unique_ptr<vk::AccelerationStructureBuildGeometryInfoKHR> cachedBuildInfo{};
        std::vector<const vk::AccelerationStructureBuildRangeInfoKHR*> cachedBuildRanges{};
    };

    struct InstanceInput {
        glm::mat4 transform{1.0f};
        std::uint32_t customInstanceIndex;
        std::uint32_t geometryIndex;

        std::uint32_t mask;
        std::uint32_t hitGroup;
    };

    class ASBuilder;


    enum class BLASGeometryFormat: std::uint8_t {
        Default = 0, // Carrot::Vertex
        ClusterCompressed = 1, // Carrot::PackedVertex
    };

    class BLASHandle: public std::enable_shared_from_this<BLASHandle> {
    public:
        bool dynamicGeometry = false;

        BLASHandle(const std::vector<std::shared_ptr<Carrot::Mesh>>& meshes,
                   const std::vector<glm::mat4>& transforms,
                   const std::vector<std::uint32_t>& materialSlots,
                   BLASGeometryFormat geometryFormat,
                   ASBuilder* builder);

        // Version of BLASHandle that does not hold its transform data but only refers to data already somewhere in memory
        BLASHandle(const std::vector<std::shared_ptr<Carrot::Mesh>>& meshes,
                   const std::vector<vk::DeviceAddress/*vk::TransformMatrixKHR*/>& transformAddresses,
                   const std::vector<std::uint32_t>& materialSlots,
                   BLASGeometryFormat geometryFormat,
                   ASBuilder* builder);

        bool isBuilt() const { return built; }
        void update();
        void setDirty();

        /// Bind semaphores to this BLAS:
        /// building this BLAS must wait on the provided semaphore for the current frame
        /// (one semaphore per swapchain image)
        void bindSemaphores(const Render::PerFrame<vk::Semaphore>& semaphores);

        virtual ~BLASHandle() noexcept;

    private:
        void innerInit(std::span<const vk::DeviceAddress/*vk::TransformMatrixKHR*/> transformAddresses);

        /// semaphore to wait for before building this BLAS
        vk::Semaphore getBoundSemaphore(std::size_t swapchainIndex) const;

        std::vector<vk::AccelerationStructureGeometryKHR> geometries{};
        std::vector<vk::AccelerationStructureBuildRangeInfoKHR> buildRanges{};

        std::unique_ptr<AccelerationStructure> as = nullptr;
        Carrot::BufferAllocation transformData;
        std::vector<std::shared_ptr<Carrot::Mesh>> meshes;
        std::vector<std::uint32_t> materialSlots;
        std::uint32_t firstGeometryIndex = (std::uint32_t)-1;
        bool built = false;
        ASBuilder* builder = nullptr;
        BLASGeometryFormat geometryFormat = BLASGeometryFormat::Default;
        Render::PerFrame<vk::Semaphore> boundSemaphores;

        friend class ASBuilder;
        friend class InstanceHandle;
    };

    class InstanceHandle {
    public:
        InstanceHandle(std::weak_ptr<BLASHandle> geometry,
                       ASBuilder* builder);

        bool isBuilt() const { return built; }
        bool hasBeenModified() const { return modified; }
        bool isUsable() { return enabled && !geometry.expired() && geometry.lock()->as; }
        void update();

        virtual ~InstanceHandle() noexcept;

    public:
        glm::mat4 transform{1.0f};
        glm::vec4 instanceColor{1.0f};
        vk::GeometryInstanceFlagsKHR flags = vk::GeometryInstanceFlagBitsKHR::eForceOpaque | vk::GeometryInstanceFlagBitsKHR::eTriangleFacingCullDisable | vk::GeometryInstanceFlagBitsKHR::eTriangleCullDisable;

        std::uint8_t mask = 0xFF;
        std::uint32_t customIndex = 0;
        bool enabled = true;

    private:
        glm::mat4 oldTransform{1.0f};
        vk::AccelerationStructureInstanceKHR instance;
        std::weak_ptr<BLASHandle> geometry;

        bool modified = false;
        bool built = false;
        ASBuilder* builder = nullptr;

        friend class ASBuilder;
    };

    template<typename T>
    class ASStorage {
        constexpr static std::size_t Granularity = 2048;
        const std::shared_ptr<T> nullEntry = nullptr;
    public:
        using Slot = std::unique_ptr<std::weak_ptr<T>>;
        using Reservation = std::weak_ptr<T>*;

        ASStorage() = default;

        /**
         * Gets an estimate of the current storage size.
         * This is an estimate because the storage size can change right after returning from this call.
         * This is also the estimation of the underlying storage, so it almost always bigger than the actual number of elements
         * (overhead is dependent on Granularity value)
         */
        std::size_t getStorageSize() const {
            Async::LockGuard g { rwlock.read() };
            return slots.size();
        }

        Reservation reserveSlot() {
            // TODO: free list
            Async::ReadLock& readLock = rwlock.read();
            readLock.lock();
            std::uint32_t newID = nextID++;
            if(newID < slots.size()) { // inside a bank that was already allocated
                auto& ptr = slots[newID];
                ptr = std::make_unique<std::weak_ptr<T>>(nullEntry);
                readLock.unlock();
                return ptr.get();
            } else {
                readLock.unlock();
                // need to allocate a new bank

                Async::WriteLock& writeLock = rwlock.write();//readLock.upgradeToWriter();
                writeLock.lock();
                std::size_t requiredSize = (newID / Granularity +1) * Granularity;
                if(requiredSize > slots.size()) { // another thread could have come here and already increased the storage size
                    slots.resize(requiredSize);
                }
                auto& ptr = slots[newID];
                ptr = std::make_unique<std::weak_ptr<T>>(nullEntry);
                writeLock.unlock();

                return ptr.get();
            }
        }

        void iterate(const std::function<void(std::shared_ptr<T>)>& forEach) {
            Async::LockGuard g { rwlock.read() };
            slots.iterate([&forEach](Slot& s) {
                if(!s) {
                    return;
                }

                if(auto v = s->lock()) {
                    forEach(v);
                }
            });
        }


    private:
        mutable Async::ReadWriteLock rwlock;
        SparseArray<Slot, Granularity> slots;
        std::atomic_int32_t nextID { 0 };
    };

    /// Helpers to build Acceleration Structures for raytracing
    // TODO: rename to RaytracingScene
    class ASBuilder: public SwapchainAware {
    public:
        explicit ASBuilder(VulkanRenderer& renderer);

        std::shared_ptr<InstanceHandle> addInstance(std::weak_ptr<BLASHandle> correspondingGeometry);

        std::shared_ptr<BLASHandle> addBottomLevel(const std::vector<std::shared_ptr<Carrot::Mesh>>& meshes,
                                                   const std::vector<glm::mat4>& transforms,
                                                   const std::vector<std::uint32_t>& materialSlots,
                                                   BLASGeometryFormat geometryFormat);

        /// Version of addBottomLevel which does not have ownership over transform data
        std::shared_ptr<BLASHandle> addBottomLevel(const std::vector<std::shared_ptr<Carrot::Mesh>>& meshes,
                                                   const std::vector<vk::DeviceAddress/*vk::TransformMatrixKHR*/>& transforms,
                                                   const std::vector<std::uint32_t>& materialSlots,
                                                   BLASGeometryFormat geometryFormat);

        void startFrame();
        void waitForCompletion(vk::CommandBuffer& cmds);

        AccelerationStructure* getTopLevelAS(const Carrot::Render::Context& renderContext);

        void onFrame(const Carrot::Render::Context& renderContext);

    public:
        void onSwapchainImageCountChange(size_t newCount) override;

        void onSwapchainSizeChange(Window& window, int newWidth, int newHeight) override;

    public:
        /**
         * Buffer containing of vector of SceneDescription::Geometry
         */
        Carrot::BufferView getGeometriesBuffer(const Render::Context& renderContext);

        /**
         * Buffer containing of vector of SceneDescription::Instance
         */
        Carrot::BufferView getInstancesBuffer(const Render::Context& renderContext);

        static vk::TransformMatrixKHR glmToRTTransformMatrix(const glm::mat4& mat);

    private:
        void createGraveyard();
        void createSemaphores();
        void createBuildCommandBuffers();
        void createQueryPools();
        void createDescriptors();

    private:
        void resetBlasBuildCommands(const Carrot::Render::Context& renderContext);
        vk::CommandBuffer getBlasBuildCommandBuffer(const Carrot::Render::Context& renderContext);
        void buildTopLevelAS(const Carrot::Render::Context& renderContext, bool update);
        void buildBottomLevels(const Carrot::Render::Context& renderContext, const std::vector<std::shared_ptr<BLASHandle>>& toBuild);

        /// Returns the buffer view which contains an identity matrix, intended for reusing the same memory location for BLASes which do not have a specific transform
        BufferView getIdentityMatrixBufferView() const;

    private:
        VulkanRenderer& renderer;
        Async::SpinLock access;
        bool enabled = false;

    private: // reuse between builds
        std::vector<vk::UniqueCommandBuffer> tlasBuildCommands{};
        std::vector<vk::UniqueQueryPool> queryPools{};
        std::vector<std::vector<std::unique_ptr<Carrot::AccelerationStructure>>> asGraveyard; // used to store BLAS that get immediatly compacted, but need to stay alive for a few frames
        std::vector<std::vector<TracyVkCtx>> blasBuildTracyCtx; // [swapchainIndex][blasIndex]

        std::shared_ptr<Carrot::BufferAllocation> geometriesBuffer;
        std::shared_ptr<Carrot::BufferAllocation> instancesBuffer;
        std::shared_ptr<Carrot::BufferAllocation> rtInstancesBuffer;
        std::shared_ptr<Carrot::BufferAllocation> rtInstancesScratchBuffer;
        Carrot::Render::PerFrame<std::shared_ptr<Carrot::BufferAllocation>> geometriesBufferPerFrame;
        Carrot::Render::PerFrame<std::shared_ptr<Carrot::BufferAllocation>> instancesBufferPerFrame;
        Carrot::Render::PerFrame<std::shared_ptr<Carrot::BufferAllocation>> rtInstancesBufferPerFrame;
        Carrot::Render::PerFrame<std::shared_ptr<Carrot::BufferAllocation>> rtInstancesScratchBufferPerFrame;
        Carrot::Render::PerFrame<std::unique_ptr<Carrot::Async::ParallelMap<std::thread::id, vk::UniqueCommandPool>>> blasBuildCommandPool;

        std::size_t lastInstanceCount = 0;
        vk::DeviceAddress instanceBufferAddress = 0;

        std::size_t lastScratchSize = 0;
        vk::DeviceAddress scratchBufferAddress = 0;

        ASStorage<BLASHandle> staticGeometries;
        ASStorage<InstanceHandle> instances;

        Render::PerFrame<std::shared_ptr<AccelerationStructure>> tlasPerFrame;
        std::shared_ptr<AccelerationStructure> currentTLAS;

        bool builtBLASThisFrame = false;
        std::vector<vk::UniqueSemaphore> instanceUploadSemaphore;
        std::vector<vk::UniqueSemaphore> geometryUploadSemaphore;
        std::vector<vk::UniqueSemaphore> tlasBuildSemaphore;
        std::vector<vk::UniqueSemaphore> preCompactBLASSemaphore;
        std::vector<vk::UniqueSemaphore> blasBuildSemaphore;

        std::int8_t framesBeforeRebuildingTLAS = 0;
        std::size_t previousActiveInstances = 0;
        std::atomic_bool dirtyBlases = false;
        std::atomic_bool dirtyInstances = false;

        std::vector<SceneDescription::Geometry> allGeometries;

        std::size_t lastFrameIndexForTLAS = 0;
        Carrot::BufferAllocation identityMatrixForBLASes;

    private:
        std::vector<vk::BufferMemoryBarrier2KHR> bottomLevelBarriers;
        std::vector<vk::BufferMemoryBarrier2KHR> topLevelBarriers;

        friend class BLASHandle;
        friend class InstanceHandle;
    };
}
