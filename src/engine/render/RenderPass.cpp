//
// Created by jglrxavpok on 20/06/2021.
//

#include "RenderPass.h"

#include <utility>
#include "RenderGraph.h"
#include "engine/utils/Assert.h"

Carrot::Render::CompiledPass::CompiledPass(
        Carrot::VulkanDriver& driver,
        Carrot::Render::Graph& graph,
        std::vector<vk::UniqueFramebuffer>&& framebuffers,
        vk::UniqueRenderPass&& renderPass,
        const std::vector<vk::ClearValue>& clearValues,
        const CompiledPassCallback& renderingCode,
        std::vector<ImageTransition>&& prePassTransitions
        ):
        driver(driver),
        graph(graph),
        framebuffers(std::move(framebuffers)),
        renderPass(std::move(renderPass)),
        clearValues(clearValues),
        renderingCode(renderingCode),
        prePassTransitions(prePassTransitions)
        {}

void Carrot::Render::CompiledPass::execute(const FrameData& data, vk::CommandBuffer& cmds) const {
    for(const auto& transition : prePassTransitions) {
        auto& tex = graph.getTexture(transition.resourceID, data.frameIndex);
        tex.assumeLayout(transition.from);
        tex.transitionInline(cmds, transition.to);
    }

    cmds.beginRenderPass(vk::RenderPassBeginInfo {
        .renderPass = *renderPass,
        .framebuffer = *framebuffers[data.frameIndex],
        .renderArea = {
                .offset = vk::Offset2D{0, 0},
                .extent = driver.getSwapchainExtent()
        },
        .clearValueCount = static_cast<uint32_t>(clearValues.size()),
        .pClearValues = clearValues.data(),
    }, vk::SubpassContents::eInline);

    driver.updateViewportAndScissor(cmds);
    renderingCode(*this, data, cmds);

    cmds.endRenderPass();
}

void Carrot::Render::PassBase::addInput(const Carrot::Render::FrameResource& resource, vk::ImageLayout expectedLayout) {
    inputs.emplace_back(&resource, expectedLayout);
}

void Carrot::Render::PassBase::addOutput(const Carrot::Render::FrameResource& resource, vk::AttachmentLoadOp loadOp,
                                         vk::ClearValue clearValue) {
    outputs.emplace_back(&resource, loadOp, clearValue);
    finalLayouts[resource.id] = vk::ImageLayout::eColorAttachmentOptimal;
}

void Carrot::Render::PassBase::present(const FrameResource& toPresent) {
    runtimeAssert(finalLayouts.find(toPresent.id) != finalLayouts.end(), "Presentable resource must be in outputs of this render pass!");
    finalLayouts[toPresent.id] = vk::ImageLayout::ePresentSrcKHR;
}

std::unique_ptr<Carrot::Render::CompiledPass> Carrot::Render::PassBase::compile(Carrot::VulkanDriver& driver, Carrot::Render::Graph& graph) {
    // TODO: input and output can be the same attachment (subpasses)
    std::vector<std::vector<vk::ImageView>> attachmentImageViews(driver.getSwapchainImageCount()); // [frameIndex][attachmentIndex], one per swapchain image
    std::vector<vk::AttachmentDescription> attachments;
    std::vector<vk::AttachmentReference> inputAttachments;
    std::vector<vk::AttachmentReference> outputAttachments;
    vk::AttachmentReference* depthAttachmentRef = nullptr;

    std::vector<ImageTransition> prePassTransitions;
    for(const auto& input : inputs) {
        auto initialLayout = graph.getFinalLayouts()[input.resource->parentID]; // input is not the resource itself, but a new instance returned by read()
        assert(initialLayout != vk::ImageLayout::eUndefined);
        if(initialLayout != input.expectedLayout) {
            prePassTransitions.emplace_back(input.resource->parentID, initialLayout, input.expectedLayout);
        }
    }

    std::vector<vk::ClearValue> clearValues;
    for(const auto& output : outputs) {

        auto initialLayout = vk::ImageLayout::eUndefined;
        auto finalLayout = finalLayouts[output.resource->id];
        graph.getFinalLayouts()[output.resource->id] = finalLayout;
        assert(finalLayout != vk::ImageLayout::eUndefined);

        if(output.resource->id != output.resource->parentID) {
            initialLayout = graph.getFinalLayouts()[output.resource->parentID];
        }

        vk::AttachmentDescription attachment {
            .format = output.resource->format,
            .samples = vk::SampleCountFlagBits::e1,

            .loadOp = output.loadOp,
            .storeOp = vk::AttachmentStoreOp::eStore,

            .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
            .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,

            .initialLayout = initialLayout,
            .finalLayout = finalLayout,
        };
        attachments.push_back(attachment);

        outputAttachments.push_back(vk::AttachmentReference {
            .attachment = static_cast<uint32_t>(attachments.size()-1),
            .layout = vk::ImageLayout::eColorAttachmentOptimal, // TODO: customisable
        });

        if(output.loadOp == vk::AttachmentLoadOp::eClear) {
            clearValues.push_back(output.clearValue);
        }
    }


    vk::SubpassDescription mainSubpass {
            .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
            .inputAttachmentCount = static_cast<uint32_t>(inputAttachments.size()),

            .colorAttachmentCount = static_cast<uint32_t>(outputAttachments.size()),
            // index in this array is used by `layout(location = 0)` inside shaders
            .pColorAttachments = outputAttachments.data(),
            .pDepthStencilAttachment = depthAttachmentRef,

            .preserveAttachmentCount = 0,
    };

    vk::SubpassDependency mainDependency {
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = 0,

            .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                            vk::PipelineStageFlagBits::eEarlyFragmentTests,
            // TODO: .srcAccessMask = 0,

            .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                            vk::PipelineStageFlagBits::eEarlyFragmentTests,
            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite |
                             vk::AccessFlagBits::eDepthStencilAttachmentWrite,
    };

    vk::UniqueRenderPass renderPass = driver.getLogicalDevice().createRenderPassUnique(vk::RenderPassCreateInfo {
            .attachmentCount = static_cast<std::uint32_t>(attachments.size()),
            .pAttachments = attachments.data(),

            // TODO: merge compatible render passes into subpasses
            .subpassCount = 1,
            .pSubpasses = &mainSubpass,
            .dependencyCount = 1,
            .pDependencies = &mainDependency,
    });

    std::vector<vk::UniqueFramebuffer> framebuffers(driver.getSwapchainImageCount());
    for (int i = 0; i < driver.getSwapchainImageCount(); ++i) {
        auto& frameImageViews = attachmentImageViews[i];

        // TODO: inputs
        for(const auto& output : outputs) {
            auto& texture = graph.getOrCreateTexture(driver, *output.resource, i);
            frameImageViews.push_back(texture.getView());
        }


        vk::FramebufferCreateInfo framebufferInfo {
                .renderPass = *renderPass,
                .attachmentCount = static_cast<uint32_t>(frameImageViews.size()),
                .pAttachments = frameImageViews.data(),
                .width = driver.getSwapchainExtent().width,
                .height = driver.getSwapchainExtent().height,
                .layers = 1,
        };

        framebuffers[i] = std::move(driver.getLogicalDevice().createFramebufferUnique(framebufferInfo, driver.getAllocationCallbacks()));
    }

    return make_unique<CompiledPass>(driver, graph, std::move(framebuffers), std::move(renderPass), clearValues, generateCallback(), std::move(prePassTransitions));
}
