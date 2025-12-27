//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

struct Constants
{
    float4x4 World;
    float4x4 WorldView;
    float4x4 WorldViewProj;
    uint     DrawMeshlets;
};

struct VertexOut
{
    float4 PositionHS   : SV_Position;
    float3 PositionVS   : POSITION0;
    float3 Normal       : NORMAL0;
    float2 TexCoord     : TEXCOORD0;
    uint   MeshletIndex : COLOR0;
};

ConstantBuffer<Constants> Globals : register(b0);
Texture2D<float4> DiffuseTexture : register(t4);
SamplerState LinearSampler : register(s0);

float4 main(VertexOut input) : SV_TARGET
{
    // Sample texture for diffuse color
    float3 diffuseColor = DiffuseTexture.Sample(LinearSampler, input.TexCoord).rgb;
    
    // Simple ambient + diffuse lighting (no specular)
    float ambientIntensity = 0.3;
    float3 lightDir = normalize(float3(1, 1, -1));
    float3 normal = normalize(input.Normal);
    
    float diffuse = saturate(dot(normal, lightDir)) * 0.7;
    float3 finalColor = diffuseColor * (ambientIntensity + diffuse);

    return float4(finalColor, 1);
}
