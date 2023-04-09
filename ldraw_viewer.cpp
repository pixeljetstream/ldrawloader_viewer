/*
* Copyright (c) 2019-2023, Christoph Kubisch. All rights reserved.
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

#define DEBUG_FILTER     1

#define USE_PROGRAM_FILTER        1
#define ALLOW_EMULATION_LAYER     1

#include <nvgl/extensions_gl.hpp>

#include <imgui/imgui_helper.h>
#include <imgui/backends/imgui_impl_gl.h>

#include <nvmath/nvmath_glsltypes.h>
#include <nvgl/glsltypes_gl.hpp>

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

namespace ldrawviewer
{
  int const SAMPLE_SIZE_WIDTH(800);
  int const SAMPLE_SIZE_HEIGHT(600);
  int const SAMPLE_MAJOR_VERSION(4);
  int const SAMPLE_MINOR_VERSION(5);

  
  class Sample : public nvgl::AppWindowProfilerGL
  {
    struct {
      nvgl::ProgramID
        draw_scene;
    } programs;

    struct {
      GLuint scene_color = 0;
      GLuint scene_depthstencil = 0;
    }textures;
    
    struct {
      GLuint scene = 0;
    }fbos;

    struct GLMultiDrawIndirect{
      uint32_t  count;
      uint32_t  instanceCount;
      uint32_t  firstIndex;
      uint32_t  baseVertex;
      uint32_t  baseInstance;
    };

    struct Scene {
      GLuint                            viewUbo = 0;
      GLuint                            objectUbo = 0;
      LdrModelHDL                       model;
      LdrRenderModelHDL                 renderModel;
      GLuint                            vbo;
      GLuint                            ibo;
      std::vector<GLMultiDrawIndirect>  instanceMdis;
    };

    struct Vertex {

      Vertex(const nvh::geometry::Vertex& vertex){
        position  = vertex.position;
        normal[0] = short(vertex.normal.x * float(32767));
        normal[1] = short(vertex.normal.y * float(32767));
        normal[2] = short(vertex.normal.z * float(32767));
        uv        = vec2(vertex.texcoord);
      }

      nvmath::vec4     position;
      short             normal[4];
      nvmath::vec2     uv;
    };
    
    struct Tweak {
      vec3        lightDir;
      bool        cull = true;
      bool        drawRenderPart = false;
      bool        edges = false;
      bool        triangles = true;
      bool        chamfered = false;
      bool        wireframe = true;
      bool        optional = false;
      bool        colors = true;
      float       transparency = 0;
      int         part = -1;
      int         tri = -1;
      int         vertex = -1;
      int         edge = -1;
    };

    nvgl::ProgramManager    m_progManager;

    ImGuiH::Registry        m_ui;
    double                  m_uiTime;

    Tweak                   m_tweak;
    Tweak                   m_lastTweak;

    ViewData                m_viewUbo;

    LdrLoaderHDL            m_loader;
    Scene                   m_scene;
    std::string             m_ldrawPath;
    std::string             m_modelFilename;
 
    nvh::CameraControl      m_control;

    bool begin() override;
    void processUI(double time);
    void think(double time) override;
    void resize(int width, int height) override;

    bool initProgram();
    bool initFramebuffers(int width, int height);
    bool initScene();

    void drawDebug();
    
    void end() override;

    // return true to prevent m_windowState updates
    bool mouse_pos(int x, int y) {
      return ImGuiH::mouse_pos(x, y);
    }
    bool mouse_button(int button, int action) {
      return ImGuiH::mouse_button(button, action);
    }
    bool mouse_wheel(int wheel) {
      return ImGuiH::mouse_wheel(wheel);
    }
    bool key_char(int button) {
      return ImGuiH::key_char(button);
    }
    bool key_button(int button, int action, int mods) {
      return ImGuiH::key_button(button, action, mods);
    }

  public:
    Sample()
    {
      m_parameterList.addFilename(".ldr", &m_modelFilename);
      m_parameterList.addFilename(".mpd", &m_modelFilename);
      m_parameterList.add("ldrawpath", &m_ldrawPath);
    }

  };

  bool Sample::initProgram()
  {
    bool validated(true);
    m_progManager.m_filetype = nvh::ShaderFileManager::FILETYPE_GLSL;
    m_progManager.addDirectory( std::string(PROJECT_NAME));
    m_progManager.addDirectory( exePath() + std::string(PROJECT_RELDIRECTORY));

    m_progManager.registerInclude("common.h", "common.h");

    programs.draw_scene = m_progManager.createProgram(
      nvgl::ProgramManager::Definition(GL_VERTEX_SHADER,          "scene.vert.glsl"),
      nvgl::ProgramManager::Definition(GL_FRAGMENT_SHADER,        "scene.frag.glsl"));

    
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
    glBindFramebuffer(GL_FRAMEBUFFER,     fbos.scene);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,        GL_TEXTURE_2D_MULTISAMPLE, textures.scene_color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D_MULTISAMPLE, textures.scene_depthstencil, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return true;
  }

  bool Sample::initScene()
  {
    {
      nvgl::newBuffer(m_scene.viewUbo);
      glNamedBufferStorage(m_scene.viewUbo, sizeof(ViewData), NULL, GL_DYNAMIC_STORAGE_BIT);
      nvgl::newBuffer(m_scene.objectUbo);
      glNamedBufferStorage(m_scene.objectUbo, sizeof(ObjectData), NULL, GL_DYNAMIC_STORAGE_BIT);
    }

    double timeLoadAll;
    double time;

    timeLoadAll = -m_profiler.getMicroSeconds();;

    LdrResult result;
    if (false){
      time = -m_profiler.getMicroSeconds();
      result = ldrCreateModel(m_loader, m_modelFilename.c_str(), LDR_FALSE, &m_scene.model);
      assert(result == LDR_SUCCESS || result == LDR_WARNING_PART_NOT_FOUND);
      time += m_profiler.getMicroSeconds();
      printf("dependency time %.2f ms\n", time / 1000.0f);
      
      // threaded loaded
      time = -m_profiler.getMicroSeconds();
      

      uint32_t numParts = ldrGetNumRegisteredParts(m_loader);
      uint32_t numThreads = std::thread::hardware_concurrency();
      uint32_t perThread = (numParts + numThreads - 1) / numThreads;

      std::vector<LdrPartID> partIds(numParts);

      std::vector<std::thread>  threads(numThreads);
      for (uint32_t i = 0; i < numThreads; i++)
      {
        threads[i] = std::thread( [&](uint32_t idx){
          uint32_t offset = idx * perThread;
          uint32_t numLocal = offset > numParts ? 0 : std::min(perThread, numParts - idx * perThread);
          if (numLocal){
            for (uint32_t p = 0; p < numLocal; p++){
               partIds[offset + p] = (LdrPartID)(offset + p);
            }
            ldrLoadDeferredParts(m_loader, numLocal, &partIds[offset], sizeof(LdrPartID));
          }
        }, i);
      }
      for (uint32_t i = 0; i < numThreads; i++){
        threads[i].join();
      }
      ldrResolveModel(m_loader, m_scene.model);

      time += m_profiler.getMicroSeconds();
      printf("threaded time %.2f ms\n", time / 1000.0f);
    }
    else {
      time = -m_profiler.getMicroSeconds();
      result = ldrCreateModel(m_loader, m_modelFilename.c_str(), LDR_TRUE, &m_scene.model);
      assert(result == LDR_SUCCESS || result == LDR_WARNING_PART_NOT_FOUND);
      time += m_profiler.getMicroSeconds();
      printf("load time %.2f ms\n", time / 1000.0f);
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

    result = ldrCreateRenderModel(m_loader, m_scene.model, LDR_TRUE, &m_scene.renderModel);
    assert(result == LDR_SUCCESS || result == LDR_WARNING_PART_NOT_FOUND);

    printf("build time %.2f ms\n", time / 1000.0f);

    srand(1238);

    return true;
  }


  bool Sample::begin()
  {
    ImGuiH::Init(m_windowState.m_winSize[0], m_windowState.m_winSize[1], this);
    ImGui::InitGL();

    LdrLoaderCreateInfo  createInfo = {};
    createInfo.partFixMode = LDR_PART_FIX_NONE;
    createInfo.renderpartBuildMode = LDR_RENDERPART_BUILD_ONLOAD;
    createInfo.partFixTjunctions = LDR_TRUE;
    createInfo.partHiResPrimitives = LDR_FALSE;
    createInfo.renderpartChamfer = 0.35f;
    createInfo.basePath = m_ldrawPath.c_str();
    LdrResult result = ldrCreateLoader(&createInfo, &m_loader);
    assert(result == LDR_SUCCESS);

    bool validated(true);

    //GLuint defaultVAO;
    //glGenVertexArrays(1, &defaultVAO);
    //glBindVertexArray(defaultVAO);

    validated = validated && initProgram();
    validated = validated && initFramebuffers(m_windowState.m_winSize[0],m_windowState.m_winSize[1]);
    validated = validated && initScene();

    m_tweak.lightDir = normalize(vec3(-1,1,1));

    m_control.m_sceneOrbit      = vec3(0.0f);
    m_control.m_sceneDimension  = 1000.0f;
    m_control.m_viewMatrix      = nvmath::look_at(m_control.m_sceneOrbit - vec3(0,0,-m_control.m_sceneDimension), m_control.m_sceneOrbit, vec3(0,1,0));

    return validated;
  }

  void Sample::end()
  {
     ldrDestroyRenderModel(m_loader, m_scene.renderModel);
     ldrDestroyModel(m_loader, m_scene.model);
     ldrDestroyLoader(m_loader);
     ImGui::ShutdownGL();
  }

  void Sample::processUI(double time)
  {

    int width = m_windowState.m_winSize[0];
    int height = m_windowState.m_winSize[1];

    // Update imgui configuration
    auto &imgui_io = ImGui::GetIO();
    imgui_io.DeltaTime = static_cast<float>(time - m_uiTime);
    imgui_io.DisplaySize = ImVec2(width, height);

    m_uiTime = time;

    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("NVIDIA " PROJECT_NAME, nullptr)) {
      ImGui::Checkbox("colors", &m_tweak.colors);
      ImGui::Checkbox("bf cull", &m_tweak.cull);
      ImGui::SliderFloat("transparency", &m_tweak.transparency, 0, 1);
      ImGui::Checkbox("edges", &m_tweak.edges);
      ImGui::Checkbox("triangles", &m_tweak.triangles);
      ImGui::Checkbox("wireframe", &m_tweak.wireframe);
      ImGui::Checkbox("optional", &m_tweak.optional);
      ImGui::Checkbox("draw render part", &m_tweak.drawRenderPart);
      ImGui::Checkbox("draw render part chamfer", &m_tweak.chamfered);
      ImGui::InputInt("part", &m_tweak.part);
      ImGui::InputInt("vertex", &m_tweak.vertex);
      ImGui::InputInt("tri", &m_tweak.tri);
      ImGui::InputInt("edge", &m_tweak.edge);
      if (m_tweak.vertex >= 0 && m_tweak.part >= 0){
        const LdrPart*     part     = ldrGetPart(m_loader, m_tweak.part);
        const LdrRenderPart* rpart  = ldrGetRenderPart(m_loader, m_tweak.part);

        const float* pos = m_tweak.drawRenderPart ? &rpart->vertices[m_tweak.vertex].position.x : &part->positions[m_tweak.vertex].x;
        ImGui::Text("%.3f %.3f %.3f\n", pos[0], pos[1], pos[2]);
      }
      if (m_tweak.part >= 0){
        const LdrPart*     part     = ldrGetPart(m_loader, m_tweak.part);
        ImGui::Text("%s\n", part->name);
      }
    }
    ImGui::End();
  }

  void Sample::think(double time)
  {
    NV_PROFILE_GL_SECTION("Frame");

    processUI(time);

    m_control.processActions(m_windowState.m_winSize,
      nvmath::vec2f(m_windowState.m_mouseCurrent[0],m_windowState.m_mouseCurrent[1]),
      m_windowState.m_mouseButtonFlags, m_windowState.m_mouseWheel);

		int width = m_windowState.m_winSize[0];
    int height  = m_windowState.m_winSize[1];

    if (m_windowState.onPress(KEY_R)){
      m_progManager.reloadPrograms();
    }
    if (!m_progManager.areProgramsValid()){
      waitEvents();
      return;
    }

    {
      NV_PROFILE_GL_SECTION("Setup");
      m_viewUbo.viewport = uvec2(width,height);

      nvmath::mat4 projection = nvmath::perspective(45.f, float(width)/float(height), 0.1f, 1000000.0f);
      nvmath::mat4 view = m_control.m_viewMatrix;

      m_viewUbo.viewProjMatrix = projection * view;
      m_viewUbo.viewProjMatrixI = nvmath::invert(m_viewUbo.viewProjMatrix);
      m_viewUbo.viewMatrix = view;
      m_viewUbo.viewMatrixI = nvmath::invert(view);
      m_viewUbo.viewMatrixIT = nvmath::transpose(m_viewUbo.viewMatrixI );
      m_viewUbo.wLightPos = vec4((m_tweak.lightDir * m_control.m_sceneDimension),1.0f);
      m_viewUbo.time = float(time);
      m_viewUbo.opacity = 1.0f - m_tweak.transparency;

      glNamedBufferSubData(m_scene.viewUbo,0,sizeof(ViewData),&m_viewUbo);

      glBindFramebuffer(GL_FRAMEBUFFER, fbos.scene);
      glViewport(0, 0, width, height);

      nvmath::vec4   bgColor(0.2,0.2,0.2,0.0);
      glClearColor(bgColor.x,bgColor.y,bgColor.z,bgColor.w);
      glClearDepth(1.0);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }

    {
      NV_PROFILE_GL_SECTION("Draw");
      drawDebug();
    }

    {
      NV_PROFILE_GL_SECTION("Blit");
      glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos.scene);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      glBlitFramebuffer(0,0,width,height,
        0,0,width,height,GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }

    {
      NV_PROFILE_GL_SECTION("GUI");
      ImGui::Render();
      ImGui::RenderDrawDataGL(ImGui::GetDrawData());
    }

    ImGui::EndFrame();
  }

  void Sample::resize(int width, int height)
  {
    initFramebuffers(width,height);
  }

  void Sample::drawDebug()
  {
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glUseProgram(m_progManager.get(programs.draw_scene ));

    //glVertexAttribFormat(VERTEX_POS,    4, GL_FLOAT, GL_FALSE,  0);
    //glVertexAttribBinding(VERTEX_POS,   0);

    glPolygonOffset(1,1);
    glPointSize(8);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glLineStipple(2,0xAAAA);

    glEnableVertexAttribArray(VERTEX_POS);
    if (m_tweak.drawRenderPart){
      glEnableVertexAttribArray(VERTEX_NORMAL);
    }

    glBindBufferBase(GL_UNIFORM_BUFFER, UBO_SCENE,  m_scene.viewUbo);
    glBindBufferBase(GL_UNIFORM_BUFFER, UBO_OBJECT, m_scene.objectUbo);

    bool cullFace = true;
    bool ccw = true;

    float widthScale = 2.0f;

    glFrontFace(GL_CCW);
    glLineWidth(1);

    srand(1123);

    if (m_tweak.transparency){
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE);
      glDisable(GL_DEPTH_TEST);
    }
    else {
      glDisable(GL_BLEND);
    }

    float wireColor = 0.5f;

    LdrModelHDL model = m_scene.model;
    for (uint32_t i = 0; i < model->num_instances; i++){
      const LdrInstance* instance = &model->instances[i];
      const LdrPart*     part     = ldrGetPart(m_loader, instance->part);
      const LdrRenderPart* rpart  = ldrGetRenderPart(m_loader, instance->part);

      if (m_tweak.part >= 0 && instance->part != m_tweak.part) continue;

      ObjectData obj;
      obj.color = {nvh::frand(),nvh::frand(),nvh::frand(),1.0f};

      const LdrMaterial* mtl = ldrGetMaterial(m_loader, instance->material);
      if (mtl && m_tweak.colors){
        obj.color = {float(mtl->baseColor[0])/float(255.0f), float(mtl->baseColor[1])/float(255.0f), float(mtl->baseColor[2])/float(255.0f), 1};
      }

      memcpy(obj.worldMatrix.mat_array, &instance->transform, sizeof(LdrMatrix));
      obj.worldMatrixIT = nvmath::transpose(nvmath::invert(obj.worldMatrix));
      float det = nvmath::det(obj.worldMatrix);

      glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(ObjectData), &obj);

      if (cullFace != !(bool(part->flag.hasNoBackFaceCulling) || !m_tweak.cull)){
        if (cullFace){
          glDisable(GL_CULL_FACE);
        }
        else {
          glEnable(GL_CULL_FACE);
        }
        cullFace = !(bool(part->flag.hasNoBackFaceCulling) || !m_tweak.cull);
      }

      if (ccw != det > 0){
        glFrontFace(det > 0 ? GL_CCW : GL_CW);
        ccw = det > 0;
      }

      if (!m_tweak.drawRenderPart){
        glUniform1i(1, 0);
        glUniform1f(0, 1.0f);
        glVertexAttribPointer(VERTEX_POS, 3, GL_FLOAT, GL_FALSE, sizeof(LdrVector), part->positions);
        if (m_tweak.triangles){
          glDrawElements(GL_TRIANGLES, part->num_triangles * 3, GL_UNSIGNED_INT, part->triangles);
        }
        glUniform1f(0, 0.2f);
        if (m_tweak.edges){
          glLineWidth(1 * widthScale);
          glDrawElements(GL_LINES, part->num_lines * 2, GL_UNSIGNED_INT, part->lines);
        }

        if (m_tweak.optional){
          glLineWidth(1 * widthScale);
          glLineStipple(4,0xAAAA);
          glEnable(GL_LINE_STIPPLE);
          glDrawElements(GL_LINES, part->num_optional_lines * 2, GL_UNSIGNED_INT, part->optional_lines);
          glDisable(GL_LINE_STIPPLE);
        }

        if (m_tweak.wireframe){
          glLineWidth(1);
          glUniform1f(0, wireColor);
          glLineStipple(2,0xAAAA);
          glEnable(GL_LINE_STIPPLE);
          glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
          glDrawElements(GL_TRIANGLES, part->num_triangles * 3, GL_UNSIGNED_INT, part->triangles);
          glDisable(GL_LINE_STIPPLE);
          glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }
      }
      else {
        const LdrVertexIndex* triangles = m_tweak.chamfered  && rpart->flag.canChamfer ?  rpart->trianglesC : rpart->triangles;
        uint32_t num_triangles = m_tweak.chamfered && rpart->flag.canChamfer ? rpart->num_trianglesC : rpart->num_triangles;

        glUniform1i(1,1);
        glUniform1f(0, 1.0f);
        glVertexAttribPointer(VERTEX_POS, 3, GL_FLOAT, GL_FALSE, sizeof(LdrRenderVertex), &rpart->vertices[0].position);
        glVertexAttribPointer(VERTEX_NORMAL, 3, GL_FLOAT, GL_FALSE, sizeof(LdrRenderVertex), &rpart->vertices[0].normal);
        if (m_tweak.triangles){
          glDrawElements(GL_TRIANGLES, num_triangles * 3, GL_UNSIGNED_INT, triangles);
        }
        glUniform1f(0, 0.2f);
        glUniform1i(1,0);
        if (m_tweak.edges){
          glLineWidth(1 * widthScale);
          glDrawElements(GL_LINES, rpart->num_lines * 2, GL_UNSIGNED_INT, rpart->lines);
        }

        if (m_tweak.wireframe){
          glLineWidth(1);
          glUniform1f(0, wireColor);
          glEnable(GL_LINE_STIPPLE);
          glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
          glDrawElements(GL_TRIANGLES, num_triangles * 3, GL_UNSIGNED_INT, triangles);
          glDisable(GL_LINE_STIPPLE);
          glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }
      }

      if (instance->part == m_tweak.part) {
        if (m_tweak.vertex >= 0){
          glUniform1f(0, 2.0f);
          glDrawArrays(GL_POINTS, m_tweak.vertex, 1);
        }
        if (m_tweak.tri >= 0){
          const LdrVertexIndex* triangles = m_tweak.drawRenderPart ? rpart->triangles : part->triangles;
          glUniform1f(0, 2.0f);
          glLineWidth(4 * widthScale);
          glDrawElements(GL_LINE_LOOP, 3, GL_UNSIGNED_INT, triangles +  (m_tweak.tri * 3));
        }
        if (m_tweak.edge >= 0){
          const LdrVertexIndex* lines = m_tweak.drawRenderPart ? rpart->lines : part->lines;
          glUniform1f(0, 1.5f);
          glLineWidth(2 * widthScale);
          glDrawElements(GL_LINES, 2, GL_UNSIGNED_INT, lines +  (m_tweak.edge * 2));
        }
      }
    }

    glDisableVertexAttribArray(VERTEX_POS);

    glBindBufferBase(GL_UNIFORM_BUFFER, UBO_SCENE, 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, UBO_OBJECT, 0);
    glBindVertexBuffer(0,0,0,0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    glDisable(GL_POLYGON_OFFSET_FILL);
  }
}

using namespace ldrawviewer;

int main(int argc, const char** argv)
{
  NVPSystem system(PROJECT_NAME);

  Sample sample;
  return sample.run(
    PROJECT_NAME,
    argc, argv,
    SAMPLE_SIZE_WIDTH, SAMPLE_SIZE_HEIGHT);
}

