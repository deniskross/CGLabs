//=============================================================================
// Taa.hlsl - Temporal Anti-Aliasing Resolve Shader
// Implementation strictly based on:
// - https://sugulee.wordpress.com/2021/06/21/temporal-anti-aliasingtaa-tutorial/
// - https://alextardif.com/TAA.html
// - https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/
//=============================================================================

cbuffer cbTaa : register(b0)
{
    float4x4 gPrevViewProj;
    float4x4 gInvViewProj;
    float2 gJitterOffset;
    float2 gInvRenderTargetSize;
    float gBlendFactor;
    float gMotionScale;
    uint gFrameCount;
    uint gTaaPad0;
};

Texture2D gHistoryBuffer  : register(t0);
Texture2D gCurrentColor   : register(t1);
Texture2D gVelocityBuffer : register(t2);

SamplerState gsamPointClamp  : register(s0);
SamplerState gsamLinearClamp : register(s1);

static const float2 gTexCoords[6] =
{
    float2(0.0f, 1.0f),
    float2(0.0f, 0.0f),
    float2(1.0f, 0.0f),
    float2(0.0f, 1.0f),
    float2(1.0f, 0.0f),
    float2(1.0f, 1.0f)
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD0;
};

VertexOut VS(uint vid : SV_VertexID)
{
    VertexOut vout;
    vout.TexC = gTexCoords[vid];
    vout.PosH = float4(2.0f * vout.TexC.x - 1.0f, 1.0f - 2.0f * vout.TexC.y, 0.0f, 1.0f);
    return vout;
}

//-----------------------------------------------------------------------------
// Color space conversions (from Alex Tardif's tutorial)
// YCoCg provides better results for neighborhood clamping
//-----------------------------------------------------------------------------
float3 RGBToYCoCg(float3 rgb)
{
    return float3(
        0.25f * rgb.r + 0.5f * rgb.g + 0.25f * rgb.b,
        0.5f * rgb.r - 0.5f * rgb.b,
        -0.25f * rgb.r + 0.5f * rgb.g - 0.25f * rgb.b
    );
}

float3 YCoCgToRGB(float3 ycocg)
{
    return float3(
        ycocg.x + ycocg.y - ycocg.z,
        ycocg.x + ycocg.z,
        ycocg.x - ycocg.y - ycocg.z
    );
}

//-----------------------------------------------------------------------------
// Luminance for weighting (from Elopezr's tutorial)
//-----------------------------------------------------------------------------
float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

//-----------------------------------------------------------------------------
// Clip to AABB (from Alex Tardif's tutorial)
// Clips color towards the center of the AABB
//-----------------------------------------------------------------------------
float3 ClipAABB(float3 aabbMin, float3 aabbMax, float3 history)
{
    float3 aabbCenter = 0.5f * (aabbMax + aabbMin);
    float3 extentClip = 0.5f * (aabbMax - aabbMin) + 0.001f;
    
    float3 colorVector = history - aabbCenter;
    float3 colorUnit = colorVector / extentClip;
    float maxUnit = max(abs(colorUnit.x), max(abs(colorUnit.y), abs(colorUnit.z)));
    
    if (maxUnit > 1.0f)
        return aabbCenter + colorVector / maxUnit;
    else
        return history;
}

//-----------------------------------------------------------------------------
// Catmull-Rom bicubic filter for sharper history sampling
// (from Sugulee's tutorial - reduces blur)
//-----------------------------------------------------------------------------
float3 SampleHistoryCatmullRom(float2 uv)
{
    float2 texSize;
    gHistoryBuffer.GetDimensions(texSize.x, texSize.y);
    float2 invTexSize = 1.0f / texSize;
    
    float2 position = uv * texSize;
    float2 centerPosition = floor(position - 0.5f) + 0.5f;
    float2 f = position - centerPosition;
    float2 f2 = f * f;
    float2 f3 = f * f2;
    
    // Catmull-Rom weights
    float2 w0 = -0.5f * f3 + f2 - 0.5f * f;
    float2 w1 = 1.5f * f3 - 2.5f * f2 + 1.0f;
    float2 w2 = -1.5f * f3 + 2.0f * f2 + 0.5f * f;
    float2 w3 = 0.5f * f3 - 0.5f * f2;
    
    // Optimized bilinear samples
    float2 w12 = w1 + w2;
    float2 tc12 = invTexSize * (centerPosition + w2 / w12);
    float2 tc0 = invTexSize * (centerPosition - 1.0f);
    float2 tc3 = invTexSize * (centerPosition + 2.0f);
    
    float3 result =
        gHistoryBuffer.SampleLevel(gsamLinearClamp, float2(tc12.x, tc0.y), 0).rgb * (w12.x * w0.y) +
        gHistoryBuffer.SampleLevel(gsamLinearClamp, float2(tc0.x, tc12.y), 0).rgb * (w0.x * w12.y) +
        gHistoryBuffer.SampleLevel(gsamLinearClamp, float2(tc12.x, tc12.y), 0).rgb * (w12.x * w12.y) +
        gHistoryBuffer.SampleLevel(gsamLinearClamp, float2(tc3.x, tc12.y), 0).rgb * (w3.x * w12.y) +
        gHistoryBuffer.SampleLevel(gsamLinearClamp, float2(tc12.x, tc3.y), 0).rgb * (w12.x * w3.y);
    
    return max(result, 0.0f);
}

