#pragma once

#include "../../Common/d3dUtil.h"
#include "../../Common/MathHelper.h"
#include <vector>
#include <memory>
#include <string>

// Quadtree node representing a terrain tile
// Level 0 = root (1 tile covering entire terrain) - uses 003 textures
// Level 1 = 4 children (2x2 tiles) - uses 002 textures  
// Level 2 = 16 grandchildren (4x4 tiles) - uses 001 textures
struct QuadTreeNode
{
    int Level;              // 0, 1, or 2
    int NodeX, NodeZ;       // Position in grid at this level (0-0 for L0, 0-1 for L1, 0-3 for L2)
    
    // World space bounds
    float MinX, MinZ;
    float MaxX, MaxZ;
    
    // Texture indices for this node
    int HeightMapIndex;
    int DiffuseMapIndex;
    int NormalMapIndex;
    
    // Children (nullptr if leaf or not subdivided)
    std::unique_ptr<QuadTreeNode> Children[4]; // [0]=SW, [1]=SE, [2]=NW, [3]=NE
    
    bool HasChildren() const { return Children[0] != nullptr; }
};

// Tile to render
struct TerrainTile
{
    int Level;
    int NodeX, NodeZ;
    float WorldMinX, WorldMinZ;
    float WorldSize;
    int HeightMapIndex;
    int DiffuseMapIndex;
    int NormalMapIndex;
    DirectX::XMFLOAT4X4 World;
};

// GPU instance data
struct TerrainTileInstance
{
    DirectX::XMFLOAT4X4 World;
    int HeightMapIndex;
    int DiffuseMapIndex;
    int NormalMapIndex;
    int LODLevel;
};

class TerrainQuadTree
{
public:
    TerrainQuadTree();
    ~TerrainQuadTree();

    void Initialize(float terrainWorldSize, float terrainMaxHeight, float fovY, float screenHeight);

    // Select tiles using screen-space error metric
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
    void BuildTree();
    void BuildNode(QuadTreeNode* node, int level, int nodeX, int nodeZ, 
                   float minX, float minZ, float maxX, float maxZ);
    
    // Recursive tile selection with screen-space error
    void SelectNode(
        QuadTreeNode* node,
        const DirectX::XMFLOAT3& cameraPos,
        const DirectX::BoundingFrustum& frustum,
        std::vector<TerrainTile>& outTiles);
    
    // Calculate screen-space error for a node
    // ρ = ε * x / (2 * d * tan(θ/2))
    // where ε = geometric error, x = screen width, d = distance, θ = FOV
    float CalculateScreenSpaceError(QuadTreeNode* node, const DirectX::XMFLOAT3& cameraPos);
    
    // Check if node is visible in frustum
    bool IsNodeVisible(QuadTreeNode* node, const DirectX::BoundingFrustum& frustum);

private:
    std::unique_ptr<QuadTreeNode> mRoot;
    
    float mTerrainSize = 512.0f;
    float mTerrainHeight = 150.0f;
    
    // For screen-space error calculation
    float mFovY = 0.25f * 3.14159f;
    float mScreenHeight = 720.0f;
    float mMaxScreenSpaceError = 4.0f; // Max allowed pixel error before subdividing
    
    // Geometric error per level (approximation of height variation)
    float mGeometricError[3] = { 50.0f, 25.0f, 12.5f };
};

// Texture path helpers
struct TerrainTextureInfo
{
    static std::wstring GetHeightMapPath(int level, int nodeX, int nodeZ);
    static std::wstring GetDiffuseMapPath(int level, int nodeX, int nodeZ);
    static std::wstring GetNormalMapPath(int level, int nodeX, int nodeZ);
};
