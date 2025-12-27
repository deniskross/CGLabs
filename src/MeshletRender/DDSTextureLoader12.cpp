//--------------------------------------------------------------------------------------
// File: DDSTextureLoader12.cpp
//
// Functions for loading a DDS texture and creating a Direct3D runtime resource for it
//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//--------------------------------------------------------------------------------------

#include "stdafx.h"
#include "DDSTextureLoader12.h"

#include <algorithm>
#include <cassert>
#include <memory>

using namespace DirectX;

//--------------------------------------------------------------------------------------
// DDS file structure definitions
//--------------------------------------------------------------------------------------
#pragma pack(push,1)

const uint32_t DDS_MAGIC = 0x20534444; // "DDS "

struct DDS_PIXELFORMAT
{
    uint32_t    size;
    uint32_t    flags;
    uint32_t    fourCC;
    uint32_t    RGBBitCount;
    uint32_t    RBitMask;
    uint32_t    GBitMask;
    uint32_t    BBitMask;
    uint32_t    ABitMask;
};

#define DDS_FOURCC      0x00000004  // DDPF_FOURCC
#define DDS_RGB         0x00000040  // DDPF_RGB
#define DDS_LUMINANCE   0x00020000  // DDPF_LUMINANCE
#define DDS_ALPHA       0x00000002  // DDPF_ALPHA
#define DDS_BUMPDUDV    0x00080000  // DDPF_BUMPDUDV

#define DDS_HEADER_FLAGS_TEXTURE        0x00001007  // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT
#define DDS_HEADER_FLAGS_MIPMAP         0x00020000  // DDSD_MIPMAPCOUNT
#define DDS_HEADER_FLAGS_VOLUME         0x00800000  // DDSD_DEPTH
#define DDS_HEADER_FLAGS_PITCH          0x00000008  // DDSD_PITCH
#define DDS_HEADER_FLAGS_LINEARSIZE     0x00080000  // DDSD_LINEARSIZE

#define DDS_SURFACE_FLAGS_TEXTURE 0x00001000 // DDSCAPS_TEXTURE
#define DDS_SURFACE_FLAGS_MIPMAP  0x00400008 // DDSCAPS_COMPLEX | DDSCAPS_MIPMAP
#define DDS_SURFACE_FLAGS_CUBEMAP 0x00000008 // DDSCAPS_COMPLEX

#define DDS_CUBEMAP_POSITIVEX 0x00000600 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_POSITIVEX
#define DDS_CUBEMAP_NEGATIVEX 0x00000a00 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_NEGATIVEX
#define DDS_CUBEMAP_POSITIVEY 0x00001200 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_POSITIVEY
#define DDS_CUBEMAP_NEGATIVEY 0x00002200 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_NEGATIVEY
#define DDS_CUBEMAP_POSITIVEZ 0x00004200 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_POSITIVEZ
#define DDS_CUBEMAP_NEGATIVEZ 0x00008200 // DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_NEGATIVEZ

#define DDS_CUBEMAP_ALLFACES ( DDS_CUBEMAP_POSITIVEX | DDS_CUBEMAP_NEGATIVEX |\
                               DDS_CUBEMAP_POSITIVEY | DDS_CUBEMAP_NEGATIVEY |\
                               DDS_CUBEMAP_POSITIVEZ | DDS_CUBEMAP_NEGATIVEZ )

#define DDS_CUBEMAP 0x00000200 // DDSCAPS2_CUBEMAP

struct DDS_HEADER
{
    uint32_t        size;
    uint32_t        flags;
    uint32_t        height;
    uint32_t        width;
    uint32_t        pitchOrLinearSize;
    uint32_t        depth;
    uint32_t        mipMapCount;
    uint32_t        reserved1[11];
    DDS_PIXELFORMAT ddspf;
    uint32_t        caps;
    uint32_t        caps2;
    uint32_t        caps3;
    uint32_t        caps4;
    uint32_t        reserved2;
};

struct DDS_HEADER_DXT10
{
    DXGI_FORMAT     dxgiFormat;
    uint32_t        resourceDimension;
    uint32_t        miscFlag;
    uint32_t        arraySize;
    uint32_t        miscFlags2;
};

#pragma pack(pop)

