#include "shadowmap_render.h"
#include <cstdlib>

/// RESOURCE ALLOCATION

void SimpleShadowmapRender::AllocateTerrainResources()
{
  terrainHmap = m_context->createImage(etna::Image::CreateInfo
  {
    .extent     = vk::Extent3D{LANDMESH_DIM, LANDMESH_DIM, 1},
    .name       = "terrain_hmap",
    .format     = vk::Format::eR32Sfloat,
    .imageUsage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled
  });
}

void SimpleShadowmapRender::DeallocateTerrainResources()
{
  terrainHmap.reset();
}


/// PIPELINES CREATION

void SimpleShadowmapRender::LoadTerrainShaders()
{
  etna::create_program("generate_hmap", {VK_GRAPHICS_BASIC_ROOT"/resources/shaders/generate_hmap.comp.spv"});

  etna::create_program("terrain_simple_forward",
    {
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/simple.frag.spv", 
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.vert.spv",
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.tesc.spv",
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.tese.spv"
    });
  etna::create_program("terrain_shadow_forward",
    {
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/simple_shadow.frag.spv", 
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.vert.spv",
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.tesc.spv",
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.tese.spv"
    });
  etna::create_program("terrain_vsm_forward",
    {
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/vsm_shadow.frag.spv", 
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.vert.spv",
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.tesc.spv",
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.tese.spv"
    });
  etna::create_program("terrain_pcf_forward",
    {
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/pcf_shadow.frag.spv", 
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.vert.spv",
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.tesc.spv",
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.tese.spv"
    });

  etna::create_program("terrain_gpass",
    {
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/simple_gpass.frag.spv", 
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.vert.spv",
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.tesc.spv",
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.tese.spv"
    });

  etna::create_program("terrain_simple_shadow", 
    {
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.vert.spv",
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.tesc.spv",
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.tese.spv"
    });

  etna::create_program("terrain_vsm_shadow",
    {
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/vsm_shadowmap.frag.spv", 
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.vert.spv",
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.tesc.spv",
      VK_GRAPHICS_BASIC_ROOT"/resources/shaders/terrain.tese.spv"
    });
}

void SimpleShadowmapRender::SetupTerrainPipelines()
{
  auto& pipelineManager = etna::get_context().getPipelineManager();
  m_hmapGeneratePipeline = pipelineManager.createComputePipeline("generate_hmap", {});
}


/// TECHNIQUE CHOICE

const char *SimpleShadowmapRender::CurrentTerrainForwardProgramName()
{
  switch (currentShadowmapTechnique)
  {
  case eShTechNone:
    return "terrain_simple_forward";
    break;
  case eSimple:
    return "terrain_shadow_forward";
    break;
  case ePcf:
    return "terrain_pcf_forward";
    break;
  case eVsm:
    return "terrain_vsm_forward";
    break;
  }
}

float4x4 SimpleShadowmapRender::GetCurrentTerrainQuadTransform()
{
  const float4x4 terrain_base_transform = translate4x4(float3(-50.f, -5.f, -50.f)) *
                                          scale4x4(float3(100.f, 1.f, 100.f));

  return translate4x4(float3(0.f, terrainMinMaxHeight.x, 0.f)) *
         terrain_base_transform * 
         scale4x4(float3(1.f, (terrainMinMaxHeight.y - terrainMinMaxHeight.x), 1.f));
}


/// COMMAND BUFFER FILLING

void SimpleShadowmapRender::RecordHmapGenerationCommands(VkCommandBuffer a_cmdBuff)
{
  etna::set_state(a_cmdBuff, terrainHmap.get(), 
    vk::PipelineStageFlagBits2::eComputeShader,
    vk::AccessFlags2(vk::AccessFlagBits2::eShaderWrite),
    vk::ImageLayout::eGeneral,
    vk::ImageAspectFlagBits::eColor);
  etna::flush_barriers(a_cmdBuff);

  auto programInfo = etna::get_shader_program("generate_hmap");
  auto set = etna::create_descriptor_set(programInfo.getDescriptorLayoutId(0), a_cmdBuff, 
    { etna::Binding{0, terrainHmap.genBinding(defaultSampler.get(), vk::ImageLayout::eGeneral)} });
  VkDescriptorSet vkSet = set.getVkSet();

  vkCmdBindPipeline(a_cmdBuff, VK_PIPELINE_BIND_POINT_COMPUTE, m_hmapGeneratePipeline.getVkPipeline());
  vkCmdBindDescriptorSets(a_cmdBuff, VK_PIPELINE_BIND_POINT_COMPUTE,
    m_hmapGeneratePipeline.getVkPipelineLayout(), 0, 1, &vkSet, 0, VK_NULL_HANDLE);

  uint32_t wgDim = (LANDMESH_DIM - 1) / HMAP_WORK_GROUP_DIM + 1;
  vkCmdDispatch(a_cmdBuff, wgDim, wgDim, 1);

  etna::set_state(a_cmdBuff, terrainHmap.get(), vk::PipelineStageFlagBits2::eAllGraphics,
    vk::AccessFlags2(vk::AccessFlagBits2::eShaderSampledRead), vk::ImageLayout::eShaderReadOnlyOptimal,
    vk::ImageAspectFlagBits::eColor);
  etna::flush_barriers(a_cmdBuff);
}

