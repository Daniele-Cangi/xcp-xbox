#include "pch.h"
#include "Direct3DApp.h"

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::UI::Core;
using namespace DirectX;

namespace XComputeProbe
{
    static std::wstring HResultToString(HRESULT hr)
    {
        std::wostringstream out;
        out << L"0x" << std::hex << std::uppercase << static_cast<uint32_t>(hr);
        return out.str();
    }

    static std::wstring LastErrorToString()
    {
        std::wostringstream out;
        out << L"Win32 error " << GetLastError();
        return out.str();
    }

    static std::vector<uint8_t> ReadBinaryFile(std::wstring const& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error("could not open binary file");
        }
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    }

    static uint32_t ExpectedIntComputeValue(uint32_t index)
    {
        uint32_t x = index + 1;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return x;
    }

    namespace
    {
        constexpr size_t MaxCreativeVertices = 196608;

        struct CreativeVertex
        {
            XMFLOAT3 position;
            XMFLOAT4 color;
        };

        struct CreativeTriangle
        {
            XMFLOAT3 a;
            XMFLOAT3 b;
            XMFLOAT3 c;
            XMFLOAT3 normal;
        };

        XMFLOAT3 Point(float x, float y, float z)
        {
            return { x, y, z };
        }

        void AddTriangle(
            std::vector<CreativeTriangle>& triangles,
            XMFLOAT3 a,
            XMFLOAT3 b,
            XMFLOAT3 c,
            XMFLOAT3 normal)
        {
            triangles.push_back({ a, b, c, normal });
        }

        std::vector<CreativeTriangle> CubeTriangles()
        {
            std::vector<CreativeTriangle> result;
            auto face = [&](XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c,
                            XMFLOAT3 d, XMFLOAT3 normal)
            {
                AddTriangle(result, a, b, c, normal);
                AddTriangle(result, a, c, d, normal);
            };
            face(Point(-.5f, -.5f, -.5f), Point(-.5f, .5f, -.5f),
                Point(.5f, .5f, -.5f), Point(.5f, -.5f, -.5f),
                Point(0, 0, -1));
            face(Point(.5f, -.5f, .5f), Point(.5f, .5f, .5f),
                Point(-.5f, .5f, .5f), Point(-.5f, -.5f, .5f),
                Point(0, 0, 1));
            face(Point(-.5f, -.5f, .5f), Point(-.5f, .5f, .5f),
                Point(-.5f, .5f, -.5f), Point(-.5f, -.5f, -.5f),
                Point(-1, 0, 0));
            face(Point(.5f, -.5f, -.5f), Point(.5f, .5f, -.5f),
                Point(.5f, .5f, .5f), Point(.5f, -.5f, .5f),
                Point(1, 0, 0));
            face(Point(-.5f, .5f, -.5f), Point(-.5f, .5f, .5f),
                Point(.5f, .5f, .5f), Point(.5f, .5f, -.5f),
                Point(0, 1, 0));
            face(Point(-.5f, -.5f, .5f), Point(-.5f, -.5f, -.5f),
                Point(.5f, -.5f, -.5f), Point(.5f, -.5f, .5f),
                Point(0, -1, 0));
            return result;
        }

        std::vector<CreativeTriangle> PlaneTriangles()
        {
            std::vector<CreativeTriangle> result;
            AddTriangle(
                result,
                Point(-.5f, 0, -.5f),
                Point(-.5f, 0, .5f),
                Point(.5f, 0, .5f),
                Point(0, 1, 0));
            AddTriangle(
                result,
                Point(-.5f, 0, -.5f),
                Point(.5f, 0, .5f),
                Point(.5f, 0, -.5f),
                Point(0, 1, 0));
            return result;
        }

        std::vector<CreativeTriangle> SphereTriangles(
            bool capsule)
        {
            constexpr uint32_t Slices = 16;
            constexpr uint32_t Stacks = 10;
            std::vector<CreativeTriangle> result;
            auto point = [&](uint32_t stack, uint32_t slice)
            {
                auto phi =
                    -XM_PIDIV2 +
                    XM_PI * static_cast<float>(stack) /
                        static_cast<float>(Stacks);
                auto theta =
                    XM_2PI * static_cast<float>(slice) /
                        static_cast<float>(Slices);
                auto y = std::sin(phi) * 0.5f;
                if (capsule)
                    y += y >= 0.0f ? 0.25f : -0.25f;
                return Point(
                    std::cos(phi) * std::sin(theta) * 0.5f,
                    y,
                    std::cos(phi) * std::cos(theta) * 0.5f);
            };
            for (uint32_t stack = 0; stack < Stacks; ++stack)
            {
                for (uint32_t slice = 0; slice < Slices; ++slice)
                {
                    auto next = (slice + 1) % Slices;
                    auto a = point(stack, slice);
                    auto b = point(stack + 1, slice);
                    auto c = point(stack + 1, next);
                    auto d = point(stack, next);
                    auto normal = [&](XMFLOAT3 value)
                    {
                        auto y = value.y;
                        if (capsule)
                            y -= y >= 0.0f ? 0.25f : -0.25f;
                        XMFLOAT3 output;
                        XMStoreFloat3(
                            &output,
                            XMVector3Normalize(
                                XMVectorSet(
                                    value.x,
                                    y,
                                    value.z,
                                    0)));
                        return output;
                    };
                    AddTriangle(result, a, b, c, normal(a));
                    AddTriangle(result, a, c, d, normal(a));
                }
            }
            return result;
        }

        std::vector<CreativeTriangle> CylinderTriangles()
        {
            constexpr uint32_t Slices = 16;
            std::vector<CreativeTriangle> result;
            for (uint32_t slice = 0; slice < Slices; ++slice)
            {
                auto a0 =
                    XM_2PI * static_cast<float>(slice) /
                    static_cast<float>(Slices);
                auto a1 =
                    XM_2PI * static_cast<float>(slice + 1) /
                    static_cast<float>(Slices);
                auto p0 = Point(
                    std::sin(a0) * .5f,
                    -.5f,
                    std::cos(a0) * .5f);
                auto p1 = Point(
                    std::sin(a0) * .5f,
                    .5f,
                    std::cos(a0) * .5f);
                auto p2 = Point(
                    std::sin(a1) * .5f,
                    .5f,
                    std::cos(a1) * .5f);
                auto p3 = Point(
                    std::sin(a1) * .5f,
                    -.5f,
                    std::cos(a1) * .5f);
                auto normal = Point(
                    std::sin(a0),
                    0,
                    std::cos(a0));
                AddTriangle(result, p0, p1, p2, normal);
                AddTriangle(result, p0, p2, p3, normal);
                AddTriangle(
                    result,
                    Point(0, .5f, 0),
                    p2,
                    p1,
                    Point(0, 1, 0));
                AddTriangle(
                    result,
                    Point(0, -.5f, 0),
                    p0,
                    p3,
                    Point(0, -1, 0));
            }
            return result;
        }

        std::vector<CreativeTriangle> const& PrimitiveTriangles(
            std::wstring const& primitive)
        {
            static auto cube = CubeTriangles();
            static auto plane = PlaneTriangles();
            static auto sphere = SphereTriangles(false);
            static auto capsule = SphereTriangles(true);
            static auto cylinder = CylinderTriangles();
            if (primitive == L"plane") return plane;
            if (primitive == L"sphere") return sphere;
            if (primitive == L"capsule") return capsule;
            if (primitive == L"cylinder") return cylinder;
            return cube;
        }

        XMMATRIX TransformMatrix(
            WorkerCreativeTransform3D const& transform)
        {
            auto rotation = transform.rotationDegrees;
            return
                XMMatrixScaling(
                    transform.scale.x,
                    transform.scale.y,
                    transform.scale.z) *
                XMMatrixRotationRollPitchYaw(
                    XMConvertToRadians(rotation.x),
                    XMConvertToRadians(rotation.y),
                    XMConvertToRadians(rotation.z)) *
                XMMatrixTranslation(
                    transform.position.x,
                    transform.position.y,
                    transform.position.z);
        }

        D2D1_COLOR_F D2DColor(
            WorkerCreativeColor color,
            float opacity = 1.0f)
        {
            return D2D1::ColorF(
                color.r,
                color.g,
                color.b,
                color.a * opacity);
        }

        D2D1_COLOR_F D2DColor(
            WorkerCreativeWorld2DColor color,
            float opacity = 1.0f)
        {
            return D2D1::ColorF(
                color.r,
                color.g,
                color.b,
                color.a * opacity);
        }

        std::wstring CreativeScalarText(
            WorkerCreativeScalar const& value)
        {
            if (auto boolean = std::get_if<bool>(&value))
                return *boolean ? L"true" : L"false";
            if (auto integer = std::get_if<int64_t>(&value))
                return std::to_wstring(*integer);
            if (auto number = std::get_if<double>(&value))
            {
                std::wostringstream out;
                out << std::setprecision(8) << *number;
                return out.str();
            }
            return std::get<std::wstring>(value);
        }
    }

    void Direct3DApp::SetWindow(CoreWindow const& window)
    {
        m_window = window;
        CreateDeviceResources();
        CreateWindowSizeResources();
    }

    void Direct3DApp::SetStatusLines(std::vector<std::wstring> lines)
    {
        std::scoped_lock lock(m_linesMutex);
        m_statusLines = std::move(lines);
    }

    void Direct3DApp::SetCreativeSnapshot(
        std::shared_ptr<WorkerCreativeForegroundSnapshot const> snapshot)
    {
        std::scoped_lock lock(m_linesMutex);
        m_creativeSnapshot = std::move(snapshot);
    }

    void Direct3DApp::SetCreativeFrameCaptureRequest(
        WorkerCreativeFrameCaptureRequest request)
    {
        m_creativeCaptureRequest = std::move(request);
        m_creativeCaptureResult.reset();
    }

    std::optional<WorkerCreativeFrameCaptureResult>
        Direct3DApp::TakeCreativeFrameCaptureResult()
    {
        auto result = std::move(m_creativeCaptureResult);
        m_creativeCaptureResult.reset();
        return result;
    }

    void Direct3DApp::CreateDeviceResources()
    {
        uint32_t flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL levels[] =
        {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };

        com_ptr<ID3D11Device> device;
        com_ptr<ID3D11DeviceContext> context;
        check_hresult(D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            levels,
            ARRAYSIZE(levels),
            D3D11_SDK_VERSION,
            device.put(),
            &m_featureLevel,
            context.put()));

        m_d3dDevice = device.as<ID3D11Device1>();
        m_d3dContext = context.as<ID3D11DeviceContext1>();

        D2D1_FACTORY_OPTIONS options{};
