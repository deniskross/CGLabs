#include "TerrainQuadTree.h"
#include <sstream>
#include <cmath>

using namespace DirectX;

TerrainQuadTree::TerrainQuadTree()
{
}

TerrainQuadTree::~TerrainQuadTree()
{
}

void TerrainQuadTree::Initialize(float terrainWorldSize, float terrainMaxHeight, float fovY, float screenHeight)
{
    mTerrainSize = terrainWorldSize;
    mTerrainHeight = terrainMaxHeight;
    mFovY = fovY;
    mScreenHeight = screenHeight;
    
    BuildTree();
}

int TerrainQuadTree::GetTextureIndex(int level, int nodeX, int nodeZ)
{
    // Texture array layout:
    // [0] = Level 0 (003 folder - 1 tile)
    // [1-4] = Level 1 (002 folder - 2x2 tiles)
    // [5-20] = Level 2 (001 folder - 4x4 tiles)
    
    switch (level)
    {
    case 0:
        return 0;
    case 1:
        return 1 + nodeZ * 2 + nodeX;
    case 2:
        return 5 + nodeZ * 4 + nodeX;
    default:
        return 0;
    }
}

void TerrainQuadTree::BuildTree()
{
    mRoot = std::make_unique<QuadTreeNode>();
    float halfSize = mTerrainSize * 0.5f;
    // Terrain goes from -halfSize to +halfSize in both X and Z
    BuildNode(mRoot.get(), 0, 0, 0, -halfSize, -halfSize, halfSize, halfSize);
}

void TerrainQuadTree::BuildNode(QuadTreeNode* node, int level, int nodeX, int nodeZ,
                                 float minX, float minZ, float maxX, float maxZ)
{
    node->Level = level;
    node->NodeX = nodeX;
    node->NodeZ = nodeZ;
    node->MinX = minX;
    node->MinZ = minZ;
    node->MaxX = maxX;
    node->MaxZ = maxZ;
    
    int texIdx = GetTextureIndex(level, nodeX, nodeZ);
    node->HeightMapIndex = texIdx;
    node->DiffuseMapIndex = texIdx;
    node->NormalMapIndex = texIdx;
    
    if (level < 2)
    {
        float midX = (minX + maxX) * 0.5f;
        float midZ = (minZ + maxZ) * 0.5f;
        
        int childLevel = level + 1;
        int childBaseX = nodeX * 2;
        int childBaseZ = nodeZ * 2;
        
        // Children: SW(0,0), SE(1,0), NW(0,1), NE(1,1)
        // SW - bottom left
        node->Children[0] = std::make_unique<QuadTreeNode>();
        BuildNode(node->Children[0].get(), childLevel, childBaseX, childBaseZ,
                  minX, minZ, midX, midZ);
        
        // SE - bottom right
        node->Children[1] = std::make_unique<QuadTreeNode>();
        BuildNode(node->Children[1].get(), childLevel, childBaseX + 1, childBaseZ,
                  midX, minZ, maxX, midZ);
        
        // NW - top left
        node->Children[2] = std::make_unique<QuadTreeNode>();
        BuildNode(node->Children[2].get(), childLevel, childBaseX, childBaseZ + 1,
                  minX, midZ, midX, maxZ);
        
        // NE - top right
        node->Children[3] = std::make_unique<QuadTreeNode>();
        BuildNode(node->Children[3].get(), childLevel, childBaseX + 1, childBaseZ + 1,
                  midX, midZ, maxX, maxZ);
    }
}

void TerrainQuadTree::SelectTiles(
    const XMFLOAT3& cameraPos,
    const BoundingFrustum& worldFrustum,
    std::vector<TerrainTile>& outTiles)
{
    outTiles.clear();
    if (mRoot)
    {
        SelectNode(mRoot.get(), cameraPos, worldFrustum, outTiles);
    }
}

bool TerrainQuadTree::IsNodeVisible(QuadTreeNode* node, const BoundingFrustum& frustum)
{
    float centerX = (node->MinX + node->MaxX) * 0.5f;
    float centerZ = (node->MinZ + node->MaxZ) * 0.5f;
    float halfSizeX = (node->MaxX - node->MinX) * 0.5f;
    float halfSizeZ = (node->MaxZ - node->MinZ) * 0.5f;
    
    XMFLOAT3 center(centerX, mTerrainHeight * 0.5f, centerZ);
    XMFLOAT3 extents(halfSizeX, mTerrainHeight * 0.5f + 20.0f, halfSizeZ);
    BoundingBox nodeBounds(center, extents);
    
    return frustum.Contains(nodeBounds) != DISJOINT;
}

