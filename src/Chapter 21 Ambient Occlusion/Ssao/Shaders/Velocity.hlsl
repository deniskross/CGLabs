//=============================================================================
// Velocity.hlsl - Motion Vector Generation Shader
// Implementation based on: https://alextardif.com/TAA.html
// 
// Generates per-pixel velocity vectors by comparing current and previous
// frame positions. Used for TAA reprojection.
//=============================================================================

#include "Common.hlsl"

struct VertexIn
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC    : TEXCOORD;
    float3 TangentU : TANGENT;
};

struct VertexOut
{
    float4 PosH      : SV_POSITION;
    float4 CurrPosH  : POSITION0;
    float4 PrevPosH  : POSITION1;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    
    // Current frame: world position -> clip space with current ViewProj
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosH = mul(posW, gViewProj);
    vout.CurrPosH = vout.PosH;
    
    // Previous frame: world position (with previous world matrix) -> clip space with previous ViewProj
    float4 prevPosW = mul(float4(vin.PosL, 1.0f), gPrevWorld);
    vout.PrevPosH = mul(prevPosW, gPrevViewProj);
    
    return vout;
}

float2 PS(VertexOut pin) : SV_Target
{
    // Convert clip space to NDC [-1, 1]
    float2 currNDC = pin.CurrPosH.xy / pin.CurrPosH.w;
    float2 prevNDC = pin.PrevPosH.xy / pin.PrevPosH.w;
    
    // Convert NDC to UV space [0, 1]
    // NDC: (-1,-1) bottom-left, (1,1) top-right
    // UV:  (0,0) top-left, (1,1) bottom-right
    float2 currUV = currNDC * float2(0.5f, -0.5f) + 0.5f;
    float2 prevUV = prevNDC * float2(0.5f, -0.5f) + 0.5f;
    
    // Velocity = current UV - previous UV
    // This gives us the motion vector to go from current to previous frame
    // In TAA resolve, we use: historyUV = currentUV - velocity
    float2 velocity = currUV - prevUV;
    
    return velocity;
}
