//
// Created by jglrxavpok on 20/06/2021.
//

#pragma once

#include "engine/vulkan/includes.h"
#include "engine/vulkan/VulkanDriver.h"
#include "engine/render/RenderContext.h"
#include "engine/render/resources/Texture.h"
#include "core/utils/UUID.h"
#include "engine/render/RenderPassData.h"
#include <any>

namespace Carrot::Render {

    class FrameResource;

    class CompiledPass;
    class Graph;

    using CompiledPassCallback = std::function<void(CompiledPass&, const Render::Context&, vk::CommandBuffer&)>;
    using SwapchainRecreationCallback = std::function<void(const CompiledPass&)>;

    struct ImageTransition {
        Carrot::UUID resourceID;
        vk::ImageLayout from;
        vk::ImageLayout to;
        vk::ImageAspectFlags aspect;

        explicit ImageTransition(Carrot::UUID resourceID, vk::ImageLayout from, vk::ImageLayout to, vk::ImageAspectFlags aspect): resourceID(resourceID), from(from), to(to), aspect(aspect) {}
    };

    class CompiledPass: public SwapchainAware {
    public:
        using InitCallback = std::function<std::vector<vk::UniqueFramebuffer>(CompiledPass&, vk::Extent2D&)>;

        /// Constructor for rasterized passes
        explicit CompiledPass(
                Graph& graph,
                std::string name,
                vk::UniqueRenderPass&& renderPass,
                const std::vector<vk::ClearValue>& clearValues,
                const CompiledPassCallback& renderingCode,
                std::vector<ImageTransition>&& prePassTransitions,
                InitCallback initCallback,
                SwapchainRecreationCallback swapchainCallback,
                bool prerecordable
        );

        /// Constructor for non-rasterized passes
        explicit CompiledPass(
                Graph& graph,
                std::string name,
                const CompiledPassCallback& renderingCode,
                std::vector<ImageTransition>&& prePassTransitions,
                InitCallback initCallback,
                SwapchainRecreationCallback swapchainCallback,
                bool prerecordable
        );

    public:
        void execute(const Render::Context& data, vk::CommandBuffer& cmds);

        const vk::RenderPass& getRenderPass() const {
            assert(rasterized); // Only rasterized passes have a render pass
            return *renderPass;
        }

        VulkanDriver& getVulkanDriver() const;

        Graph& getGraph() { return graph; }

        const Graph& getGraph() const { return graph; }

        std::string_view getName() const { return name; }

        void refresh();

    public:
        void onSwapchainImageCountChange(size_t newCount) override;

        void onSwapchainSizeChange(int newWidth, int newHeight) override;

    private:
        void createFramebuffers();
        void createCommandPool();
        void createCommandBuffers(const Render::Context& renderContext);
        void recordCommands(const Render::Context& renderContext);
        void performTransitions(const Render::Context& renderContext, vk::CommandBuffer& cmds);

    private:
        Graph& graph;
        bool rasterized = true;
        bool prerecordable = false;
        std::vector<bool> needsRecord; // do pre-recorded buffers need to be re-recorded?
        std::vector<vk::UniqueFramebuffer> framebuffers;
        vk::UniqueRenderPass renderPass;
        std::vector<vk::ClearValue> clearValues;
        std::vector<ImageTransition> prePassTransitions;
        CompiledPassCallback renderingCode;
        InitCallback initCallback;
        SwapchainRecreationCallback swapchainRecreationCallback;
        std::string name;
        vk::Extent2D renderSize;

    private: // Pre-recording
        vk::UniqueCommandPool commandPool;
        std::vector<vk::CommandBuffer> commandBuffers;
    };

    template<typename> class Pass;

    template<typename Data>
    using PostCompileCallback = std::function<void(CompiledPass&, Data&)>;

    class PassBase {
    public:
        bool rasterized = true;
        bool prerecordable = false;

        explicit PassBase(VulkanDriver& driver, std::string name): driver(driver), name(std::move(name)) {}

        virtual ~PassBase() = default;

