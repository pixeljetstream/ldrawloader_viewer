/*
* Copyright (c) 2019-2025, Christoph Kubisch. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* SPDX-FileCopyrightText: Copyright (c) 2018-2023 Christoph Kubisch
* SPDX-License-Identifier: Apache-2.0
*/

#include <nvgl/extensions_gl.hpp>

#include <imgui/imgui_helper.h>
#include <imgui/backends/imgui_impl_gl.h>

#include <nvmath/nvmath_glsltypes.h>
#include <nvgl/glsltypes_gl.hpp>

#include <nvmath/nvmath.h>

#include <nvh/geometry.hpp>
#include <nvh/misc.hpp>
#include <nvh/cameracontrol.hpp>

#include <nvgl/appwindowprofiler_gl.hpp>
#include <nvgl/error_gl.hpp>
#include <nvgl/programmanager_gl.hpp>
#include <nvgl/base_gl.hpp>

#include "external/ldrawloader/src/ldrawloader.h"

#include <thread>

#include "common.h"

namespace ldrawviewer {
int const SAMPLE_SIZE_WIDTH(1024);
int const SAMPLE_SIZE_HEIGHT(1024);
int const SAMPLE_MAJOR_VERSION(4);
int const SAMPLE_MINOR_VERSION(5);


class Sample : public nvgl::AppWindowProfilerGL
{
  struct
  {
    nvgl::ProgramID draw_scene;
  } programs;

  struct
  {
    GLuint scene_color        = 0;
    GLuint scene_depthstencil = 0;
  } textures;

  struct
  {
    GLuint scene = 0;
  } fbos;

  struct GLMultiDrawIndirect
  {
    uint32_t count;
    uint32_t instanceCount;
    uint32_t firstIndex;
    uint32_t baseVertex;
    uint32_t baseInstance;
  };

  struct DrawPart
  {
    uint32_t vertexCount;
    uint32_t vertexOffset;
    uint32_t triangleCount;
    uint32_t triangleOffset;
    uint32_t triangleCountC;
    uint32_t triangleOffsetC;
    uint32_t edgesCount;
    uint32_t edgesOffset;
    uint32_t optionalCount;
    uint32_t optionalOffset;
    uint32_t materialIDOffset;
    uint32_t materialIDOffsetC;
  };

  struct Common
  {
    GLuint vao             = 0;
    GLuint viewBuffer      = 0;
    GLuint objectBuffer    = 0;
    GLuint materialsBuffer = 0;
  };

  struct Scene
  {
    LdrModelHDL       model       = nullptr;
    LdrRenderModelHDL renderModel = nullptr;

    GLuint                vertexBuffer        = 0;
    GLuint                indexBuffer         = 0;
    GLuint                materialIndexBuffer = 0;
    std::vector<DrawPart> drawParts;
  };

  struct Tweak
  {
    nvmath::vec3 lightDir;
    nvmath::vec4 inheritColor   = {1, 1, 0, 1};
    bool         cull           = true;
    bool         drawRenderPart = false;
    bool         edges          = false;
    bool         triangles      = true;
    bool         chamfered      = false;
    bool         wireframe      = true;
    bool         optional       = false;
    bool         colors         = true;
    float        transparency   = 0;
    int          instance       = -1;
    int          part           = -1;
    int          tri            = -1;
    int          vertex         = -1;
    int          edge           = -1;
    bool         threadedLoad   = false;
  };

  nvgl::ProgramManager m_progManager;

  ImGuiH::Registry m_ui;
  double           m_uiTime;

  Tweak m_tweak;
  Tweak m_tweakLast;

  bool m_renderModel = false;

  LdrLoaderCreateInfo m_loaderCreateInfo;
  LdrLoaderCreateInfo m_loaderCreateInfoLast;
  LdrLoaderHDL        m_loader = nullptr;

  glsldata::ViewData m_viewUbo;

  Scene       m_scene;
  Common      m_common;
  std::string m_ldrawPath;
  std::string m_modelFilename;

  nvh::CameraControl m_control;

  bool begin() override;
  void processUI(double time);
  void think(double time) override;
  void resize(int width, int height) override;

  bool initProgram();
  bool initFramebuffers(int width, int height);
  bool initScene();
  void deinitScene();

  bool resetLoader();
  bool resetScene();

  void rebuildSceneBuffers();
  void drawDebug();


  void end() override;

  // return true to prevent m_windowState updates
  bool mouse_pos(int x, int y) { return ImGuiH::mouse_pos(x, y); }
  bool mouse_button(int button, int action) { return ImGuiH::mouse_button(button, action); }
  bool mouse_wheel(int wheel) { return ImGuiH::mouse_wheel(wheel); }
  bool key_char(int button) { return ImGuiH::key_char(button); }
  bool key_button(int button, int action, int mods) { return ImGuiH::key_button(button, action, mods); }

