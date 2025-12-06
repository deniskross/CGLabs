//***************************************************************************************
// Taa.cpp - Temporal Anti-Aliasing implementation
//***************************************************************************************
#include "Taa.h"

using namespace DirectX;
using namespace Microsoft::WRL;

Taa::Taa(ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    UINT width, UINT height)
    : md3dDevice(device), mWidth(0), mHeight(0)
{
    BuildHaltonSequence();
    OnResize(width, height);
}

void Taa::BuildHaltonSequence()
{
    for (int i = 0; i < JitterSampleCount; ++i)
    {
        mJitterSequence[i].x = Halton(i + 1, 2);
        mJitterSequence[i].y = Halton(i + 1, 3);
    }
}

float Taa::Halton(int index, int base)
{
    float result = 0.0f;
    float f = 1.0f / base;
    int i = index;

    while (i > 0)
    {
        result += f * (i % base);
        i = i / base;
        f = f / base;
    }

    return result;
}

XMFLOAT2 Taa::GetJitterOffset(UINT frameIndex) const
{
    int idx = frameIndex % JitterSampleCount;
    return XMFLOAT2(
        mJitterSequence[idx].x - 0.5f,
        mJitterSequence[idx].y - 0.5f
    );
}

XMFLOAT2 Taa::GetJitterOffsetNDC(UINT frameIndex) const
{
    XMFLOAT2 pixelOffset = GetJitterOffset(frameIndex);
    return XMFLOAT2(
        pixelOffset.x * 2.0f / mWidth,
        pixelOffset.y * 2.0f / mHeight
    );
}

void Taa::BuildDescriptors(
    CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv,
    CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv,
    CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuRtv,
    UINT cbvSrvUavDescriptorSize,
    UINT rtvDescriptorSize)
{
    mCbvSrvUavDescriptorSize = cbvSrvUavDescriptorSize;
    mRtvDescriptorSize = rtvDescriptorSize;

    // TAA needs 4 SRVs: History, CurrentColor, Velocity, Output
    mhHistoryBufferCpuSrv = hCpuSrv;
    mhCurrentColorCpuSrv = CD3DX12_CPU_DESCRIPTOR_HANDLE(hCpuSrv, 1, cbvSrvUavDescriptorSize);
    mhVelocityCpuSrv = CD3DX12_CPU_DESCRIPTOR_HANDLE(hCpuSrv, 2, cbvSrvUavDescriptorSize);
    mhOutputCpuSrv = CD3DX12_CPU_DESCRIPTOR_HANDLE(hCpuSrv, 3, cbvSrvUavDescriptorSize);

    mhHistoryBufferGpuSrv = hGpuSrv;
    mhCurrentColorGpuSrv = CD3DX12_GPU_DESCRIPTOR_HANDLE(hGpuSrv, 1, cbvSrvUavDescriptorSize);
    mhVelocityGpuSrv = CD3DX12_GPU_DESCRIPTOR_HANDLE(hGpuSrv, 2, cbvSrvUavDescriptorSize);
    mhOutputGpuSrv = CD3DX12_GPU_DESCRIPTOR_HANDLE(hGpuSrv, 3, cbvSrvUavDescriptorSize);

    // TAA needs 3 RTVs: CurrentColor, Velocity, Output
    mhCurrentColorCpuRtv = hCpuRtv;
    mhVelocityCpuRtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(hCpuRtv, 1, rtvDescriptorSize);
    mhOutputCpuRtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(hCpuRtv, 2, rtvDescriptorSize);

    RebuildDescriptors();
}

void Taa::RebuildDescriptors()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    srvDesc.Format = ColorFormat;
    md3dDevice->CreateShaderResourceView(mHistoryBuffer.Get(), &srvDesc, mhHistoryBufferCpuSrv);
    md3dDevice->CreateShaderResourceView(mCurrentColorBuffer.Get(), &srvDesc, mhCurrentColorCpuSrv);
    md3dDevice->CreateShaderResourceView(mOutputBuffer.Get(), &srvDesc, mhOutputCpuSrv);

    srvDesc.Format = VelocityFormat;
    md3dDevice->CreateShaderResourceView(mVelocityBuffer.Get(), &srvDesc, mhVelocityCpuSrv);

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;
    rtvDesc.Texture2D.PlaneSlice = 0;

    rtvDesc.Format = ColorFormat;
    md3dDevice->CreateRenderTargetView(mCurrentColorBuffer.Get(), &rtvDesc, mhCurrentColorCpuRtv);
    md3dDevice->CreateRenderTargetView(mOutputBuffer.Get(), &rtvDesc, mhOutputCpuRtv);

    rtvDesc.Format = VelocityFormat;
    md3dDevice->CreateRenderTargetView(mVelocityBuffer.Get(), &rtvDesc, mhVelocityCpuRtv);
}