void SimpleShadowmapRender::RecordDrawTerrainForwardCommands(VkCommandBuffer a_cmdBuff, const float4x4& a_wvp)
{
  auto programInfo = etna::get_shader_program(CurrentTerrainForwardProgramName());

  auto bindings = CurrentRTBindings();
  bindings[0].push_back(etna::Binding{ 8, terrainHmap.genBinding(defaultSampler.get(), vk::ImageLayout::eShaderReadOnlyOptimal) });

  // @TODO(PKiyashko): this happens in a few places and should be pulled out to some utils
  std::vector<etna::DescriptorSet> sets(bindings.size());
  std::vector<VkDescriptorSet> vkSets(bindings.size());
  for (size_t i = 0; i < bindings.size(); ++i)
  {
    if (bindings[i].size() == 0)
      continue;
    auto set = etna::create_descriptor_set(programInfo.getDescriptorLayoutId(i), a_cmdBuff, std::move(bindings[i]));
    vkSets[i] = set.getVkSet();
    sets[i] = std::move(set);
  }

  vkCmdBindPipeline(a_cmdBuff, VK_PIPELINE_BIND_POINT_GRAPHICS, m_terrainForwardPipeline.getVkPipeline());
  vkCmdBindDescriptorSets(a_cmdBuff, VK_PIPELINE_BIND_POINT_GRAPHICS,
    m_terrainForwardPipeline.getVkPipelineLayout(), 0, vkSets.size(), vkSets.data(), 0, VK_NULL_HANDLE);

  pushConst2M.projView = a_wvp;
  pushConst2M.model = GetCurrentTerrainQuadTransform();
  vkCmdPushConstants(a_cmdBuff, m_terrainGpassPipeline.getVkPipelineLayout(),
    VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, 0, sizeof(pushConst2M), &pushConst2M);

  vkCmdDraw(a_cmdBuff, 4, 1, 0, 0);
}

void SimpleShadowmapRender::RecordDrawTerrainGpassCommands(VkCommandBuffer a_cmdBuff, const float4x4& a_wvp)
{
  auto programInfo = etna::get_shader_program("terrain_gpass");
  auto set = etna::create_descriptor_set(programInfo.getDescriptorLayoutId(0), a_cmdBuff,
    { etna::Binding{8, terrainHmap.genBinding(defaultSampler.get(), vk::ImageLayout::eShaderReadOnlyOptimal)} });
  VkDescriptorSet vkSet = set.getVkSet();

  vkCmdBindPipeline(a_cmdBuff, VK_PIPELINE_BIND_POINT_GRAPHICS, m_terrainGpassPipeline.getVkPipeline());
  vkCmdBindDescriptorSets(a_cmdBuff, VK_PIPELINE_BIND_POINT_GRAPHICS,
    m_terrainGpassPipeline.getVkPipelineLayout(), 0, 1, &vkSet, 0, VK_NULL_HANDLE);

  pushConst2M.projView = a_wvp;
  pushConst2M.model = GetCurrentTerrainQuadTransform();
  vkCmdPushConstants(a_cmdBuff, m_terrainGpassPipeline.getVkPipelineLayout(),
    VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, 0, sizeof(pushConst2M), &pushConst2M);

  vkCmdDraw(a_cmdBuff, 4, 1, 0, 0);
}

void SimpleShadowmapRender::RecordDrawTerrainToShadowmapCommands(VkCommandBuffer a_cmdBuff, const float4x4 &a_wvp)
{
  const char *programName = nullptr;
  etna::GraphicsPipeline *pipeline = nullptr;
  switch (currentShadowmapTechnique)
  {
  case eSimple:
  case ePcf:
    programName = "terrain_simple_shadow";
    pipeline    = &m_terrainSimpleShadowPipeline;
    break;
  case eVsm:
    programName = "terrain_vsm_shadow";
    pipeline    = &m_terrainVsmPipeline;
    break;
  }

  auto programInfo = etna::get_shader_program(programName);
  auto set = etna::create_descriptor_set(programInfo.getDescriptorLayoutId(0), a_cmdBuff,
    { etna::Binding{8, terrainHmap.genBinding(defaultSampler.get(), vk::ImageLayout::eShaderReadOnlyOptimal)} });
  VkDescriptorSet vkSet = set.getVkSet();

  vkCmdBindPipeline(a_cmdBuff, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->getVkPipeline());
  vkCmdBindDescriptorSets(a_cmdBuff, VK_PIPELINE_BIND_POINT_GRAPHICS,
    pipeline->getVkPipelineLayout(), 0, 1, &vkSet, 0, VK_NULL_HANDLE);

  pushConst2M.projView = a_wvp;
  pushConst2M.model = GetCurrentTerrainQuadTransform();
  vkCmdPushConstants(a_cmdBuff, pipeline->getVkPipelineLayout(),
    VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, 0, sizeof(pushConst2M), &pushConst2M);

  vkCmdDraw(a_cmdBuff, 4, 1, 0, 0);
}
