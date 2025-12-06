//***************************************************************************************
// TerrainApp.cpp - Simple terrain with distance-based LOD (no quadtree)
//***************************************************************************************

#include "../../Common/d3dApp.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Common/GeometryGenerator.h"
#include "../../Common/Camera.h"
#include "FrameResource.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")

const int gNumFrameResources = 3;
const int gTotalTileTextures = 21; // 1 + 4 + 16

struct TerrainConstants
{
    float TerrainHeight;
    float TerrainSize;
    float TexelSize;
    float Pad0;
    XMFLOAT4 TerrainDiffuse;
    XMFLOAT3 TerrainFresnelR0;
    float TerrainRoughness;
};

struct TerrainVertex
{
    XMFLOAT3 Pos;
    XMFLOAT2 TexC;
};

// GPU instance data for terrain tiles
struct TerrainTileInstance
{
    XMFLOAT4X4 World;
    int HeightMapIndex;
    int DiffuseMapIndex;
    int NormalMapIndex;
    int LODLevel;
};

// Simple tile info for CPU
struct SimpleTile
{
    int LODLevel;       // 0=003(1 tile), 1=002(4 tiles), 2=001(16 tiles)
    int TileX, TileZ;   // Position in grid at this LOD level
    float WorldMinX, WorldMinZ;
    float WorldSize;
    int HeightMapIndex;
    int DiffuseMapIndex;
    int NormalMapIndex;
};

class TerrainApp : public D3DApp
{
public:
    TerrainApp(HINSTANCE hInstance);
    ~TerrainApp();

    virtual bool Initialize() override;

private:
    virtual void OnResize() override;
    virtual void Update(const GameTimer& gt) override;
    virtual void Draw(const GameTimer& gt) override;

    virtual void OnMouseDown(WPARAM btnState, int x, int y) override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y) override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y) override;

    void OnKeyboardInput(const GameTimer& gt);
    void UpdateTerrainInstances(const GameTimer& gt);
    void UpdateMainPassCB(const GameTimer& gt);
    void UpdateTerrainCB(const GameTimer& gt);

    void LoadAllTerrainTextures();
    void BuildRootSignature();
    void BuildDescriptorHeaps();
    void BuildShadersAndInputLayout();
    void BuildTerrainGeometry();
    void BuildPSOs();
    void BuildFrameResources();
    void DrawTerrain(ID3D12GraphicsCommandList* cmdList);

    std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> GetStaticSamplers();

    // LOD helpers
    int SelectLODLevel(float distanceToCamera);
    int GetTextureIndex(int level, int tileX, int tileZ);
    std::wstring GetHeightMapPath(int level, int tileX, int tileZ);
    std::wstring GetDiffuseMapPath(int level, int tileX, int tileZ);
    std::wstring GetNormalMapPath(int level, int tileX, int tileZ);
    bool IsTileVisible(const BoundingFrustum& frustum, float minX, float minZ, float maxX, float maxZ);

