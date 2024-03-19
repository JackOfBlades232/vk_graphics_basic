#include "quad2d_render.h"
#include "utils/input_definitions.h"

#include <etna/Etna.hpp>

void Quad2D_Render::ProcessInput(const AppInput &input)
{
  // recreate pipeline to reload shaders
  if(input.keyPressed[GLFW_KEY_B])
  {
#ifdef WIN32
    std::system("cd ../resources/shaders && python compile_quad_render_shaders.py");
#else
    std::system("cd ../resources/shaders && python3 compile_quad_render_shaders.py");
#endif

    etna::reload_shaders();

    for (uint32_t i = 0; i < m_framesInFlight; ++i)
      BuildCommandBufferSimple(m_cmdBuffersDrawMain[i], m_swapchain.GetAttachment(i).image, m_swapchain.GetAttachment(i).view);
  }
}