float TerrainQuadTree::CalculateScreenSpaceError(QuadTreeNode* node, const XMFLOAT3& cameraPos)
{
    float centerX = (node->MinX + node->MaxX) * 0.5f;
    float centerZ = (node->MinZ + node->MaxZ) * 0.5f;
    
    float dx = cameraPos.x - centerX;
    float dy = cameraPos.y - mTerrainHeight * 0.5f;
    float dz = cameraPos.z - centerZ;
    float distance = sqrtf(dx * dx + dy * dy + dz * dz);
    
    if (distance < 1.0f)
        distance = 1.0f;
    
    float geometricError = mGeometricError[node->Level];
    float tanHalfFov = tanf(mFovY * 0.5f);
    float screenSpaceError = (geometricError * mScreenHeight) / (2.0f * distance * tanHalfFov);
    
    return screenSpaceError;
}

void TerrainQuadTree::SelectNode(
    QuadTreeNode* node,
    const XMFLOAT3& cameraPos,
    const BoundingFrustum& frustum,
    std::vector<TerrainTile>& outTiles)
{
    if (!IsNodeVisible(node, frustum))
        return;
    
    float screenError = CalculateScreenSpaceError(node, cameraPos);
    bool shouldSubdivide = (screenError > mMaxScreenSpaceError) && node->HasChildren();
    
    if (shouldSubdivide)
    {
        for (int i = 0; i < 4; ++i)
        {
            if (node->Children[i])
                SelectNode(node->Children[i].get(), cameraPos, frustum, outTiles);
        }
    }
    else
    {
        TerrainTile tile;
        tile.Level = node->Level;
        tile.NodeX = node->NodeX;
        tile.NodeZ = node->NodeZ;
        tile.WorldMinX = node->MinX;
        tile.WorldMinZ = node->MinZ;
        tile.WorldSize = node->MaxX - node->MinX;
        tile.HeightMapIndex = node->HeightMapIndex;
        tile.DiffuseMapIndex = node->DiffuseMapIndex;
        tile.NormalMapIndex = node->NormalMapIndex;
        
        // World transform: mesh is [0,1]x[0,1], scale to tile size and translate
        XMMATRIX world = XMMatrixScaling(tile.WorldSize, 1.0f, tile.WorldSize) *
                         XMMatrixTranslation(node->MinX, 0.0f, node->MinZ);
        XMStoreFloat4x4(&tile.World, XMMatrixTranspose(world));
        
        outTiles.push_back(tile);
    }
}

// Gaea exports tiles as Height_Out_y{row}_x{col}.dds
// row 0 = bottom, row increases upward (positive Z in world)
// col 0 = left, col increases rightward (positive X in world)
std::wstring TerrainTextureInfo::GetHeightMapPath(int level, int nodeX, int nodeZ)
{
    std::wstringstream ss;
    ss << L"../../Textures/terrain/";
    
    switch (level)
    {
    case 0:
        ss << L"003/Height_Out.dds";
        break;
    case 1:
        ss << L"002/Height/Height_Out_y" << nodeZ << L"_x" << nodeX << L".dds";
        break;
    case 2:
        ss << L"001/Height/Height_Out_y" << nodeZ << L"_x" << nodeX << L".dds";
        break;
    }
    return ss.str();
}

std::wstring TerrainTextureInfo::GetDiffuseMapPath(int level, int nodeX, int nodeZ)
{
    std::wstringstream ss;
    ss << L"../../Textures/terrain/";
    
    switch (level)
    {
    case 0:
        ss << L"003/Weathering_Out.dds";
        break;
    case 1:
        ss << L"002/Weathering/Weathering_Out_y" << nodeZ << L"_x" << nodeX << L".dds";
        break;
    case 2:
        ss << L"001/Weathering/Weathering_Out_y" << nodeZ << L"_x" << nodeX << L".dds";
        break;
    }
    return ss.str();
}

std::wstring TerrainTextureInfo::GetNormalMapPath(int level, int nodeX, int nodeZ)
{
    std::wstringstream ss;
    ss << L"../../Textures/terrain/";
    
    switch (level)
    {
    case 0:
        ss << L"003/Normals_Out.dds";
        break;
    case 1:
        ss << L"002/Normals/Normals_Out_y" << nodeZ << L"_x" << nodeX << L".dds";
        break;
    case 2:
        ss << L"001/Normals/Normals_Out_y" << nodeZ << L"_x" << nodeX << L".dds";
        break;
    }
    return ss.str();
}