private:
    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    UINT mCbvSrvDescriptorSize = 0;

    ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
    ComPtr<ID3D12DescriptorHeap> mSrvDescriptorHeap = nullptr;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
    std::unordered_map<std::string, std::unique_ptr<Texture>> mTextures;
    std::unordered_map<std::string, ComPtr<ID3DBlob>> mShaders;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mTerrainInputLayout;

    std::vector<SimpleTile> mVisibleTiles;
    // Per-frame instance buffers to avoid GPU/CPU sync issues
    std::unique_ptr<UploadBuffer<TerrainTileInstance>> mTileInstanceBuffers[gNumFrameResources];
    std::unique_ptr<UploadBuffer<TerrainConstants>> mTerrainCB;

    // Texture names in index order
    std::vector<std::string> mHeightMapNames;
    std::vector<std::string> mDiffuseMapNames;
    std::vector<std::string> mNormalMapNames;

    float mTerrainSize = 512.0f;
    float mTerrainHeight = 150.0f;
    int mPatchGridSize = 65;

    // LOD distance thresholds with large hysteresis to prevent flickering
    // When moving AWAY from terrain (increasing distance):
    float mLOD2to1Distance = 350.0f;  // Switch from LOD2 to LOD1
    float mLOD1to0Distance = 650.0f;  // Switch from LOD1 to LOD0
    // When moving TOWARD terrain (decreasing distance):
    float mLOD0to1Distance = 550.0f;  // Switch from LOD0 to LOD1
    float mLOD1to2Distance = 250.0f;  // Switch from LOD1 to LOD2
    
    int mCurrentLOD = 2;  // Track current LOD level for hysteresis
    float mLODSwitchCooldown = 0.0f;  // Cooldown timer to prevent rapid switching
    const float mLODSwitchDelay = 0.3f;  // 300ms delay between LOD switches

    bool mWireframe = false;
    BoundingFrustum mCamFrustum;
    PassConstants mMainPassCB;
    Camera mCamera;
    POINT mLastMousePos;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance, PSTR cmdLine, int showCmd)
{
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        TerrainApp theApp(hInstance);
        if (!theApp.Initialize())
            return 0;
        return theApp.Run();
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}

TerrainApp::TerrainApp(HINSTANCE hInstance) : D3DApp(hInstance)
{
    mMainWndCaption = L"Terrain LOD Demo";
}

TerrainApp::~TerrainApp()
{
    if (md3dDevice != nullptr)
        FlushCommandQueue();
}

bool TerrainApp::Initialize()
{
    if (!D3DApp::Initialize())
        return false;

    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    mCbvSrvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Camera starts above terrain looking at center
    mCamera.SetPosition(0.0f, mTerrainHeight + 100.0f, -mTerrainSize * 0.4f);
    mCamera.LookAt(mCamera.GetPosition3f(), XMFLOAT3(0.0f, 50.0f, 0.0f), XMFLOAT3(0.0f, 1.0f, 0.0f));

    LoadAllTerrainTextures();
    BuildRootSignature();
    BuildDescriptorHeaps();
    BuildShadersAndInputLayout();
    BuildTerrainGeometry();
    BuildFrameResources();
    BuildPSOs();

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    FlushCommandQueue();

    return true;
}

void TerrainApp::OnResize()
{
    D3DApp::OnResize();
    mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 3000.0f);
    BoundingFrustum::CreateFromMatrix(mCamFrustum, mCamera.GetProj());
}

void TerrainApp::Update(const GameTimer& gt)
{
    OnKeyboardInput(gt);

    mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
    mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

    if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
    {
        HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }

    UpdateTerrainInstances(gt);
    UpdateTerrainCB(gt);
    UpdateMainPassCB(gt);
}

void TerrainApp::Draw(const GameTimer& gt)
{
    auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;
    ThrowIfFailed(cmdListAlloc->Reset());

    auto pso = mWireframe ? mPSOs["terrain_wireframe"].Get() : mPSOs["terrain"].Get();
    ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), pso));

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::Black, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

    ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

    auto passCB = mCurrFrameResource->PassCB->Resource();
    mCommandList->SetGraphicsRootConstantBufferView(0, passCB->GetGPUVirtualAddress());
    mCommandList->SetGraphicsRootConstantBufferView(1, mTerrainCB->Resource()->GetGPUVirtualAddress());
    mCommandList->SetGraphicsRootShaderResourceView(2, mTileInstanceBuffers[mCurrFrameResourceIndex]->Resource()->GetGPUVirtualAddress());

    CD3DX12_GPU_DESCRIPTOR_HANDLE texHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    mCommandList->SetGraphicsRootDescriptorTable(3, texHandle);
    texHandle.Offset(gTotalTileTextures, mCbvSrvDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(4, texHandle);
    texHandle.Offset(gTotalTileTextures, mCbvSrvDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(5, texHandle);

    DrawTerrain(mCommandList.Get());

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

    ThrowIfFailed(mCommandList->Close());

    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    ThrowIfFailed(mSwapChain->Present(0, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    mCurrFrameResource->Fence = ++mCurrentFence;
    mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

void TerrainApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    mLastMousePos.x = x;
    mLastMousePos.y = y;
    SetCapture(mhMainWnd);
}

void TerrainApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ReleaseCapture();
}

void TerrainApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    if ((btnState & MK_LBUTTON) != 0)
    {
        float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
        float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));
        mCamera.Pitch(dy);
        mCamera.RotateY(dx);
    }
    mLastMousePos.x = x;
    mLastMousePos.y = y;
}

