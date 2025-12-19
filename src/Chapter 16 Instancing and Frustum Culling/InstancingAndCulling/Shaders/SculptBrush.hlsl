//***************************************************************************************
// SculptBrush.hlsl - GPU-based terrain sculpting compute shader
// 
// Architecture: Each thread processes one texel of the sculpt map (512x512 R32_FLOAT)
// Brush model: Circular falloff with quadratic attenuation for smooth edges
// Threading: 8x8 thread groups → 64x64 groups total → 262,144 threads
//***************************************************************************************

// Constant buffer matching SculptBrushCB struct in C++
cbuffer cbBrush : register(b0)
{
    float2 gBrushPosUV;      // Brush center in normalized texture coordinates [0,1]
    float gBrushRadius;      // Brush radius in UV space (not pixel space!)
    float gBrushStrength;    // Height modification magnitude per frame
    float gTerrainSize;      // World-space terrain size (for potential conversions)
    int gBrushActive;        // Boolean: 0=skip processing, 1=apply brush
    int gBrushType;          // Operation type: 0=subtract (dig), 1=add (raise)
    float gPad;              // HLSL constant buffer padding
};

// UAV: Read-write access to height modification texture
RWTexture2D<float> gSculptMap : register(u0);

// Compute shader entry point: 8x8 threads per group
[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    // Early exit if brush is disabled (CPU-side optimization)
    if (gBrushActive == 0)
        return;
    
    // Get texture dimensions for bounds checking
    uint width, height;
    gSculptMap.GetDimensions(width, height);
    
    // Bounds check: ensure thread ID is within texture bounds
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
        return;
    
    // Convert thread ID to normalized UV coordinates
    // Formula: UV = threadID / textureSize, where UV ∈ [0,1]
    float2 pixelUV = float2(dispatchThreadID.x, dispatchThreadID.y) / float2(width, height);
    
    // Calculate distance from current pixel to brush center
    // Distance in UV space (normalized coordinates)
    float2 diff = pixelUV - gBrushPosUV;
    float dist = length(diff);  // Euclidean distance: √(dx² + dy²)
    
    // Early exit if pixel is outside brush radius (performance optimization)
    if (dist > gBrushRadius)
        return;
    
    // Compute brush falloff using quadratic attenuation
    // Formula: falloff = (1 - d/r)², where d=distance, r=radius
    // This creates smooth edges with C¹ continuity at brush boundary
    float falloff = 1.0f - (dist / gBrushRadius);  // Linear falloff [1,0]
    falloff = falloff * falloff;                   // Quadratic falloff [1,0]
    
    // Read current height value and compute modification
    float currentHeight = gSculptMap[dispatchThreadID.xy];
    float delta = gBrushStrength * falloff;
    
    // Apply brush operation based on type
    if (gBrushType == 0)
        delta = -delta;  // Subtractive brush (dig holes)
    // else: additive brush (raise mountains) - delta remains positive
    
    // Write modified height back to texture
    gSculptMap[dispatchThreadID.xy] = currentHeight + delta;
}