//--------------------------------------------------------------------------------------
#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3)                              \
                ((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) |       \
                ((uint32_t)(uint8_t)(ch2) << 16) | ((uint32_t)(uint8_t)(ch3) << 24 ))
#endif

//--------------------------------------------------------------------------------------
static DXGI_FORMAT GetDXGIFormat(const DDS_PIXELFORMAT& ddpf) noexcept
{
    if (ddpf.flags & DDS_RGB)
    {
        switch (ddpf.RGBBitCount)
        {
        case 32:
            if (ddpf.RBitMask == 0x000000ff && ddpf.GBitMask == 0x0000ff00 && 
                ddpf.BBitMask == 0x00ff0000 && ddpf.ABitMask == 0xff000000)
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            if (ddpf.RBitMask == 0x00ff0000 && ddpf.GBitMask == 0x0000ff00 && 
                ddpf.BBitMask == 0x000000ff && ddpf.ABitMask == 0xff000000)
                return DXGI_FORMAT_B8G8R8A8_UNORM;
            if (ddpf.RBitMask == 0x00ff0000 && ddpf.GBitMask == 0x0000ff00 && 
                ddpf.BBitMask == 0x000000ff && ddpf.ABitMask == 0x00000000)
                return DXGI_FORMAT_B8G8R8X8_UNORM;
            break;
        }
    }
    else if (ddpf.flags & DDS_FOURCC)
    {
        if (MAKEFOURCC('D', 'X', 'T', '1') == ddpf.fourCC)
            return DXGI_FORMAT_BC1_UNORM;
        if (MAKEFOURCC('D', 'X', 'T', '3') == ddpf.fourCC)
            return DXGI_FORMAT_BC2_UNORM;
        if (MAKEFOURCC('D', 'X', 'T', '5') == ddpf.fourCC)
            return DXGI_FORMAT_BC3_UNORM;
        if (MAKEFOURCC('D', 'X', '1', '0') == ddpf.fourCC)
            return DXGI_FORMAT_UNKNOWN; // Will use DX10 header
        if (MAKEFOURCC('B', 'C', '4', 'U') == ddpf.fourCC)
            return DXGI_FORMAT_BC4_UNORM;
        if (MAKEFOURCC('B', 'C', '4', 'S') == ddpf.fourCC)
            return DXGI_FORMAT_BC4_SNORM;
        if (MAKEFOURCC('B', 'C', '5', 'U') == ddpf.fourCC)
            return DXGI_FORMAT_BC5_UNORM;
        if (MAKEFOURCC('B', 'C', '5', 'S') == ddpf.fourCC)
            return DXGI_FORMAT_BC5_SNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}


//--------------------------------------------------------------------------------------
static size_t BitsPerPixel(DXGI_FORMAT fmt) noexcept
{
    switch (fmt)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 128;

    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return 96;

    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
        return 64;

    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return 32;

    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
        return 16;

    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM:
        return 8;

    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return 4;

    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return 8;

    default:
        return 0;
    }
}