void TerrainApp::OnKeyboardInput(const GameTimer& gt)
{
    const float dt = gt.DeltaTime();
    float speed = 80.0f;

    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        speed *= 3.0f;

    if (GetAsyncKeyState('W') & 0x8000) mCamera.Walk(speed * dt);
    if (GetAsyncKeyState('S') & 0x8000) mCamera.Walk(-speed * dt);
    if (GetAsyncKeyState('A') & 0x8000) mCamera.Strafe(-speed * dt);
    if (GetAsyncKeyState('D') & 0x8000) mCamera.Strafe(speed * dt);
    
    if (GetAsyncKeyState('Q') & 0x8000)
    {
        XMFLOAT3 pos = mCamera.GetPosition3f();
        mCamera.SetPosition(pos.x, pos.y + speed * dt, pos.z);
    }
    if (GetAsyncKeyState('E') & 0x8000)
    {
        XMFLOAT3 pos = mCamera.GetPosition3f();
        mCamera.SetPosition(pos.x, pos.y - speed * dt, pos.z);
    }

    if (GetAsyncKeyState('1') & 0x8000) mWireframe = false;
    if (GetAsyncKeyState('2') & 0x8000) mWireframe = true;

    mCamera.UpdateViewMatrix();
}

int TerrainApp::SelectLODLevel(float distanceToCamera)
{
    // LOD selection with hysteresis and cooldown to prevent flickering
    // Don't switch LOD if cooldown is active
    if (mLODSwitchCooldown > 0.0f)
        return mCurrentLOD;
    
    int newLOD = mCurrentLOD;
    
    if (mCurrentLOD == 0)
    {
        // Currently at LOD0 (far), check if should switch to LOD1
        if (distanceToCamera < mLOD0to1Distance)
            newLOD = 1;
    }
    else if (mCurrentLOD == 1)
    {
        // Currently at LOD1 (medium)
        if (distanceToCamera > mLOD1to0Distance)
            newLOD = 0;  // Moving away -> LOD0
        else if (distanceToCamera < mLOD1to2Distance)
            newLOD = 2;  // Moving closer -> LOD2
    }
    else // mCurrentLOD == 2
    {
        // Currently at LOD2 (close), check if should switch to LOD1
        if (distanceToCamera > mLOD2to1Distance)
            newLOD = 1;
    }
    
    // If LOD changed, start cooldown
    if (newLOD != mCurrentLOD)
    {
        mCurrentLOD = newLOD;
        mLODSwitchCooldown = mLODSwitchDelay;
    }
    
    return mCurrentLOD;
}

int TerrainApp::GetTextureIndex(int level, int tileX, int tileZ)
{
    // Level 0: index 0 (003 folder - 1 tile)
    // Level 1: indices 1-4 (002 folder - 2x2 tiles)
    // Level 2: indices 5-20 (001 folder - 4x4 tiles)
    if (level == 0)
        return 0;
    else if (level == 1)
        return 1 + tileZ * 2 + tileX;
    else
        return 5 + tileZ * 4 + tileX;
}

bool TerrainApp::IsTileVisible(const BoundingFrustum& frustum, float minX, float minZ, float maxX, float maxZ)
{
    // Create AABB for the tile (use full height range)
    BoundingBox box;
    XMFLOAT3 center((minX + maxX) * 0.5f, mTerrainHeight * 0.5f, (minZ + maxZ) * 0.5f);
    XMFLOAT3 extents((maxX - minX) * 0.5f, mTerrainHeight * 0.5f, (maxZ - minZ) * 0.5f);
    box.Center = center;
    box.Extents = extents;
    
    return frustum.Contains(box) != ContainmentType::DISJOINT;
}

