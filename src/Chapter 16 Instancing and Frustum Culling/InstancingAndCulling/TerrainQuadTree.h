#pragma once

#include "../../Common/d3dUtil.h"
#include "../../Common/MathHelper.h"
#include <vector>
#include <memory>
#include <string>

// Tile to render - represents one terrain cell at a specific LOD
struct TerrainTile
{
    int Level;                  // 0=coarsest, 2=finest
    int NodeX, NodeZ;           // Texture tile coordinates
    float WorldMinX, WorldMinZ; // World position
    float WorldSize;            // Size in world units
    int HeightMapIndex;
    int DiffuseMapIndex;
    int NormalMapIndex;
    DirectX::XMFLOAT4X4 World;  // Transform matrix
    // UV offset and scale for texture atlas lookup
    DirectX::XMFLOAT2 UVOffset;
    DirectX::XMFLOAT2 UVScale;
};

// GPU instance data (matches shader)
struct TerrainTileInstance
{
    DirectX::XMFLOAT4X4 World;
    int HeightMapIndex;
    int DiffuseMapIndex;
    int NormalMapIndex;
    int LODLevel;
    // UV offset and scale for texture atlas lookup
    // Level 2: offset=(0,0), scale=(1,1) - full texture
    // Level 1: offset=(x/2, z/2), scale=(0.5, 0.5) - quarter of texture
    // Level 0: offset=(x/4, z/4), scale=(0.25, 0.25) - 1/16 of texture
    DirectX::XMFLOAT2 UVOffset;
    DirectX::XMFLOAT2 UVScale;
};

// Geometry Clipmaps implementation
// Creates concentric rings of LOD around camera position:
//   Level 2 (finest): closest to camera, uses 001 folder (4x4 tiles)
//   Level 1 (medium): middle ring, uses 002 folder (2x2 tiles)
//   Level 0 (coarsest): outer ring, uses 003 folder (1 tile)
class TerrainQuadTree
{
public:
    TerrainQuadTree();
    ~TerrainQuadTree();

    void Initialize(float terrainWorldSize, float terrainMaxHeight, float fovY, float screenHeight);

    // Select tiles based on distance to camera (clipmap rings)
    void SelectTiles(
        const DirectX::XMFLOAT3& cameraPos,
        const DirectX::BoundingFrustum& worldFrustum,
        std::vector<TerrainTile>& outTiles);

    float GetTerrainSize() const { return mTerrainSize; }
    float GetTerrainHeight() const { return mTerrainHeight; }

    // Texture index calculation
    // Level 0: index 0 (003 folder - 1 tile)
    // Level 1: indices 1-4 (002 folder - 2x2 tiles)
    // Level 2: indices 5-20 (001 folder - 4x4 tiles)
    static int GetTextureIndex(int level, int nodeX, int nodeZ);

private:
    // Frustum culling for a cell
    bool IsBlockVisible(float minX, float minZ, float maxX, float maxZ,
                        const DirectX::BoundingFrustum& frustum);

private:
    float mTerrainSize = 512.0f;
    float mTerrainHeight = 150.0f;
    
    float mFovY = 0.25f * 3.14159f;
    float mScreenHeight = 720.0f;
    
    // Distance thresholds for LOD rings
    static const int NUM_LEVELS = 3;
    float mLevelDistance[NUM_LEVELS] = { 1000.0f, 300.0f, 100.0f };
};

// Texture path helpers
struct TerrainTextureInfo
{
    static std::wstring GetHeightMapPath(int level, int nodeX, int nodeZ);
    static std::wstring GetDiffuseMapPath(int level, int nodeX, int nodeZ);
    static std::wstring GetNormalMapPath(int level, int nodeX, int nodeZ);
};
