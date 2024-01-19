#include "bbox_renderer.h"

#include "etna/Etna.hpp"
#include "etna/RenderTargetStates.hpp"

const LiteMath::float4 BboxRenderer::s_boxVert[8] =
{
  {0.f, 0.f, 0.f, 1.f},
  {1.f, 0.f, 0.f, 1.f},
  {0.f, 1.f, 0.f, 1.f},
  {0.f, 0.f, 1.f, 1.f},
  {1.f, 1.f, 0.f, 1.f},
  {1.f, 0.f, 1.f, 1.f},
  {0.f, 1.f, 1.f, 1.f},
  {1.f, 1.f, 1.f, 1.f}
};

const uint32_t BboxRenderer::s_boxInd[12*2] =
{
  0, 1,
  0, 2,
  1, 4,
  2, 4,
  3, 5,
  3, 6,
  5, 7,
  6, 7,
  0, 3,
  1, 5,
  2, 6,
  4, 7
};

void BboxRenderer::Create(const char *vspath, const char *fspath, CreateInfo info)
{
  m_drawInstanced = info.drawInstanced;
  m_extent        = info.extent;

  m_context = &etna::get_context();

  m_programId = etna::create_program("bbox_wireframe", {fspath, vspath});

  auto &pipelineManager = etna::get_context().getPipelineManager();
  m_pipeline = pipelineManager.createGraphicsPipeline("bbox_wireframe",
    {
      .vertexShaderInput =
      {
        .bindings = {{{sizeof(s_boxVert[0]), {{vk::Format::eR32G32B32A32Sfloat, 0}}}}}
      },
      .inputAssemblyConfig = 
      {
        .topology = vk::PrimitiveTopology::eLineList
      },
      .rasterizationConfig =
      {
        .polygonMode = vk::PolygonMode::eLine,
        .cullMode = vk::CullModeFlagBits::eNone,
        .lineWidth = 1.
      },
      .fragmentShaderOutput =
      {
        .colorAttachmentFormats = {info.colorFormat},
        .depthAttachmentFormat  = {info.depthFormat}
      }
    });

  // @TODO: somehow automate eTransferDst for buffers that will be updated with copy
  m_vertexBuffer = m_context->createBuffer(
    {
      .size = sizeof(s_boxVert),
      .bufferUsage = vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
      .memoryUsage = VMA_MEMORY_USAGE_GPU_ONLY,
      .name = "bbox_wireframe_vertices"
    });
  m_indexBuffer = m_context->createBuffer(
    {
      .size = sizeof(s_boxInd),
      .bufferUsage = vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
      .memoryUsage = VMA_MEMORY_USAGE_GPU_ONLY,
      .name = "bbox_wireframe_indices"
    });

  m_vertexBuffer.updateOnce((std::byte *)s_boxVert, sizeof(s_boxVert));
  m_indexBuffer.updateOnce((std::byte *)s_boxInd, sizeof(s_boxInd));
}

void BboxRenderer::SetBoxes(const LiteMath::Box4f *boxes, size_t cnt)
{
  bool needToResize = cnt != m_instances.size();
  if (needToResize)
    m_instances.resize(cnt);
  for (size_t i = 0; i < m_instances.size(); i++)
  {
    LiteMath::float4x4 &inst   = m_instances[i];
    const LiteMath::Box4f &box = boxes[i];

    inst.identity();

    // Translate
    inst[0][3] = box.boxMin.x - s_boxEps;
    inst[1][3] = box.boxMin.y - s_boxEps;
    inst[2][3] = box.boxMin.z - s_boxEps;
    inst[3][3] = 1.f;

    // Scale
    inst[0][0] = box.boxMax.x - box.boxMin.x + 2.f * s_boxEps;
    inst[1][1] = box.boxMax.y - box.boxMin.y + 2.f*s_boxEps;
    inst[2][2] = box.boxMax.z - box.boxMin.z + 2.f*s_boxEps;
  }

  if (m_drawInstanced)
  {
    size_t bufSize = sizeof(LiteMath::float4x4) * m_instances.size();
    
    if (needToResize)
    {
      m_boxesInstBuffer = m_context->createBuffer(
        {
          .size = bufSize,
          .bufferUsage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
          .memoryUsage = VMA_MEMORY_USAGE_GPU_ONLY,
          .name = "bbox_wireframe_instances"
        });
      m_boxesInstBuffer.createStagingBuffer(etna::StagingBufferType::eBothWays);
      m_boxesInstBuffer.update((std::byte *)m_instances.data(), bufSize);
    }
    else 
    {
      m_boxesInstBuffer.update((std::byte *)m_instances.data(), bufSize);
    }
  }
}

void BboxRenderer::DrawCmd(vk::CommandBuffer cmdBuff, 
  vk::Image targetImage, vk::ImageView targetImageView, 
  const etna::Image &depthImage, const LiteMath::float4x4 &mViewProj)
{
  etna::RenderTargetState renderTargets(cmdBuff,
    {0, 0, m_extent.width, m_extent.height},
    {{targetImage, targetImageView, false}}, {depthImage, false});

  cmdBuff.bindPipeline(vk::PipelineBindPoint::eGraphics, m_pipeline.getVkPipeline());

  cmdBuff.bindVertexBuffers(0, {m_vertexBuffer.get()}, (vk::DeviceSize)0);
  cmdBuff.bindIndexBuffer(m_indexBuffer.get(), 0, vk::IndexType::eUint32);

  if (m_drawInstanced)
  {
    auto programInfo = etna::get_shader_program(m_programId);
    auto set = etna::create_descriptor_set(programInfo.getDescriptorLayoutId(0), cmdBuff,
      {
        etna::Binding{0, m_boxesInstBuffer.genBinding()}
      });

    vk::DescriptorSet vkSet = set.getVkSet();

    cmdBuff.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_pipeline.getVkPipelineLayout(), 0, {vkSet}, {});

    cmdBuff.pushConstants(m_pipeline.getVkPipelineLayout(), 
      vk::ShaderStageFlagBits::eVertex, 0, sizeof(LiteMath::float4x4), &mViewProj);
    cmdBuff.drawIndexed(sizeof(s_boxInd)/sizeof(s_boxInd[0]), m_instances.size(), 0, 0, 0);
  }
  else
  {
    for (const LiteMath::float4x4 &inst : m_instances)
    {
      PushConst2M pushConst = {};
      pushConst.mInst       = inst;
      memcpy(&pushConst.mViewProj, &mViewProj, sizeof(LiteMath::float4x4));

      cmdBuff.pushConstants(m_pipeline.getVkPipelineLayout(), 
        vk::ShaderStageFlagBits::eVertex, 0, sizeof(pushConst), &pushConst);
      cmdBuff.drawIndexed(sizeof(s_boxInd)/sizeof(s_boxInd[0]), 1, 0, 0, 0);
    }
  }
}