  template <typename T>
  bool tweakChanged(T& currentVar) const
  {
    const uint8_t* current = (const uint8_t*)&currentVar;
    size_t         offs    = (size_t)(current) - (size_t)(&m_tweak);
    const uint8_t* last    = ((const uint8_t*)&m_tweakLast) + offs;

    return memcmp(current, last, sizeof(T)) != 0;
  }

public:
  Sample()
  {
    m_loaderCreateInfo                     = {};
    m_loaderCreateInfo.partFixMode         = LDR_PART_FIX_NONE;
    m_loaderCreateInfo.renderpartBuildMode = LDR_RENDERPART_BUILD_ONLOAD;
    m_loaderCreateInfo.partFixTjunctions   = LDR_TRUE;
    m_loaderCreateInfo.partFixOverlap      = LDR_TRUE;
    m_loaderCreateInfo.partHiResPrimitives = LDR_FALSE;
    m_loaderCreateInfo.renderpartChamfer   = 0.2f;

    m_parameterList.addFilename(".ldr", &m_modelFilename);
    m_parameterList.addFilename(".mpd", &m_modelFilename);
    m_parameterList.add("threadedload", &m_tweak.threadedLoad);
    m_parameterList.add("renderpartbuild", (int*)&m_loaderCreateInfo.renderpartBuildMode);
    m_parameterList.add("renderpartchamfer", &m_loaderCreateInfo.renderpartChamfer);
    m_parameterList.add("partfix", (int*)&m_loaderCreateInfo.partFixMode);
    m_parameterList.add("partfixtj", (int*)&m_loaderCreateInfo.partFixTjunctions);
    m_parameterList.add("partfixov", (int*)&m_loaderCreateInfo.partFixOverlap);
    m_parameterList.add("drawrenderpart", &m_tweak.drawRenderPart);
    m_parameterList.add("chamfered", &m_tweak.chamfered);

    m_parameterList.add("ldrawpath", &m_ldrawPath);

    const char* ldrawPath = getenv("LDRAWDIR");
    if(ldrawPath) {
      m_ldrawPath = std::string(ldrawPath);
    }
  }
};

bool Sample::initProgram()
{
  bool validated(true);
  m_progManager.m_filetype = nvh::ShaderFileManager::FILETYPE_GLSL;
  m_progManager.addDirectory(std::string(PROJECT_NAME));
  m_progManager.addDirectory(exePath() + std::string(PROJECT_RELDIRECTORY));

  m_progManager.registerInclude("common.h", "common.h");

  programs.draw_scene = m_progManager.createProgram(nvgl::ProgramManager::Definition(GL_VERTEX_SHADER, "scene.vert.glsl"),
                                                    nvgl::ProgramManager::Definition(GL_FRAGMENT_SHADER, "scene.frag.glsl"));


  validated = m_progManager.areProgramsValid();

  return validated;
}

bool Sample::initFramebuffers(int width, int height)
{
  nvgl::newTexture(textures.scene_color, GL_TEXTURE_2D_MULTISAMPLE);
  glTextureStorage2DMultisample(textures.scene_color, 8, GL_RGBA8, width, height, GL_FALSE);

  nvgl::newTexture(textures.scene_depthstencil, GL_TEXTURE_2D_MULTISAMPLE);
  glTextureStorage2DMultisample(textures.scene_depthstencil, 8, GL_DEPTH24_STENCIL8, width, height, GL_FALSE);

  nvgl::newFramebuffer(fbos.scene);
  glBindFramebuffer(GL_FRAMEBUFFER, fbos.scene);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, textures.scene_color, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D_MULTISAMPLE, textures.scene_depthstencil, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  return true;
}

bool Sample::initScene()
{
  if(m_modelFilename.empty())
    return true;

  double timeLoadAll;
  double time;

  timeLoadAll = -m_profiler.getMicroSeconds();

  LdrResult result;
  if(m_tweak.threadedLoad) {
    time   = -m_profiler.getMicroSeconds();
    result = ldrCreateModel(m_loader, m_modelFilename.c_str(), LDR_FALSE, &m_scene.model);
    assert(result == LDR_SUCCESS || result == LDR_WARNING_PART_NOT_FOUND);
    time += m_profiler.getMicroSeconds();
    printf("dependency time %.2f ms\n", time / 1000.0f);

    if(!(result == LDR_SUCCESS || result == LDR_WARNING_PART_NOT_FOUND))
      return false;

    // threaded loaded
    time = -m_profiler.getMicroSeconds();

    uint32_t numParts   = ldrGetNumRegisteredParts(m_loader);
    uint32_t numThreads = std::thread::hardware_concurrency();
    uint32_t perThread  = (numParts + numThreads - 1) / numThreads;

    std::vector<LdrPartID> partIds(numParts);
    std::vector<LdrResult> errors(numThreads);

    std::vector<std::thread> threads(numThreads);
    for(uint32_t i = 0; i < numThreads; i++) {
      threads[i] = std::thread(
          [&](uint32_t idx) {
            uint32_t offset   = idx * perThread;
            uint32_t numLocal = offset > numParts ? 0 : std::min(perThread, numParts - idx * perThread);
            if(numLocal) {
              for(uint32_t p = 0; p < numLocal; p++) {
                partIds[offset + p] = (LdrPartID)(offset + p);
              }
              errors[idx] = ldrLoadDeferredParts(m_loader, numLocal, &partIds[offset], sizeof(LdrPartID));
            }
          },
          i);
    }
    for(uint32_t i = 0; i < numThreads; i++) {
      threads[i].join();
      if(!(errors[i] == LDR_SUCCESS || errors[i] == LDR_WARNING_PART_NOT_FOUND)) {
        assert(0);
        return false;
      }
    }
    ldrResolveModel(m_loader, m_scene.model);

    time += m_profiler.getMicroSeconds();
    printf("threaded time %.2f ms\n", time / 1000.0f);
  }
  else {
    time   = -m_profiler.getMicroSeconds();
    result = ldrCreateModel(m_loader, m_modelFilename.c_str(), LDR_TRUE, &m_scene.model);
    assert(result == LDR_SUCCESS || result == LDR_WARNING_PART_NOT_FOUND);
    time += m_profiler.getMicroSeconds();
    printf("load time %.2f ms\n", time / 1000.0f);

    if(!(result == LDR_SUCCESS || result == LDR_WARNING_PART_NOT_FOUND))
      return false;
  }

  timeLoadAll += m_profiler.getMicroSeconds();
  printf("total load time %.2f ms\n", timeLoadAll / 1000.0f);

  time = -m_profiler.getMicroSeconds();
  //ldrFixParts(m_loader, ~0, nullptr, 0);
  time += m_profiler.getMicroSeconds();
  printf("fix time %.2f ms\n", time / 1000.0f);

  time = -m_profiler.getMicroSeconds();
  //ldrBuildRenderParts(m_loader, ~0, nullptr, 0);
  time += m_profiler.getMicroSeconds();

  if(m_loaderCreateInfo.renderpartBuildMode == LDR_RENDERPART_BUILD_ONLOAD) {
    result = ldrCreateRenderModel(m_loader, m_scene.model, LDR_TRUE, &m_scene.renderModel);
    assert(result == LDR_SUCCESS || result == LDR_WARNING_PART_NOT_FOUND);
  }

  printf("build time %.2f ms\n", time / 1000.0f);

  return (result == LDR_SUCCESS || result == LDR_WARNING_PART_NOT_FOUND);
}


void Sample::deinitScene()
{
  ldrDestroyModel(m_loader, m_scene.model);
  ldrDestroyRenderModel(m_loader, m_scene.renderModel);

  nvgl::deleteBuffer(m_scene.vertexBuffer);
  nvgl::deleteBuffer(m_scene.indexBuffer);
  nvgl::deleteBuffer(m_scene.materialIndexBuffer);

  glFlush();
  glFinish();

  m_scene = Scene();
}

bool Sample::resetLoader()
{
  ldrDestroyLoader(m_loader);
  m_loader = nullptr;

  LdrResult result = ldrCreateLoader(&m_loaderCreateInfo, &m_loader);
  assert(result == LDR_SUCCESS);

  m_loaderCreateInfoLast = m_loaderCreateInfo;

  if(m_loader) {
    std::vector<glsldata::MaterialData> triangleMaterials;
    uint32_t                            numMaterials = ldrGetNumRegisteredMaterials(m_loader);
    triangleMaterials.resize(numMaterials);

    for(uint32_t m = 0; m < numMaterials; m++) {
      const LdrMaterial* mtl     = ldrGetMaterial(m_loader, m);
      triangleMaterials[m].color = {float(mtl->baseColor[0]) / float(255.0f), float(mtl->baseColor[1]) / float(255.0f),
                                    float(mtl->baseColor[2]) / float(255.0f), 1};
    }

    nvgl::newBuffer(m_common.materialsBuffer);
    glNamedBufferStorage(m_common.materialsBuffer, sizeof(glsldata::MaterialData) * triangleMaterials.size(),
                         triangleMaterials.data(), 0);
  }


  printf("reset loader status: %d\n", result == LDR_SUCCESS ? 1 : 0);
  return result == LDR_SUCCESS;
}

bool Sample::resetScene()
{
  deinitScene();
  bool result = resetLoader();
  result      = result && initScene();
  if(result) {
    rebuildSceneBuffers();
  }
  printf("reset scene status: %d\n", result ? 1 : 0);
  return result;
}

bool Sample::begin()
{
  ImGuiH::Init(m_windowState.m_winSize[0], m_windowState.m_winSize[1], this);
  ImGui::InitGL();

  nvgl::newBuffer(m_common.viewBuffer);
  glNamedBufferStorage(m_common.viewBuffer, sizeof(glsldata::ViewData), NULL, GL_DYNAMIC_STORAGE_BIT);
  nvgl::newBuffer(m_common.objectBuffer);
  glNamedBufferStorage(m_common.objectBuffer, sizeof(glsldata::ObjectData), NULL, GL_DYNAMIC_STORAGE_BIT);
  nvgl::newVertexArray(m_common.vao);

  m_loaderCreateInfo.basePath = m_ldrawPath.c_str();

  bool validated = resetLoader();

  validated = validated && initProgram();
  validated = validated && initFramebuffers(m_windowState.m_winSize[0], m_windowState.m_winSize[1]);
  validated = validated && initScene();

  m_tweak.lightDir = nvmath::normalize(nvmath::vec3(-1, -1, 1));

  m_control.m_sceneOrbit     = nvmath::vec3(0.0f);
  m_control.m_sceneDimension = 1000.0f;
  m_control.m_sceneUp        = glm::vec3(0, -1, 0);
  m_control.m_viewMatrix =
      (glm::mat4)nvmath::look_at(nvmath::vec3(m_control.m_sceneOrbit) + nvmath::vec3(0, 0, -m_control.m_sceneDimension),
                                 nvmath::vec3(m_control.m_sceneOrbit), (nvmath::vec3)m_control.m_sceneUp);

  rebuildSceneBuffers();

  m_tweakLast = m_tweak;

  return validated;
}

void Sample::end()
{
  deinitScene();
  ldrDestroyLoader(m_loader);
  nvgl::deleteBuffer(m_common.objectBuffer);
  nvgl::deleteBuffer(m_common.viewBuffer);
  nvgl::deleteBuffer(m_common.materialsBuffer);
  nvgl::deleteVertexArray(m_common.vao);

  ImGui::ShutdownGL();
}

void Sample::processUI(double time)
{

  int width  = m_windowState.m_winSize[0];
  int height = m_windowState.m_winSize[1];

  // Update imgui configuration
  auto& imgui_io       = ImGui::GetIO();
  imgui_io.DeltaTime   = static_cast<float>(time - m_uiTime);
  imgui_io.DisplaySize = ImVec2(width, height);

  m_uiTime = time;

  ImGui::NewFrame();
  ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(5, 5), ImGuiCond_FirstUseEver);