//--------------------------------------------------------------------------------------
static bool IsCompressed(DXGI_FORMAT fmt) noexcept
{
    switch (fmt)
    {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

//--------------------------------------------------------------------------------------
static HRESULT GetSurfaceInfo(
    size_t width,
    size_t height,
    DXGI_FORMAT fmt,
    size_t* outNumBytes,
    size_t* outRowBytes,
    size_t* outNumRows) noexcept
{
    size_t numBytes = 0;
    size_t rowBytes = 0;
    size_t numRows = 0;

    bool bc = false;
    bool packed = false;
    size_t bpe = 0;
    switch (fmt)
    {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        bc = true;
        bpe = 8;
        break;

    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        bc = true;
        bpe = 16;
        break;

    default:
        break;
    }

    if (bc)
    {
        size_t numBlocksWide = 0;
        if (width > 0)
        {
            numBlocksWide = std::max<size_t>(1u, (width + 3u) / 4u);
        }
        size_t numBlocksHigh = 0;
        if (height > 0)
        {
            numBlocksHigh = std::max<size_t>(1u, (height + 3u) / 4u);
        }
        rowBytes = numBlocksWide * bpe;
        numRows = numBlocksHigh;
        numBytes = rowBytes * numBlocksHigh;
    }
    else if (packed)
    {
        rowBytes = ((width + 1u) >> 1) * bpe;
        numRows = height;
        numBytes = rowBytes * height;
    }
    else
    {
        size_t bpp = BitsPerPixel(fmt);
        if (!bpp)
            return E_INVALIDARG;

        rowBytes = (width * bpp + 7u) / 8u;
        numRows = height;
        numBytes = rowBytes * height;
    }

    if (outNumBytes)
        *outNumBytes = numBytes;
    if (outRowBytes)
        *outRowBytes = rowBytes;
    if (outNumRows)
        *outNumRows = numRows;

    return S_OK;
}


//--------------------------------------------------------------------------------------
static HRESULT FillInitData(
    size_t width,
    size_t height,
    size_t depth,
    size_t mipCount,
    size_t arraySize,
    DXGI_FORMAT format,
    size_t maxsize,
    size_t bitSize,
    const uint8_t* bitData,
    size_t& twidth,
    size_t& theight,
    size_t& tdepth,
    size_t& skipMip,
    std::vector<D3D12_SUBRESOURCE_DATA>& initData) noexcept
{
    if (!bitData)
        return E_POINTER;

    skipMip = 0;
    twidth = 0;
    theight = 0;
    tdepth = 0;

    size_t NumBytes = 0;
    size_t RowBytes = 0;
    const uint8_t* pSrcBits = bitData;
    const uint8_t* pEndBits = bitData + bitSize;

    initData.clear();

    for (size_t j = 0; j < arraySize; j++)
    {
        size_t w = width;
        size_t h = height;
        size_t d = depth;
        for (size_t i = 0; i < mipCount; i++)
        {
            HRESULT hr = GetSurfaceInfo(w, h, format, &NumBytes, &RowBytes, nullptr);
            if (FAILED(hr))
                return hr;

            if (mipCount <= 1 || !maxsize || (w <= maxsize && h <= maxsize && d <= maxsize))
            {
                if (!twidth)
                {
                    twidth = w;
                    theight = h;
                    tdepth = d;
                }

                D3D12_SUBRESOURCE_DATA res = {};
                res.pData = pSrcBits;
                res.RowPitch = static_cast<LONG_PTR>(RowBytes);
                res.SlicePitch = static_cast<LONG_PTR>(NumBytes);
                initData.push_back(res);
            }
            else if (!j)
            {
                skipMip++;
            }

            if (pSrcBits + (NumBytes * d) > pEndBits)
            {
                return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
            }

            pSrcBits += NumBytes * d;

            w = w >> 1;
            h = h >> 1;
            d = d >> 1;
            if (w == 0)
                w = 1;
            if (h == 0)
                h = 1;
            if (d == 0)
                d = 1;
        }
    }

    return initData.empty() ? E_FAIL : S_OK;
}

//--------------------------------------------------------------------------------------
static HRESULT CreateTextureResource(
    ID3D12Device* d3dDevice,
    D3D12_RESOURCE_DIMENSION resDim,
    size_t width,
    size_t height,
    size_t depth,
    size_t mipCount,
    size_t arraySize,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS resFlags,
    bool forceSRGB,
    ID3D12Resource** texture) noexcept
{
    if (!d3dDevice)
        return E_POINTER;

    HRESULT hr = E_FAIL;

    if (forceSRGB)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            break;
        case DXGI_FORMAT_BC1_UNORM:
            format = DXGI_FORMAT_BC1_UNORM_SRGB;
            break;
        case DXGI_FORMAT_BC2_UNORM:
            format = DXGI_FORMAT_BC2_UNORM_SRGB;
            break;
        case DXGI_FORMAT_BC3_UNORM:
            format = DXGI_FORMAT_BC3_UNORM_SRGB;
            break;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            break;
        case DXGI_FORMAT_B8G8R8X8_UNORM:
            format = DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
            break;
        case DXGI_FORMAT_BC7_UNORM:
            format = DXGI_FORMAT_BC7_UNORM_SRGB;
            break;
        default:
            break;
        }
    }

    D3D12_RESOURCE_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = static_cast<UINT16>(mipCount);
    desc.DepthOrArraySize = (resDim == D3D12_RESOURCE_DIMENSION_TEXTURE3D) 
        ? static_cast<UINT16>(depth) : static_cast<UINT16>(arraySize);
    desc.Format = format;
    desc.Flags = resFlags;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Dimension = resDim;

    CD3DX12_HEAP_PROPERTIES defaultHeapProperties(D3D12_HEAP_TYPE_DEFAULT);

    hr = d3dDevice->CreateCommittedResource(
        &defaultHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(texture));

    return hr;
}

