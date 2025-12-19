//***************************************************************************************
// TerrainApp.cpp - Terrain with per-tile LOD using quadtree (Geometry Clipmaps style)
// Based on: GPU Gems 2, Chapter 2 - Terrain Rendering Using GPU-Based Geometry Clipmaps
// https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry
//***************************************************************************************

#include "../../Common/d3dApp.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Common/GeometryGenerator.h"
#include "../../Common/Camera.h"
#include "FrameResource.h"
#include "TerrainQuadTree.h"

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

// Constant buffer for compute shader brush parameters
// Must match cbBrush layout in SculptBrush.hlsl exactly
struct SculptBrushCB
{
    XMFLOAT2 BrushPosUV;     // Brush center in normalized UV coordinates [0,1]
    float BrushRadius;       // Brush radius in UV space (not world space!)
    float BrushStrength;     // Height delta per frame (positive values)
    float TerrainSize;       // World-space terrain size for UV conversion
    int BrushActive;         // Boolean flag for compute shader early exit
    int BrushType;           // 0 = subtract height (dig), 1 = add height (raise)
    float Pad;               // HLSL packing alignment
};

struct TerrainVertex
{
    XMFLOAT3 Pos;
    XMFLOAT2 TexC;
};

// GPU instance data for terrain tiles (matches TerrainTileInstance in TerrainQuadTree.h)
struct TerrainTileInstanceGPU
{
    XMFLOAT4X4 World;
    int HeightMapIndex;
    int DiffuseMapIndex;
    int NormalMapIndex;
    int LODLevel;
    // UV offset and scale for texture atlas lookup
    XMFLOAT2 UVOffset;
    XMFLOAT2 UVScale;
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

    // Texture path helpers (use TerrainTextureInfo from TerrainQuadTree.h)
    std::wstring GetHeightMapPath(int level, int tileX, int tileZ);
    std::wstring GetDiffuseMapPath(int level, int tileX, int tileZ);
    std::wstring GetNormalMapPath(int level, int tileX, int tileZ);

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

    // Per-tile LOD selection using quadtree (Geometry Clipmaps approach)
    TerrainQuadTree mQuadTree;
    std::vector<TerrainTile> mVisibleTiles;
    
    // Per-frame instance buffers to avoid GPU/CPU sync issues
    std::unique_ptr<UploadBuffer<TerrainTileInstanceGPU>> mTileInstanceBuffers[gNumFrameResources];
    std::unique_ptr<UploadBuffer<TerrainConstants>> mTerrainCB;

    // Texture names in index order
    std::vector<std::string> mHeightMapNames;
    std::vector<std::string> mDiffuseMapNames;
    std::vector<std::string> mNormalMapNames;

    float mTerrainSize = 512.0f;
    float mTerrainHeight = 150.0f;
    int mPatchGridSize = 65;

    bool mWireframe = false;
    BoundingFrustum mCamFrustum;
    PassConstants mMainPassCB;
    Camera mCamera;
    POINT mLastMousePos;
    
    // Interactive terrain sculpting state
    bool mSculptMode = false;           // P key toggles sculpt mode on/off
    bool mSculpting = false;            // True while LMB held down in sculpt mode
    int mSculptBrushType = 0;           // Brush operation: 0=dig holes, 1=raise mountains
    float mBrushRadius = 0.05f;         // Brush size in UV space (5% of terrain)
    float mBrushStrength = 0.002f;      // Height change per frame (world units)
    
    // R32_FLOAT texture storing height deltas (added to base heightmaps)
    static const int SCULPT_MAP_SIZE = 512;
    ComPtr<ID3D12Resource> mSculptMap;
    ComPtr<ID3D12Resource> mSculptMapUpload;
    
