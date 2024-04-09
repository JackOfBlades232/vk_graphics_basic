#include "shadowmap_render.h"


/// RESOURCE ALLOCATION

void SimpleShadowmapRender::AllocateShadowmapResources()
{
  const vk::Extent3D shadowmapExtent{ 2048, 2048, 1 };

  shadowMap = m_context->createImage(etna::Image::CreateInfo
  {
    .extent = shadowmapExtent,
    .name = "shadow_map",
    .format = vk::Format::eD16Unorm,
    .imageUsage = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled
  });
  vsmMomentMap = m_context->createImage(etna::Image::CreateInfo
  {
    .extent = shadowmapExtent,
    .name = "vsm_moment_map",
    .format = vk::Format::eR32G32Sfloat,
    .imageUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled
  });
  vsmSmoothMomentMap = m_context->createImage(etna::Image::CreateInfo
  {
    .extent = shadowmapExtent,
    .name = "vsm_smooth_moment_map",
    .format = vk::Format::eR32G32Sfloat,
    .imageUsage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled
  });
}

void SimpleShadowmapRender::DeallocateShadowmapResources()
{
  shadowMap.reset();
  vsmMomentMap.reset();
}


/// PIPELINES CREATION

void SimpleShadowmapRender::LoadShadowmapShaders()
{
  etna::create_program("vsm_material",
    {VK_GRAPHICS_BASIC_ROOT"/resources/shaders/vsm_shadow.frag.spv", VK_GRAPHICS_BASIC_ROOT"/resources/shaders/simple.vert.spv"});
  etna::create_program("vsm_shadow",
    {VK_GRAPHICS_BASIC_ROOT"/resources/shaders/vsm_shadowmap.frag.spv", VK_GRAPHICS_BASIC_ROOT"/resources/shaders/simple.vert.spv"});
  etna::create_program("vsm_filtering", {VK_GRAPHICS_BASIC_ROOT"/resources/shaders/vsm_filter.comp.spv"});
  etna::create_program("pcf_material",
    {VK_GRAPHICS_BASIC_ROOT"/resources/shaders/pcf_shadow.frag.spv", VK_GRAPHICS_BASIC_ROOT"/resources/shaders/simple.vert.spv"});
}

void SimpleShadowmapRender::SetupShadowmapPipelines()
{
  etna::VertexShaderInputDescription sceneVertexInputDesc{
      .bindings = {etna::VertexShaderInputDescription::Binding{
          .byteStreamDescription = m_pScnMgr->GetVertexStreamDescription()
        }}
    };

  auto& pipelineManager = etna::get_context().getPipelineManager();
  m_simpleShadowPipeline = pipelineManager.createGraphicsPipeline("simple_shadow",
    {
      .vertexShaderInput = sceneVertexInputDesc,
      .fragmentShaderOutput =
        {
          .depthAttachmentFormat = vk::Format::eD16Unorm
        }
    });

  m_vsmShadowPipeline = pipelineManager.createGraphicsPipeline("vsm_shadow",
    {
      .vertexShaderInput = sceneVertexInputDesc,
      .fragmentShaderOutput =
        {
          .colorAttachmentFormats = {vk::Format::eR32G32Sfloat},
          .depthAttachmentFormat = vk::Format::eD16Unorm
        }
    });
  m_vsmFilteringPipeline = pipelineManager.createComputePipeline("vsm_filtering", {});
}


/// TECHNIQUE CHOICE

std::vector<etna::RenderTargetState::AttachmentParams> SimpleShadowmapRender::CurrentShadowColorAttachments()
{
  switch (currentShadowmapTechnique)
  {
  case eSimple:
  case ePcf:
    return {};
  case eVsm:
    return {{.image = vsmMomentMap.get(), .view = vsmMomentMap.getView({})}};
  }
}

etna::GraphicsPipeline &SimpleShadowmapRender::CurrentShadowmapPipeline()
{
  switch (currentShadowmapTechnique)
  {
  case eSimple:
  case ePcf:
    return m_simpleShadowPipeline;
  case eVsm:
    return m_vsmShadowPipeline;
  }
}

