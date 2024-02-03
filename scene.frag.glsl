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


#version 430
/**/

#extension GL_ARB_shading_language_include : enable
#include "common.h"

in Interpolants {
  vec3 wPos;
  vec3 wNormal;
} IN;

layout(location=0,index=0) out vec4 out_Color;

layout(location=UNI_COLORMUL) uniform float colorMul;
layout(location=UNI_LIGHTING) uniform bool lighting;
layout(location=UNI_MATERIALID) uniform uint materialID;
layout(location=UNI_MATERIALIDOFFSET) uniform uint materialIDOffset;

void main()
{
  vec4 objColor = max(vec4(0.1),object.color);
  if (view.useObjectColor != 0)
  {
    uint usedMaterialID = materialID;
  
    if (materialIDOffset != ~0) {
      // using gl_PrimitiveID may not be exactly fast
      // more portable is to split vertices along material edges and encode materialID within them
      // should add support in loader library for that
      usedMaterialID = materialIndices[materialIDOffset + gl_PrimitiveID];
      if (usedMaterialID == 16) {
        usedMaterialID = materialID;
      }
    }
  
    if (usedMaterialID == 16) {
      objColor = view.inheritColor;
    }
    else {
      objColor = materials[usedMaterialID].color;
    }
  }
  

  if (lighting){  
    vec3 wEyePos = vec3(view.viewMatrixIT[0].w,view.viewMatrixIT[1].w,view.viewMatrixIT[2].w);

    //vec3 lightDir = normalize(view.wLightPos.xyz - IN.wPos);
    vec3 viewDir  = normalize(wEyePos - IN.wPos);
    vec3 lightDir = viewDir;
    vec3 halfDir  = normalize(lightDir + viewDir);
    vec3 normal   = normalize(IN.wNormal);
  
    float ndotl = dot(normal,lightDir);
    float intensity = max(0,ndotl) + min(0,ndotl) * 0.2 + 0.2;
    //intensity += pow(max(0,dot(normal,halfDir)),8);
    
    out_Color = objColor * intensity;
    //out_Color = vec4(IN.wNormal * 0.5 + 0.5, 1);
  }
  else {
    out_Color = objColor * colorMul;
  }
  
  if (!gl_FrontFacing)
  {
    ivec2 spos = ivec2(gl_FragCoord.xy) / 8;
    int state = (spos.x & 1) ^ (spos.y & 1);
    out_Color += (float(state)-0.5)*0.1;
  }

  out_Color.w = view.opacity;
}