    // GPU compute shader pipeline for real-time height modification
    ComPtr<ID3D12RootSignature> mSculptRootSignature;  // CS root signature (CBV + UAV)
    ComPtr<ID3D12PipelineState> mSculptPSO;            // Compute pipeline state object
    std::unique_ptr<UploadBuffer<SculptBrushCB>> mSculptBrushCB;  // Per-frame brush params
    
    // Descriptor heap offsets for sculpt map binding
    UINT mSculptMapUavIndex = 0;
    UINT mSculptMapSrvIndex = 0;
    
    void BuildSculptResources();
    void BuildSculptRootSignature();
    void BuildSculptPSO();
    void ApplySculptBrush(float brushX, float brushZ);
    bool RaycastTerrain(int mouseX, int mouseY, XMFLOAT3& hitPoint);
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

    // Initialize the quadtree for per-tile LOD selection
    // This implements the Geometry Clipmaps concept from GPU Gems 2, Chapter 2:
    // Tiles closer to camera get higher detail (LOD2), farther tiles get lower detail (LOD0)
    mQuadTree.Initialize(mTerrainSize, mTerrainHeight, 0.25f * MathHelper::Pi, (float)mClientHeight);

    LoadAllTerrainTextures();
    BuildSculptResources();
    BuildRootSignature();
    BuildSculptRootSignature();
    BuildDescriptorHeaps();
    BuildShadersAndInputLayout();
    BuildTerrainGeometry();
    BuildFrameResources();
    BuildPSOs();
    BuildSculptPSO();

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
    
    // Set descriptor heaps early (needed for compute shader too)
    ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
    
    // Real-time terrain modification via compute shader dispatch
    if (mSculpting)
    {
        XMFLOAT3 hitPoint;
        if (RaycastTerrain(mLastMousePos.x, mLastMousePos.y, hitPoint))
        {
            ApplySculptBrush(hitPoint.x, hitPoint.z);  // Dispatch CS with world coords
        }
        mCommandList->SetPipelineState(pso);  // Restore graphics PSO after CS dispatch
    }
    
    // Resource state transition: sculpt map from COMMON to shader-readable
    // NOTE: Using NON_PIXEL_SHADER_RESOURCE because vertex shader reads it
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mSculptMap.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::Black, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

    // Descriptor heaps already set at the beginning of Draw()
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
    
    // Bind sculpt map SRV
    CD3DX12_GPU_DESCRIPTOR_HANDLE sculptHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    sculptHandle.Offset(mSculptMapSrvIndex, mCbvSrvDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(6, sculptHandle);

    DrawTerrain(mCommandList.Get());
    
    // Transition sculpt map back to common state
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mSculptMap.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));

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
    
    if (mSculptMode && (btnState & MK_LBUTTON))
        mSculpting = true;
}

void TerrainApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ReleaseCapture();
    mSculpting = false;
}

void TerrainApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    // Mouse input handling: different behavior based on current mode
    if (mSculptMode && (btnState & MK_LBUTTON) != 0)
    {
        // Sculpt mode: LMB triggers terrain modification
        mSculpting = true;
    }
    else if ((btnState & MK_LBUTTON) != 0)
    {
        // Normal mode: LMB rotates camera (standard FPS controls)
        float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
        float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));
        mCamera.Pitch(dy);
        mCamera.RotateY(dx);
    }
    else
    {
        mSculpting = false;  // Stop sculpting when LMB released
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

    // Context-sensitive key bindings: 1/2 keys change meaning based on current mode
    if (mSculptMode)
    {
        // Sculpt mode: select brush operation type
        if (GetAsyncKeyState('1') & 0x8000) mSculptBrushType = 0; // Subtractive brush (dig)
        if (GetAsyncKeyState('2') & 0x8000) mSculptBrushType = 1; // Additive brush (raise)
    }
    else
    {
        // Normal mode: toggle rendering style
        if (GetAsyncKeyState('1') & 0x8000) mWireframe = false;   // Solid rendering
        if (GetAsyncKeyState('2') & 0x8000) mWireframe = true;    // Wireframe rendering
    }
    
    // Toggle sculpt mode with P key
    static bool pKeyWasDown = false;
    bool pKeyIsDown = (GetAsyncKeyState('P') & 0x8000) != 0;
    if (pKeyIsDown && !pKeyWasDown)
    {
        mSculptMode = !mSculptMode;
    }
    pKeyWasDown = pKeyIsDown;
    
    // Adjust brush size with [ and ]
    if (GetAsyncKeyState(VK_OEM_4) & 0x8000) // [
        mBrushRadius = max(0.01f, mBrushRadius - 0.001f);
    if (GetAsyncKeyState(VK_OEM_6) & 0x8000) // ]
        mBrushRadius = min(0.2f, mBrushRadius + 0.001f);

    mCamera.UpdateViewMatrix();
}