float4 PS(VertexOut pin) : SV_Target
{
    float2 uv = pin.TexC;
    
    //-------------------------------------------------------------------------
    // Sample velocity and compute history UV
    // (from Alex Tardif's tutorial)
    //-------------------------------------------------------------------------
    float2 velocity = gVelocityBuffer.SampleLevel(gsamPointClamp, uv, 0).rg;
    float2 historyUV = uv - velocity;
    
    //-------------------------------------------------------------------------
    // Sample current color
    //-------------------------------------------------------------------------
    float3 currentColor = gCurrentColor.SampleLevel(gsamPointClamp, uv, 0).rgb;
    
    //-------------------------------------------------------------------------
    // Neighborhood sampling for variance clipping
    // (from Sugulee's tutorial - 3x3 neighborhood)
    //-------------------------------------------------------------------------
    float3 m1 = float3(0.0f, 0.0f, 0.0f);
    float3 m2 = float3(0.0f, 0.0f, 0.0f);
    float3 neighborMin = float3(9999.0f, 9999.0f, 9999.0f);
    float3 neighborMax = float3(-9999.0f, -9999.0f, -9999.0f);
    
    // Sample 3x3 neighborhood in YCoCg space
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 sampleUV = uv + float2(x, y) * gInvRenderTargetSize;
            float3 sampleColor = gCurrentColor.SampleLevel(gsamPointClamp, sampleUV, 0).rgb;
            float3 sampleYCoCg = RGBToYCoCg(sampleColor);
            
            m1 += sampleYCoCg;
            m2 += sampleYCoCg * sampleYCoCg;
            neighborMin = min(neighborMin, sampleYCoCg);
            neighborMax = max(neighborMax, sampleYCoCg);
        }
    }
    
    //-------------------------------------------------------------------------
    // Variance clipping (from Sugulee's tutorial)
    // gamma = 1.0 for standard variance clip
    //-------------------------------------------------------------------------
    float3 mu = m1 / 9.0f;
    float3 sigma = sqrt(abs(m2 / 9.0f - mu * mu));
    float gamma = 1.0f;
    
    float3 aabbMin = mu - gamma * sigma;
    float3 aabbMax = mu + gamma * sigma;
    
    // Intersect with min/max AABB for tighter bounds
    aabbMin = max(aabbMin, neighborMin);
    aabbMax = min(aabbMax, neighborMax);
    
    //-------------------------------------------------------------------------
    // Sample and clip history
    //-------------------------------------------------------------------------
    float3 historyColor;
    
    if (historyUV.x < 0.0f || historyUV.x > 1.0f || 
        historyUV.y < 0.0f || historyUV.y > 1.0f)
    {
        // History is off-screen
        historyColor = currentColor;
    }
    else
    {
        // Sample history with Catmull-Rom for sharpness
        historyColor = SampleHistoryCatmullRom(historyUV);
        
        // Clip history in YCoCg space (from Alex Tardif's tutorial)
        float3 historyYCoCg = RGBToYCoCg(historyColor);
        historyYCoCg = ClipAABB(aabbMin, aabbMax, historyYCoCg);
        historyColor = YCoCgToRGB(historyYCoCg);
    }
    
    //-------------------------------------------------------------------------
    // Blend factor with luminance weighting (from Elopezr's tutorial)
    // Higher velocity = more current frame
    //-------------------------------------------------------------------------
    float velocityMagnitude = length(velocity);
    
    // Base blend factor (alpha), adjusted by velocity
    float alpha = gBlendFactor;
    
    // Increase blend for fast motion (from Elopezr's tutorial)
    alpha = lerp(alpha, 0.8f, saturate(velocityMagnitude * gMotionScale));
    
    // Luminance difference weighting (from Elopezr's tutorial)
    // Reduces ghosting on high contrast edges
    float lumCurrent = Luminance(currentColor);
    float lumHistory = Luminance(historyColor);
    float lumDiff = abs(lumCurrent - lumHistory) / max(lumCurrent, max(lumHistory, 0.2f));
    alpha = lerp(alpha, 0.5f, lumDiff * 0.5f);
    
    // First frame - use current color
    if (gFrameCount == 0)
    {
        alpha = 1.0f;
    }
    
    //-------------------------------------------------------------------------
    // Final blend: result = lerp(history, current, alpha)
    //-------------------------------------------------------------------------
    float3 result = lerp(historyColor, currentColor, alpha);
    
    // Clamp to valid range
    result = max(result, 0.0f);
    
    return float4(result, 1.0f);
}
