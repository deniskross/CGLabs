//***************************************************************************************
// Terrain.hlsl - Simple terrain with distance-based LOD
//***************************************************************************************

#ifndef NUM_DIR_LIGHTS
    #define NUM_DIR_LIGHTS 3
#endif

#ifndef NUM_POINT_LIGHTS
    #define NUM_POINT_LIGHTS 0
#endif

#ifndef NUM_SPOT_LIGHTS
    #define NUM_SPOT_LIGHTS 0
#endif

#include "LightingUtil.hlsl"

struct TerrainTileInstance
{
    float4x4 World;
    int HeightMapIndex;
    int DiffuseMapIndex;
    int NormalMapIndex;
    int LODLevel;
};

Texture2D gHeightMaps[21] : register(t0);
Texture2D gDiffuseMaps[21] : register(t21);
Texture2D gNormalMaps[21] : register(t42);

StructuredBuffer<TerrainTileInstance> gTileInstances : register(t0, space1);

SamplerState gsamPointClamp : register(s0);
SamplerState gsamLinearClamp : register(s1);
SamplerState gsamLinearWrap : register(s2);
SamplerState gsamAnisotropicWrap : register(s3);

cbuffer cbPass : register(b0)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float3 gEyePosW;
    float cbPerObjectPad1;
    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;
    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;
    float4 gAmbientLight;
    Light gLights[MaxLights];
};

cbuffer cbTerrain : register(b1)
{
    float gTerrainHeight;
    float gTerrainSize;
    float gTexelSize;
    float gPad0;
    float4 gTerrainDiffuse;
    float3 gTerrainFresnelR0;
    float gTerrainRoughness;
};

struct VertexIn
{
    float3 PosL : POSITION;
    float2 TexC : TEXCOORD;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD0;
    nointerpolation uint InstanceID : INSTANCEID;
};

float SampleHeight(int heightMapIndex, float2 uv)
{
    // Clamp UV to avoid edge artifacts
    uv = clamp(uv, 0.001, 0.999);
    float h = gHeightMaps[heightMapIndex].SampleLevel(gsamLinearClamp, uv, 0).r;
    return h * gTerrainHeight;
}

float3 CalculateNormal(int heightMapIndex, float2 uv, float tileWorldSize)
{
    float texelSize = 1.0 / 512.0;
    
    float hL = SampleHeight(heightMapIndex, uv - float2(texelSize, 0));
    float hR = SampleHeight(heightMapIndex, uv + float2(texelSize, 0));
    float hD = SampleHeight(heightMapIndex, uv - float2(0, texelSize));
    float hU = SampleHeight(heightMapIndex, uv + float2(0, texelSize));
    
    float worldTexel = tileWorldSize * texelSize;
    
    float3 normal;
    normal.x = (hL - hR);
    normal.z = (hD - hU);
    normal.y = 2.0 * worldTexel;
    
    return normalize(normal);
}

VertexOut VS(VertexIn vin, uint instanceID : SV_InstanceID)
{
    VertexOut vout = (VertexOut)0;
    
    TerrainTileInstance inst = gTileInstances[instanceID];
    
    // UV for heightmap sampling
    float2 uv = vin.TexC;
    
    // Sample height at this UV
    float height = SampleHeight(inst.HeightMapIndex, uv);
    
    // Local position: XZ from vertex, Y from heightmap
    float3 posL = float3(vin.PosL.x, height, vin.PosL.z);
    
    // Transform to world
    float4 posW = mul(float4(posL, 1.0f), inst.World);
    vout.PosW = posW.xyz;
    
    // Get tile size from world matrix scale (X scale)
    float tileWorldSize = length(float3(inst.World[0][0], inst.World[1][0], inst.World[2][0]));
    
    // Calculate normal from heightmap
    vout.NormalW = CalculateNormal(inst.HeightMapIndex, uv, tileWorldSize);
    
    // Clip space
    vout.PosH = mul(posW, gViewProj);
    
    // UV for diffuse texture
    vout.TexC = uv;
    vout.InstanceID = instanceID;
    
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    TerrainTileInstance inst = gTileInstances[pin.InstanceID];
    
    float4 diffuseAlbedo = gDiffuseMaps[inst.DiffuseMapIndex].Sample(gsamAnisotropicWrap, pin.TexC);
    diffuseAlbedo *= gTerrainDiffuse;
    
    float3 normalW = normalize(pin.NormalW);
    float3 toEyeW = normalize(gEyePosW - pin.PosW);
    
    float4 ambient = gAmbientLight * diffuseAlbedo;
    
    const float shininess = 1.0f - gTerrainRoughness;
    Material mat = { diffuseAlbedo, gTerrainFresnelR0, shininess };
    
    float3 shadowFactor = 1.0f;
    float4 directLight = ComputeLighting(gLights, mat, pin.PosW, normalW, toEyeW, shadowFactor);
    
    float4 litColor = ambient + directLight;
    litColor.a = 1.0f;
    
    return litColor;
}

float4 PS_Wireframe(VertexOut pin) : SV_Target
{
    TerrainTileInstance inst = gTileInstances[pin.InstanceID];
    
    // L0=Blue, L1=Green, L2=Red
    if (inst.LODLevel == 0)
        return float4(0, 0, 1, 1);
    else if (inst.LODLevel == 1)
        return float4(0, 1, 0, 1);
    else
        return float4(1, 0, 0, 1);
}