  if(ImGui::Begin(PROJECT_NAME, nullptr)) {
    ImGui::PushItemWidth(200);

    if(ImGui::Button("LOAD")) {
      std::string newFile = NVPWindow::openFileDialog("Pick Model", "Supported (ldr,mpd)|*.ldr;*.mpd|All (*.*)|*.*");
      if(!newFile.empty()) {
        m_modelFilename = newFile;
        deinitScene();
        resetLoader();
        initScene();
        rebuildSceneBuffers();
      }
    }
    ImGui::SameLine();
    if(ImGui::Button("RELOAD")) {
      resetScene();
    }
    if(m_scene.model && ImGui::CollapsingHeader("render settings", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Checkbox("colors", &m_tweak.colors);
      ImGui::Checkbox("bf cull", &m_tweak.cull);
      ImGui::SliderFloat("x-ray transp.", &m_tweak.transparency, 0, 1);
      ImGui::ColorEdit3("inheritcolor", &m_tweak.inheritColor.x);
      ImGui::Checkbox("edges", &m_tweak.edges);
      ImGui::Checkbox("triangles", &m_tweak.triangles);
      ImGui::Checkbox("wireframe", &m_tweak.wireframe);
      ImGui::Checkbox("optional", &m_tweak.optional);
      if(m_scene.renderModel) {
        ImGui::Checkbox("draw render part", &m_tweak.drawRenderPart);
        ImGui::Checkbox("draw render part chamfer", &m_tweak.chamfered);
      }
      ImGui::InputInt("instance", &m_tweak.instance);
      ImGui::InputInt("part", &m_tweak.part);
      ImGui::InputInt("vertex", &m_tweak.vertex);
      ImGui::InputInt("tri", &m_tweak.tri);
      ImGui::InputInt("edge", &m_tweak.edge);

      m_tweak.instance = std::max(-1, m_tweak.instance);
      m_tweak.part     = std::max(-1, m_tweak.part);
      m_tweak.vertex   = std::max(-1, m_tweak.vertex);
      m_tweak.tri      = std::max(-1, m_tweak.tri);
      m_tweak.edge     = std::max(-1, m_tweak.edge);

      if(m_tweak.instance >= 0) {
        m_tweak.instance = std::min((uint32_t)m_tweak.instance, m_scene.model->numInstances - 1);
        m_tweak.part     = m_scene.model->instances[m_tweak.instance].part;
      }
      else if(m_tweakLast.instance >= 0) {
        m_tweak.part = -1;
      }

      if(m_tweak.part >= 0) {
        m_tweak.part = std::min((uint32_t)m_tweak.part, ldrGetNumRegisteredParts(m_loader) - 1);
      }

      if(m_tweak.part >= 0 && m_tweak.tri >= 0) {
        const LdrVertexIndex* indices = nullptr;
        if(m_scene.renderModel && m_tweak.drawRenderPart) {
          const LdrRenderPart* rpart = ldrGetRenderPart(m_loader, m_tweak.part);
          if(rpart) {
            m_tweak.tri = std::min(uint32_t(m_tweak.tri), rpart->numTriangles - 1);
            indices     = &rpart->triangles[m_tweak.tri * 3];
          }
        }
        else {
          const LdrPart* part = ldrGetPart(m_loader, m_tweak.part);
          m_tweak.tri         = std::min(uint32_t(m_tweak.tri), part->numTriangles - 1);
          indices             = &part->triangles[m_tweak.tri * 3];
        }
        if(indices) {
          ImGui::Text("tri: %d %d %d\n", indices[0], indices[1], indices[2]);
        }
        else {
          ImGui::Text("tri: -\n");
        }
      }

      if(m_tweak.part >= 0 && m_tweak.edge >= 0) {
        const LdrVertexIndex* indices = nullptr;
        if(m_scene.renderModel && m_tweak.drawRenderPart) {
          const LdrRenderPart* rpart = ldrGetRenderPart(m_loader, m_tweak.part);
          if(rpart) {
            m_tweak.edge = std::min(uint32_t(m_tweak.edge), rpart->numLines - 1);
            indices      = &rpart->lines[m_tweak.edge * 2];
          }
        }
        else {
          const LdrPart* part = ldrGetPart(m_loader, m_tweak.part);
          m_tweak.edge        = std::min(uint32_t(m_tweak.edge), part->numLines - 1);
          indices             = &part->lines[m_tweak.edge * 2];
        }
        if(indices) {
          ImGui::Text("line: %d %d\n", indices[0], indices[1]);
        }
        else {
          ImGui::Text("line: -\n");
        }
      }

      if(m_tweak.vertex >= 0 && m_tweak.part >= 0) {
        const LdrPart* part = ldrGetPart(m_loader, m_tweak.part);

        const float* pos = nullptr;
        const float* nrm = nullptr;
        if(m_scene.renderModel && m_tweak.drawRenderPart) {
          const LdrRenderPart* rpart = ldrGetRenderPart(m_loader, m_tweak.part);
          if(rpart) {
            m_tweak.vertex = std::min(uint32_t(m_tweak.vertex), rpart->numVertices - 1);
            pos            = &rpart->vertices[m_tweak.vertex].position.x;
            nrm            = &rpart->vertices[m_tweak.vertex].normal.x;
          }
        }
        else {
          const LdrPart* part = ldrGetPart(m_loader, m_tweak.part);
          m_tweak.vertex      = std::min(uint32_t(m_tweak.vertex), part->numPositions - 1);
          pos                 = &part->positions[m_tweak.vertex].x;
        }
        if(pos) {
          ImGui::Text("vert: %.3f %.3f %.3f\n", pos[0], pos[1], pos[2]);
        }
        else {
          ImGui::Text("pos: -\n");
        }
        if(nrm) {
          ImGui::Text("norm: %.3f %.3f %.3f\n", nrm[0], nrm[1], nrm[2]);
        }
        else {
          ImGui::Text("norm: -\n");
        }
      }

      if(m_tweak.part >= 0) {
        const LdrPart* part = ldrGetPart(m_loader, m_tweak.part);
        if(part) {
          ImGui::Text("%s\n", part->name);
          if(m_scene.renderModel && m_tweak.drawRenderPart) {
            const LdrRenderPart* rpart = ldrGetRenderPart(m_loader, m_tweak.part);
            if(rpart) {
              ImGui::Text("  instances %6d\n", 0);
              ImGui::Text("  points    %6d\n", rpart->numVertices);
              ImGui::Text("  tris      %6d\n", rpart->numTriangles);
              ImGui::Text("  lines     %6d\n", rpart->numLines);
              ImGui::Text("  olines    %6d\n", 0);
            }
          }
          else {
            ImGui::Text("  instances %6d\n", part->numInstances);
            ImGui::Text("  points    %6d\n", part->numPositions);
            ImGui::Text("  tris      %6d\n", part->numTriangles);
            ImGui::Text("  lines     %6d\n", part->numLines);
            ImGui::Text("  olines    %6d\n", part->numOptionalLines);
          }
        }
      }
    }

    if(ImGui::CollapsingHeader("loader settings", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Checkbox("build renderparts", (bool*)&m_loaderCreateInfo.renderpartBuildMode);
      ImGui::Checkbox("fix parts", (bool*)&m_loaderCreateInfo.partFixMode);
      ImGui::Checkbox("fix coplanar overlap", (bool*)&m_loaderCreateInfo.partFixOverlap);
      ImGui::Checkbox("fix t junctions", (bool*)&m_loaderCreateInfo.partFixTjunctions);
      ImGui::Checkbox("hi-res primitives", (bool*)&m_loaderCreateInfo.partHiResPrimitives);
      ImGui::InputFloat("render chamfer", &m_loaderCreateInfo.renderpartChamfer, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue);
    }
    ImGui::PopItemWidth();
  }
  ImGui::End();
}

void Sample::think(double time)
{
  NV_PROFILE_GL_SECTION("Frame");

  processUI(time);

  m_control.processActions(glm::ivec2(m_windowState.m_winSize[0], m_windowState.m_winSize[1]),
                           nvmath::vec2f(m_windowState.m_mouseCurrent[0], m_windowState.m_mouseCurrent[1]),
                           m_windowState.m_mouseButtonFlags, m_windowState.m_mouseWheel);

  int width  = m_windowState.m_winSize[0];
  int height = m_windowState.m_winSize[1];

  if(m_windowState.onPress(KEY_R)) {
    m_progManager.reloadPrograms();
  }
  if(!m_progManager.areProgramsValid()) {
    waitEvents();
    return;
  }

  {
    NV_PROFILE_GL_SECTION("Setup");
    m_viewUbo.viewport = nvmath::uvec2(width, height);

    nvmath::mat4 projection = nvmath::perspective(45.f, float(width) / float(height), 0.1f, 1000000.0f);
    nvmath::mat4 view       = m_control.m_viewMatrix;

    m_viewUbo.viewProjMatrix  = projection * view;
    m_viewUbo.viewProjMatrixI = nvmath::invert(m_viewUbo.viewProjMatrix);
    m_viewUbo.viewMatrix      = view;
    m_viewUbo.viewMatrixI     = nvmath::invert(view);
    m_viewUbo.viewMatrixIT    = nvmath::transpose(m_viewUbo.viewMatrixI);
    m_viewUbo.wLightPos       = nvmath::vec4((m_tweak.lightDir * m_control.m_sceneDimension), 1.0f);
    m_viewUbo.time            = float(time);
    m_viewUbo.opacity         = 1.0f - m_tweak.transparency;
    m_viewUbo.useObjectColor  = m_tweak.colors ? 1 : 0;
    m_viewUbo.inheritColor    = m_tweak.inheritColor;

    glNamedBufferSubData(m_common.viewBuffer, 0, sizeof(glsldata::ViewData), &m_viewUbo);

    glBindFramebuffer(GL_FRAMEBUFFER, fbos.scene);
    glViewport(0, 0, width, height);

    nvmath::vec4 bgColor(0.2, 0.2, 0.2, 0.0);
    glClearColor(bgColor.x, bgColor.y, bgColor.z, bgColor.w);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  }

  bool doRebuild = false;
  if(memcmp(&m_loaderCreateInfoLast, &m_loaderCreateInfo, sizeof(m_loaderCreateInfo)) != 0 || tweakChanged(m_tweak.threadedLoad)) {
    deinitScene();
    resetLoader();
    initScene();
    doRebuild = true;
  }

  if(!m_scene.renderModel)
    m_tweak.drawRenderPart = false;

  if(doRebuild || tweakChanged(m_tweak.chamfered) || tweakChanged(m_tweak.drawRenderPart)) {
    rebuildSceneBuffers();
  }

  {
    NV_PROFILE_GL_SECTION("Draw");
    drawDebug();
  }

  {
    NV_PROFILE_GL_SECTION("Blit");
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos.scene);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
  }

  {
    NV_PROFILE_GL_SECTION("GUI");
    ImGui::Render();
    ImGui::RenderDrawDataGL(ImGui::GetDrawData());
  }

  ImGui::EndFrame();

  m_tweakLast = m_tweak;
}

void Sample::resize(int width, int height)
{
  initFramebuffers(width, height);
}

void Sample::rebuildSceneBuffers()
{
  LdrModelHDL model = m_scene.model;
  if(!model)
    return;

  uint32_t numParts = ldrGetNumRegisteredParts(m_loader);
  m_scene.drawParts.resize(numParts);

  std::vector<bool> activeParts(numParts, false);

  for(uint32_t i = 0; i < model->numInstances; i++) {
    const LdrInstance* instance = &model->instances[i];
    if(instance->part != LDR_INVALID_ID)
      activeParts[instance->part] = true;
  }

  uint32_t vboOffset = 0;
  uint32_t iboOffset = 0;
  uint32_t mtlOffset = 0;

  for(uint32_t i = 0; i < numParts; i++) {
    if(!activeParts[i])
      continue;

    const LdrPart* part = ldrGetPart(m_loader, i);

    DrawPart& drawPart = m_scene.drawParts[i];

    uint32_t materialIndexCount  = 0;
    uint32_t materialIndexCountC = 0;

    if(!m_tweak.drawRenderPart) {
      drawPart.vertexCount    = part->numPositions;
      drawPart.triangleCount  = part->numTriangles;
      drawPart.edgesCount     = part->numLines;
      drawPart.optionalCount  = part->numOptionalLines;
      drawPart.triangleCountC = 0;
      if(part->triangleMaterials && part->flags.hasComplexMaterial) {
        materialIndexCount = part->numTriangles;
      }
    }
    else {
      const LdrRenderPart* rpart = ldrGetRenderPart(m_loader, i);
      if(rpart) {
        drawPart.vertexCount    = rpart->numVertices;
        drawPart.triangleCount  = rpart->numTriangles;
        drawPart.edgesCount     = rpart->numLines;
        drawPart.optionalCount  = 0;
        drawPart.triangleCountC = rpart->numTrianglesC;
        if(rpart->triangleMaterials && rpart->flags.hasComplexMaterial)
          materialIndexCount = rpart->numTriangles;
        if(rpart->materialsC && rpart->flags.hasComplexMaterial)
          materialIndexCountC = rpart->numTrianglesC;
      }
      else {
        drawPart.vertexCount    = 0;
        drawPart.triangleCount  = 0;
        drawPart.edgesCount     = 0;
        drawPart.optionalCount  = 0;
        drawPart.triangleCountC = 0;
      }
    }

    drawPart.vertexOffset     = vboOffset;
    drawPart.triangleOffset   = iboOffset;
    drawPart.materialIDOffset = mtlOffset;
    mtlOffset += materialIndexCount;
    drawPart.materialIDOffsetC = mtlOffset;
    mtlOffset += materialIndexCountC;

    vboOffset += drawPart.vertexCount;
    iboOffset += drawPart.triangleCount * 3;
    mtlOffset += materialIndexCount;

    drawPart.edgesOffset = iboOffset;
    iboOffset += drawPart.edgesCount * 2;
    drawPart.optionalOffset = iboOffset;
    iboOffset += drawPart.optionalCount * 2;
    drawPart.triangleOffsetC = iboOffset;
    iboOffset += drawPart.triangleCountC * 3;
  }

  glFlush();
  glFinish();

  nvgl::newBuffer(m_scene.vertexBuffer);
  nvgl::newBuffer(m_scene.indexBuffer);
  nvgl::newBuffer(m_scene.materialIndexBuffer);

  glFlush();
  glFinish();

  size_t vertexSize = (m_tweak.drawRenderPart ? sizeof(LdrRenderVertex) : sizeof(LdrVector));

  if(vboOffset) {
    printf("vbo size: %9d - %9d KB\n", vboOffset, (uint32_t)(vertexSize * vboOffset + 1023) & ~1023);
    glNamedBufferStorage(m_scene.vertexBuffer, vertexSize * vboOffset, nullptr, GL_DYNAMIC_STORAGE_BIT);
  }
  if(iboOffset) {
    printf("ibo size: %9d - %9d KB\n", iboOffset, (uint32_t)(sizeof(uint32_t) * iboOffset + 1023) & ~1023);
    glNamedBufferStorage(m_scene.indexBuffer, sizeof(uint32_t) * iboOffset, nullptr, GL_DYNAMIC_STORAGE_BIT);
  }
  if(mtlOffset) {
    printf("mtl size: %9d - %9d KB\n", mtlOffset, (uint32_t)(sizeof(LdrMaterialID) * mtlOffset + 1023) & ~1023);
    glNamedBufferStorage(m_scene.materialIndexBuffer, sizeof(LdrMaterialID) * mtlOffset, nullptr, GL_DYNAMIC_STORAGE_BIT);
  }

  for(uint32_t i = 0; i < numParts; i++) {
    if(!activeParts[i])
      continue;

    const DrawPart& drawPart = m_scene.drawParts[i];

    if(!m_tweak.drawRenderPart) {
      const LdrPart* part = ldrGetPart(m_loader, i);

      glNamedBufferSubData(m_scene.vertexBuffer, vertexSize * drawPart.vertexOffset, vertexSize * drawPart.vertexCount,
                           part->positions);
      glNamedBufferSubData(m_scene.indexBuffer, sizeof(uint32_t) * drawPart.triangleOffset,
                           sizeof(uint32_t) * drawPart.triangleCount * 3, part->triangles);
      glNamedBufferSubData(m_scene.indexBuffer, sizeof(uint32_t) * drawPart.edgesOffset,
                           sizeof(uint32_t) * drawPart.edgesCount * 2, part->lines);
      glNamedBufferSubData(m_scene.indexBuffer, sizeof(uint32_t) * drawPart.optionalOffset,
                           sizeof(uint32_t) * drawPart.optionalCount * 2, part->optional_lines);

      if(part->triangleMaterials && part->flags.hasComplexMaterial) {
        glNamedBufferSubData(m_scene.materialIndexBuffer, sizeof(LdrMaterialID) * drawPart.materialIDOffset,
                             sizeof(LdrMaterialID) * drawPart.triangleCount, part->triangleMaterials);
      }
    }
    else {
      const LdrRenderPart* rpart = ldrGetRenderPart(m_loader, i);

      if(!rpart)
        continue;

      glNamedBufferSubData(m_scene.vertexBuffer, vertexSize * drawPart.vertexOffset, vertexSize * drawPart.vertexCount,
                           rpart->vertices);
      glNamedBufferSubData(m_scene.indexBuffer, sizeof(uint32_t) * drawPart.triangleOffset,
                           sizeof(uint32_t) * drawPart.triangleCount * 3, rpart->triangles);
      glNamedBufferSubData(m_scene.indexBuffer, sizeof(uint32_t) * drawPart.edgesOffset,
                           sizeof(uint32_t) * drawPart.edgesCount * 2, rpart->lines);

      glNamedBufferSubData(m_scene.indexBuffer, sizeof(uint32_t) * drawPart.triangleOffsetC,
                           sizeof(uint32_t) * drawPart.triangleCountC * 3, rpart->trianglesC);

      if(rpart->triangleMaterials && rpart->flags.hasComplexMaterial) {
        glNamedBufferSubData(m_scene.materialIndexBuffer, sizeof(LdrMaterialID) * drawPart.materialIDOffset,
                             sizeof(LdrMaterialID) * drawPart.triangleCount, rpart->triangleMaterials);
      }
      if(rpart->materialsC && rpart->flags.hasComplexMaterial) {
        glNamedBufferSubData(m_scene.materialIndexBuffer, sizeof(LdrMaterialID) * drawPart.materialIDOffsetC,
                             sizeof(LdrMaterialID) * drawPart.triangleCountC, rpart->materialsC);
      }
    }
  }
}

void Sample::drawDebug()
{
  if(!m_scene.model)
    return;

  glBindVertexArray(m_common.vao);

  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE);
  glUseProgram(m_progManager.get(programs.draw_scene));