//--------------------------------------------------------------------------------------
static HRESULT CreateTextureFromDDS(
    ID3D12Device* d3dDevice,
    const DDS_HEADER* header,
    const uint8_t* bitData,
    size_t bitSize,
    size_t maxsize,
    D3D12_RESOURCE_FLAGS resFlags,
    bool forceSRGB,
    ID3D12Resource** texture,
    std::vector<D3D12_SUBRESOURCE_DATA>& subresources,
    bool* isCubeMap) noexcept
{
    HRESULT hr = S_OK;

    UINT width = header->width;
    UINT height = header->height;
    UINT depth = header->depth;

    D3D12_RESOURCE_DIMENSION resDim = D3D12_RESOURCE_DIMENSION_UNKNOWN;
    UINT arraySize = 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool isCube = false;

    size_t mipCount = header->mipMapCount;
    if (0 == mipCount)
        mipCount = 1;

    if ((header->ddspf.flags & DDS_FOURCC) && (MAKEFOURCC('D', 'X', '1', '0') == header->ddspf.fourCC))
    {
        auto d3d10ext = reinterpret_cast<const DDS_HEADER_DXT10*>(reinterpret_cast<const char*>(header) + sizeof(DDS_HEADER));

        arraySize = d3d10ext->arraySize;
        if (arraySize == 0)
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);

        switch (d3d10ext->dxgiFormat)
        {
        case DXGI_FORMAT_AI44:
        case DXGI_FORMAT_IA44:
        case DXGI_FORMAT_P8:
        case DXGI_FORMAT_A8P8:
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        default:
            if (BitsPerPixel(d3d10ext->dxgiFormat) == 0)
                return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }

        format = d3d10ext->dxgiFormat;

        switch (d3d10ext->resourceDimension)
        {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            if ((header->flags & DDS_HEADER_FLAGS_VOLUME) || (height != 1))
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            break;

        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
            if (d3d10ext->miscFlag & 0x4 /* D3D11_RESOURCE_MISC_TEXTURECUBE */)
            {
                arraySize *= 6;
                isCube = true;
            }
            depth = 1;
            break;

        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            if (!(header->flags & DDS_HEADER_FLAGS_VOLUME))
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            if (arraySize > 1)
                return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            break;

        default:
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }

        resDim = static_cast<D3D12_RESOURCE_DIMENSION>(d3d10ext->resourceDimension);
    }
    else
    {
        format = GetDXGIFormat(header->ddspf);

        if (format == DXGI_FORMAT_UNKNOWN)
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

        if (header->flags & DDS_HEADER_FLAGS_VOLUME)
        {
            resDim = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        }
        else
        {
            if (header->caps2 & DDS_CUBEMAP)
            {
                if ((header->caps2 & DDS_CUBEMAP_ALLFACES) != DDS_CUBEMAP_ALLFACES)
                    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

                arraySize = 6;
                isCube = true;
            }

            depth = 1;
            resDim = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        }
    }

    if (isCubeMap)
        *isCubeMap = isCube;

    // Create the texture
    size_t skipMip = 0;
    size_t twidth = 0;
    size_t theight = 0;
    size_t tdepth = 0;

    hr = FillInitData(width, height, depth, mipCount, arraySize, format, maxsize, bitSize, bitData,
        twidth, theight, tdepth, skipMip, subresources);

    if (SUCCEEDED(hr))
    {
        hr = CreateTextureResource(d3dDevice, resDim, twidth, theight, tdepth, mipCount - skipMip, arraySize,
            format, resFlags, forceSRGB, texture);
    }

    return hr;
}


