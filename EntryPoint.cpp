#define WIN32_LEAN_AND_MEAN
#define STRICT
#include <windows.h>

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

#include <dxgi1_4.h>
#include <d3d12.h>
#include <DirectXMath.h>
using namespace DirectX;
#include <d3dcompiler.h>

#include <memory>
#include <sstream>
#include <vector>

#include "DirectXTex/WICTextureLoader/WICTextureLoader12.h"
#include "d3dx12.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

struct Settings {
    long screenWidth, screenHeight;
    bool isFullscreen;
    bool isVsyncEnabled;
};

class Model {
public:
    struct Vertex {
        XMFLOAT3 position;
        XMFLOAT2 tex;
    };
};

struct ConstantBufferPerObject {
    XMFLOAT4X4 wvpMatrix;
};

class Graphics {
public:
    static std::unique_ptr<Graphics> GetInstance();
    ~Graphics();
    Graphics();

    HRESULT Initialize(HWND, Settings const &);
    void Terminate();

    bool Render();
    void Update();

private:
    bool UpdatePipeline();
    bool WaitForPreviousFrame(UINT bufferIndex);

    const int MAX_FRAME_BUFFERS = 2;

    ComPtr<ID3D12Device> m_pDevice;
    ComPtr<ID3D12CommandQueue> m_pCommandQueue;

    bool m_isVsyncEnabled;
    ComPtr<IDXGISwapChain3> m_pSwapChain;
    ComPtr<ID3D12DescriptorHeap> m_pRtvHeap;
    std::vector<ComPtr<ID3D12Resource>> m_backBuffers;
    UINT m_currentBackBufferIndex;

    ComPtr<ID3D12DescriptorHeap> m_pDsvHeap;
    ComPtr<ID3D12Resource> m_pDepthStencilBuffer;

    std::vector<ComPtr<ID3D12CommandAllocator>> m_commandAllocators;
    ComPtr<ID3D12GraphicsCommandList> m_pCommandList;

    std::vector<ComPtr<ID3D12Fence>> m_fences;
    std::vector<unsigned long long> m_fenceValues;
    HANDLE m_fenceEvent;

    ComPtr<ID3D12PipelineState> m_pPipelineState;
    ComPtr<ID3D12RootSignature> m_pRootsignature;
    D3D12_VIEWPORT m_viewport;
    D3D12_RECT m_scissorRect;
    
    ComPtr<ID3D12Resource> m_pVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView;

    ComPtr<ID3D12Resource> m_pIndexBuffer;
    D3D12_INDEX_BUFFER_VIEW m_indexBufferView;

    const int CONSTANT_BUFFER_PER_OBJECT_ALIGNED_SIZE = (sizeof ConstantBufferPerObject + 255) & ~255;
    ConstantBufferPerObject m_cbPerObject;
    std::vector<ComPtr<ID3D12Resource>> m_cbUploadHeaps;
    std::vector<uint8_t*> m_cbGpuAddresses;

    XMFLOAT4X4 cameraProjection;
    XMFLOAT4X4 cameraView;

    XMFLOAT4 cameraPosition;
    XMFLOAT4 cameraTarget;
    XMFLOAT4 cameraUp;

    XMFLOAT4X4 cube1World;
    XMFLOAT4X4 cube1Rotation;
    XMFLOAT4 cube1Position;

    XMFLOAT4X4 cube2World;
    XMFLOAT4X4 cube2Rotation;
    XMFLOAT4 cube2PositionOffset;

    int numCubeIndices;

    ComPtr<ID3D12Resource> m_pTextureBuffer;

    ComPtr<ID3D12DescriptorHeap> m_pMainDescriptorHeap;
    ComPtr<ID3D12Resource> m_pTextureBufferUploadHeap;
};

std::unique_ptr<Graphics> Graphics::GetInstance() {
    return std::make_unique<Graphics>();
}

Graphics::Graphics() : m_isVsyncEnabled(false), m_currentBackBufferIndex(0), m_fenceEvent(nullptr), numCubeIndices(0) { }

Graphics::~Graphics() {
    Terminate();
}