void TerrainApp::UpdateTerrainInstances(const GameTimer& gt)
{
    // Use the quadtree for per-tile LOD selection
    // This implements the Geometry Clipmaps concept from GPU Gems 2, Chapter 2:
    // Each tile independently selects its LOD based on distance to camera,
    // so close tiles are highly detailed while distant tiles are coarser.
    
    XMMATRIX view = mCamera.GetView();
    XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
    
    // Transform frustum to world space
    BoundingFrustum worldFrustum;
    mCamFrustum.Transform(worldFrustum, invView);

    XMFLOAT3 camPos = mCamera.GetPosition3f();
    
    // The quadtree traverses the terrain hierarchy and selects tiles based on
    // screen-space error: tiles with too much error get subdivided, others are rendered.
    // This naturally creates a "nested grid" pattern where close areas are detailed.
    mQuadTree.SelectTiles(camPos, worldFrustum, mVisibleTiles);

    // Upload instance data to GPU
    for (size_t i = 0; i < mVisibleTiles.size() && i < 64; ++i)
    {
        const TerrainTile& tile = mVisibleTiles[i];
        
        TerrainTileInstanceGPU inst;
        inst.World = tile.World;
        inst.HeightMapIndex = tile.HeightMapIndex;
        inst.DiffuseMapIndex = tile.DiffuseMapIndex;
        inst.NormalMapIndex = tile.NormalMapIndex;
        inst.LODLevel = tile.Level;
        inst.UVOffset = tile.UVOffset;
        inst.UVScale = tile.UVScale;
        
        mTileInstanceBuffers[mCurrFrameResourceIndex]->CopyData((int)i, inst);
    }

    // Update window title with LOD statistics
    int countL0 = 0, countL1 = 0, countL2 = 0;
    for (const auto& t : mVisibleTiles)
    {
        if (t.Level == 0) countL0++;
        else if (t.Level == 1) countL1++;
        else countL2++;
    }

    std::wostringstream outs;
    outs << L"Terrain Clipmap LOD - Tiles: " << mVisibleTiles.size()
         << L" (L0:" << countL0 << L" L1:" << countL1 << L" L2:" << countL2 << L")";
    if (mSculptMode)
    {
        outs << L" | SCULPT: " << (mSculptBrushType == 0 ? L"DIG(1)" : L"RAISE(2)");
        outs << L" r=" << mBrushRadius << L" [/]=size | P=exit";
    }
    else
        outs << L" | P=Sculpt | 1/2=Solid/Wire | WASD+QE+Mouse";
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
            int idx = TerrainQuadTree::GetTextureIndex(1, x, z);
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
            int idx = TerrainQuadTree::GetTextureIndex(2, x, z);
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
    
    CD3DX12_DESCRIPTOR_RANGE sculptTable;
    sculptTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 63, 0); // t63 for sculpt map

    CD3DX12_ROOT_PARAMETER slotRootParameter[7];
    slotRootParameter[0].InitAsConstantBufferView(0);
    slotRootParameter[1].InitAsConstantBufferView(1);
    slotRootParameter[2].InitAsShaderResourceView(0, 1);
    slotRootParameter[3].InitAsDescriptorTable(1, &heightTable, D3D12_SHADER_VISIBILITY_VERTEX);
    slotRootParameter[4].InitAsDescriptorTable(1, &diffuseTable, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[5].InitAsDescriptorTable(1, &normalTable, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[6].InitAsDescriptorTable(1, &sculptTable, D3D12_SHADER_VISIBILITY_VERTEX);

    auto staticSamplers = GetStaticSamplers();

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(7, slotRootParameter,
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
    // +2 for sculpt map (1 SRV + 1 UAV)
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = gTotalTileTextures * 3 + 2;
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
    
    // Sculpt map SRV (for vertex shader to read)
    mSculptMapSrvIndex = gTotalTileTextures * 3;
    D3D12_SHADER_RESOURCE_VIEW_DESC sculptSrvDesc = {};
    sculptSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sculptSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    sculptSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sculptSrvDesc.Texture2D.MostDetailedMip = 0;
    sculptSrvDesc.Texture2D.MipLevels = 1;
    sculptSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    md3dDevice->CreateShaderResourceView(mSculptMap.Get(), &sculptSrvDesc, hDescriptor);
    hDescriptor.Offset(1, mCbvSrvDescriptorSize);
    
    // Sculpt map UAV (for compute shader to write)
    mSculptMapUavIndex = gTotalTileTextures * 3 + 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC sculptUavDesc = {};
    sculptUavDesc.Format = DXGI_FORMAT_R32_FLOAT;
    sculptUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    sculptUavDesc.Texture2D.MipSlice = 0;
    md3dDevice->CreateUnorderedAccessView(mSculptMap.Get(), nullptr, &sculptUavDesc, hDescriptor);
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
    // Create unit grid [0,1] x [0,1] with skirts on all 4 edges
    // Skirts are vertical strips that hang down to hide gaps between LOD levels
    int gridSize = mPatchGridSize;
    float step = 1.0f / (gridSize - 1);
    
    // Main grid vertices + skirt vertices (4 edges, each edge has gridSize vertices)
    int mainVertexCount = gridSize * gridSize;
    int skirtVertexCount = gridSize * 4; // 4 edges
    int vertexCount = mainVertexCount + skirtVertexCount;
    
    // Main grid indices + skirt indices (4 edges, each edge has (gridSize-1) quads = (gridSize-1)*6 indices)
    int mainIndexCount = (gridSize - 1) * (gridSize - 1) * 6;
    int skirtIndexCount = (gridSize - 1) * 4 * 6; // 4 edges
    int indexCount = mainIndexCount + skirtIndexCount;

    std::vector<TerrainVertex> vertices(vertexCount);
    std::vector<std::uint32_t> indices(indexCount);

    // Create main grid vertices (Y=0, will be displaced by heightmap in shader)
    for (int z = 0; z < gridSize; ++z)
    {
        for (int x = 0; x < gridSize; ++x)
        {
            int i = z * gridSize + x;
            vertices[i].Pos = XMFLOAT3(x * step, 0.0f, z * step);
            vertices[i].TexC = XMFLOAT2(x * step, z * step);
        }
    }
    
    // Create skirt vertices (Y=-1, shader will recognize and drop them down)
    // Skirt vertices have same XZ and UV as edge vertices, but Y=-1 marks them as skirt
    int skirtBase = mainVertexCount;
    
    // Bottom edge (z=0)
    for (int x = 0; x < gridSize; ++x)
    {
        int i = skirtBase + x;
        vertices[i].Pos = XMFLOAT3(x * step, -1.0f, 0.0f);
        vertices[i].TexC = XMFLOAT2(x * step, 0.0f);
    }
    
    // Top edge (z=gridSize-1)
    for (int x = 0; x < gridSize; ++x)
    {
        int i = skirtBase + gridSize + x;
        vertices[i].Pos = XMFLOAT3(x * step, -1.0f, 1.0f);
        vertices[i].TexC = XMFLOAT2(x * step, 1.0f);
    }
    
    // Left edge (x=0)
    for (int z = 0; z < gridSize; ++z)
    {
        int i = skirtBase + gridSize * 2 + z;
        vertices[i].Pos = XMFLOAT3(0.0f, -1.0f, z * step);
        vertices[i].TexC = XMFLOAT2(0.0f, z * step);
    }
    
    // Right edge (x=gridSize-1)
    for (int z = 0; z < gridSize; ++z)
    {
        int i = skirtBase + gridSize * 3 + z;
        vertices[i].Pos = XMFLOAT3(1.0f, -1.0f, z * step);
        vertices[i].TexC = XMFLOAT2(1.0f, z * step);
    }

    // Create main grid indices
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
    
    // Create skirt indices - connect edge vertices to skirt vertices
    // Bottom edge skirt (hangs down from z=0 edge)
    for (int x = 0; x < gridSize - 1; ++x)
    {
        int edgeL = x;                          // main grid vertex
        int edgeR = x + 1;                      // main grid vertex
        int skirtL = skirtBase + x;             // skirt vertex
        int skirtR = skirtBase + x + 1;         // skirt vertex
        
        // Two triangles forming quad (winding for front face when viewed from outside)
        indices[idx++] = skirtL;
        indices[idx++] = edgeL;
        indices[idx++] = skirtR;
        indices[idx++] = skirtR;
        indices[idx++] = edgeL;
        indices[idx++] = edgeR;
    }
    
    // Top edge skirt (hangs down from z=gridSize-1 edge)
    for (int x = 0; x < gridSize - 1; ++x)
    {
        int edgeL = (gridSize - 1) * gridSize + x;
        int edgeR = edgeL + 1;
        int skirtL = skirtBase + gridSize + x;
        int skirtR = skirtL + 1;
        
        // Opposite winding
        indices[idx++] = edgeL;
        indices[idx++] = skirtL;
        indices[idx++] = edgeR;
        indices[idx++] = edgeR;
        indices[idx++] = skirtL;
        indices[idx++] = skirtR;
    }
    
    // Left edge skirt (hangs down from x=0 edge)
    for (int z = 0; z < gridSize - 1; ++z)
    {
        int edgeB = z * gridSize;
        int edgeT = (z + 1) * gridSize;
        int skirtB = skirtBase + gridSize * 2 + z;
        int skirtT = skirtB + 1;
        
        indices[idx++] = edgeB;
        indices[idx++] = skirtB;
        indices[idx++] = edgeT;
        indices[idx++] = edgeT;
        indices[idx++] = skirtB;
        indices[idx++] = skirtT;
    }
    
    // Right edge skirt (hangs down from x=gridSize-1 edge)
    for (int z = 0; z < gridSize - 1; ++z)
    {
        int edgeB = z * gridSize + (gridSize - 1);
        int edgeT = (z + 1) * gridSize + (gridSize - 1);
        int skirtB = skirtBase + gridSize * 3 + z;
        int skirtT = skirtB + 1;
        
        indices[idx++] = skirtB;
        indices[idx++] = edgeB;
        indices[idx++] = skirtT;
        indices[idx++] = skirtT;
        indices[idx++] = edgeB;
        indices[idx++] = edgeT;
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
        mTileInstanceBuffers[i] = std::make_unique<UploadBuffer<TerrainTileInstanceGPU>>(md3dDevice.Get(), 64, false);
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

// Terrain sculpting implementation

void TerrainApp::BuildSculptResources()
{
    // Create sculpt map texture (R32_FLOAT, stores height modifications)
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = SCULPT_MAP_SIZE;
    texDesc.Height = SCULPT_MAP_SIZE;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&mSculptMap)));
    
    mSculptMap->SetName(L"SculptMap");
    
    // Create upload buffer for initial clear (all zeros)
    const UINT64 uploadBufferSize = GetRequiredIntermediateSize(mSculptMap.Get(), 0, 1);
    CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
    
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &uploadBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mSculptMapUpload)));
    
    // Initialize sculpt map to zeros
    std::vector<float> zeroData(SCULPT_MAP_SIZE * SCULPT_MAP_SIZE, 0.0f);
    
    D3D12_SUBRESOURCE_DATA subresourceData = {};
    subresourceData.pData = zeroData.data();
    subresourceData.RowPitch = SCULPT_MAP_SIZE * sizeof(float);
    subresourceData.SlicePitch = subresourceData.RowPitch * SCULPT_MAP_SIZE;
    
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mSculptMap.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
    
    UpdateSubresources(mCommandList.Get(), mSculptMap.Get(), mSculptMapUpload.Get(), 0, 0, 1, &subresourceData);
    
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mSculptMap.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON));
    
    // Create constant buffer for brush parameters
    mSculptBrushCB = std::make_unique<UploadBuffer<SculptBrushCB>>(md3dDevice.Get(), 1, true);
}