  glEnableVertexAttribArray(VERTEX_POS);
  if(m_tweak.drawRenderPart)
    glEnableVertexAttribArray(VERTEX_NORMAL);

  glPolygonOffset(1, 1);
  glPointSize(8);
  glEnable(GL_POLYGON_OFFSET_FILL);
  glLineStipple(2, 0xAAAA);

  glBindBufferBase(GL_UNIFORM_BUFFER, UBO_SCENE, m_common.viewBuffer);
  glBindBufferBase(GL_UNIFORM_BUFFER, UBO_OBJECT, m_common.objectBuffer);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, SSBO_MATERIALS, m_common.materialsBuffer);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, SSBO_MATERIALIDS, m_scene.materialIndexBuffer);

  bool cullFace = true;
  bool ccw      = true;

  float lineWidthScale = 2.0f;
  float lineWidthBase  = 1.0f;

  glFrontFace(GL_CCW);
  glLineWidth(lineWidthBase);

  srand(1123);

  if(m_tweak.transparency) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDisable(GL_DEPTH_TEST);
  }
  else {
    glDisable(GL_BLEND);
  }

  float wireColor = 0.5f;

  glBindBuffer(GL_ARRAY_BUFFER, m_scene.vertexBuffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_scene.indexBuffer);

  if(!m_tweak.drawRenderPart) {

    glVertexAttribPointer(VERTEX_POS, 3, GL_FLOAT, GL_FALSE, sizeof(LdrVector), 0);
  }
  else {
    glVertexAttribPointer(VERTEX_POS, 3, GL_FLOAT, GL_FALSE, sizeof(LdrRenderVertex),
                          (const void*)offsetof(LdrRenderVertex, position));
    glVertexAttribPointer(VERTEX_NORMAL, 3, GL_FLOAT, GL_FALSE, sizeof(LdrRenderVertex),
                          (const void*)offsetof(LdrRenderVertex, normal));
  }

  glBindBuffer(GL_UNIFORM_BUFFER, m_common.objectBuffer);

  glUniform1f(UNI_COLORMUL, 1.0f);
  glUniform1i(UNI_LIGHTING, 0);
  glUniform1ui(UNI_MATERIALID, LDR_MATERIALID_INHERIT);
  glUniform1ui(UNI_MATERIALIDOFFSET, ~0);

  LdrModelHDL model = m_scene.model;
  for(uint32_t i = 0; i < model->numInstances; i++) {
    const LdrInstance*   instance = &model->instances[i];
    const LdrPart*       part     = ldrGetPart(m_loader, instance->part);
    const LdrRenderPart* rpart    = ldrGetRenderPart(m_loader, instance->part);
    const DrawPart&      drawPart = m_scene.drawParts[instance->part];

    if(m_tweak.instance >= 0 && i != (uint32_t)m_tweak.instance)
      continue;

    if(!part || m_tweak.part >= 0 && instance->part != m_tweak.part)
      continue;

    glsldata::ObjectData obj;
    obj.color = {nvh::frand(), nvh::frand(), nvh::frand(), 1.0f};

    memcpy(obj.worldMatrix.mat_array, &instance->transform, sizeof(LdrMatrix));
    obj.worldMatrixIT = nvmath::transpose(nvmath::invert(obj.worldMatrix));
    float det         = nvmath::det(obj.worldMatrix);

    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(glsldata::ObjectData), &obj);

    glUniform1ui(UNI_MATERIALID, instance->material);

    if(cullFace != !(bool(part->flags.hasNoBackFaceCulling) || !m_tweak.cull)) {
      if(cullFace) {
        glDisable(GL_CULL_FACE);
      }
      else {
        glEnable(GL_CULL_FACE);
      }
      cullFace = !(bool(part->flags.hasNoBackFaceCulling) || !m_tweak.cull);
    }

    if(ccw != det > 0) {
      glFrontFace(det > 0 ? GL_CCW : GL_CW);
      ccw = det > 0;
    }

    if(!m_tweak.drawRenderPart) {
      glUniform1i(UNI_LIGHTING, 0);
      glUniform1f(UNI_COLORMUL, 1.0f);

      if(m_tweak.triangles) {
        if(part->triangleMaterials && part->flags.hasComplexMaterial)
          glUniform1ui(UNI_MATERIALIDOFFSET, drawPart.materialIDOffset);

        glDrawElementsBaseVertex(GL_TRIANGLES, part->numTriangles * 3, GL_UNSIGNED_INT,
                                 (const void*)(sizeof(uint32_t) * drawPart.triangleOffset), drawPart.vertexOffset);
        if(part->triangleMaterials && part->flags.hasComplexMaterial)
          glUniform1ui(UNI_MATERIALIDOFFSET, ~0);
      }
      glUniform1f(UNI_COLORMUL, 0.2f);
      if(m_tweak.edges) {
        glLineWidth(lineWidthBase * lineWidthScale);
        glDrawElementsBaseVertex(GL_LINES, part->numLines * 2, GL_UNSIGNED_INT,
                                 (const void*)(sizeof(uint32_t) * drawPart.edgesOffset), drawPart.vertexOffset);
      }

      if(m_tweak.optional) {
        glLineWidth(lineWidthBase * lineWidthScale);
        glLineStipple(4, 0xAAAA);
        glEnable(GL_LINE_STIPPLE);
        glDrawElementsBaseVertex(GL_LINES, part->numOptionalLines * 2, GL_UNSIGNED_INT,
                                 (const void*)(sizeof(uint32_t) * drawPart.optionalOffset), drawPart.vertexOffset);
        glDisable(GL_LINE_STIPPLE);
      }

      if(m_tweak.wireframe) {
        glLineWidth(lineWidthBase);
        glUniform1f(UNI_COLORMUL, wireColor);
        glLineStipple(2, 0xAAAA);
        glEnable(GL_LINE_STIPPLE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glDrawElementsBaseVertex(GL_TRIANGLES, part->numTriangles * 3, GL_UNSIGNED_INT,
                                 (const void*)(sizeof(uint32_t) * drawPart.triangleOffset), drawPart.vertexOffset);
        glDisable(GL_LINE_STIPPLE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
      }
    }
    else if(rpart) {

      uint32_t triangles = m_tweak.chamfered && rpart->flags.canChamfer ? drawPart.triangleOffsetC : drawPart.triangleOffset;
      uint32_t numTriangles = m_tweak.chamfered && rpart->flags.canChamfer ? rpart->numTrianglesC : rpart->numTriangles;
      const LdrMaterialID* triangleMaterials =
          m_tweak.chamfered && rpart->flags.canChamfer ? rpart->materialsC : rpart->triangleMaterials;
      uint32_t material_offset = m_tweak.chamfered && rpart->flags.canChamfer ? drawPart.materialIDOffsetC : drawPart.materialIDOffset;

      glUniform1i(UNI_LIGHTING, 1);
      glUniform1f(UNI_COLORMUL, 1.0f);

      if(m_tweak.triangles) {
        if(triangleMaterials && rpart->flags.hasComplexMaterial)
          glUniform1ui(UNI_MATERIALIDOFFSET, material_offset);

        glDrawElementsBaseVertex(GL_TRIANGLES, numTriangles * 3, GL_UNSIGNED_INT,
                                 (const void*)(sizeof(uint32_t) * triangles), drawPart.vertexOffset);

        if(triangleMaterials && rpart->flags.hasComplexMaterial)
          glUniform1ui(UNI_MATERIALIDOFFSET, ~0);
      }

      glUniform1f(UNI_COLORMUL, 0.2f);
      glUniform1i(UNI_LIGHTING, 0);
      if(m_tweak.edges) {
        glLineWidth(lineWidthBase * lineWidthScale);
        glDrawElementsBaseVertex(GL_LINES, rpart->numLines * 2, GL_UNSIGNED_INT,
                                 (const void*)(sizeof(uint32_t) * drawPart.edgesOffset), drawPart.vertexOffset);
      }

      if(m_tweak.wireframe) {
        glLineWidth(lineWidthBase);
        glUniform1f(UNI_COLORMUL, wireColor);
        glEnable(GL_LINE_STIPPLE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glDrawElementsBaseVertex(GL_TRIANGLES, numTriangles * 3, GL_UNSIGNED_INT,
                                 (const void*)(sizeof(uint32_t) * triangles), drawPart.vertexOffset);
        glDisable(GL_LINE_STIPPLE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
      }
    }

    if(instance->part == m_tweak.part) {
      if(m_tweak.vertex >= 0) {
        glUniform1f(UNI_COLORMUL, 2.0f);
        glDrawArrays(GL_POINTS, m_tweak.vertex + drawPart.vertexOffset, 1);
      }
      if(m_tweak.tri >= 0) {
        glUniform1f(UNI_COLORMUL, 1.7f);
        glLineWidth(lineWidthBase * 3 * lineWidthScale);
        glEnable(GL_LINE_STIPPLE);
        glLineStipple(3, 0xAAAA);
        glDrawElementsBaseVertex(GL_LINE_LOOP, 3, GL_UNSIGNED_INT,
                                 (const void*)(sizeof(uint32_t) * (drawPart.triangleOffset + (m_tweak.tri * 3))),
                                 drawPart.vertexOffset);
        glDisable(GL_LINE_STIPPLE);
      }
      if(m_tweak.edge >= 0) {
        glUniform1f(UNI_COLORMUL, 2.0f);
        glLineWidth(lineWidthBase * 2 * lineWidthScale);
        glDrawElementsBaseVertex(GL_LINES, 2, GL_UNSIGNED_INT,
                                 (const void*)(sizeof(uint32_t) * (drawPart.edgesOffset + (m_tweak.edge * 2))),
                                 drawPart.vertexOffset);
      }
    }
  }

  glDisableVertexAttribArray(VERTEX_POS);
  glDisableVertexAttribArray(VERTEX_NORMAL);

  glBindBufferBase(GL_UNIFORM_BUFFER, UBO_OBJECT, 0);
  glBindBufferBase(GL_UNIFORM_BUFFER, UBO_SCENE, 0);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, SSBO_MATERIALIDS, 0);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, SSBO_MATERIALS, 0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glDisable(GL_POLYGON_OFFSET_FILL);
  glLineWidth(1);
  glPointSize(1);
  glDisable(GL_LINE_STIPPLE);
  glUseProgram(0);

  glBindVertexArray(0);
}
}  // namespace ldrawviewer

using namespace ldrawviewer;

int main(int argc, const char** argv)
{
  NVPSystem system(PROJECT_NAME);

  Sample sample;
  return sample.run(PROJECT_NAME, argc, argv, SAMPLE_SIZE_WIDTH, SAMPLE_SIZE_HEIGHT);
}