const char *SimpleShadowmapRender::CurrentShadowForwardProgramOverride()
{
  switch (currentShadowmapTechnique)
  {
  case eSimple:
    return nullptr;
  case ePcf:
    return "pcf_material";
  case eVsm:
    return "vsm_material";
  }
}

etna::DescriptorSet SimpleShadowmapRender::CreateCurrentForwardDSet(VkCommandBuffer a_cmdBuff)
{
  switch (currentShadowmapTechnique)
  {
  case eSimple: 
  case ePcf:
  {
    auto materialInfo = etna::get_shader_program("simple_material");
    return std::move(etna::create_descriptor_set(materialInfo.getDescriptorLayoutId(0), a_cmdBuff, {
      etna::Binding{ 0, constants.genBinding() }, 
      etna::Binding{ 1, shadowMap.genBinding(defaultSampler.get(), vk::ImageLayout::eShaderReadOnlyOptimal) } 
    }));
  }
  case eVsm:
  {
    auto materialInfo = etna::get_shader_program("vsm_material");
    return std::move(etna::create_descriptor_set(materialInfo.getDescriptorLayoutId(0), a_cmdBuff, {
      etna::Binding{ 0, constants.genBinding() }, 
      etna::Binding{ 1, vsmSmoothMomentMap.genBinding(defaultSampler.get(), vk::ImageLayout::eShaderReadOnlyOptimal) }
    }));
  }
  }
}


/// COMMAND BUFFER FILLING

void SimpleShadowmapRender::RecordShadowmapProcessingCommands(VkCommandBuffer a_cmdBuff)
{
  if (currentShadowmapTechnique == eVsm)
  {
    // Filter the shadowmap
    etna::set_state(a_cmdBuff, vsmMomentMap.get(), 
      vk::PipelineStageFlagBits2::eComputeShader,
      vk::AccessFlags2(vk::AccessFlagBits2::eShaderSampledRead),
      vk::ImageLayout::eShaderReadOnlyOptimal,
      vk::ImageAspectFlagBits::eColor);
    etna::set_state(a_cmdBuff, vsmSmoothMomentMap.get(), 
      vk::PipelineStageFlagBits2::eComputeShader,
      vk::AccessFlags2(vk::AccessFlagBits2::eShaderWrite),
      vk::ImageLayout::eGeneral,
      vk::ImageAspectFlagBits::eColor);
    etna::flush_barriers(a_cmdBuff);

    auto programInfo = etna::get_shader_program("vsm_filtering");
    auto set = etna::create_descriptor_set(programInfo.getDescriptorLayoutId(0), a_cmdBuff, { 
      etna::Binding {0, vsmMomentMap.genBinding(defaultSampler.get(), vk::ImageLayout::eShaderReadOnlyOptimal)},
      etna::Binding {1, vsmSmoothMomentMap.genBinding(defaultSampler.get(), vk::ImageLayout::eGeneral)}
    });

    VkDescriptorSet vkSet = set.getVkSet();

    uint32_t wgDim = (2048 - 1) / WORK_GROUP_DIM + 1;

    vkCmdBindPipeline(a_cmdBuff, VK_PIPELINE_BIND_POINT_COMPUTE, m_vsmFilteringPipeline.getVkPipeline());
    vkCmdBindDescriptorSets(a_cmdBuff, VK_PIPELINE_BIND_POINT_COMPUTE,
      m_vsmFilteringPipeline.getVkPipelineLayout(), 0, 1, &vkSet, 0, VK_NULL_HANDLE);
    vkCmdDispatch(a_cmdBuff, wgDim, wgDim, 1);

    etna::set_state(a_cmdBuff, vsmSmoothMomentMap.get(), vk::PipelineStageFlagBits2::eAllGraphics,
      vk::AccessFlags2(vk::AccessFlagBits2::eShaderSampledRead), vk::ImageLayout::eShaderReadOnlyOptimal,
      vk::ImageAspectFlagBits::eColor);
    etna::flush_barriers(a_cmdBuff);
  }
}