void Taa::SetPSOs(ID3D12PipelineState* taaPso, ID3D12PipelineState* velocityPso)
{
    mTaaPso = taaPso;
    mVelocityPso = velocityPso;
}

void Taa::OnResize(UINT newWidth, UINT newHeight)
{
    if (mWidth != newWidth || mHeight != newHeight)
    {
        mWidth = newWidth;
        mHeight = newHeight;

        mViewport.TopLeftX = 0.0f;
        mViewport.TopLeftY = 0.0f;
        mViewport.Width = static_cast<float>(mWidth);
        mViewport.Height = static_cast<float>(mHeight);
        mViewport.MinDepth = 0.0f;
        mViewport.MaxDepth = 1.0f;

        mScissorRect = { 0, 0, static_cast<int>(mWidth), static_cast<int>(mHeight) };

        BuildResources();
    }
}

void Taa::BuildResources()
{
    mHistoryBuffer = nullptr;
    mCurrentColorBuffer = nullptr;
    mVelocityBuffer = nullptr;
    mOutputBuffer = nullptr;

    // Color buffers
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = mWidth;
    texDesc.Height = mHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = ColorFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    float colorClearValue[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    CD3DX12_CLEAR_VALUE colorOptClear(ColorFormat, colorClearValue);

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        &colorOptClear,
        IID_PPV_ARGS(&mHistoryBuffer)));
    mHistoryBuffer->SetName(L"TAA History Buffer");

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        &colorOptClear,
        IID_PPV_ARGS(&mCurrentColorBuffer)));
    mCurrentColorBuffer->SetName(L"TAA Current Color Buffer");

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        &colorOptClear,
        IID_PPV_ARGS(&mOutputBuffer)));
    mOutputBuffer->SetName(L"TAA Output Buffer");

    // Velocity buffer (R16G16_FLOAT)
    texDesc.Format = VelocityFormat;
    float velocityClearValue[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    CD3DX12_CLEAR_VALUE velocityOptClear(VelocityFormat, velocityClearValue);

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        &velocityOptClear,
        IID_PPV_ARGS(&mVelocityBuffer)));
    mVelocityBuffer->SetName(L"TAA Velocity Buffer");
}

void Taa::TransitionVelocityForWrite(ID3D12GraphicsCommandList* cmdList)
{
    cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mVelocityBuffer.Get(),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        D3D12_RESOURCE_STATE_RENDER_TARGET));
}

void Taa::TransitionVelocityForRead(ID3D12GraphicsCommandList* cmdList)
{
    cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mVelocityBuffer.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_GENERIC_READ));
}

void Taa::Execute(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12RootSignature* rootSig,
    FrameResource* currFrame)
{
    cmdList->RSSetViewports(1, &mViewport);
    cmdList->RSSetScissorRects(1, &mScissorRect);

    cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputBuffer.Get(),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        D3D12_RESOURCE_STATE_RENDER_TARGET));

    float clearValue[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    cmdList->ClearRenderTargetView(mhOutputCpuRtv, clearValue, 0, nullptr);

    cmdList->OMSetRenderTargets(1, &mhOutputCpuRtv, true, nullptr);

    auto taaCBAddress = currFrame->TaaCB->Resource()->GetGPUVirtualAddress();
    cmdList->SetGraphicsRootConstantBufferView(0, taaCBAddress);

    cmdList->SetGraphicsRootDescriptorTable(1, mhHistoryBufferGpuSrv);

    cmdList->SetPipelineState(mTaaPso);

    cmdList->IASetVertexBuffers(0, 0, nullptr);
    cmdList->IASetIndexBuffer(nullptr);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(6, 1, 0, 0);

    cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputBuffer.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_GENERIC_READ));
}

void Taa::CopyToHistory(ID3D12GraphicsCommandList* cmdList)
{
    CD3DX12_RESOURCE_BARRIER barriers[2];
    barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        mHistoryBuffer.Get(),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        D3D12_RESOURCE_STATE_COPY_DEST);
    barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputBuffer.Get(),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->ResourceBarrier(2, barriers);

    cmdList->CopyResource(mHistoryBuffer.Get(), mOutputBuffer.Get());

    barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        mHistoryBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    cmdList->ResourceBarrier(2, barriers);
}