#if defined(_DEBUG)
        options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
        check_hresult(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1),
            &options,
            m_d2dFactory.put_void()));

        auto dxgiDevice = m_d3dDevice.as<IDXGIDevice>();
        check_hresult(m_d2dFactory->CreateDevice(dxgiDevice.get(), m_d2dDevice.put()));
        check_hresult(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_d2dContext.put()));

        check_hresult(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.put())));

        check_hresult(m_dwriteFactory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            26.0f,
            L"en-us",
            m_textFormat.put()));
        check_hresult(m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING));
        check_hresult(m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR));
        CreateCreative3DResources();
    }

    void Direct3DApp::CreateCreative3DResources()
    {
        auto installedPath =
            std::wstring(
                Package::Current().InstalledLocation().Path().c_str());
        auto vertexBytes = ReadBinaryFile(
            installedPath + L"\\ColorVertexShader.cso");
        auto pixelBytes = ReadBinaryFile(
            installedPath + L"\\ColorPixelShader.cso");
        check_hresult(m_d3dDevice->CreateVertexShader(
            vertexBytes.data(),
            vertexBytes.size(),
            nullptr,
            m_creativeVertexShader.put()));
        check_hresult(m_d3dDevice->CreatePixelShader(
            pixelBytes.data(),
            pixelBytes.size(),
            nullptr,
            m_creativePixelShader.put()));
        D3D11_INPUT_ELEMENT_DESC elements[] =
        {
            {
                "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,
                0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0
            },
            {
                "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,
                0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0
            }
        };
        check_hresult(m_d3dDevice->CreateInputLayout(
            elements,
            ARRAYSIZE(elements),
            vertexBytes.data(),
            vertexBytes.size(),
            m_creativeInputLayout.put()));

        D3D11_BUFFER_DESC vertexDesc{};
        vertexDesc.ByteWidth = static_cast<UINT>(
            MaxCreativeVertices * sizeof(CreativeVertex));
        vertexDesc.Usage = D3D11_USAGE_DYNAMIC;
        vertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vertexDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check_hresult(m_d3dDevice->CreateBuffer(
            &vertexDesc,
            nullptr,
            m_creativeVertexBuffer.put()));

        D3D11_DEPTH_STENCIL_DESC depthState{};
        depthState.DepthEnable = TRUE;
        depthState.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depthState.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        check_hresult(m_d3dDevice->CreateDepthStencilState(
            &depthState,
            m_creativeDepthState.put()));

        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        check_hresult(m_d3dDevice->CreateRasterizerState(
            &rasterizer,
            m_creativeRasterizer.put()));
    }

    void Direct3DApp::CreateWindowSizeResources()
    {
        if (!m_window) return;

        auto bounds = m_window.Bounds();
        m_width = (std::max<uint32_t>)(1, static_cast<uint32_t>(bounds.Width));
        m_height = (std::max<uint32_t>)(1, static_cast<uint32_t>(bounds.Height));
        // D2DERR_RECREATE_TARGET invalidates every device-dependent bitmap.
        // Keeping sprite entries here makes each following creative draw reuse
        // stale resources and recreate the target forever, leaving only the
        // D3D clear color in the swap chain and in frame captures.
        m_creativeSpriteBitmaps.clear();
        m_d2dTargetBitmap = nullptr;

        if (m_swapChain)
        {
            check_hresult(m_swapChain->ResizeBuffers(2, m_width, m_height, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
        }
        else
        {
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = m_width;
            desc.Height = m_height;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.Stereo = false;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

            auto dxgiDevice = m_d3dDevice.as<IDXGIDevice3>();
            com_ptr<IDXGIAdapter> adapter;
            check_hresult(dxgiDevice->GetAdapter(adapter.put()));
            com_ptr<IDXGIFactory2> factory;
            check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
            auto windowUnknown = m_window.as<IUnknown>();
            check_hresult(factory->CreateSwapChainForCoreWindow(
                m_d3dDevice.get(),
                windowUnknown.get(),
                &desc,
                nullptr,
                m_swapChain.put()));
        }

        com_ptr<IDXGISurface> surface;
        check_hresult(m_swapChain->GetBuffer(0, __uuidof(IDXGISurface), surface.put_void()));

        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f,
            96.0f);
        check_hresult(m_d2dContext->CreateBitmapFromDxgiSurface(surface.get(), &props, m_d2dTargetBitmap.put()));
        m_d2dContext->SetTarget(m_d2dTargetBitmap.get());
        check_hresult(m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.93f, 0.97f, 1.0f), m_textBrush.put()));
        CreateCreativeDepthResources();
    }

    void Direct3DApp::CreateCreativeDepthResources()
    {
        m_creativeDepthView = nullptr;
        m_creativeDepthTexture = nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        check_hresult(m_d3dDevice->CreateTexture2D(
            &desc,
            nullptr,
            m_creativeDepthTexture.put()));
        check_hresult(m_d3dDevice->CreateDepthStencilView(
            m_creativeDepthTexture.get(),
            nullptr,
            m_creativeDepthView.put()));
    }

    void Direct3DApp::Render()
    {
        if (!m_d3dContext || !m_swapChain || !m_d2dContext) return;

        std::vector<std::wstring> lines;
        std::shared_ptr<WorkerCreativeForegroundSnapshot const>
            creative;
        {
            std::scoped_lock lock(m_linesMutex);
            lines = m_statusLines;
            creative = m_creativeSnapshot;
        }

        WorkerCreativeColor background{
            0.02f, 0.04f, 0.07f, 1.0f
        };
        if (creative && creative->plan &&
            creative->plan->world2d.has_value())
        {
            auto const& world =
                creative->plan->world2d->background;
            background = {
                world.r, world.g, world.b, world.a
            };
        }
        else if (creative && creative->plan &&
            creative->plan->canvas.has_value())
        {
            background =
                creative->plan->canvas->background;
        }
        float color[] = {
            background.r,
            background.g,
            background.b,
            background.a
        };
        com_ptr<ID3D11Texture2D> backBuffer;
        check_hresult(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), backBuffer.put_void()));
        com_ptr<ID3D11RenderTargetView> renderTarget;
        check_hresult(m_d3dDevice->CreateRenderTargetView(backBuffer.get(), nullptr, renderTarget.put()));
        m_d3dContext->ClearRenderTargetView(renderTarget.get(), color);

        if (creative && creative->plan &&
            creative->plan->scene3d.has_value())
        {
            RenderCreative3D(*creative, renderTarget.get());
        }

        m_d2dContext->BeginDraw();
        if (creative && creative->plan)
        {
            if (creative->plan->world2d.has_value())
            {
                DrawCreativeWorld2D(*creative);
            }
            if (creative->plan->canvas.has_value())
            {
                DrawCreative2D(*creative);
            }
            if (creative->plan->ui.has_value())
            {
                DrawCreativeUi(*creative);
            }
            DrawCreativeOverlay(*creative);
        }
        else
        {
            std::wostringstream text;
            for (auto const& line : lines)
            {
                text << line << L"\n";
            }
            D2D1_RECT_F rect = D2D1::RectF(
                64.0f,
                56.0f,
                static_cast<float>(m_width - 64),
                static_cast<float>(m_height - 56));
            auto fullText = text.str();
            m_d2dContext->DrawText(
                fullText.c_str(),
                static_cast<uint32_t>(fullText.size()),
                m_textFormat.get(),
                rect,
                m_textBrush.get());
        }
        HRESULT hr = m_d2dContext->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET)
        {
            CreateWindowSizeResources();
        }
        else
        {
            check_hresult(hr);
        }

        if (m_creativeCaptureRequest.has_value())
        {
            m_creativeCaptureResult =
                CaptureCreativeFrame(
                    backBuffer.get(),
                    *m_creativeCaptureRequest);
            m_creativeCaptureRequest.reset();
        }
        check_hresult(m_swapChain->Present(1, 0));
    }

    WorkerCreativeFrameCaptureResult
        Direct3DApp::CaptureCreativeFrame(
            ID3D11Texture2D* backBuffer,
            WorkerCreativeFrameCaptureRequest const& request)
    {
        WorkerCreativeFrameCaptureResult result;
        result.request = request;
        try
        {
            D3D11_TEXTURE2D_DESC source{};
            backBuffer->GetDesc(&source);
            if (source.Format !=
                    DXGI_FORMAT_B8G8R8A8_UNORM ||
                source.Width == 0 ||
                source.Height == 0)
            {
                result.errorCode =
                    L"xcp.creative.frame_capture_format_unsupported";
                return result;
            }
            D3D11_TEXTURE2D_DESC staging = source;
            staging.BindFlags = 0;
            staging.MiscFlags = 0;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            com_ptr<ID3D11Texture2D> readback;
            check_hresult(m_d3dDevice->CreateTexture2D(
                &staging,
                nullptr,
                readback.put()));
            m_d3dContext->CopyResource(
                readback.get(),
                backBuffer);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            check_hresult(m_d3dContext->Map(
                readback.get(),
                0,
                D3D11_MAP_READ,
                0,
                &mapped));
            auto captureWidth =
                (std::min)(source.Width, 640u);
            auto captureHeight =
                (std::min)(source.Height, 360u);
            result.width = captureWidth;
            result.height = captureHeight;
            result.stride = captureWidth * 4u;
            result.bgra.resize(
                static_cast<size_t>(result.stride) *
                captureHeight);
            auto sourceBytes =
                static_cast<uint8_t const*>(mapped.pData);
            for (uint32_t y = 0;
                 y < captureHeight;
                 ++y)
            {
                auto sourceY =
                    static_cast<uint32_t>(
                        static_cast<uint64_t>(y) *
                        source.Height /
                        captureHeight);
                auto row =
                    sourceBytes +
                    static_cast<size_t>(sourceY) *
                        mapped.RowPitch;
                auto output =
                    result.bgra.data() +
                    static_cast<size_t>(y) *
                        result.stride;
                for (uint32_t x = 0;
                     x < captureWidth;
                     ++x)
                {
                    auto sourceX =
                        static_cast<uint32_t>(
                            static_cast<uint64_t>(x) *
                            source.Width /
                            captureWidth);
                    std::memcpy(
                        output +
                            static_cast<size_t>(x) * 4u,
                        row +
                            static_cast<size_t>(sourceX) * 4u,
                        4u);
                }
            }
            m_d3dContext->Unmap(
                readback.get(),
                0);
        }
        catch (...)
        {
            result.width = 0;
            result.height = 0;
            result.stride = 0;
            result.bgra.clear();
            result.errorCode =
                L"xcp.creative.frame_capture_gpu_failed";
        }
        return result;
    }

    void Direct3DApp::RenderCreative3D(
        WorkerCreativeForegroundSnapshot const& snapshot,
        ID3D11RenderTargetView* renderTarget)
    {
        if (!snapshot.plan ||
            !snapshot.plan->scene3d.has_value() ||
            !m_creativeVertexBuffer ||
            !m_creativeDepthView)
        {
            return;
        }
        auto const& scene = *snapshot.plan->scene3d;
        auto cameraDefinition = std::find_if(
            scene.cameras.begin(),
            scene.cameras.end(),
            [&](auto const& camera)
            {
                return camera.id == scene.activeCamera;
            });
        if (cameraDefinition == scene.cameras.end())
        {
            return;
        }
        auto cameraTransform = cameraDefinition->transform;
        auto dynamicCamera =
            snapshot.cameraTransforms.find(scene.activeCamera);
        if (dynamicCamera != snapshot.cameraTransforms.end())
        {
            cameraTransform = dynamicCamera->second;
        }
        auto rotation = XMMatrixRotationRollPitchYaw(
            XMConvertToRadians(
                cameraTransform.rotationDegrees.x),
            XMConvertToRadians(
                cameraTransform.rotationDegrees.y),
            XMConvertToRadians(
                cameraTransform.rotationDegrees.z));
        auto eye = XMVectorSet(
            cameraTransform.position.x,
            cameraTransform.position.y,
            cameraTransform.position.z,
            1.0f);
        auto direction = XMVector3TransformNormal(
            XMVectorSet(0, 0, 1, 0),
            rotation);
        auto up = XMVector3TransformNormal(
            XMVectorSet(0, 1, 0, 0),
            rotation);
        auto view = XMMatrixLookToLH(eye, direction, up);
        auto projection = XMMatrixPerspectiveFovLH(
            XMConvertToRadians(
                cameraDefinition->fieldOfViewDegrees),
            static_cast<float>(m_width) /
                static_cast<float>(m_height),
            cameraDefinition->nearPlane,
            cameraDefinition->farPlane);

        std::map<std::wstring, WorkerCreativeSceneNode const*>
            nodes;
        for (auto const& node : scene.nodes)
            nodes.emplace(node.id, &node);
        std::map<std::wstring, XMMATRIX> worldCache;
        std::function<XMMATRIX(std::wstring const&)> worldFor =
            [&](std::wstring const& id) -> XMMATRIX
        {
            auto cached = worldCache.find(id);
            if (cached != worldCache.end())
                return cached->second;
            auto found = nodes.find(id);
            if (found == nodes.end())
                return XMMatrixIdentity();
            auto transform = found->second->transform;
            auto translation =
                snapshot.nodeTranslation3D.find(id);
            if (translation !=
                snapshot.nodeTranslation3D.end())
            {
                transform.position.x +=
                    translation->second.x;
                transform.position.y +=
                    translation->second.y;
                transform.position.z +=
                    translation->second.z;
            }
            auto world = TransformMatrix(transform);
            if (!found->second->parentId.empty())
            {
                world *= worldFor(
                    found->second->parentId);
            }
            worldCache.emplace(id, world);
            return world;
        };
        std::map<std::wstring, WorkerCreativeMesh const*>
            meshes;
        for (auto const& mesh : scene.meshes)
            meshes.emplace(mesh.id, &mesh);
        std::map<std::wstring, WorkerCreativeMaterial const*>
            materials;
        for (auto const& material : scene.materials)
            materials.emplace(material.id, &material);

        std::vector<CreativeVertex> vertices;
        vertices.reserve(4096);
        for (auto const& node : scene.nodes)
        {
            auto visible = node.visible;
            auto override =
                snapshot.nodeVisibility.find(node.id);
            if (override != snapshot.nodeVisibility.end())
                visible = override->second;
            if (!visible || node.meshId.empty())
                continue;
            auto mesh = meshes.find(node.meshId);
            if (mesh == meshes.end() ||
                (mesh->second->primitive.empty() &&
                    mesh->second->triangles.empty()))
                continue;
            WorkerCreativeMaterial fallback;
            fallback.baseColor = {
                0.55f, 0.68f, 0.82f, 1.0f
            };
            WorkerCreativeMaterial const* material = &fallback;
            auto selected =
                materials.find(node.materialId);
            if (selected != materials.end())
                material = selected->second;
            auto world = worldFor(node.id);
            auto viewWorld = world * view;
            std::vector<CreativeTriangle> decodedTriangles;
            std::vector<CreativeTriangle> const* triangles = nullptr;
            if (!mesh->second->triangles.empty())
            {
                decodedTriangles.reserve(
                    mesh->second->triangles.size());
                for (auto const& triangle :
                     mesh->second->triangles)
                {
                    decodedTriangles.push_back({
                        Point(
                            triangle.a.x,
                            triangle.a.y,
                            triangle.a.z),
                        Point(
                            triangle.b.x,
                            triangle.b.y,
                            triangle.b.z),
                        Point(
                            triangle.c.x,
                            triangle.c.y,
                            triangle.c.z),
                        Point(
                            triangle.normal.x,
                            triangle.normal.y,
                            triangle.normal.z)
                    });
                }
                triangles = &decodedTriangles;
            }
            else
            {
                triangles =
                    &PrimitiveTriangles(
                        mesh->second->primitive);
            }
            for (auto const& triangle : *triangles)
            {
                if (vertices.size() + 3 >
                    MaxCreativeVertices)
                    break;
                auto pointA = XMLoadFloat3(&triangle.a);
                auto pointB = XMLoadFloat3(&triangle.b);
                auto pointC = XMLoadFloat3(&triangle.c);
                auto viewA =
                    XMVector3TransformCoord(pointA, viewWorld);
                auto viewB =
                    XMVector3TransformCoord(pointB, viewWorld);
                auto viewC =
                    XMVector3TransformCoord(pointC, viewWorld);
                if (XMVectorGetZ(viewA) <=
                        cameraDefinition->nearPlane ||
                    XMVectorGetZ(viewB) <=
                        cameraDefinition->nearPlane ||
                    XMVectorGetZ(viewC) <=
                        cameraDefinition->nearPlane)
                {
                    continue;
                }
                auto normal = XMVector3Normalize(
                    XMVector3TransformNormal(
                        XMLoadFloat3(&triangle.normal),
                        world));
                auto center = XMVectorScale(
                    XMVectorAdd(
                        XMVectorAdd(
                            XMVector3TransformCoord(pointA, world),
                            XMVector3TransformCoord(pointB, world)),
                        XMVector3TransformCoord(pointC, world)),
                    1.0f / 3.0f);
                float illumination = 0.18f;
                for (auto const& light : scene.lights)
                {
                    auto lightColor =
                        (light.color.r +
                            light.color.g +
                            light.color.b) /
                        3.0f;
                    if (light.type == L"directional")
                    {
                        auto lightDirection =
                            XMVector3Normalize(
                                XMVectorSet(
                                    -light.direction.x,
                                    -light.direction.y,
                                    -light.direction.z,
                                    0));
                        illumination +=
                            (std::max)(
                                0.0f,
                                XMVectorGetX(
                                    XMVector3Dot(
                                        normal,
                                        lightDirection))) *
                            light.intensity *
                            lightColor *
                            0.38f;
                    }
                    else
                    {
                        auto toLight = XMVectorSubtract(
                            XMVectorSet(
                                light.position.x,
                                light.position.y,
                                light.position.z,
                                1),
                            center);
                        auto distance =
                            XMVectorGetX(
                                XMVector3Length(toLight));
                        if (distance < light.range)
                        {
                            auto attenuation =
                                1.0f - distance /
                                    light.range;
                            illumination +=
                                (std::max)(
                                    0.0f,
                                    XMVectorGetX(
                                        XMVector3Dot(
                                            normal,
                                            XMVector3Normalize(
                                                toLight)))) *
                                attenuation *
                                light.intensity *
                                lightColor *
                                0.18f;
                        }
                    }
                }
                illumination =
                    (std::min)(illumination, 1.35f);
                XMFLOAT4 color{
                    (std::min)(
                        1.0f,
                        material->baseColor.r *
                            illumination +
                            material->emissive.r),
                    (std::min)(
                        1.0f,
                        material->baseColor.g *
                            illumination +
                            material->emissive.g),
                    (std::min)(
                        1.0f,
                        material->baseColor.b *
                            illumination +
                            material->emissive.b),
                    material->baseColor.a
                };
                auto append = [&](XMVECTOR point)
                {
                    XMFLOAT3 projected;
                    XMStoreFloat3(
                        &projected,
                        XMVector3TransformCoord(
                            point,
                            world * view * projection));
                    vertices.push_back(
                        { projected, color });
                };
                append(pointA);
                append(pointB);
                append(pointC);
            }
        }
        if (vertices.empty())
            return;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        check_hresult(m_d3dContext->Map(
            m_creativeVertexBuffer.get(),
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &mapped));
        std::memcpy(
            mapped.pData,
            vertices.data(),
            vertices.size() * sizeof(CreativeVertex));
        m_d3dContext->Unmap(
            m_creativeVertexBuffer.get(),
            0);

        m_d3dContext->ClearDepthStencilView(
            m_creativeDepthView.get(),
            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
            1.0f,
            0);
        ID3D11RenderTargetView* targets[] = { renderTarget };
        m_d3dContext->OMSetRenderTargets(
            1,
            targets,
            m_creativeDepthView.get());
        m_d3dContext->OMSetDepthStencilState(
            m_creativeDepthState.get(),
            0);
        m_d3dContext->RSSetState(
            m_creativeRasterizer.get());
        D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            static_cast<float>(m_width),
            static_cast<float>(m_height),
            0.0f,
            1.0f
        };
        m_d3dContext->RSSetViewports(1, &viewport);
        UINT stride = sizeof(CreativeVertex);
        UINT offset = 0;
        ID3D11Buffer* buffers[] = {
            m_creativeVertexBuffer.get()
        };
        m_d3dContext->IASetVertexBuffers(
            0,
            1,
            buffers,
            &stride,
            &offset);
        m_d3dContext->IASetInputLayout(
            m_creativeInputLayout.get());
        m_d3dContext->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_d3dContext->VSSetShader(
            m_creativeVertexShader.get(),
            nullptr,
            0);
        m_d3dContext->PSSetShader(
            m_creativePixelShader.get(),
            nullptr,
            0);
        m_d3dContext->Draw(
            static_cast<UINT>(vertices.size()),
            0);
        ID3D11RenderTargetView* noTargets[] = { nullptr };
        m_d3dContext->OMSetRenderTargets(
            1,
            noTargets,
            nullptr);
    }

    void Direct3DApp::DrawCreativeText(
        std::wstring const& text,
        float size,
        D2D1_RECT_F const& rect,
        WorkerCreativeColor color,
        DWRITE_FONT_WEIGHT weight)
    {
        if (text.empty())
            return;
        com_ptr<IDWriteTextFormat> format;
        check_hresult(m_dwriteFactory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            weight,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            size,
            L"en-us",
            format.put()));
        check_hresult(format->SetWordWrapping(
            DWRITE_WORD_WRAPPING_WRAP));
        m_textBrush->SetColor(D2DColor(color));
        m_d2dContext->DrawText(
            text.c_str(),
            static_cast<uint32_t>(text.size()),
            format.get(),
            rect,
            m_textBrush.get());
    }

    void Direct3DApp::DrawCreativeWorld2D(
        WorkerCreativeForegroundSnapshot const& snapshot)
    {
        auto const& world = *snapshot.plan->world2d;
        auto scale = (std::min)(
            static_cast<float>(m_width) /
                static_cast<float>(world.width),
            static_cast<float>(m_height) /
                static_cast<float>(world.height));
        auto logicalWidth =
            world.authoredV2
                ? static_cast<float>(world.width)
                : static_cast<float>(world.columns) * world.cellSize;
        auto logicalHeight =
            world.authoredV2
                ? static_cast<float>(world.height)
                : static_cast<float>(world.rows) * world.cellSize;
        auto originX =
            (static_cast<float>(m_width) -
                logicalWidth * scale) * 0.5f +
            (world.authoredV2
                ? world.contentOrigin.x * scale
                : 0.0f);
        auto originY =
            (static_cast<float>(m_height) -
                logicalHeight * scale) * 0.5f +
            (world.authoredV2
                ? world.contentOrigin.y * scale
                : 0.0f);
        auto cell = world.cellSize * scale;
        auto viewport = D2D1::RectF(
            (static_cast<float>(m_width) -
                static_cast<float>(world.width) * scale) * 0.5f,
            (static_cast<float>(m_height) -
                static_cast<float>(world.height) * scale) * 0.5f,
            (static_cast<float>(m_width) +
                static_cast<float>(world.width) * scale) * 0.5f,
            (static_cast<float>(m_height) +
                static_cast<float>(world.height) * scale) * 0.5f);
        if (world.authoredV2)
        {
            m_d2dContext->PushAxisAlignedClip(
                viewport,
                D2D1_ANTIALIAS_MODE_ALIASED);
            if (!world.backgroundAssetId.empty())
            {
                auto asset =
                    snapshot.plan->assets.find(
                        world.backgroundAssetId);
                if (asset != snapshot.plan->assets.end())
                {
                    auto bitmap =
                        CreativeSpriteBitmap(asset->second);
                    if (bitmap != nullptr)
                    {
                        m_d2dContext->DrawBitmap(
                            bitmap,
                            viewport,
                            1.0f,
                            D2D1_INTERPOLATION_MODE_LINEAR);
                    }
                }
            }
        }
        auto drawItem =
            [&](WorkerCreativeWorld2DVec2 position,
                WorkerCreativeWorld2DColor color,
                std::wstring const& assetId,
                float inset,
                WorkerCreativeWorld2DRender const* authored)
        {
            auto rect = authored
                ? D2D1::RectF(
                    originX + position.x * cell +
                        authored->offset.x * scale,
                    originY + position.y * cell +
                        authored->offset.y * scale,
                    originX + position.x * cell +
                        (authored->offset.x + authored->size.x) * scale,
                    originY + position.y * cell +
                        (authored->offset.y + authored->size.y) * scale)
                : D2D1::RectF(
                    originX + position.x * cell + inset,
                    originY + position.y * cell + inset,
                    originX + (position.x + 1) * cell - inset,
                    originY + (position.y + 1) * cell - inset);
            m_textBrush->SetColor(D2DColor(color));
            m_d2dContext->FillRectangle(
                rect,
                m_textBrush.get());
            auto const& renderAssetId =
                authored ? authored->assetId : assetId;
            if (!renderAssetId.empty())
            {
                auto asset =
                    snapshot.plan->assets.find(renderAssetId);
                if (asset != snapshot.plan->assets.end())
                {
                    auto bitmap =
                        CreativeSpriteBitmap(asset->second);
                    if (bitmap != nullptr)
                    {
                        if (authored &&
                            authored->hasSourceRect)
                        {
                            auto source = D2D1::RectF(
                                authored->sourceRect.x,
                                authored->sourceRect.y,
                                authored->sourceRect.x +
                                    authored->sourceRect.width,
                                authored->sourceRect.y +
                                    authored->sourceRect.height);
                            m_d2dContext->DrawBitmap(
                                bitmap,
                                rect,
                                authored->opacity,
                                D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                                source);
                        }
                        else
                        {
                            m_d2dContext->DrawBitmap(
                                bitmap,
                                rect,
                                authored
                                    ? authored->opacity
                                    : color.a,
                                D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                        }
                    }
                }
            }
        };
        for (auto const& tile : world.tiles)
        {
            if (tile.visible)
                drawItem(
                    {
                        static_cast<float>(tile.position.x),
                        static_cast<float>(tile.position.y)
                    },
                    tile.color,
                    tile.assetId,
                    0.0f,
                    tile.hasAuthoredRender
                        ? &tile.render
                        : nullptr);
        }
        for (auto const& entity : world.entities)
        {
            auto visible =
                snapshot.world2dState.entityVisibility.find(
                    entity.id);
            if (visible ==
                    snapshot.world2dState.entityVisibility.end() ||
                !visible->second)
            {
                continue;
            }
            auto position =
                snapshot.world2dState.entityPositions.find(entity.id);
            if (position ==
                snapshot.world2dState.entityPositions.end())
            {
                continue;
            }
            drawItem(
                position->second,
                entity.color,
                entity.assetId,
                entity.hasAuthoredRender
                    ? 0.0f
                    : (std::max)(1.0f, cell * 0.08f),
                entity.hasAuthoredRender
                    ? &entity.render
                    : nullptr);
        }
        if (world.authoredV2)
            m_d2dContext->PopAxisAlignedClip();
        m_d2dContext->SetTransform(
            D2D1::Matrix3x2F::Identity());
    }

    void Direct3DApp::DrawCreative2D(
        WorkerCreativeForegroundSnapshot const& snapshot)
    {
        auto const& canvas = *snapshot.plan->canvas;
        auto scale = (std::min)(
            static_cast<float>(m_width) /
                static_cast<float>(canvas.width),
            static_cast<float>(m_height) /
                static_cast<float>(canvas.height));
        auto offsetX =
            (static_cast<float>(m_width) -
                canvas.width * scale) * 0.5f;
        auto offsetY =
            (static_cast<float>(m_height) -
                canvas.height * scale) * 0.5f;
        for (auto const& node : canvas.nodes)
        {
            auto visible = node.visible;
            auto override =
                snapshot.nodeVisibility.find(node.id);
            if (override != snapshot.nodeVisibility.end())
                visible = override->second;
            if (!visible)
                continue;
            auto translation =
                snapshot.nodeTranslation2D.find(node.id);
            auto position = node.transform.position;
            if (translation !=
                snapshot.nodeTranslation2D.end())
            {
                position.x += translation->second.x;
                position.y += translation->second.y;
            }
            auto center = D2D1::Point2F(
                offsetX + position.x * scale,
                offsetY + position.y * scale);
            auto transform =
                D2D1::Matrix3x2F::Scale(
                    node.transform.scale.x * scale,
                    node.transform.scale.y * scale) *
                D2D1::Matrix3x2F::Rotation(
                    node.transform.rotationDegrees) *
                D2D1::Matrix3x2F::Translation(
                    center.x,
                    center.y);
            m_d2dContext->SetTransform(transform);
            auto opacity = node.opacity;
            m_textBrush->SetColor(
                D2DColor(node.fill, opacity));
            if (node.type == L"rectangle")
            {
                auto rect = D2D1::RectF(
                    -node.size.x * 0.5f,
                    -node.size.y * 0.5f,
                    node.size.x * 0.5f,
                    node.size.y * 0.5f);
                m_d2dContext->FillRectangle(
                    rect,
                    m_textBrush.get());
                if (node.strokeWidth > 0.0f)
                {
                    m_textBrush->SetColor(
                        D2DColor(node.stroke, opacity));
                    m_d2dContext->DrawRectangle(
                        rect,
                        m_textBrush.get(),
                        node.strokeWidth);
                }
            }
            else if (node.type == L"sprite")
            {
                auto asset =
                    snapshot.plan->assets.find(
                        node.assetId);
                if (asset != snapshot.plan->assets.end())
                {
                    auto bitmap =
                        CreativeSpriteBitmap(
                            asset->second);
                    if (bitmap != nullptr)
                    {
                        auto rect = D2D1::RectF(
                            -node.size.x * 0.5f,
                            -node.size.y * 0.5f,
                            node.size.x * 0.5f,
                            node.size.y * 0.5f);
                        m_d2dContext->DrawBitmap(
                            bitmap,
                            rect,
                            opacity,
                            D2D1_INTERPOLATION_MODE_LINEAR);
                        if (node.strokeWidth > 0.0f)
                        {
                            m_textBrush->SetColor(
                                D2DColor(
                                    node.stroke,
                                    opacity));
                            m_d2dContext->DrawRectangle(
                                rect,
                                m_textBrush.get(),
                                node.strokeWidth);
                        }
                    }
                }
            }
            else if (node.type == L"ellipse")
            {
                auto ellipse = D2D1::Ellipse(
                    D2D1::Point2F(0, 0),
                    node.size.x * 0.5f,
                    node.size.y * 0.5f);
                m_d2dContext->FillEllipse(
                    ellipse,
                    m_textBrush.get());
                if (node.strokeWidth > 0.0f)
                {
                    m_textBrush->SetColor(
                        D2DColor(node.stroke, opacity));
                    m_d2dContext->DrawEllipse(
                        ellipse,
                        m_textBrush.get(),
                        node.strokeWidth);
                }
            }
            else if (node.type == L"line")
            {
                m_textBrush->SetColor(
                    D2DColor(
                        node.strokeWidth > 0.0f
                            ? node.stroke
                            : node.fill,
                        opacity));
                m_d2dContext->DrawLine(
                    D2D1::Point2F(0, 0),
                    D2D1::Point2F(
                        node.lineEnd.x -
                            node.transform.position.x,
                        node.lineEnd.y -
                            node.transform.position.y),
                    m_textBrush.get(),
                    (std::max)(1.0f, node.strokeWidth));
            }
            else if (node.type == L"text")
            {
                m_d2dContext->SetTransform(
                    D2D1::Matrix3x2F::Identity());
                DrawCreativeText(
                    node.text,
                    node.fontSize * scale,
                    D2D1::RectF(
                        center.x - 500.0f * scale,
                        center.y -
                            node.fontSize * scale,
                        center.x + 500.0f * scale,
                        center.y +
                            node.fontSize * 2.0f * scale),
                    node.fill,
                    DWRITE_FONT_WEIGHT_SEMI_BOLD);
            }
        }
        m_d2dContext->SetTransform(
            D2D1::Matrix3x2F::Identity());
    }

    ID2D1Bitmap1* Direct3DApp::CreativeSpriteBitmap(
        WorkerCreativeAsset const& asset)
    {
        auto found =
            m_creativeSpriteBitmaps.find(asset.sha256);
        if (found != m_creativeSpriteBitmaps.end())
            return found->second.get();
        if (asset.pixelWidth == 0 ||
            asset.pixelHeight == 0 ||
            asset.pixelStride == 0 ||
            asset.decodedPixels.empty())
        {
            return nullptr;
        }
        D2D1_BITMAP_PROPERTIES1 properties =
            D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM,
                    D2D1_ALPHA_MODE_PREMULTIPLIED));
        com_ptr<ID2D1Bitmap1> bitmap;
        check_hresult(m_d2dContext->CreateBitmap(
            D2D1::SizeU(
                asset.pixelWidth,
                asset.pixelHeight),
            asset.decodedPixels.data(),
            asset.pixelStride,
            &properties,
            bitmap.put()));
        auto inserted =
            m_creativeSpriteBitmaps.emplace(
                asset.sha256,
                std::move(bitmap));
        return inserted.first->second.get();
    }

    void Direct3DApp::DrawCreativeUi(
        WorkerCreativeForegroundSnapshot const& snapshot)
    {
        auto const& ui = *snapshot.plan->ui;
        auto root = std::find_if(
            ui.elements.begin(),
            ui.elements.end(),
            [&](auto const& element)
            {
                return element.id == ui.rootId;
            });
        auto width = root != ui.elements.end() &&
                root->width > 0.0f
            ? root->width
            : 960.0f;
        auto height = root != ui.elements.end() &&
                root->height > 0.0f
            ? root->height
            : 540.0f;
        auto scale = (std::min)(
            1.0f,
            (std::min)(
                static_cast<float>(m_width) / 1280.0f,
                static_cast<float>(m_height) / 720.0f));
        width *= scale;
        height *= scale;
        auto margin = 32.0f * scale;
        auto left =
            (static_cast<float>(m_width) - width) * 0.5f;
        if (root != ui.elements.end() && root->align == L"end")
        {
            left = static_cast<float>(m_width) - width - margin;
        }
        else if (root != ui.elements.end() && root->align == L"stretch")
        {
            left = margin;
            width = (std::max)(
                0.0f,
                static_cast<float>(m_width) - margin * 2.0f);
        }
        auto top =
            (static_cast<float>(m_height) - height) * 0.5f;
        auto padding = root != ui.elements.end()
            ? root->padding * scale
            : 32.0f * scale;
        auto gap = root != ui.elements.end()
            ? root->gap * scale
            : 16.0f * scale;
        auto panel = D2D1::RectF(
            left,
            top,
            left + width,
            top + height);
        m_textBrush->SetColor(
            D2D1::ColorF(0.025f, 0.04f, 0.03f, 0.86f));
        m_d2dContext->FillRoundedRectangle(
            D2D1::RoundedRect(panel, 24.0f, 24.0f),
            m_textBrush.get());

        auto isVisible = [&](WorkerCreativeUiElement const& element)
        {
            if (element.visibleState.empty())
                return true;
            auto value = snapshot.state.find(element.visibleState);
            return value != snapshot.state.end() &&
                std::holds_alternative<bool>(value->second) &&
                std::get<bool>(value->second);
        };
        auto elementHeight = [&](WorkerCreativeUiElement const& element)
        {
            return (element.type == L"label" ? 54.0f : 68.0f) *
                scale;
        };

        // State-controlled UI is status feedback rather than ordinary list
        // overflow. Keep active feedback pinned, then expose a bounded window
        // of the ordinary flow that always contains the focused control.
        std::vector<WorkerCreativeUiElement const*> pinned;
        std::vector<WorkerCreativeUiElement const*> flow;
        for (auto const& element : ui.elements)
        {
            if (element.id == ui.rootId || !isVisible(element))
                continue;
            if (!element.visibleState.empty())
                pinned.push_back(&element);
            else
                flow.push_back(&element);
        }

        auto contentHeight = (std::max)(0.0f, height - padding * 2.0f);
        float pinnedHeight = 0.0f;
        for (auto const* element : pinned)
        {
            auto next = elementHeight(*element);
            if (pinnedHeight > 0.0f)
                next += gap;
            if (pinnedHeight + next > contentHeight)
                break;
            pinnedHeight += next;
        }
        auto flowHeight = (std::max)(
            0.0f,
            contentHeight - pinnedHeight -
                (pinnedHeight > 0.0f && !flow.empty() ? gap : 0.0f));
        size_t flowStart = 0;
        auto focused = std::find_if(
            flow.begin(),
            flow.end(),
            [&](auto const* element)
            {
                return element->id == snapshot.focusedElementId;
            });
        if (focused != flow.end())
        {
            flowStart = static_cast<size_t>(
                std::distance(flow.begin(), focused));
            float used = elementHeight(**focused);
            while (flowStart > 0)
            {
                auto candidate = elementHeight(*flow[flowStart - 1]);
                if (used + gap + candidate > flowHeight)
                    break;
                used += gap + candidate;
                --flowStart;
            }
        }

        std::vector<WorkerCreativeUiElement const*> displayed;
        float usedHeight = 0.0f;
        auto appendIfFits = [&](WorkerCreativeUiElement const* element)
        {
            auto next = elementHeight(*element);
            if (!displayed.empty())
                next += gap;
            if (usedHeight + next > contentHeight)
                return false;
            usedHeight += next;
            displayed.push_back(element);
            return true;
        };
        for (auto const* element : pinned)
        {
            if (!appendIfFits(element))
                break;
        }
        for (size_t index = flowStart; index < flow.size(); ++index)
        {
            if (!appendIfFits(flow[index]))
                break;
        }

        float y = top + padding;
        for (auto const* elementPointer : displayed)
        {
            auto const& element = *elementPointer;
            auto currentHeight = elementHeight(element);
            auto rect = D2D1::RectF(
                left + padding,
                y,
                left + width - padding,
                y + currentHeight);
            auto elementFocused =
                element.id == snapshot.focusedElementId;
            if (element.type == L"button" ||
                element.type == L"toggle" ||
                element.type == L"slider" ||
                element.type == L"list")
            {
                m_textBrush->SetColor(
                    elementFocused
                        ? D2D1::ColorF(
                            0.47f, 0.89f, 0.11f, 0.94f)
                        : D2D1::ColorF(
                            0.12f, 0.16f, 0.13f, 0.92f));
                m_d2dContext->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        rect,
                        12.0f,
                        12.0f),
                    m_textBrush.get());
            }
            if (element.type == L"progress")
            {
                m_textBrush->SetColor(
                    D2D1::ColorF(
                        0.12f, 0.16f, 0.13f, 0.95f));
                m_d2dContext->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        rect,
                        10.0f,
                        10.0f),
                    m_textBrush.get());
                double value = element.minimum;
                auto state =
                    snapshot.state.find(
                        element.valueState);
                if (state != snapshot.state.end())
                {
                    if (auto integer =
                        std::get_if<int64_t>(
                            &state->second))
                        value =
                            static_cast<double>(*integer);
                    if (auto number =
                        std::get_if<double>(
                            &state->second))
                        value = *number;
                }
                auto denominator =
                    (std::max)(
                        0.000001,
                        element.maximum -
                            element.minimum);
                auto ratio =
                    static_cast<float>(
                        (std::max)(
                            0.0,
                            (std::min)(
                                1.0,
                                (value -
                                    element.minimum) /
                                    denominator)));
                auto fill = rect;
                fill.right =
                    fill.left +
                    (fill.right - fill.left) * ratio;
                m_textBrush->SetColor(
                    D2D1::ColorF(
                        0.47f, 0.89f, 0.11f, 1.0f));
                m_d2dContext->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        fill,
                        10.0f,
                        10.0f),
                    m_textBrush.get());
            }
            std::wstring text = element.text;
            if (!element.valueState.empty())
            {
                auto value =
                    snapshot.state.find(
                        element.valueState);
                if (value != snapshot.state.end())
                    text +=
                        (text.empty() ? L"" : L": ") +
                        CreativeScalarText(value->second);
            }
            DrawCreativeText(
                text,
                28.0f * scale,
                D2D1::RectF(
                    rect.left + 18.0f * scale,
                    rect.top + 12.0f * scale,
                    rect.right - 18.0f * scale,
                    rect.bottom),
                elementFocused
                    ? WorkerCreativeColor{
                        0.02f, 0.05f, 0.02f, 1.0f }
                    : WorkerCreativeColor{
                        0.93f, 0.97f, 0.93f, 1.0f },
                DWRITE_FONT_WEIGHT_SEMI_BOLD);
            y += currentHeight + gap;
        }
    }

    void Direct3DApp::DrawCreativeOverlay(
        WorkerCreativeForegroundSnapshot const& snapshot)
    {
        if (!snapshot.plan)
            return;
        if (snapshot.plan->scene3d.has_value() &&
            !snapshot.plan->ui.has_value())
        {
            auto rect = D2D1::RectF(
                28.0f,
                24.0f,
                (std::min)(
                    static_cast<float>(m_width) - 28.0f,
                    520.0f),
                190.0f);
            m_textBrush->SetColor(
                D2D1::ColorF(0.01f, 0.02f, 0.04f, 0.76f));
            m_d2dContext->FillRoundedRectangle(
                D2D1::RoundedRect(rect, 14.0f, 14.0f),
                m_textBrush.get());
            std::wstring text =
                snapshot.plan->projectId + L"  " +
                snapshot.plan->projectVersion +
                L"\nplan " +
                snapshot.plan->planSha256.substr(0, 12) +
                L"  frame " +
                std::to_wstring(snapshot.frameCount);
            for (auto const& [name, value] : snapshot.state)
            {
                text += L"\n" + name + L": " +
                    CreativeScalarText(value);
                if (text.size() > 360)
                    break;
            }
            DrawCreativeText(
                text,
                20.0f,
                D2D1::RectF(
                    rect.left + 16.0f,
                    rect.top + 12.0f,
                    rect.right - 12.0f,
                    rect.bottom - 8.0f),
                { 0.93f, 0.97f, 1.0f, 1.0f });
        }
        if (!snapshot.lastErrorCode.empty())
        {
            DrawCreativeText(
                L"Creative runtime: " +
                    snapshot.lastErrorCode,
                18.0f,
                D2D1::RectF(
                    28.0f,
                    static_cast<float>(m_height) - 52.0f,
                    static_cast<float>(m_width) - 28.0f,
                    static_cast<float>(m_height) - 20.0f),
                { 1.0f, 0.45f, 0.35f, 1.0f },
                DWRITE_FONT_WEIGHT_SEMI_BOLD);
        }
    }

    std::wstring Direct3DApp::FeatureLevelString() const
    {
        switch (m_featureLevel)
        {
        case D3D_FEATURE_LEVEL_11_1: return L"11_1";
        case D3D_FEATURE_LEVEL_11_0: return L"11_0";
        case D3D_FEATURE_LEVEL_10_1: return L"10_1";
        case D3D_FEATURE_LEVEL_10_0: return L"10_0";
        default: return L"unknown";
        }
    }

    std::wstring Direct3DApp::AdapterDescription() const
    {
        if (!m_d3dDevice) return L"unavailable";
        auto dxgiDevice = m_d3dDevice.as<IDXGIDevice3>();
        com_ptr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(adapter.put()))) return L"unavailable";
        DXGI_ADAPTER_DESC desc{};
        if (FAILED(adapter->GetDesc(&desc))) return L"unavailable";
        return desc.Description;
    }

    ProbeResult Direct3DApp::MakeGraphicsProbe() const
    {
        ProbeResult result;
        result.id = L"graphics.direct3d11";
        result.status = m_d3dDevice ? L"pass" : L"error";
        result.evidenceClass = L"MEASURED";
        result.startedUtc = UtcNow();
        result.endedUtc = result.startedUtc;
        result.durationMs = 0;
        result.apiCalls = { L"D3D11CreateDevice", L"IDXGIAdapter::GetDesc", L"CreateSwapChainForCoreWindow" };
        result.AddString(L"adapter_description", AdapterDescription());
        result.AddString(L"feature_level", FeatureLevelString());
        result.AddNumber(L"render_width", m_width);
        result.AddNumber(L"render_height", m_height);
        result.notes.push_back(L"Baseline uses Direct3D 11. D3D12 remains UNKNOWN until a separate official build/runtime probe is validated.");
        return result;
    }

    std::vector<ProbeResult> Direct3DApp::MakeGraphicsProbes() const
    {
        auto flavor = std::wstring(XCOMPUTE_MANIFEST_FLAVOR);
        if (flavor == L"gamebucket-d3d11compute")
        {
            return {
                MakeGraphicsProbe(),
                RunD3D11ComputeProbe()
            };
        }
        if (flavor == L"gamebucket-shaderruntime")
        {
            return {
                MakeGraphicsProbe(),
                RunShaderRuntimeProbe()
            };
        }

        return { MakeGraphicsProbe() };
    }

    ProbeResult Direct3DApp::RunD3D11ComputeProbe() const
    {
        auto started = std::chrono::steady_clock::now();
        ProbeResult result;
        result.id = L"graphics.d3d11_compute";
        result.status = L"pass";
        result.evidenceClass = L"MEASURED";
        result.startedUtc = UtcNow();
        result.apiCalls = {
            L"Package.Current.InstalledLocation",
            L"ID3D11Device::CreateComputeShader",
            L"ID3D11DeviceContext::Dispatch",
            L"ID3D11DeviceContext::Map"
        };
        auto finish = [&]() -> ProbeResult
        {
            result.endedUtc = UtcNow();
            result.durationMs = ElapsedMs(started);
            return result;
        };

        if (!m_d3dDevice || !m_d3dContext)
        {
            result.status = L"error";
            result.errors.push_back(L"D3D11 device/context unavailable.");
            return finish();
        }

        try
        {
            constexpr uint32_t elementCount = 4096;
            constexpr uint32_t threadsPerGroup = 64;
            constexpr uint32_t dispatchGroups = elementCount / threadsPerGroup;

            auto installedPath = std::wstring(Package::Current().InstalledLocation().Path().c_str());
            auto shaderPath = installedPath + L"\\IntCompute.cso";
            auto shaderBytes = ReadBinaryFile(shaderPath);

            winrt::com_ptr<ID3D11ComputeShader> shader;
            HRESULT hr = m_d3dDevice->CreateComputeShader(
                shaderBytes.data(),
                shaderBytes.size(),
                nullptr,
                shader.put());
            if (FAILED(hr))
            {
                result.status = L"error";
                result.errors.push_back(L"CreateComputeShader failed: " + HResultToString(hr));
                return finish();
            }

            D3D11_BUFFER_DESC outputDesc{};
            outputDesc.ByteWidth = elementCount * sizeof(uint32_t);
            outputDesc.Usage = D3D11_USAGE_DEFAULT;
            outputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            outputDesc.CPUAccessFlags = 0;
            outputDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            outputDesc.StructureByteStride = sizeof(uint32_t);

            winrt::com_ptr<ID3D11Buffer> outputBuffer;
            hr = m_d3dDevice->CreateBuffer(&outputDesc, nullptr, outputBuffer.put());
            if (FAILED(hr))
            {
                result.status = L"error";
                result.errors.push_back(L"CreateBuffer output failed: " + HResultToString(hr));
                return finish();
            }

            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.FirstElement = 0;
            uavDesc.Buffer.NumElements = elementCount;

            winrt::com_ptr<ID3D11UnorderedAccessView> uav;
            hr = m_d3dDevice->CreateUnorderedAccessView(outputBuffer.get(), &uavDesc, uav.put());
            if (FAILED(hr))
            {
                result.status = L"error";
                result.errors.push_back(L"CreateUnorderedAccessView failed: " + HResultToString(hr));
                return finish();
            }

            D3D11_BUFFER_DESC stagingDesc{};
            stagingDesc.ByteWidth = elementCount * sizeof(uint32_t);
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

            winrt::com_ptr<ID3D11Buffer> stagingBuffer;
            hr = m_d3dDevice->CreateBuffer(&stagingDesc, nullptr, stagingBuffer.put());
            if (FAILED(hr))
            {
                result.status = L"error";
                result.errors.push_back(L"CreateBuffer staging failed: " + HResultToString(hr));
                return finish();
            }

            auto dispatchStarted = std::chrono::steady_clock::now();
            ID3D11UnorderedAccessView* views[] = { uav.get() };
            UINT initialCounts[] = { 0 };
            m_d3dContext->CSSetShader(shader.get(), nullptr, 0);
            m_d3dContext->CSSetUnorderedAccessViews(0, 1, views, initialCounts);
            m_d3dContext->Dispatch(dispatchGroups, 1, 1);
            m_d3dContext->CopyResource(stagingBuffer.get(), outputBuffer.get());

            D3D11_MAPPED_SUBRESOURCE mapped{};
            hr = m_d3dContext->Map(stagingBuffer.get(), 0, D3D11_MAP_READ, 0, &mapped);
            uint64_t dispatchAndReadbackMs = ElapsedMs(dispatchStarted);
            if (FAILED(hr))
            {
                result.status = L"error";
                result.errors.push_back(L"Map staging failed: " + HResultToString(hr));
                return finish();
            }

            auto values = static_cast<uint32_t const*>(mapped.pData);
            uint64_t mismatches = 0;
            uint64_t checksum = 0;
            for (uint32_t i = 0; i < elementCount; ++i)
            {
                uint32_t expected = ExpectedIntComputeValue(i);
                if (values[i] != expected)
                {
                    ++mismatches;
                }
                checksum += values[i];
            }
            m_d3dContext->Unmap(stagingBuffer.get(), 0);

            ID3D11UnorderedAccessView* nullViews[] = { nullptr };
            m_d3dContext->CSSetUnorderedAccessViews(0, 1, nullViews, nullptr);
            m_d3dContext->CSSetShader(nullptr, nullptr, 0);

            result.AddString(L"shader_path", shaderPath);
            result.AddNumber(L"shader_bytes", shaderBytes.size());
            result.AddNumber(L"element_count", elementCount);
            result.AddNumber(L"dispatch_groups", dispatchGroups);
            result.AddNumber(L"dispatch_and_readback_ms", dispatchAndReadbackMs);
            result.AddNumber(L"mismatch_count", mismatches);
            result.AddNumber(L"checksum", checksum);
            result.AddBool(L"readback_verified", mismatches == 0);
            if (mismatches != 0)
            {
                result.status = L"fail";
            }
        }
        catch (std::exception const& ex)
        {
            result.status = L"error";
            std::wstring message(ex.what(), ex.what() + strlen(ex.what()));
            result.errors.push_back(message);
        }
        catch (hresult_error const& ex)
        {
            result.status = L"error";
            result.errors.push_back(ex.message().c_str());
        }

        return finish();
    }

    ProbeResult Direct3DApp::RunShaderRuntimeProbe() const
    {
        auto started = std::chrono::steady_clock::now();
        ProbeResult result;
        result.id = L"graphics.shader_runtime_compile";
        result.status = L"blocked";
        result.evidenceClass = L"MEASURED";
        result.startedUtc = UtcNow();
        result.apiCalls = {
            L"LoadPackagedLibrary(d3dcompiler_47.dll)",
            L"GetProcAddress(D3DCompile)",
            L"ID3D11Device::CreateComputeShader"
        };

        if (!m_d3dDevice || !m_d3dContext)
        {
            result.status = L"error";
            result.errors.push_back(L"D3D11 device/context unavailable.");
            result.endedUtc = UtcNow();
            result.durationMs = ElapsedMs(started);
            return result;
        }

        HMODULE compilerModule = LoadPackagedLibrary(D3DCOMPILER_DLL_W, 0);
        result.AddBool(L"d3dcompiler_load_succeeded", compilerModule != nullptr);
        if (!compilerModule)
        {
            result.errors.push_back(LastErrorToString());
            result.notes.push_back(L"Runtime HLSL compilation is optional. Precompiled CSO shader path remains valid.");
            result.endedUtc = UtcNow();
            result.durationMs = ElapsedMs(started);
            return result;
        }

        auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(compilerModule, "D3DCompile"));
        result.AddBool(L"d3dcompile_proc_found", compile != nullptr);
        if (!compile)
        {
            result.errors.push_back(LastErrorToString());
            result.endedUtc = UtcNow();
            result.durationMs = ElapsedMs(started);
            return result;
        }

        char const* source =
            "RWStructuredBuffer<uint> Output : register(u0);\n"
            "[numthreads(1, 1, 1)]\n"
            "void main(uint3 id : SV_DispatchThreadID) { Output[0] = 12345; }\n";

        winrt::com_ptr<ID3DBlob> bytecode;
        winrt::com_ptr<ID3DBlob> errors;
        auto compileStarted = std::chrono::steady_clock::now();
        HRESULT hr = compile(
            source,
            strlen(source),
            "runtime-minimal",
            nullptr,
            nullptr,
            "main",
            "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS,
            0,
            bytecode.put(),
            errors.put());
        result.AddNumber(L"compile_duration_ms", ElapsedMs(compileStarted));
        result.AddBool(L"d3dcompile_succeeded", SUCCEEDED(hr));
        if (errors)
        {
            char const* text = static_cast<char const*>(errors->GetBufferPointer());
            std::string errorText(text, text + errors->GetBufferSize());
            result.errors.push_back(std::wstring(errorText.begin(), errorText.end()));
        }
        if (FAILED(hr))
        {
            result.errors.push_back(L"D3DCompile failed: " + HResultToString(hr));
            result.endedUtc = UtcNow();
            result.durationMs = ElapsedMs(started);
            return result;
        }

        winrt::com_ptr<ID3D11ComputeShader> shader;
        hr = m_d3dDevice->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, shader.put());
        if (FAILED(hr))
        {
            result.errors.push_back(L"CreateComputeShader failed: " + HResultToString(hr));
            result.endedUtc = UtcNow();
            result.durationMs = ElapsedMs(started);
            return result;
        }

        D3D11_BUFFER_DESC outputDesc{};
        outputDesc.ByteWidth = sizeof(uint32_t);
        outputDesc.Usage = D3D11_USAGE_DEFAULT;
        outputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        outputDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        outputDesc.StructureByteStride = sizeof(uint32_t);

        winrt::com_ptr<ID3D11Buffer> outputBuffer;
        hr = m_d3dDevice->CreateBuffer(&outputDesc, nullptr, outputBuffer.put());
        if (FAILED(hr))
        {
            result.errors.push_back(L"CreateBuffer output failed: " + HResultToString(hr));
            result.endedUtc = UtcNow();
            result.durationMs = ElapsedMs(started);
            return result;
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = 1;

        winrt::com_ptr<ID3D11UnorderedAccessView> uav;
        hr = m_d3dDevice->CreateUnorderedAccessView(outputBuffer.get(), &uavDesc, uav.put());
        if (FAILED(hr))
        {
            result.errors.push_back(L"CreateUnorderedAccessView failed: " + HResultToString(hr));
            result.endedUtc = UtcNow();
            result.durationMs = ElapsedMs(started);
            return result;
        }

        D3D11_BUFFER_DESC stagingDesc{};
        stagingDesc.ByteWidth = sizeof(uint32_t);
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        winrt::com_ptr<ID3D11Buffer> stagingBuffer;
        hr = m_d3dDevice->CreateBuffer(&stagingDesc, nullptr, stagingBuffer.put());
        if (FAILED(hr))
        {
            result.errors.push_back(L"CreateBuffer staging failed: " + HResultToString(hr));
            result.endedUtc = UtcNow();
            result.durationMs = ElapsedMs(started);
            return result;
        }

        ID3D11UnorderedAccessView* views[] = { uav.get() };
        UINT initialCounts[] = { 0 };
        m_d3dContext->CSSetShader(shader.get(), nullptr, 0);
        m_d3dContext->CSSetUnorderedAccessViews(0, 1, views, initialCounts);
        m_d3dContext->Dispatch(1, 1, 1);
        m_d3dContext->CopyResource(stagingBuffer.get(), outputBuffer.get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = m_d3dContext->Map(stagingBuffer.get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr))
        {
            result.errors.push_back(L"Map staging failed: " + HResultToString(hr));
            result.endedUtc = UtcNow();
            result.durationMs = ElapsedMs(started);
            return result;
        }

        uint32_t value = *static_cast<uint32_t const*>(mapped.pData);
        m_d3dContext->Unmap(stagingBuffer.get(), 0);
        ID3D11UnorderedAccessView* nullViews[] = { nullptr };
        m_d3dContext->CSSetUnorderedAccessViews(0, 1, nullViews, nullptr);
        m_d3dContext->CSSetShader(nullptr, nullptr, 0);

        result.status = (value == 12345) ? L"pass" : L"fail";
        result.AddNumber(L"runtime_shader_bytecode_bytes", bytecode->GetBufferSize());
        result.AddNumber(L"expected_return_value", 12345);
        result.AddNumber(L"actual_return_value", value);
        result.AddBool(L"readback_verified", value == 12345);
        result.endedUtc = UtcNow();
        result.durationMs = ElapsedMs(started);
        return result;
    }
}