void TerrainApp::BuildSculptRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE uavTable;
    uavTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
    
    CD3DX12_ROOT_PARAMETER slotRootParameter[2];
    slotRootParameter[0].InitAsConstantBufferView(0);
    slotRootParameter[1].InitAsDescriptorTable(1, &uavTable);
    
    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(2, slotRootParameter, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_NONE);
    
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
        IID_PPV_ARGS(&mSculptRootSignature)));
}

void TerrainApp::BuildSculptPSO()
{
    mShaders["sculptCS"] = d3dUtil::CompileShader(L"Shaders\\SculptBrush.hlsl", nullptr, "CS", "cs_5_1");
    
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = mSculptRootSignature.Get();
    psoDesc.CS = { reinterpret_cast<BYTE*>(mShaders["sculptCS"]->GetBufferPointer()),
                   mShaders["sculptCS"]->GetBufferSize() };
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    
    ThrowIfFailed(md3dDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mSculptPSO)));
}

void TerrainApp::ApplySculptBrush(float worldX, float worldZ)
{
    // World-to-UV coordinate transformation: world ∈ [-size/2, size/2] → UV ∈ [0,1]
    // Formula: UV = (world + size/2) / size
    float halfSize = mTerrainSize * 0.5f;
    float u = (worldX + halfSize) / mTerrainSize;
    float v = (worldZ + halfSize) / mTerrainSize;
    
    // Clamp to texture bounds (prevent out-of-bounds access)
    u = max(0.0f, min(1.0f, u));
    v = max(0.0f, min(1.0f, v));
    
    // Upload brush parameters to GPU constant buffer
    SculptBrushCB brushCB;
    brushCB.BrushPosUV = XMFLOAT2(u, v);        // Brush center in texture space
    brushCB.BrushRadius = mBrushRadius;          // Radius in UV units (not pixels!)
    brushCB.BrushStrength = mBrushStrength;      // Height delta magnitude
    brushCB.TerrainSize = mTerrainSize;          // For potential world-space calculations
    brushCB.BrushActive = 1;                     // Enable brush in compute shader
    brushCB.BrushType = mSculptBrushType;        // Operation type (add/subtract)
    brushCB.Pad = 0.0f;
    mSculptBrushCB->CopyData(0, brushCB);
    
    // Execute compute shader to modify height texture
    auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;
    
    // Resource state management: enable UAV writes to sculpt map
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mSculptMap.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    
    // Bind compute pipeline and root signature
    mCommandList->SetPipelineState(mSculptPSO.Get());
    mCommandList->SetComputeRootSignature(mSculptRootSignature.Get());
    
    // Bind constant buffer (brush parameters)
    mCommandList->SetComputeRootConstantBufferView(0, mSculptBrushCB->Resource()->GetGPUVirtualAddress());
    
    // Bind UAV descriptor (writable sculpt map texture)
    CD3DX12_GPU_DESCRIPTOR_HANDLE uavHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    uavHandle.Offset(mSculptMapUavIndex, mCbvSrvDescriptorSize);
    mCommandList->SetComputeRootDescriptorTable(1, uavHandle);
    
    // Dispatch compute threads: ceil(512/8) = 64 groups per dimension
    // Total threads: 64×64×8×8 = 262,144 threads for 512×512 texture
    UINT numGroupsX = (SCULPT_MAP_SIZE + 7) / 8;  // Integer ceiling division
    UINT numGroupsY = (SCULPT_MAP_SIZE + 7) / 8;
    mCommandList->Dispatch(numGroupsX, numGroupsY, 1);
    
    // Restore resource state for next frame's vertex shader reads
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mSculptMap.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
}