void TerrainApp::UpdateTerrainInstances(const GameTimer& gt)
{
    // Update LOD switch cooldown
    if (mLODSwitchCooldown > 0.0f)
        mLODSwitchCooldown -= gt.DeltaTime();
    
    XMMATRIX view = mCamera.GetView();
    XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
    
    // Transform frustum to world space
    BoundingFrustum worldFrustum;
    mCamFrustum.Transform(worldFrustum, invView);

    XMFLOAT3 camPos = mCamera.GetPosition3f();
    
    // Calculate distance from camera to terrain center
    float terrainCenterX = 0.0f;
    float terrainCenterZ = 0.0f;
    float dx = camPos.x - terrainCenterX;
    float dz = camPos.z - terrainCenterZ;
    float distToCenter = sqrtf(dx * dx + dz * dz);
    
    // Select LOD based on distance
    int lodLevel = SelectLODLevel(distToCenter);
    
    mVisibleTiles.clear();
    
    // Terrain bounds: centered at origin, size = mTerrainSize
    float halfSize = mTerrainSize * 0.5f;
    float terrainMinX = -halfSize;
    float terrainMinZ = -halfSize;
    
    if (lodLevel == 0)
    {
        // LOD0: Single tile covering entire terrain
        if (IsTileVisible(worldFrustum, terrainMinX, terrainMinZ, terrainMinX + mTerrainSize, terrainMinZ + mTerrainSize))
        {
            SimpleTile tile;
            tile.LODLevel = 0;
            tile.TileX = 0;
            tile.TileZ = 0;
            tile.WorldMinX = terrainMinX;
            tile.WorldMinZ = terrainMinZ;
            tile.WorldSize = mTerrainSize;
            tile.HeightMapIndex = GetTextureIndex(0, 0, 0);
            tile.DiffuseMapIndex = tile.HeightMapIndex;
            tile.NormalMapIndex = tile.HeightMapIndex;
            mVisibleTiles.push_back(tile);
        }
    }
    else if (lodLevel == 1)
    {
        // LOD1: 2x2 tiles
        float tileSize = mTerrainSize / 2.0f;
        for (int z = 0; z < 2; ++z)
        {
            for (int x = 0; x < 2; ++x)
            {
                float minX = terrainMinX + x * tileSize;
                float minZ = terrainMinZ + z * tileSize;
                
                if (IsTileVisible(worldFrustum, minX, minZ, minX + tileSize, minZ + tileSize))
                {
                    SimpleTile tile;
                    tile.LODLevel = 1;
                    tile.TileX = x;
                    tile.TileZ = z;
                    tile.WorldMinX = minX;
                    tile.WorldMinZ = minZ;
                    tile.WorldSize = tileSize;
                    tile.HeightMapIndex = GetTextureIndex(1, x, z);
                    tile.DiffuseMapIndex = tile.HeightMapIndex;
                    tile.NormalMapIndex = tile.HeightMapIndex;
                    mVisibleTiles.push_back(tile);
                }
            }
        }
    }
    else
    {
        // LOD2: 4x4 tiles
        float tileSize = mTerrainSize / 4.0f;
        for (int z = 0; z < 4; ++z)
        {
            for (int x = 0; x < 4; ++x)
            {
                float minX = terrainMinX + x * tileSize;
                float minZ = terrainMinZ + z * tileSize;
                
                if (IsTileVisible(worldFrustum, minX, minZ, minX + tileSize, minZ + tileSize))
                {
                    SimpleTile tile;
                    tile.LODLevel = 2;
                    tile.TileX = x;
                    tile.TileZ = z;
                    tile.WorldMinX = minX;
                    tile.WorldMinZ = minZ;
                    tile.WorldSize = tileSize;
                    tile.HeightMapIndex = GetTextureIndex(2, x, z);
                    tile.DiffuseMapIndex = tile.HeightMapIndex;
                    tile.NormalMapIndex = tile.HeightMapIndex;
                    mVisibleTiles.push_back(tile);
                }
            }
        }
    }

    // Upload instance data
    for (size_t i = 0; i < mVisibleTiles.size() && i < 64; ++i)
    {
        const SimpleTile& tile = mVisibleTiles[i];
        
        // World matrix: scale and translate
        XMMATRIX scale = XMMatrixScaling(tile.WorldSize, 1.0f, tile.WorldSize);
        XMMATRIX translate = XMMatrixTranslation(tile.WorldMinX, 0.0f, tile.WorldMinZ);
        XMMATRIX world = scale * translate;
        
        TerrainTileInstance inst;
        XMStoreFloat4x4(&inst.World, XMMatrixTranspose(world));
        inst.HeightMapIndex = tile.HeightMapIndex;
        inst.DiffuseMapIndex = tile.DiffuseMapIndex;
        inst.NormalMapIndex = tile.NormalMapIndex;
        inst.LODLevel = tile.LODLevel;
        
        mTileInstanceBuffers[mCurrFrameResourceIndex]->CopyData((int)i, inst);
    }

    // Update window title
    int countL0 = 0, countL1 = 0, countL2 = 0;
    for (const auto& t : mVisibleTiles)
    {
        if (t.LODLevel == 0) countL0++;
        else if (t.LODLevel == 1) countL1++;
        else countL2++;
    }

    std::wostringstream outs;
    outs << L"Terrain LOD - Tiles: " << mVisibleTiles.size()
         << L" (L0:" << countL0 << L" L1:" << countL1 << L" L2:" << countL2 << L")"
         << L" Dist:" << (int)distToCenter
         << L" | 1/2=Solid/Wire | WASD+QE";
    mMainWndCaption = outs.str();
}

