#include "TerrainQuadTree.h"
#include <sstream>
#include <cmath>

using namespace DirectX;

// Helper: distance from point to axis-aligned box
static float DistanceToBox(float px, float pz, float minX, float minZ, float maxX, float maxZ)
{
    float dx = 0.0f, dz = 0.0f;
    
    if (px < minX) dx = minX - px;
    else if (px > maxX) dx = px - maxX;
    
    if (pz < minZ) dz = minZ - pz;
    else if (pz > maxZ) dz = pz - maxZ;
    
    return sqrtf(dx * dx + dz * dz);
}

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
    
    // Distance thresholds for clipmap rings (centered on camera)
    // Level 2 (finest): cells within this distance from camera
    // Level 1 (medium): cells between level2 and level1 threshold
    // Level 0 (coarsest): everything else
    mLevelDistance[2] = mTerrainSize * 0.10f;  // ~77 units for 512 terrain
    mLevelDistance[1] = mTerrainSize * 0.25f;  // ~180 units
    mLevelDistance[0] = mTerrainSize * 2.0f;   // covers all
}

int TerrainQuadTree::GetTextureIndex(int level, int nodeX, int nodeZ)
{
    // Texture array layout:
    // [0] = Level 0 (003 folder - 1 tile, coarsest)
    // [1-4] = Level 1 (002 folder - 2x2 tiles)
    // [5-20] = Level 2 (001 folder - 4x4 tiles, finest)
    
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

bool TerrainQuadTree::IsBlockVisible(float minX, float minZ, float maxX, float maxZ,
                                      const BoundingFrustum& frustum)
{
    float centerX = (minX + maxX) * 0.5f;
    float centerZ = (minZ + maxZ) * 0.5f;
    float halfSizeX = (maxX - minX) * 0.5f;
    float halfSizeZ = (maxZ - minZ) * 0.5f;
    
    XMFLOAT3 center(centerX, mTerrainHeight * 0.5f, centerZ);
    XMFLOAT3 extents(halfSizeX, mTerrainHeight * 0.5f + 50.0f, halfSizeZ);
    BoundingBox blockBounds(center, extents);
    
    return frustum.Contains(blockBounds) != DISJOINT;
}

void TerrainQuadTree::SelectTiles(
    const XMFLOAT3& cameraPos,
    const BoundingFrustum& worldFrustum,
    std::vector<TerrainTile>& outTiles)
{
    outTiles.clear();
    
    float halfSize = mTerrainSize * 0.5f;
    float camX = cameraPos.x;
    float camZ = cameraPos.z;
    
    // =========================================================================
    // Geometry Clipmaps: concentric rings of LOD around camera
    // 
    // The terrain is divided into a 4x4 grid (16 cells at finest level).
    // Each cell is assigned ONE LOD based on distance to camera:
    //   - Level 2 (finest): closest to camera, uses 001 textures (4x4)
    //   - Level 1 (medium): middle ring, uses 002 textures (2x2)
    //   - Level 0 (coarsest): outer ring, uses 003 texture (1x1)
    // =========================================================================
    
    const int GRID_SIZE = 4;
    float cellSize = mTerrainSize / GRID_SIZE;
    
    // Determine LOD for each cell based on distance
    int cellLOD[GRID_SIZE][GRID_SIZE];
    
    for (int cz = 0; cz < GRID_SIZE; ++cz)
    {
        for (int cx = 0; cx < GRID_SIZE; ++cx)
        {
            float cellMinX = -halfSize + cx * cellSize;
            float cellMinZ = -halfSize + cz * cellSize;
            float cellMaxX = cellMinX + cellSize;
            float cellMaxZ = cellMinZ + cellSize;
            
            // Distance from camera to closest point of cell
            float dist = DistanceToBox(camX, camZ, cellMinX, cellMinZ, cellMaxX, cellMaxZ);
            
            // Assign LOD based on distance thresholds
            if (dist < mLevelDistance[2])
                cellLOD[cz][cx] = 2;  // Finest
            else if (dist < mLevelDistance[1])
                cellLOD[cz][cx] = 1;  // Medium
            else
                cellLOD[cz][cx] = 0;  // Coarsest
        }
    }
    
    // =========================================================================
    // Emit tiles for each LOD level
    // =========================================================================
    
    // --- Level 2 (finest): each cell maps 1:1 to a texture tile ---
    for (int cz = 0; cz < GRID_SIZE; ++cz)
    {
        for (int cx = 0; cx < GRID_SIZE; ++cx)
        {
            if (cellLOD[cz][cx] != 2) continue;
            
            float cellMinX = -halfSize + cx * cellSize;
            float cellMinZ = -halfSize + cz * cellSize;
            float cellMaxX = cellMinX + cellSize;
            float cellMaxZ = cellMinZ + cellSize;
            
            if (!IsBlockVisible(cellMinX, cellMinZ, cellMaxX, cellMaxZ, worldFrustum))
                continue;
            
            TerrainTile tile;
            tile.Level = 2;
            tile.NodeX = cx;
            tile.NodeZ = cz;
            tile.WorldMinX = cellMinX;
            tile.WorldMinZ = cellMinZ;
            tile.WorldSize = cellSize;
            
            int texIdx = GetTextureIndex(2, cx, cz);
            tile.HeightMapIndex = texIdx;
            tile.DiffuseMapIndex = texIdx;
            tile.NormalMapIndex = texIdx;
            
            // Level 2: each tile uses full texture (1:1 mapping)
            tile.UVOffset = XMFLOAT2(0.0f, 0.0f);
            tile.UVScale = XMFLOAT2(1.0f, 1.0f);
            
            XMMATRIX world = XMMatrixScaling(cellSize, 1.0f, cellSize) *
                             XMMatrixTranslation(cellMinX, 0.0f, cellMinZ);
            XMStoreFloat4x4(&tile.World, XMMatrixTranspose(world));
            
            outTiles.push_back(tile);
        }
    }
    
    // --- Level 1 (medium): 2x2 cells share one texture ---
    // Each Level 1 texture covers 2x2 cells, so each cell uses 1/4 of the texture
    for (int cz = 0; cz < GRID_SIZE; ++cz)
    {
        for (int cx = 0; cx < GRID_SIZE; ++cx)
        {
            if (cellLOD[cz][cx] != 1) continue;
            
            float cellMinX = -halfSize + cx * cellSize;
            float cellMinZ = -halfSize + cz * cellSize;
            float cellMaxX = cellMinX + cellSize;
            float cellMaxZ = cellMinZ + cellSize;
            
            if (!IsBlockVisible(cellMinX, cellMinZ, cellMaxX, cellMaxZ, worldFrustum))
                continue;
            
            TerrainTile tile;
            tile.Level = 1;
            tile.NodeX = cx / 2;
            tile.NodeZ = cz / 2;
            tile.WorldMinX = cellMinX;
            tile.WorldMinZ = cellMinZ;
            tile.WorldSize = cellSize;
            
            int texIdx = GetTextureIndex(1, cx / 2, cz / 2);
            tile.HeightMapIndex = texIdx;
            tile.DiffuseMapIndex = texIdx;
            tile.NormalMapIndex = texIdx;
            
            // Level 1: each texture covers 2x2 cells
            // Cell (cx, cz) uses portion based on (cx % 2, cz % 2)
            int localX = cx % 2;
            int localZ = cz % 2;
            tile.UVOffset = XMFLOAT2(localX * 0.5f, localZ * 0.5f);
            tile.UVScale = XMFLOAT2(0.5f, 0.5f);
            
            XMMATRIX world = XMMatrixScaling(cellSize, 1.0f, cellSize) *
                             XMMatrixTranslation(cellMinX, 0.0f, cellMinZ);
            XMStoreFloat4x4(&tile.World, XMMatrixTranspose(world));
            
            outTiles.push_back(tile);
        }
    }
    
    // --- Level 0 (coarsest): all cells share one texture ---
    // The single Level 0 texture covers all 4x4 cells, so each cell uses 1/16 of the texture
    for (int cz = 0; cz < GRID_SIZE; ++cz)
    {
        for (int cx = 0; cx < GRID_SIZE; ++cx)
        {
            if (cellLOD[cz][cx] != 0) continue;
            
            float cellMinX = -halfSize + cx * cellSize;
            float cellMinZ = -halfSize + cz * cellSize;
            float cellMaxX = cellMinX + cellSize;
            float cellMaxZ = cellMinZ + cellSize;
            
            if (!IsBlockVisible(cellMinX, cellMinZ, cellMaxX, cellMaxZ, worldFrustum))
                continue;
            
            TerrainTile tile;
            tile.Level = 0;
            tile.NodeX = 0;
            tile.NodeZ = 0;
            tile.WorldMinX = cellMinX;
            tile.WorldMinZ = cellMinZ;
            tile.WorldSize = cellSize;
            
            int texIdx = GetTextureIndex(0, 0, 0);
            tile.HeightMapIndex = texIdx;
            tile.DiffuseMapIndex = texIdx;
            tile.NormalMapIndex = texIdx;
            
            // Level 0: single texture covers all 4x4 cells
            // Cell (cx, cz) uses portion based on its position in the grid
            tile.UVOffset = XMFLOAT2(cx * 0.25f, cz * 0.25f);
            tile.UVScale = XMFLOAT2(0.25f, 0.25f);
            
            XMMATRIX world = XMMatrixScaling(cellSize, 1.0f, cellSize) *
                             XMMatrixTranslation(cellMinX, 0.0f, cellMinZ);
            XMStoreFloat4x4(&tile.World, XMMatrixTranspose(world));
            
            outTiles.push_back(tile);
        }
    }
}

// Gaea exports tiles as Height_Out_y{row}_x{col}.dds
// Level 0 (003): 1 texture, coarsest
// Level 1 (002): 2x2 textures
// Level 2 (001): 4x4 textures, finest
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