HRESULT Graphics::Initialize(HWND hwnd, Settings const & settings) {
    auto retVal = S_OK;

    ComPtr<IDXGIFactory4> pFactory;
    if (FAILED(retVal = CreateDXGIFactory1(IID_PPV_ARGS(&pFactory)))) {
        MessageBox(hwnd, L"Failed to create dxgi factory", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    ComPtr<IDXGIAdapter1> pAdapter;
    auto adapterFound = false;
    auto adapterIndex = 0;
    while(DXGI_ERROR_NOT_FOUND != pFactory->EnumAdapters1(adapterIndex, &pAdapter)) {
        auto adapterDesc = DXGI_ADAPTER_DESC1{};
        if(FAILED(retVal = pAdapter->GetDesc1(&adapterDesc))) {
            MessageBox(hwnd, L"Failed to get adapter desc", L"ERROR", MB_ICONERROR | MB_OK);
            continue;
        }

        if(adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            ++adapterIndex;
            continue;
        }

        if(SUCCEEDED(retVal = D3D12CreateDevice(pAdapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) {
            adapterFound = true;
            break;
        }

        ++adapterIndex;
    }

    if(!adapterFound) {
        MessageBox(hwnd, L"No adapter found that supports minimum feature level", L"ERROR", MB_ICONERROR | MB_OK);
        return E_FAIL;
    }

    if(FAILED(retVal = D3D12CreateDevice(pAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_pDevice)))) {
        MessageBox(hwnd, L"Device could not be created at the requested feature level", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    auto commandQueueDesc = D3D12_COMMAND_QUEUE_DESC{};
    ZeroMemory(&commandQueueDesc, sizeof commandQueueDesc);
    commandQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    commandQueueDesc.NodeMask = 0;
    commandQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    // generally one queue per GPU
    if(FAILED(retVal = m_pDevice->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&m_pCommandQueue)))) {
        MessageBox(hwnd, L"Failed to create command queue", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    ComPtr<IDXGIOutput> pAdapterOutput;
    if(FAILED(pAdapter->EnumOutputs(0, &pAdapterOutput))) {
        MessageBox(hwnd, L"Failed to enumerate adapter outputs", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    DXGI_ADAPTER_DESC adapterDesc;
    if(FAILED(retVal = pAdapter->GetDesc(&adapterDesc))) {
        MessageBox(hwnd, L"Failed to get the adpater desc", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    pAdapter.Reset();

    std::wstringstream wss;
    wss << "Detected " << adapterDesc.Description << " (" << adapterDesc.DedicatedVideoMemory / 1024 / 1024 << "MB)";
    MessageBox(hwnd, wss.str().c_str(), L"Adapter Info", MB_OK);

    auto numModes = 0U;
    retVal = pAdapterOutput->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ENUM_MODES_INTERLACED, &numModes, nullptr);
    if(FAILED(retVal)) {
        MessageBox(hwnd, L"Failed to get the display mode count", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    std::vector<DXGI_MODE_DESC> displayModes(numModes);
    retVal = pAdapterOutput->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ENUM_MODES_INTERLACED, &numModes, &displayModes[0]);
    if(FAILED(retVal)) {
        MessageBox(hwnd, L"Failed to get the display mode list", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    pAdapterOutput.Reset();

    // try to find a matching refresh rate so that flips are true flips instead of buffer copies
    auto refreshRate = DXGI_RATIONAL{ 0, 1 };
    for( auto& displayMode : displayModes) {
        if((displayMode.Height == settings.screenHeight) && (displayMode.Width == settings.screenWidth)) {
            refreshRate = displayMode.RefreshRate;
        }
    }

    auto swapChainDesc = DXGI_SWAP_CHAIN_DESC{};
    ZeroMemory(&swapChainDesc, sizeof swapChainDesc);
    swapChainDesc.BufferCount = MAX_FRAME_BUFFERS;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferDesc.Height = settings.screenHeight;
    swapChainDesc.BufferDesc.RefreshRate = settings.isVsyncEnabled ? refreshRate : DXGI_RATIONAL{ 0, 1 };
    swapChainDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    swapChainDesc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    swapChainDesc.BufferDesc.Width = settings.screenWidth;
    swapChainDesc.Flags = 0;
    swapChainDesc.OutputWindow = hwnd;
    swapChainDesc.SampleDesc = DXGI_SAMPLE_DESC{ 1, 0 };
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.Windowed = !settings.isFullscreen;

    IDXGISwapChain* pSwapChainTemp;
    if(FAILED(retVal = pFactory->CreateSwapChain(m_pCommandQueue.Get(), &swapChainDesc, &pSwapChainTemp))) {
        MessageBox(hwnd, L"Failed to create swap chain", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    if(FAILED(pSwapChainTemp->QueryInterface(IID_PPV_ARGS(&m_pSwapChain)))) {
        MessageBox(hwnd, L"Failed to upgrade swap chain interface", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    pSwapChainTemp = nullptr;
    pFactory.Reset();

    m_isVsyncEnabled = settings.isVsyncEnabled;

    auto rtvHeapDesc = D3D12_DESCRIPTOR_HEAP_DESC{};
    ZeroMemory(&rtvHeapDesc, sizeof rtvHeapDesc);
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtvHeapDesc.NodeMask = 0;
    rtvHeapDesc.NumDescriptors = MAX_FRAME_BUFFERS;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

    if(FAILED(retVal = m_pDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_pRtvHeap)))) {
        MessageBox(hwnd, L"Failed to create the rtv heap", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    auto rtvHandle = m_pRtvHeap->GetCPUDescriptorHandleForHeapStart();
    auto rtvDescriptorSize = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    m_backBuffers.resize(MAX_FRAME_BUFFERS);
    for (auto i = 0; i < m_backBuffers.size(); ++i) {
        if (FAILED(retVal = m_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])))) {
            MessageBox(hwnd, L"Failed to get back buffer", L"ERROR", MB_ICONERROR | MB_OK);
            return retVal;
        }

        m_pDevice->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += rtvDescriptorSize;
    }

    m_currentBackBufferIndex = m_pSwapChain->GetCurrentBackBufferIndex();
    
    m_commandAllocators.resize(MAX_FRAME_BUFFERS);
    for (auto i = 0; i < m_commandAllocators.size(); ++i) {
        retVal = m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i]));
        if (FAILED(retVal)) {
            MessageBox(hwnd, L"Failed to create command allocator", L"ERROR", MB_ICONERROR | MB_OK);
            return retVal;
        }
    }

    // TODO: why always index 0?
    retVal = m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[0].Get(), nullptr,
        IID_PPV_ARGS(&m_pCommandList));
    if(FAILED(retVal)) {
        MessageBox(hwnd, L"Failed to create command list", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    m_fences.resize(MAX_FRAME_BUFFERS);
    m_fenceValues.resize(MAX_FRAME_BUFFERS);
    for (auto i = 0; i < m_fences.size(); ++i) {
        if (FAILED(retVal = m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fences[i])))) {
            MessageBox(hwnd, L"Failed to create fence", L"ERROR", MB_ICONERROR | MB_OK);
            return retVal;
        }
        m_fenceValues[i] = 0;
    }

    if(nullptr == (m_fenceEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS))) {
        MessageBox(hwnd, L"Failed to create fence event", L"ERROR", MB_ICONERROR | MB_OK);
        return E_FAIL;
    }

    // for object creation

    auto rootCbvDescriptor = D3D12_ROOT_DESCRIPTOR{};
    rootCbvDescriptor.RegisterSpace = 0;
    rootCbvDescriptor.ShaderRegister = 0;

    std::vector<D3D12_DESCRIPTOR_RANGE> descriptorTableRanges(1);
    descriptorTableRanges[0].BaseShaderRegister = 0;
    descriptorTableRanges[0].NumDescriptors = 1;
    descriptorTableRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptorTableRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorTableRanges[0].RegisterSpace = 0;

    auto descriptorTable = D3D12_ROOT_DESCRIPTOR_TABLE{};
    descriptorTable.NumDescriptorRanges = static_cast<UINT>(descriptorTableRanges.size());
    descriptorTable.pDescriptorRanges = &descriptorTableRanges[0];

    std::vector<D3D12_ROOT_PARAMETER> rootParameters(2);
    rootParameters[0].Descriptor = rootCbvDescriptor;
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    rootParameters[1].DescriptorTable = descriptorTable;
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    auto sampler = D3D12_STATIC_SAMPLER_DESC{};
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.MaxAnisotropy = 0;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.MinLOD = 0.0f;
    sampler.MipLODBias = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    auto rootSignatureDesc = D3D12_ROOT_SIGNATURE_DESC{};
    ZeroMemory(&rootSignatureDesc, sizeof rootSignatureDesc);
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    rootSignatureDesc.NumParameters = static_cast<UINT>(rootParameters.size());
    rootSignatureDesc.NumStaticSamplers = 1;
    rootSignatureDesc.pParameters = &rootParameters[0];
    rootSignatureDesc.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> signature;
    if(FAILED(retVal = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, nullptr))) {
        MessageBox(hwnd, L"Failed to serialize root signature", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    retVal = m_pDevice->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_pRootsignature));
    if(FAILED(retVal)) {
        MessageBox(hwnd, L"Failed to create root signature", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> errorBuffer;
    retVal = D3DCompileFromFile(
        L"vs.hlsl",
        nullptr,
        nullptr,
        "main",
        "vs_5_0",
        D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
        0,
        &vertexShader,
        &errorBuffer
    );
    if(FAILED(retVal)) {
        OutputDebugStringA(reinterpret_cast<char*>(errorBuffer->GetBufferPointer()));
        return retVal;
    }

    auto vertexShaderBytecode = D3D12_SHADER_BYTECODE{};
    vertexShaderBytecode.BytecodeLength = vertexShader->GetBufferSize();
    vertexShaderBytecode.pShaderBytecode = vertexShader->GetBufferPointer();

    ComPtr<ID3DBlob> pixelShader;
    retVal = D3DCompileFromFile(
        L"ps.hlsl",
        nullptr,
        nullptr,
        "main",
        "ps_5_0",
        D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
        0,
        &pixelShader,
        &errorBuffer
    );
    if (FAILED(retVal)) {
        OutputDebugStringA(reinterpret_cast<char*>(errorBuffer->GetBufferPointer()));
        return retVal;
    }

    auto pixelShaderBytecode = D3D12_SHADER_BYTECODE{};
    pixelShaderBytecode.BytecodeLength = pixelShader->GetBufferSize();
    pixelShaderBytecode.pShaderBytecode = pixelShader->GetBufferPointer();

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    auto inputLayoutDesc = D3D12_INPUT_LAYOUT_DESC{};
    ZeroMemory(&inputLayoutDesc, sizeof inputLayoutDesc);
    inputLayoutDesc.NumElements = _countof(inputLayout);
    inputLayoutDesc.pInputElementDescs = inputLayout;

    auto psoDesc = D3D12_GRAPHICS_PIPELINE_STATE_DESC{};
    ZeroMemory(&psoDesc, sizeof psoDesc);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.InputLayout = inputLayoutDesc;
    psoDesc.NumRenderTargets = 1;
    psoDesc.pRootSignature = m_pRootsignature.Get();
    psoDesc.PS = pixelShaderBytecode;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.SampleDesc = swapChainDesc.SampleDesc;
    psoDesc.SampleMask = 0xffffffff;
    psoDesc.VS = vertexShaderBytecode;

    if(FAILED(retVal = m_pDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pPipelineState)))) {
        MessageBox(hwnd, L"Failed to create pipeline state", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    Model::Vertex vbData[] = {
        // front face
        {{ -0.5f,  0.5f, -0.5f }, { 0.0f, 0.0f }},
        {{  0.5f, -0.5f, -0.5f }, { 1.0f, 1.0f }},
        {{ -0.5f, -0.5f, -0.5f }, { 0.0f, 1.0f }},
        {{  0.5f,  0.5f, -0.5f }, { 1.0f, 0.0f }},

        // right side face
        {{  0.5f, -0.5f, -0.5f }, { 0.0f, 0.0f }},
        {{  0.5f,  0.5f,  0.5f }, { 1.0f, 1.0f }},
        {{  0.5f, -0.5f,  0.5f }, { 0.0f, 1.0f }},
        {{  0.5f,  0.5f, -0.5f }, { 1.0f, 0.0f }},

        // left side face
        {{ -0.5f,  0.5f,  0.5f }, { 0.0f, 0.0f }},
        {{ -0.5f, -0.5f, -0.5f }, { 1.0f, 1.0f }},
        {{ -0.5f, -0.5f,  0.5f }, { 0.0f, 1.0f }},
        {{ -0.5f,  0.5f, -0.5f }, { 1.0f, 0.0f }},

        // back face
        {{  0.5f,  0.5f,  0.5f }, { 0.0f, 0.0f }},
        {{ -0.5f, -0.5f,  0.5f }, { 1.0f, 1.0f }},
        {{  0.5f, -0.5f,  0.5f }, { 0.0f, 1.0f }},
        {{ -0.5f,  0.5f,  0.5f }, { 1.0f, 0.0f }},

        // top face
        {{ -0.5f,  0.5f, -0.5f }, { 0.0f, 0.0f }},
        {{  0.5f,  0.5f,  0.5f }, { 1.0f, 1.0f }},
        {{  0.5f,  0.5f, -0.5f }, { 0.0f, 1.0f }},
        {{ -0.5f,  0.5f,  0.5f }, { 1.0f, 0.0f }},

        // bottom face
        {{  0.5f, -0.5f,  0.5f }, { 0.0f, 0.0f }},
        {{ -0.5f, -0.5f, -0.5f }, { 1.0f, 1.0f }},
        {{  0.5f, -0.5f, -0.5f }, { 0.0f, 1.0f }},
        {{ -0.5f, -0.5f,  0.5f }, { 1.0f, 0.0f }},
    };
    const auto vbSize = sizeof vbData;

    auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);

    retVal = m_pDevice->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_pVertexBuffer)
    );
    if(FAILED(retVal)) {
        MessageBox(hwnd, L"Failed to create vertex buffer", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }
    m_pVertexBuffer->SetName(L"Vertex Buffer Resource Heap");

    heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    ComPtr<ID3D12Resource> vbUploadHeap;
    retVal = m_pDevice->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&vbUploadHeap)
    );
    if(FAILED(retVal)) {
        MessageBox(hwnd, L"Failed to create vertex buffer upload heap", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }
    vbUploadHeap->SetName(L"Vertex Buffer Upload Resource Heap");

    auto vertexData = D3D12_SUBRESOURCE_DATA{};
    vertexData.pData = reinterpret_cast<BYTE*>(vbData);
    vertexData.RowPitch = vbSize;
    vertexData.SlicePitch = vbSize;

    UpdateSubresources(m_pCommandList.Get(), m_pVertexBuffer.Get(), vbUploadHeap.Get(), 0, 0, 1, &vertexData);

    auto barrier = D3D12_RESOURCE_BARRIER{};
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.pResource = m_pVertexBuffer.Get();
    m_pCommandList->ResourceBarrier(1, &barrier);

    uint32_t ibData[] = {
        // front
         0,  1,  2,
         0,  3,  1,
        // left
         4,  5,  6,
         4,  7,  5,
        // right
         8,  9, 10,
         8, 11,  9,
        // back
        12, 13, 14,
        12, 15, 13,
        // top
        16, 17, 18,
        16, 19, 17,
        // bottom
        20, 21, 22,
        20, 23, 21,
    };
    const auto ibSize = sizeof ibData;
    numCubeIndices = sizeof ibData / sizeof ibData[0];

    heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(ibSize);
    retVal = m_pDevice->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_pIndexBuffer)
    );
    if (FAILED(retVal)) {
        MessageBox(hwnd, L"Failed to create index buffer", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }
    m_pIndexBuffer->SetName(L"Index Buffer Resource Heap");

    heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    ComPtr<ID3D12Resource> ibUploadHeap;
    retVal = m_pDevice->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&ibUploadHeap)
    );
    if (FAILED(retVal)) {
        MessageBox(hwnd, L"Failed to create index buffer upload heap", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }
    ibUploadHeap->SetName(L"Index Buffer Upload Resource Heap");

    auto indexData = D3D12_SUBRESOURCE_DATA{};
    indexData.pData = reinterpret_cast<BYTE*>(ibData);
    indexData.RowPitch = ibSize;
    indexData.SlicePitch = ibSize;

    UpdateSubresources(m_pCommandList.Get(), m_pIndexBuffer.Get(), ibUploadHeap.Get(), 0, 0, 1, &indexData);

    barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_pIndexBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
    );
    m_pCommandList->ResourceBarrier(1, &barrier);

    // depth/stencil
    auto dsvHeapDesc = D3D12_DESCRIPTOR_HEAP_DESC{};
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dsvHeapDesc.NodeMask = 0;
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if(FAILED(retVal = m_pDevice->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_pDsvHeap)))) {
        MessageBox(hwnd, L"Failed to create the dsv heap", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }
    m_pDsvHeap->SetName(L"Depth/Stencil Resource Heap");

    auto dsvDesc = D3D12_DEPTH_STENCIL_VIEW_DESC{};
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

    auto depthOptimizedClearValue = D3D12_CLEAR_VALUE{};
    depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
    depthOptimizedClearValue.DepthStencil.Stencil = 0;
    depthOptimizedClearValue.Format = DXGI_FORMAT_D32_FLOAT;

    heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_D32_FLOAT,
        settings.screenWidth,
        settings.screenHeight,
        1, 0, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
    );
    retVal = m_pDevice->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depthOptimizedClearValue,
        IID_PPV_ARGS(&m_pDepthStencilBuffer)
    );
    if (FAILED(retVal)) {
        MessageBox(hwnd, L"Failed to create depth/stencil buffer", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    m_pDevice->CreateDepthStencilView(m_pDepthStencilBuffer.Get(), &dsvDesc, m_pDsvHeap->GetCPUDescriptorHandleForHeapStart());

    // constant buffer(s)

    m_cbUploadHeaps.resize(MAX_FRAME_BUFFERS);
    m_cbGpuAddresses.resize(MAX_FRAME_BUFFERS);
    for(auto i = 0; i < m_cbUploadHeaps.size(); ++i) {
        heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(64 * 1024);
        retVal = m_pDevice->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_cbUploadHeaps[i])
        );
        if (FAILED(retVal)) {
            MessageBox(hwnd, L"Failed to create constant buffer upload heap", L"ERROR", MB_ICONERROR | MB_OK);
            return retVal;
        }
        m_cbUploadHeaps[i]->SetName(L"Constant Buffer Upload Resource Heap");
        ZeroMemory(&m_cbPerObject, sizeof m_cbPerObject);

        CD3DX12_RANGE readRange(0, 0);
        retVal = m_cbUploadHeaps[i]->Map(0, &readRange, reinterpret_cast<void**>(&m_cbGpuAddresses[i]));
        if (FAILED(retVal)) {
            MessageBox(hwnd, L"Failed to map constant buffer address", L"ERROR", MB_ICONERROR | MB_OK);
            return retVal;
        }

        CopyMemory(m_cbGpuAddresses[i], &m_cbPerObject, sizeof m_cbPerObject);
        CopyMemory(m_cbGpuAddresses[i] + CONSTANT_BUFFER_PER_OBJECT_ALIGNED_SIZE, &m_cbPerObject, sizeof m_cbPerObject);
    }

    // texture
    auto textureData = D3D12_SUBRESOURCE_DATA{};
    std::unique_ptr<uint8_t[]> imageData;
    if(FAILED(retVal = LoadWICTextureFromFile(m_pDevice.Get(), L"image.png", &m_pTextureBuffer, imageData, textureData))) {
        MessageBox(hwnd, L"Failed to load textrue from file", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    auto textureUploadBufferSize = GetRequiredIntermediateSize(m_pTextureBuffer.Get(), 0, 1);

    heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(textureUploadBufferSize);
    retVal = m_pDevice->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_pTextureBufferUploadHeap)
    );
    if (FAILED(retVal)) {
        MessageBox(hwnd, L"Failed to create texture upload buffer", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }
    m_pTextureBufferUploadHeap->SetName(L"Texture Buffer Upload Heap");

    UpdateSubresources(m_pCommandList.Get(), m_pTextureBuffer.Get(), m_pTextureBufferUploadHeap.Get(), 0, 0, 1, &textureData);

    barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_pTextureBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
    m_pCommandList->ResourceBarrier(1, &barrier);

    auto heapDesc = D3D12_DESCRIPTOR_HEAP_DESC{};
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDesc.NodeMask = 0;
    heapDesc.NumDescriptors = 1;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    if(FAILED(retVal = m_pDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_pMainDescriptorHeap)))) {
        MessageBox(hwnd, L"Failed to create texture descriptor resource heap", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    auto srvDesc = D3D12_SHADER_RESOURCE_VIEW_DESC{};
    srvDesc.Format = m_pTextureBuffer->GetDesc().Format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    m_pDevice->CreateShaderResourceView(m_pTextureBuffer.Get(), &srvDesc, m_pMainDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    if (FAILED(retVal = m_pCommandList->Close())) {
        MessageBox(hwnd, L"Failed to close command list", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    auto commandLists = std::vector<ID3D12CommandList*>{ m_pCommandList.Get() };
    m_pCommandQueue->ExecuteCommandLists(static_cast<UINT>(commandLists.size()), &commandLists[0]);
    ++m_fenceValues[m_currentBackBufferIndex];
    retVal = m_pCommandQueue->Signal(m_fences[m_currentBackBufferIndex].Get(), m_fenceValues[m_currentBackBufferIndex]);
    if(FAILED(retVal)) {
        MessageBox(hwnd, L"Failed to signal", L"ERROR", MB_ICONERROR | MB_OK);
        return retVal;
    }

    m_vertexBufferView.BufferLocation = m_pVertexBuffer->GetGPUVirtualAddress();
    m_vertexBufferView.StrideInBytes = sizeof(Model::Vertex);
    m_vertexBufferView.SizeInBytes = vbSize;

    m_indexBufferView.BufferLocation = m_pIndexBuffer->GetGPUVirtualAddress();
    m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;
    m_indexBufferView.SizeInBytes = ibSize;

    m_viewport.TopLeftX = 0;
    m_viewport.TopLeftY = 0;
    m_viewport.Width = static_cast<float>(settings.screenWidth);
    m_viewport.Height = static_cast<float>(settings.screenHeight);
    m_viewport.MinDepth = 0.0f;
    m_viewport.MaxDepth = 1.0f;

    m_scissorRect.left = 0;
    m_scissorRect.top = 0;
    m_scissorRect.right = settings.screenWidth;
    m_scissorRect.bottom = settings.screenHeight;

    auto tmpMat = XMMatrixPerspectiveFovLH(
        45.0f*(3.14f / 180.0f),
        static_cast<float>(settings.screenWidth) / static_cast<float>(settings.screenHeight),
        0.1f,
        1000.0f
    );
    XMStoreFloat4x4(&cameraProjection, tmpMat);

    // set starting camera state
    cameraPosition = XMFLOAT4(0.0f, 2.0f, -4.0f, 0.0f);
    cameraTarget = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    cameraUp = XMFLOAT4(0.0f, 1.0f, 0.0f, 0.0f);

    // build view matrix
    auto cPos = XMLoadFloat4(&cameraPosition);
    auto cTarg = XMLoadFloat4(&cameraTarget);
    auto cUp = XMLoadFloat4(&cameraUp);
    tmpMat = XMMatrixLookAtLH(cPos, cTarg, cUp);
    XMStoreFloat4x4(&cameraView, tmpMat);

    // set starting cubes position
    // first cube
    cube1Position = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    auto posVec = XMLoadFloat4(&cube1Position);

    tmpMat = XMMatrixTranslationFromVector(posVec);
    XMStoreFloat4x4(&cube1Rotation, XMMatrixIdentity());
    XMStoreFloat4x4(&cube1World, tmpMat);

    // second cube
    cube2PositionOffset = XMFLOAT4(1.5f, 0.0f, 0.0f, 0.0f);
    posVec = XMLoadFloat4(&cube2PositionOffset) + XMLoadFloat4(&cube1Position);
    tmpMat = XMMatrixTranslationFromVector(posVec);
    XMStoreFloat4x4(&cube2Rotation, XMMatrixIdentity());
    XMStoreFloat4x4(&cube2World, tmpMat);

    return S_OK;
}

void Graphics::Terminate() {

    for (auto i = 0; i < MAX_FRAME_BUFFERS; ++i) {
        WaitForPreviousFrame(i);
    }

    if (m_pSwapChain) {
        m_pSwapChain->SetFullscreenState(false, nullptr);
    }

    for(auto& constantBufferUploadHeap : m_cbUploadHeaps) {
        constantBufferUploadHeap.Reset();
    }
    m_pDepthStencilBuffer.Reset();
    m_pDsvHeap.Reset();

    m_pIndexBuffer.Reset();
    m_pVertexBuffer.Reset();
    m_pRootsignature.Reset();
    m_pPipelineState.Reset();

    CloseHandle(m_fenceEvent);
    for (auto& fence : m_fences) {
        fence.Reset();
    }
    m_pCommandList.Reset();
    for (auto& commandAllocator : m_commandAllocators) {
        commandAllocator.Reset();
    }
    for(auto& backBuffer : m_backBuffers) {
        backBuffer.Reset();
    }
    m_pRtvHeap.Reset();
    m_pSwapChain.Reset();
    m_pCommandQueue.Reset();
    m_pDevice.Reset();
}

bool Graphics::Render() {

    auto isRunning = UpdatePipeline();

    // queue the list(s)
    auto commandLists = std::vector<ID3D12CommandList*>{ m_pCommandList.Get() };
    m_pCommandQueue->ExecuteCommandLists(static_cast<UINT>(commandLists.size()), &commandLists[0]);

    // set the fence at the end of this submission
    auto retVal = S_OK;
    if(FAILED(retVal = m_pCommandQueue->Signal(m_fences[m_currentBackBufferIndex].Get(), m_fenceValues[m_currentBackBufferIndex]))) {
        isRunning = false;
    }

    if (FAILED(retVal = m_pSwapChain->Present(m_isVsyncEnabled ? 1 : 0, 0))) {
        isRunning = false;
    }

    return isRunning;
}

void Graphics::Update() {
    auto rotXMat = XMMatrixRotationX(0.0001f);
    auto rotYMat = XMMatrixRotationY(0.0002f);
    auto rotZMat = XMMatrixRotationZ(0.0003f);

    auto rotMat = XMLoadFloat4x4(&cube1Rotation) * rotXMat * rotYMat * rotZMat;
    XMStoreFloat4x4(&cube1Rotation, rotMat);

    auto translationMat = XMMatrixTranslationFromVector(XMLoadFloat4(&cube1Position));

    auto worldMat = rotMat * translationMat;

    XMStoreFloat4x4(&cube1World, worldMat);

    auto viewMat = XMLoadFloat4x4(&cameraView);
    auto projMat = XMLoadFloat4x4(&cameraProjection);
    auto wvpMat = XMLoadFloat4x4(&cube1World) * viewMat * projMat;
    auto transposed = XMMatrixTranspose(wvpMat);
    XMStoreFloat4x4(&m_cbPerObject.wvpMatrix, transposed);

    CopyMemory(m_cbGpuAddresses[m_currentBackBufferIndex], &m_cbPerObject, sizeof m_cbPerObject);

    rotXMat = XMMatrixRotationX(0.0003f);
    rotYMat = XMMatrixRotationY(0.0002f);
    rotZMat = XMMatrixRotationZ(0.0001f);

    rotMat = rotZMat * (XMLoadFloat4x4(&cube2Rotation) * (rotXMat * rotYMat));
    XMStoreFloat4x4(&cube2Rotation, rotMat);

    auto translationOffsetMat = XMMatrixTranslationFromVector(XMLoadFloat4(&cube2PositionOffset));

    auto scaleMat = XMMatrixScaling(0.5f, 0.5f, 0.5f);

    worldMat = scaleMat * translationOffsetMat * rotMat * translationMat;

    wvpMat = XMLoadFloat4x4(&cube2World) * viewMat * projMat;
    transposed = XMMatrixTranspose(wvpMat);
    XMStoreFloat4x4(&m_cbPerObject.wvpMatrix, transposed);

    CopyMemory(m_cbGpuAddresses[m_currentBackBufferIndex] + CONSTANT_BUFFER_PER_OBJECT_ALIGNED_SIZE, &m_cbPerObject, sizeof m_cbPerObject);

    XMStoreFloat4x4(&cube2World, worldMat);
}

bool Graphics::UpdatePipeline() {

    m_currentBackBufferIndex = m_pSwapChain->GetCurrentBackBufferIndex();
    auto isRunning = WaitForPreviousFrame(m_currentBackBufferIndex);

    auto retVal = S_OK;
    if(FAILED(retVal = m_commandAllocators[m_currentBackBufferIndex]->Reset())) {
        isRunning = false;
    }

    if (FAILED(retVal = m_pCommandList->Reset(m_commandAllocators[m_currentBackBufferIndex].Get(), m_pPipelineState.Get()))) {
        isRunning = false;
    }

    auto barrier = D3D12_RESOURCE_BARRIER{};
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.pResource = m_backBuffers[m_currentBackBufferIndex].Get();
    m_pCommandList->ResourceBarrier(1, &barrier);

    auto rtvHandle = m_pRtvHeap->GetCPUDescriptorHandleForHeapStart();
    auto rtvDescriptorSize = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (auto i = 0U; i < m_currentBackBufferIndex; ++i) {
        rtvHandle.ptr += rtvDescriptorSize;
    }
    auto dsvHandle = m_pDsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_pCommandList->OMSetRenderTargets(1, &rtvHandle, false, &dsvHandle);

    auto clearColor = std::vector<float>{ 1.0f, 0.0f, 1.0f, 1.0f };
    m_pCommandList->ClearRenderTargetView(rtvHandle, &clearColor[0], 0, nullptr);
    m_pCommandList->ClearDepthStencilView(
        m_pDsvHeap->GetCPUDescriptorHandleForHeapStart(),
        D3D12_CLEAR_FLAG_DEPTH,
        1.0f, 0, 0, nullptr
    );

    // render primitives
    m_pCommandList->SetGraphicsRootSignature(m_pRootsignature.Get());

    ID3D12DescriptorHeap* descriptorHeaps[] = { m_pMainDescriptorHeap.Get() };
    m_pCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    m_pCommandList->SetGraphicsRootDescriptorTable(1, m_pMainDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

    m_pCommandList->RSSetViewports(1, &m_viewport);
    m_pCommandList->RSSetScissorRects(1, &m_scissorRect);
    m_pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_pCommandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_pCommandList->IASetIndexBuffer(&m_indexBufferView);
    
    m_pCommandList->SetGraphicsRootConstantBufferView(0, m_cbUploadHeaps[m_currentBackBufferIndex]->GetGPUVirtualAddress());
    m_pCommandList->DrawIndexedInstanced(numCubeIndices, 1, 0, 0, 0);

    m_pCommandList->SetGraphicsRootConstantBufferView(0, m_cbUploadHeaps[m_currentBackBufferIndex]->GetGPUVirtualAddress() + CONSTANT_BUFFER_PER_OBJECT_ALIGNED_SIZE);
    m_pCommandList->DrawIndexedInstanced(numCubeIndices, 1, 0, 0, 0);

    m_pCommandList->DrawIndexedInstanced(6, 1, 0, 4, 0);

    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_pCommandList->ResourceBarrier(1, &barrier);

    if (FAILED(retVal = m_pCommandList->Close())) {
        isRunning = false;
    }

    return isRunning;
}

bool Graphics::WaitForPreviousFrame(UINT bufferIndex) {

    auto completedValue = m_fences[bufferIndex]->GetCompletedValue();
    auto isRunning = true;
    if(completedValue < m_fenceValues[bufferIndex]) {
        auto retVal = m_fences[bufferIndex]->SetEventOnCompletion(m_fenceValues[bufferIndex], m_fenceEvent);
        if(FAILED(retVal)) {
            isRunning = false;
        }
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    ++m_fenceValues[bufferIndex];
    return isRunning;
}

//
// Windows set up
//

auto g_isRunning = true;

LRESULT CALLBACK MessageHandler(HWND hwnd, UINT msgId, WPARAM wParam, LPARAM lParam) {
    switch(msgId) {
    case WM_DESTROY:
        g_isRunning = false;
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        if(VK_ESCAPE == wParam) {
            g_isRunning = false;
            DestroyWindow(hwnd);
        }
        return 0;

    default: 
        break;
    }

    return DefWindowProc(hwnd, msgId, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE, PSTR, int) {

    auto wcex = WNDCLASSEX{};
    ZeroMemory(&wcex, sizeof wcex);
    wcex.cbSize = sizeof wcex;
    wcex.lpszClassName = L"WNDCLASSNAME";
    wcex.lpfnWndProc = MessageHandler;
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.hInstance = hinst;

    if(!RegisterClassEx(&wcex)) {
        MessageBox(nullptr, L"Error registering window class", L"ERROR", MB_ICONERROR | MB_OK);
        return -1;
    }

    auto const INITIAL_WIDTH = 1600;
    auto const INITIAL_HEIGHT = 1080;

    auto hwnd = CreateWindowEx(
        WS_EX_APPWINDOW,
        wcex.lpszClassName,
        L"Hello DX12",
        WS_POPUP,
        (GetSystemMetrics(SM_CXSCREEN) - INITIAL_WIDTH) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - INITIAL_HEIGHT) / 2,
        INITIAL_WIDTH,
        INITIAL_HEIGHT,
        nullptr,
        nullptr,
        wcex.hInstance,
        nullptr
    );
    if(nullptr == hwnd) {
        UnregisterClass(wcex.lpszClassName, wcex.hInstance);
        MessageBox(nullptr, L"Error creating window", L"ERROR", MB_ICONERROR | MB_OK);
        return -2;
    }

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    ShowCursor(false);

    auto graphics = Graphics::GetInstance();

    auto rc = RECT{};
    GetClientRect(hwnd, &rc);

    auto settings = Settings{};
    ZeroMemory(&settings, sizeof settings);
    settings.screenWidth = rc.right - rc.left;
    settings.screenHeight = rc.bottom - rc.top;
    settings.isFullscreen = false;
    settings.isVsyncEnabled = false;

    if(FAILED(graphics->Initialize(hwnd, settings))) {
        DestroyWindow(hwnd);
        UnregisterClass(wcex.lpszClassName, wcex.hInstance);
        MessageBox(hwnd, L"Error initializing graphics", L"ERROR", MB_ICONERROR | MB_OK);
        return -3;
    }

    auto msg = MSG{ nullptr, 0 };
    while(g_isRunning) {
        if(PeekMessage(&msg, hwnd, 0, 0, PM_REMOVE)) {
            if(WM_QUIT == msg.message) {
                //
                // EARLY TERMINATION
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            graphics->Update();
            g_isRunning = graphics->Render();
        }
    }

    graphics.reset();

    return static_cast<int>(msg.wParam);
}