    public:
        void addInput(FrameResource& resource, vk::ImageLayout expectedLayout, vk::ImageAspectFlags aspect);
        void addOutput(FrameResource& resource, vk::AttachmentLoadOp loadOp, vk::ClearValue clearValue, vk::ImageAspectFlags aspect, vk::ImageLayout layout, bool isCreatedInThisPass);
        void present(FrameResource& toPresent);

    public:
        std::unique_ptr<CompiledPass> compile(Carrot::VulkanDriver& driver, Graph& graph);

    protected:
        struct Input {
            FrameResource resource;
            vk::ImageLayout expectedLayout = vk::ImageLayout::eUndefined;
            vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor;

            Input(const FrameResource& resource, vk::ImageLayout expectedLayout, vk::ImageAspectFlags aspect): resource(resource), expectedLayout(expectedLayout), aspect(aspect) {}
        };

        struct Output {
            FrameResource resource;
            vk::AttachmentLoadOp loadOp;
            vk::ClearValue clearValue;
            vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor;
            bool isCreatedInThisPass = false;

            Output(const FrameResource& resource, vk::AttachmentLoadOp loadOp, vk::ClearValue clearValue, vk::ImageAspectFlags aspect): resource(resource), loadOp(loadOp), clearValue(clearValue), aspect(aspect) {}
        };

        Carrot::VulkanDriver& driver;
        std::list<Input> inputs;
        std::list<Output> outputs;
        std::unordered_map<Carrot::UUID, vk::ImageLayout> finalLayouts;

    protected:
        std::string name;

        virtual CompiledPassCallback generateCallback() = 0;
        virtual SwapchainRecreationCallback generateSwapchainCallback() = 0;
        virtual void postCompile(CompiledPass& pass) = 0;

        friend class GraphBuilder;
    };

    template<typename Data>
    using ExecutePassCallback = std::function<void(const CompiledPass&, const Render::Context&, Data&, vk::CommandBuffer&)>;

    template<typename Data>
    using PassConditionCallback = std::function<bool(const CompiledPass&, const Render::Context&, const Data&)>;

    template<typename Data>
    using SwapchainRecreationCallbackWithData = std::function<void(const CompiledPass&, const Data&)>;

    template<typename Data>
    class Pass: public PassBase {
    public:

        explicit Pass(VulkanDriver& driver,
                      const std::string& name,
                      const ExecutePassCallback<Data>& callback,
                      const PostCompileCallback<Data>& postCompileCallback):
                      PassBase(driver, name),
                      executeCallback(callback),
                      postCompileCallback(postCompileCallback) {};

        CompiledPassCallback generateCallback() override {
            return [executeCallback = executeCallback, data = data, condition = condition, lastConditionValue = false](CompiledPass& pass, const Render::Context& frameData, vk::CommandBuffer& cmds)
            mutable {
                bool conditionValue = condition(pass, frameData, data);
                if(conditionValue != lastConditionValue) {
                    pass.refresh();
                }
                if(conditionValue) {
                    executeCallback(pass, frameData, data, cmds);
                }
                lastConditionValue = conditionValue;
            };
        }

        SwapchainRecreationCallback generateSwapchainCallback() override {
            return [swapchainCallback = swapchainRecreationCallback, data = data](const CompiledPass& pass) {
                return swapchainCallback(pass, data);
            };
        }

        void postCompile(CompiledPass& pass) override {
            postCompileCallback(pass, data);
        }

        const Data& getData() const { return data; }

        void setCondition(const PassConditionCallback<Data>& condition) {
            this->condition = condition;
        }

        void setSwapchainRecreation(const SwapchainRecreationCallbackWithData<Data>& swapchainRecreationCallback) {
            this->swapchainRecreationCallback = swapchainRecreationCallback;
        }

    private:
        ExecutePassCallback<Data> executeCallback;
        PostCompileCallback<Data> postCompileCallback;
        SwapchainRecreationCallbackWithData<Data> swapchainRecreationCallback = [](const CompiledPass&, const Data&) {};
        PassConditionCallback<Data> condition = [](const CompiledPass&, const Render::Context&, const Data&) { return true; };
        Data data;

        friend class GraphBuilder;
    };

}
