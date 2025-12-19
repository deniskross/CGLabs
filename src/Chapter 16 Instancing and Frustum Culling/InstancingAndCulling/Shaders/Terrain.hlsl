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
    // UV offset and scale for texture atlas lookup
    float2 UVOffset;
    float2 UVScale;
};

// Texture arrays for different LOD levels (21 textures total)
Texture2D gHeightMaps[21] : register(t0);      // Base height data from Gaea
Texture2D gDiffuseMaps[21] : register(t21);    // Albedo/color textures  
Texture2D gNormalMaps[21] : register(t42);     // Normal maps for lighting
Texture2D<float> gSculptMap : register(t63);   // Real-time height modifications (R32_FLOAT)

// Per-tile instance data for GPU-driven rendering
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
    float2 TexC : TEXCOORD0;
    float Z : TEXCOORD1;      // Elevation for color lookup (GPU Gems 2)
    float Alpha : TEXCOORD2;  // Transition blend factor (GPU Gems 2)
    nointerpolation uint InstanceID : INSTANCEID;
};

// Combined height sampling: base heightmap + real-time sculpting modifications
// This enables dynamic terrain editing without regenerating base textures
float SampleHeight(int heightMapIndex, float2 uv, float2 globalUV)
{
    // Clamp UV to prevent texture bleeding at tile edges
    uv = clamp(uv, 0.001, 0.999);
    
    // Sample base height from pre-generated heightmap (normalized [0,1])
    float h = gHeightMaps[heightMapIndex].SampleLevel(gsamLinearClamp, uv, 0).r;
    
    // Sample sculpting modifications (world-space height deltas)
    float sculptMod = gSculptMap.SampleLevel(gsamLinearClamp, globalUV, 0);
    
    // Combine: base_height * scale + sculpt_delta
    return h * gTerrainHeight + sculptMod * gTerrainHeight;
}