bool TerrainApp::RaycastTerrain(int mouseX, int mouseY, XMFLOAT3& hitPoint)
{
    // Screen-to-world ray casting for mouse picking
    // Step 1: Convert screen coordinates to NDC space
    // NDC: x,y ∈ [-1,1], z ∈ [0,1] (D3D12 convention)
    float ndcX = (2.0f * mouseX / mClientWidth - 1.0f);   // [0,width] → [-1,1]
    float ndcY = (1.0f - 2.0f * mouseY / mClientHeight);  // [0,height] → [1,-1] (flip Y)
    
    // Step 2: Compute inverse view-projection matrix for unprojection
    XMMATRIX view = mCamera.GetView();
    XMMATRIX proj = mCamera.GetProj();
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);
    
    // Step 3: Create ray in world space by unprojecting near/far points
    // Near plane (z=0) and far plane (z=1) in NDC
    XMVECTOR rayOriginNDC = XMVectorSet(ndcX, ndcY, 0.0f, 1.0f);  // Near plane
    XMVECTOR rayEndNDC = XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);     // Far plane
    
    // Transform from NDC to world space using inverse view-projection
    XMVECTOR rayOriginWorld = XMVector3TransformCoord(rayOriginNDC, invViewProj);
    XMVECTOR rayEndWorld = XMVector3TransformCoord(rayEndNDC, invViewProj);
    XMVECTOR rayDir = XMVector3Normalize(rayEndWorld - rayOriginWorld);
    
    // Step 4: Ray-plane intersection (simplified terrain collision)
    // Assumption: terrain lies on horizontal plane at average height
    // TODO: Could implement proper ray-heightmap intersection for accuracy
    float avgHeight = mTerrainHeight * 0.3f;  // Empirical average terrain height
    
    XMFLOAT3 origin, dir;
    XMStoreFloat3(&origin, rayOriginWorld);
    XMStoreFloat3(&dir, rayDir);
    
    // Ray equation: P(t) = origin + t * dir
    // Plane equation: Y = avgHeight
    // Intersection: origin.y + t * dir.y = avgHeight
    // Solve for t: t = (avgHeight - origin.y) / dir.y
    if (fabsf(dir.y) < 0.0001f)
        return false;  // Ray parallel to plane (no intersection)
    
    float t = (avgHeight - origin.y) / dir.y;
    if (t < 0.0f)
        return false;  // Intersection behind camera (negative t)
    
    // Compute 3D intersection point
    hitPoint.x = origin.x + t * dir.x;
    hitPoint.y = avgHeight;
    hitPoint.z = origin.z + t * dir.z;
    
    // Step 5: Bounds checking - ensure hit point is within terrain area
    float halfSize = mTerrainSize * 0.5f;
    if (hitPoint.x < -halfSize || hitPoint.x > halfSize ||
        hitPoint.z < -halfSize || hitPoint.z > halfSize)
        return false;  // Outside terrain bounds
    
    return true;  // Valid intersection found
}