void TerrainApp::UpdateTerrainCB(const GameTimer& gt)
{
    TerrainConstants terrainCB;
    terrainCB.TerrainHeight = mTerrainHeight;
    terrainCB.TerrainSize = mTerrainSize;
    terrainCB.TexelSize = 1.0f / 512.0f;
    terrainCB.Pad0 = 0.0f;
    terrainCB.TerrainDiffuse = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    terrainCB.TerrainFresnelR0 = XMFLOAT3(0.02f, 0.02f, 0.02f);
    terrainCB.TerrainRoughness = 0.9f;

    mTerrainCB->CopyData(0, terrainCB);
}

void TerrainApp::UpdateMainPassCB(const GameTimer& gt)
{
    XMMATRIX view = mCamera.GetView();
    XMMATRIX proj = mCamera.GetProj();
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
    XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
    XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

    XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
    XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
    XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
    mMainPassCB.EyePosW = mCamera.GetPosition3f();
    mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
    mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
    mMainPassCB.NearZ = 1.0f;
    mMainPassCB.FarZ = 3000.0f;
    mMainPassCB.TotalTime = gt.TotalTime();
    mMainPassCB.DeltaTime = gt.DeltaTime();
    mMainPassCB.AmbientLight = { 0.4f, 0.4f, 0.45f, 1.0f };
    
    mMainPassCB.Lights[0].Direction = { 0.57735f, -0.57735f, 0.57735f };
    mMainPassCB.Lights[0].Strength = { 1.0f, 0.95f, 0.9f };
    mMainPassCB.Lights[1].Direction = { -0.57735f, -0.57735f, 0.57735f };
    mMainPassCB.Lights[1].Strength = { 0.25f, 0.25f, 0.25f };
    mMainPassCB.Lights[2].Direction = { 0.0f, -0.707f, -0.707f };
    mMainPassCB.Lights[2].Strength = { 0.15f, 0.15f, 0.15f };

    auto currPassCB = mCurrFrameResource->PassCB.get();
    currPassCB->CopyData(0, mMainPassCB);
}