// Vertex shader
VertexOut VS(VertexIn vin, uint instanceID : SV_InstanceID)
{
    VertexOut vout = (VertexOut)0;
    
    TerrainTileInstance inst = gTileInstances[instanceID];
    
    // Tile-specific UV transformation for texture atlas lookup
    // Each tile uses a portion of its LOD texture based on UVScale and UVOffset
    float2 uv = vin.TexC * inst.UVScale + inst.UVOffset;
    
    // Global UV calculation for sculpt map sampling
    // Need terrain-wide coordinates [0,1] for sculpt map, not tile-local coordinates
    float4 localPos = float4(vin.PosL.x, 0, vin.PosL.z, 1);  // Local tile position
    float4 worldPos = mul(localPos, inst.World);              // Transform to world space
    float2 globalUV = (worldPos.xz / gTerrainSize) + 0.5f;   // World [-size/2,size/2] → UV [0,1]
    
    // Sample terrain elevation with sculpting applied
    float zf = SampleHeight(inst.HeightMapIndex, uv, globalUV);
    
    // Skirt geometry: vertical strips at tile edges to hide LOD seams
    // Skirt vertices are marked with Y=-1 in mesh generation
    bool isSkirt = (vin.PosL.y < -0.5f);
    float skirtDrop = 30.0f;  // How far skirts extend below terrain surface
    
    // Compute final vertex position with height displacement
    float3 posL;
    if (isSkirt)
    {
        posL = float3(vin.PosL.x, zf - skirtDrop, vin.PosL.z);  // Drop skirt below surface
    }
    else
    {
        posL = float3(vin.PosL.x, zf, vin.PosL.z);  // Normal vertex at terrain height
    }
    
    // Transform to world space using per-tile world matrix
    float4 posW = mul(float4(posL, 1.0f), inst.World);
    
    // LOD transition blending (alpha parameter for smooth LOD transitions)
    
    // Distance-based LOD transition zones for smooth blending between detail levels
    // Each LOD level has different transition thresholds to prevent popping artifacts
    float2 viewerPos = gEyePosW.xz;
    float2 worldPosXZ = posW.xz;
    float distToViewer = length(worldPosXZ - viewerPos);
    
    float alphaOffset, transitionWidth;
    if (inst.LODLevel == 2)      // Finest detail level
    {
        alphaOffset = gTerrainSize * 0.12f;      // Start transition at 12% of terrain size
        transitionWidth = gTerrainSize * 0.06f;  // Transition zone width
    }
    else if (inst.LODLevel == 1) // Medium detail level
    {
        alphaOffset = gTerrainSize * 0.30f;      // Start transition at 30% of terrain size
        transitionWidth = gTerrainSize * 0.10f;  // Wider transition zone
    }
    else                         // Coarsest detail level
    {
        alphaOffset = gTerrainSize * 2.0f;       // Very far transition (essentially no blending)
        transitionWidth = gTerrainSize * 0.5f;
    }
    
    // Compute alpha for LOD blending: α = saturate((distance - offset) / width)
    float alpha = saturate((distToViewer - alphaOffset) / transitionWidth);
    
    // Output vertex attributes
    vout.PosW = posW.xyz;                    // World position for lighting calculations
    vout.PosH = mul(posW, gViewProj);        // Clip-space position for rasterization
    vout.TexC = uv;                          // Texture coordinates for pixel shader
    vout.Z = zf / gTerrainHeight;            // Normalized elevation [0,1] for color mapping
    vout.Alpha = alpha;                      // LOD transition parameter
    vout.InstanceID = instanceID;            // Pass instance ID to pixel shader
    
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    TerrainTileInstance inst = gTileInstances[pin.InstanceID];
    
    // Sample normal map (stored as RGB with normals in tangent space)
    float4 normalSample = gNormalMaps[inst.NormalMapIndex].Sample(gsamLinearClamp, pin.TexC);
    
    // Unpack normal from [0,1] to [-1,+1] range
    float3 normal = normalize(normalSample.xyz * 2.0f - 1.0f);
    
    // Transform from tangent space to world space
    // For terrain, tangent space is approximately aligned with world XZ plane
    float3 normalW = normalize(float3(normal.x, normal.z, normal.y));
    
    // GPU Gems 2: Compute diffuse lighting
    // s = clamp(dot(normal, LightDirection), 0, 1)
    float3 lightDir = normalize(gLights[0].Direction);
    float s = saturate(dot(normalW, -lightDir));
    
    // GPU Gems 2: Elevation-based color
    // Instead of 1D texture lookup, we use gradient based on elevation (z)
    // Low elevation = darker/green, High elevation = lighter/brown-white
    float z = saturate(pin.Z);
    
    // Elevation color gradient (similar to ZBasedColorSampler)
    float4 lowColor = float4(0.2f, 0.35f, 0.15f, 1.0f);   // Dark green (low)
    float4 midColor = float4(0.45f, 0.40f, 0.30f, 1.0f);  // Brown (mid)
    float4 highColor = float4(0.9f, 0.9f, 0.85f, 1.0f);   // Snow/rock (high)
    
    float4 elevationColor;
    if (z < 0.4f)
    {
        elevationColor = lerp(lowColor, midColor, z / 0.4f);
    }
    else
    {
        elevationColor = lerp(midColor, highColor, (z - 0.4f) / 0.6f);
    }
    
    // Sample diffuse texture and blend with elevation color
    float4 diffuseAlbedo = gDiffuseMaps[inst.DiffuseMapIndex].Sample(gsamAnisotropicWrap, pin.TexC);
    diffuseAlbedo = lerp(diffuseAlbedo, elevationColor, 0.3f); // 30% elevation tint
    diffuseAlbedo *= gTerrainDiffuse;
    
    // Ambient + diffuse lighting (GPU Gems 2 simplified model)
    float4 ambient = gAmbientLight * diffuseAlbedo;
    float4 diffuse = s * diffuseAlbedo;
    
    float4 litColor = ambient + diffuse;
    litColor.a = 1.0f;
    
    return litColor;
}

// Wireframe mode: color by LOD level
// Red = Level 2 (finest, closest to camera)
// Green = Level 1 (medium)
// Blue = Level 0 (coarsest, farthest)
float4 PS_Wireframe(VertexOut pin) : SV_Target
{
    TerrainTileInstance inst = gTileInstances[pin.InstanceID];
    
    if (inst.LODLevel == 2)
        return float4(1, 0, 0, 1);  // Red - finest
    else if (inst.LODLevel == 1)
        return float4(0, 1, 0, 1);  // Green - medium
    else
        return float4(0, 0, 1, 1);  // Blue - coarsest
}