//--------------------------------------------------------------------------------------
// Entry-points
//--------------------------------------------------------------------------------------
HRESULT DirectX::LoadDDSTextureFromMemory(
    ID3D12Device* d3dDevice,
    const uint8_t* ddsData,
    size_t ddsDataSize,
    ID3D12Resource** texture,
    std::vector<D3D12_SUBRESOURCE_DATA>& subresources,
    size_t maxsize,
    DDS_ALPHA_MODE* alphaMode,
    bool* isCubeMap)
{
    if (texture)
        *texture = nullptr;
    if (alphaMode)
        *alphaMode = DDS_ALPHA_MODE_UNKNOWN;
    if (isCubeMap)
        *isCubeMap = false;

    if (!d3dDevice || !ddsData)
        return E_INVALIDARG;

    // Validate DDS file in memory
    if (ddsDataSize < (sizeof(uint32_t) + sizeof(DDS_HEADER)))
        return E_FAIL;

    auto dwMagicNumber = *reinterpret_cast<const uint32_t*>(ddsData);
    if (dwMagicNumber != DDS_MAGIC)
        return E_FAIL;

    auto header = reinterpret_cast<const DDS_HEADER*>(ddsData + sizeof(uint32_t));

    // Verify header to validate DDS file
    if (header->size != sizeof(DDS_HEADER) || header->ddspf.size != sizeof(DDS_PIXELFORMAT))
        return E_FAIL;

    size_t offset = sizeof(uint32_t) + sizeof(DDS_HEADER);

    // Check for extensions
    if ((header->ddspf.flags & DDS_FOURCC) && (MAKEFOURCC('D', 'X', '1', '0') == header->ddspf.fourCC))
    {
        if (ddsDataSize < (sizeof(uint32_t) + sizeof(DDS_HEADER) + sizeof(DDS_HEADER_DXT10)))
            return E_FAIL;

        offset += sizeof(DDS_HEADER_DXT10);
    }

    return CreateTextureFromDDS(d3dDevice, header, ddsData + offset, ddsDataSize - offset, maxsize,
        D3D12_RESOURCE_FLAG_NONE, false, texture, subresources, isCubeMap);
}

//--------------------------------------------------------------------------------------
HRESULT DirectX::LoadDDSTextureFromFile(
    ID3D12Device* d3dDevice,
    const wchar_t* szFileName,
    ID3D12Resource** texture,
    std::unique_ptr<uint8_t[]>& ddsData,
    std::vector<D3D12_SUBRESOURCE_DATA>& subresources,
    size_t maxsize,
    DDS_ALPHA_MODE* alphaMode,
    bool* isCubeMap)
{
    if (texture)
        *texture = nullptr;
    if (alphaMode)
        *alphaMode = DDS_ALPHA_MODE_UNKNOWN;
    if (isCubeMap)
        *isCubeMap = false;

    if (!d3dDevice || !szFileName)
        return E_INVALIDARG;

    // Open the file
    HANDLE hFile = CreateFileW(szFileName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());

    // Get the file size
    FILE_STANDARD_INFO fileInfo;
    if (!GetFileInformationByHandleEx(hFile, FileStandardInfo, &fileInfo, sizeof(fileInfo)))
    {
        CloseHandle(hFile);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // File is too big for 32-bit allocation
    if (fileInfo.EndOfFile.HighPart > 0)
    {
        CloseHandle(hFile);
        return E_FAIL;
    }

    // Need at least enough data to fill the header and magic number
    if (fileInfo.EndOfFile.LowPart < (sizeof(uint32_t) + sizeof(DDS_HEADER)))
    {
        CloseHandle(hFile);
        return E_FAIL;
    }

    // Create enough space for the file data
    ddsData.reset(new (std::nothrow) uint8_t[fileInfo.EndOfFile.LowPart]);
    if (!ddsData)
    {
        CloseHandle(hFile);
        return E_OUTOFMEMORY;
    }

    // Read the data in
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, ddsData.get(), fileInfo.EndOfFile.LowPart, &bytesRead, nullptr))
    {
        CloseHandle(hFile);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    if (bytesRead < fileInfo.EndOfFile.LowPart)
    {
        CloseHandle(hFile);
        return E_FAIL;
    }

    CloseHandle(hFile);

    return LoadDDSTextureFromMemory(d3dDevice, ddsData.get(), bytesRead, texture, subresources, maxsize, alphaMode, isCubeMap);
}