std::wstring TerrainApp::GetHeightMapPath(int level, int tileX, int tileZ)
{
    // Gaea naming: y{row}_x{col} where y0=bottom row, x0=left column
    // Level 0 (003): single file without coordinates
    // Level 1 (002) and Level 2 (001): subfolders with _Out_y{z}_x{x}.dds
    std::wostringstream path;
    
    if (level == 0)
    {
        path << L"../../Textures/terrain/003/Height_Out.dds";
    }
    else if (level == 1)
    {
        path << L"../../Textures/terrain/002/Height/Height_Out_y" << tileZ << L"_x" << tileX << L".dds";
    }
    else
    {
        path << L"../../Textures/terrain/001/Height/Height_Out_y" << tileZ << L"_x" << tileX << L".dds";
    }
    return path.str();
}

std::wstring TerrainApp::GetDiffuseMapPath(int level, int tileX, int tileZ)
{
    std::wostringstream path;
    
    if (level == 0)
    {
        path << L"../../Textures/terrain/003/Weathering_Out.dds";
    }
    else if (level == 1)
    {
        path << L"../../Textures/terrain/002/Weathering/Weathering_Out_y" << tileZ << L"_x" << tileX << L".dds";
    }
    else
    {
        path << L"../../Textures/terrain/001/Weathering/Weathering_Out_y" << tileZ << L"_x" << tileX << L".dds";
    }
    return path.str();
}

std::wstring TerrainApp::GetNormalMapPath(int level, int tileX, int tileZ)
{
    std::wostringstream path;
    
    if (level == 0)
    {
        path << L"../../Textures/terrain/003/Normals_Out.dds";
    }
    else if (level == 1)
    {
        path << L"../../Textures/terrain/002/Normals/Normals_Out_y" << tileZ << L"_x" << tileX << L".dds";
    }
    else
    {
        path << L"../../Textures/terrain/001/Normals/Normals_Out_y" << tileZ << L"_x" << tileX << L".dds";
    }
    return path.str();
}

void TerrainApp::LoadAllTerrainTextures()
{
    auto LoadTex = [&](const std::wstring& path, const std::string& name) {
        auto tex = std::make_unique<Texture>();
        tex->Name = name;
        tex->Filename = path;
        
        HRESULT hr = DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
            mCommandList.Get(), tex->Filename.c_str(),
            tex->Resource, tex->UploadHeap);
        
        if (FAILED(hr))
            OutputDebugStringW((L"Failed to load: " + path + L"\n").c_str());
        
        mTextures[name] = std::move(tex);
    };

    // Index 0: Level 0 (003) - 1 tile
    LoadTex(GetHeightMapPath(0, 0, 0), "h_0");
    LoadTex(GetDiffuseMapPath(0, 0, 0), "d_0");
    LoadTex(GetNormalMapPath(0, 0, 0), "n_0");
    mHeightMapNames.push_back("h_0");
    mDiffuseMapNames.push_back("d_0");
    mNormalMapNames.push_back("n_0");

    // Index 1-4: Level 1 (002) - 2x2 tiles
    for (int z = 0; z < 2; ++z)
    {
        for (int x = 0; x < 2; ++x)
        {
            int idx = GetTextureIndex(1, x, z);
            std::string suffix = std::to_string(idx);
            
            LoadTex(GetHeightMapPath(1, x, z), "h_" + suffix);
            LoadTex(GetDiffuseMapPath(1, x, z), "d_" + suffix);
            LoadTex(GetNormalMapPath(1, x, z), "n_" + suffix);
            
            mHeightMapNames.push_back("h_" + suffix);
            mDiffuseMapNames.push_back("d_" + suffix);
            mNormalMapNames.push_back("n_" + suffix);
        }
    }

    // Index 5-20: Level 2 (001) - 4x4 tiles
    for (int z = 0; z < 4; ++z)
    {
        for (int x = 0; x < 4; ++x)
        {
            int idx = GetTextureIndex(2, x, z);
            std::string suffix = std::to_string(idx);
            
            LoadTex(GetHeightMapPath(2, x, z), "h_" + suffix);
            LoadTex(GetDiffuseMapPath(2, x, z), "d_" + suffix);
            LoadTex(GetNormalMapPath(2, x, z), "n_" + suffix);
            
            mHeightMapNames.push_back("h_" + suffix);
            mDiffuseMapNames.push_back("d_" + suffix);
            mNormalMapNames.push_back("n_" + suffix);
        }
    }
}

