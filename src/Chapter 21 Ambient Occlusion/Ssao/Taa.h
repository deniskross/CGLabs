//***************************************************************************************
// Taa.h - Temporal Anti-Aliasing implementation
// Based on tutorials:
// - https://sugulee.wordpress.com/2021/06/21/temporal-anti-aliasingtaa-tutorial/
// - https://alextardif.com/TAA.html
// - https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/
//***************************************************************************************

#ifndef TAA_H
#define TAA_H

#pragma once

#include "../../Common/d3dUtil.h"
#include "FrameResource.h"

class Taa
{
public:
    Taa(ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        UINT width, UINT height);
    Taa(const Taa& rhs) = delete;
    Taa& operator=(const Taa& rhs) = delete;
    ~Taa() = default;

    static const DXGI_FORMAT ColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    static const DXGI_FORMAT VelocityFormat = DXGI_FORMAT_R16G16_FLOAT;
    static const int JitterSampleCount = 16;

    UINT Width() const { return mWidth; }
    UINT Height() const { return mHeight; }

    // Get jitter offset for current frame (in pixels)
    DirectX::XMFLOAT2 GetJitterOffset(UINT frameIndex) const;
    
    // Get jitter offset in NDC space [-1, 1]
    DirectX::XMFLOAT2 GetJitterOffsetNDC(UINT frameIndex) const;

    // Resources
    ID3D12Resource* HistoryBuffer() { return mHistoryBuffer.Get(); }
    ID3D12Resource* CurrentColorBuffer() { return mCurrentColorBuffer.Get(); }
    ID3D12Resource* VelocityBuffer() { return mVelocityBuffer.Get(); }
    ID3D12Resource* OutputBuffer() { return mOutputBuffer.Get(); }

    // Descriptor handles
    CD3DX12_GPU_DESCRIPTOR_HANDLE HistoryBufferSrvGpu() const { return mhHistoryBufferGpuSrv; }
    CD3DX12_CPU_DESCRIPTOR_HANDLE VelocityRtv() const { return mhVelocityCpuRtv; }
    CD3DX12_CPU_DESCRIPTOR_HANDLE OutputRtv() const { return mhOutputCpuRtv; }

    void BuildDescriptors(
        CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv,
        CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv,
        CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuRtv,
        UINT cbvSrvUavDescriptorSize,
        UINT rtvDescriptorSize);

    void RebuildDescriptors();

    void SetPSOs(ID3D12PipelineState* taaPso, ID3D12PipelineState* velocityPso);

    void OnResize(UINT newWidth, UINT newHeight);

    // Execute TAA resolve pass
    void Execute(
        ID3D12GraphicsCommandList* cmdList,
        ID3D12RootSignature* rootSig,
        FrameResource* currFrame);

    // Copy output to history for next frame
    void CopyToHistory(ID3D12GraphicsCommandList* cmdList);

    // Velocity buffer state transitions
    void TransitionVelocityForWrite(ID3D12GraphicsCommandList* cmdList);
    void TransitionVelocityForRead(ID3D12GraphicsCommandList* cmdList);

    D3D12_VIEWPORT Viewport() const { return mViewport; }
    D3D12_RECT ScissorRect() const { return mScissorRect; }

private:
    void BuildResources();
    void BuildHaltonSequence();
    float Halton(int index, int base);

private:
    ID3D12Device* md3dDevice;

    ID3D12PipelineState* mTaaPso = nullptr;
    ID3D12PipelineState* mVelocityPso = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> mHistoryBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> mCurrentColorBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> mVelocityBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> mOutputBuffer;

    // SRV handles
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhHistoryBufferCpuSrv;
    CD3DX12_GPU_DESCRIPTOR_HANDLE mhHistoryBufferGpuSrv;
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhCurrentColorCpuSrv;
    CD3DX12_GPU_DESCRIPTOR_HANDLE mhCurrentColorGpuSrv;
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhVelocityCpuSrv;
    CD3DX12_GPU_DESCRIPTOR_HANDLE mhVelocityGpuSrv;
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhOutputCpuSrv;
    CD3DX12_GPU_DESCRIPTOR_HANDLE mhOutputGpuSrv;

    // RTV handles
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhCurrentColorCpuRtv;
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhVelocityCpuRtv;
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhOutputCpuRtv;

    UINT mWidth = 0;
    UINT mHeight = 0;

    UINT mCbvSrvUavDescriptorSize = 0;
    UINT mRtvDescriptorSize = 0;

    // Halton jitter sequence
    DirectX::XMFLOAT2 mJitterSequence[JitterSampleCount];

    D3D12_VIEWPORT mViewport;
    D3D12_RECT mScissorRect;
};

#endif // TAA_H
