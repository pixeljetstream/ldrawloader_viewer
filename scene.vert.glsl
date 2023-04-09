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

in layout(location=VERTEX_POS)    vec3 inPos;
in layout(location=VERTEX_NORMAL) vec3 inNormal;

out Interpolants {
  vec3 wPos;
  vec3 wNormal;
} OUT;

void main()
{
  vec3 wPos     = (object.worldMatrix   * vec4(inPos.xyz,1)).xyz;
  vec3 wNormal  = mat3(object.worldMatrixIT) * inNormal.xyz;
  gl_Position   = view.viewProjMatrix * vec4(wPos,1);
  OUT.wPos = wPos;
  OUT.wNormal = wNormal;
}