void TerrainApp::BuildRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE heightTable;
    heightTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, gTotalTileTextures, 0, 0);
    
    CD3DX12_DESCRIPTOR_RANGE diffuseTable;
    diffuseTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, gTotalTileTextures, 21, 0);
    
    CD3DX12_DESCRIPTOR_RANGE normalTable;
    normalTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, gTotalTileTextures, 42, 0);

    CD3DX12_ROOT_PARAMETER slotRootParameter[6];
    slotRootParameter[0].InitAsConstantBufferView(0);
    slotRootParameter[1].InitAsConstantBufferView(1);
    slotRootParameter[2].InitAsShaderResourceView(0, 1);
    slotRootParameter[3].InitAsDescriptorTable(1, &heightTable, D3D12_SHADER_VISIBILITY_VERTEX);
    slotRootParameter[4].InitAsDescriptorTable(1, &diffuseTable, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[5].InitAsDescriptorTable(1, &normalTable, D3D12_SHADER_VISIBILITY_ALL);

    auto staticSamplers = GetStaticSamplers();

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(6, slotRootParameter,
        (UINT)staticSamplers.size(), staticSamplers.data(),
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void TerrainApp::BuildDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = gTotalTileTextures * 3;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

    CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    for (const auto& name : mHeightMapNames)
    {
        auto& tex = mTextures[name];
        if (tex && tex->Resource)
        {
            srvDesc.Format = tex->Resource->GetDesc().Format;
            srvDesc.Texture2D.MipLevels = tex->Resource->GetDesc().MipLevels;
            md3dDevice->CreateShaderResourceView(tex->Resource.Get(), &srvDesc, hDescriptor);
        }
        hDescriptor.Offset(1, mCbvSrvDescriptorSize);
    }

    for (const auto& name : mDiffuseMapNames)
    {
        auto& tex = mTextures[name];
        if (tex && tex->Resource)
        {
            srvDesc.Format = tex->Resource->GetDesc().Format;
            srvDesc.Texture2D.MipLevels = tex->Resource->GetDesc().MipLevels;
            md3dDevice->CreateShaderResourceView(tex->Resource.Get(), &srvDesc, hDescriptor);
        }
        hDescriptor.Offset(1, mCbvSrvDescriptorSize);
    }

    for (const auto& name : mNormalMapNames)
    {
        auto& tex = mTextures[name];
        if (tex && tex->Resource)
        {
            srvDesc.Format = tex->Resource->GetDesc().Format;
            srvDesc.Texture2D.MipLevels = tex->Resource->GetDesc().MipLevels;
            md3dDevice->CreateShaderResourceView(tex->Resource.Get(), &srvDesc, hDescriptor);
        }
        hDescriptor.Offset(1, mCbvSrvDescriptorSize);
    }
}

void TerrainApp::BuildShadersAndInputLayout()
{
    mShaders["terrainVS"] = d3dUtil::CompileShader(L"Shaders\\Terrain.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["terrainPS"] = d3dUtil::CompileShader(L"Shaders\\Terrain.hlsl", nullptr, "PS", "ps_5_1");
    mShaders["terrainWireframePS"] = d3dUtil::CompileShader(L"Shaders\\Terrain.hlsl", nullptr, "PS_Wireframe", "ps_5_1");

    mTerrainInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
}

void TerrainApp::BuildTerrainGeometry()
{
    // Create unit grid [0,1] x [0,1]
    // Vertex shader will scale and translate based on instance world matrix
    int gridSize = mPatchGridSize;
    float step = 1.0f / (gridSize - 1);
    
    int vertexCount = gridSize * gridSize;
    int indexCount = (gridSize - 1) * (gridSize - 1) * 6;

    std::vector<TerrainVertex> vertices(vertexCount);
    std::vector<std::uint32_t> indices(indexCount);

    // Create vertices
    for (int z = 0; z < gridSize; ++z)
    {
        for (int x = 0; x < gridSize; ++x)
        {
            int i = z * gridSize + x;
            vertices[i].Pos = XMFLOAT3(x * step, 0.0f, z * step);
            vertices[i].TexC = XMFLOAT2(x * step, z * step);
        }
    }

    // Create indices
    int idx = 0;
    for (int z = 0; z < gridSize - 1; ++z)
    {
        for (int x = 0; x < gridSize - 1; ++x)
        {
            int tl = z * gridSize + x;
            int tr = tl + 1;
            int bl = (z + 1) * gridSize + x;
            int br = bl + 1;

            indices[idx++] = tl;
            indices[idx++] = bl;
            indices[idx++] = tr;
            indices[idx++] = tr;
            indices[idx++] = bl;
            indices[idx++] = br;
        }
    }

    const UINT vbByteSize = (UINT)vertices.size() * sizeof(TerrainVertex);
    const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint32_t);

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "terrainPatchGeo";

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
    CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

    ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
    CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

    geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

    geo->VertexByteStride = sizeof(TerrainVertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    SubmeshGeometry submesh;
    submesh.IndexCount = (UINT)indices.size();
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;

    geo->DrawArgs["patch"] = submesh;
    mGeometries[geo->Name] = std::move(geo);
}

void TerrainApp::BuildPSOs()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
    ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
    psoDesc.InputLayout = { mTerrainInputLayout.data(), (UINT)mTerrainInputLayout.size() };
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.VS = { reinterpret_cast<BYTE*>(mShaders["terrainVS"]->GetBufferPointer()), mShaders["terrainVS"]->GetBufferSize() };
    psoDesc.PS = { reinterpret_cast<BYTE*>(mShaders["terrainPS"]->GetBufferPointer()), mShaders["terrainPS"]->GetBufferSize() };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = mBackBufferFormat;
    psoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    psoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    psoDesc.DSVFormat = mDepthStencilFormat;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSOs["terrain"])));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC wirePsoDesc = psoDesc;
    wirePsoDesc.PS = { reinterpret_cast<BYTE*>(mShaders["terrainWireframePS"]->GetBufferPointer()), mShaders["terrainWireframePS"]->GetBufferSize() };
    wirePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    wirePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&wirePsoDesc, IID_PPV_ARGS(&mPSOs["terrain_wireframe"])));
}

void TerrainApp::BuildFrameResources()
{
    for (int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(), 1, 64, 1));
        // Create per-frame instance buffer to avoid GPU/CPU sync issues
        mTileInstanceBuffers[i] = std::make_unique<UploadBuffer<TerrainTileInstance>>(md3dDevice.Get(), 64, false);
    }

    mTerrainCB = std::make_unique<UploadBuffer<TerrainConstants>>(md3dDevice.Get(), 1, true);
}

void TerrainApp::DrawTerrain(ID3D12GraphicsCommandList* cmdList)
{
    if (mVisibleTiles.empty())
        return;

    auto geo = mGeometries["terrainPatchGeo"].get();

    cmdList->IASetVertexBuffers(0, 1, &geo->VertexBufferView());
    cmdList->IASetIndexBuffer(&geo->IndexBufferView());
    cmdList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UINT indexCount = geo->DrawArgs["patch"].IndexCount;
    UINT instanceCount = (UINT)mVisibleTiles.size();

    cmdList->DrawIndexedInstanced(indexCount, instanceCount, 0, 0, 0);
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> TerrainApp::GetStaticSamplers()
{
    const CD3DX12_STATIC_SAMPLER_DESC pointClamp(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    const CD3DX12_STATIC_SAMPLER_DESC linearClamp(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    const CD3DX12_STATIC_SAMPLER_DESC linearWrap(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(3, D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0.0f, 8);

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(4, D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f, 8);

    const CD3DX12_STATIC_SAMPLER_DESC anisotropic16(5, D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f, 16);

    return { pointClamp, linearClamp, linearWrap, anisotropicWrap, anisotropicClamp, anisotropic16 };
}
