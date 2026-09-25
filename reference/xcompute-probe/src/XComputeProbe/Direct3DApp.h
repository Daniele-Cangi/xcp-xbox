#pragma once

#include "pch.h"
#include "ProbeResult.h"
#include "runtime/WorkerCreativeForegroundRuntime.h"

namespace XComputeProbe
{
    class Direct3DApp
    {
    public:
        void SetWindow(winrt::Windows::UI::Core::CoreWindow const& window);
        void SetStatusLines(std::vector<std::wstring> lines);
        void SetCreativeSnapshot(
            std::shared_ptr<WorkerCreativeForegroundSnapshot const> snapshot);
        void SetCreativeFrameCaptureRequest(
            WorkerCreativeFrameCaptureRequest request);
        std::optional<WorkerCreativeFrameCaptureResult>
            TakeCreativeFrameCaptureResult();
        void Render();
        ProbeResult MakeGraphicsProbe() const;
        std::vector<ProbeResult> MakeGraphicsProbes() const;

    private:
        void CreateDeviceResources();
        void CreateWindowSizeResources();
        void CreateCreative3DResources();
        void CreateCreativeDepthResources();
        void RenderCreative3D(
            WorkerCreativeForegroundSnapshot const& snapshot,
            ID3D11RenderTargetView* renderTarget);
        void DrawCreative2D(
            WorkerCreativeForegroundSnapshot const& snapshot);
        void DrawCreativeWorld2D(
            WorkerCreativeForegroundSnapshot const& snapshot);
        void DrawCreativeUi(
            WorkerCreativeForegroundSnapshot const& snapshot);
        void DrawCreativeOverlay(
            WorkerCreativeForegroundSnapshot const& snapshot);
        ID2D1Bitmap1* CreativeSpriteBitmap(
            WorkerCreativeAsset const& asset);
        WorkerCreativeFrameCaptureResult CaptureCreativeFrame(
            ID3D11Texture2D* backBuffer,
            WorkerCreativeFrameCaptureRequest const& request);
        void DrawCreativeText(
            std::wstring const& text,
            float size,
            D2D1_RECT_F const& rect,
            WorkerCreativeColor color,
            DWRITE_FONT_WEIGHT weight =
                DWRITE_FONT_WEIGHT_NORMAL);
        std::wstring FeatureLevelString() const;
        std::wstring AdapterDescription() const;
        ProbeResult RunD3D11ComputeProbe() const;
        ProbeResult RunShaderRuntimeProbe() const;

        winrt::Windows::UI::Core::CoreWindow m_window{ nullptr };
        winrt::com_ptr<ID3D11Device1> m_d3dDevice;
        winrt::com_ptr<ID3D11DeviceContext1> m_d3dContext;
        winrt::com_ptr<IDXGISwapChain1> m_swapChain;
        winrt::com_ptr<ID2D1Factory1> m_d2dFactory;
        winrt::com_ptr<ID2D1Device> m_d2dDevice;
        winrt::com_ptr<ID2D1DeviceContext> m_d2dContext;
        winrt::com_ptr<ID2D1Bitmap1> m_d2dTargetBitmap;
        winrt::com_ptr<IDWriteFactory> m_dwriteFactory;
        winrt::com_ptr<IDWriteTextFormat> m_textFormat;
        winrt::com_ptr<ID2D1SolidColorBrush> m_textBrush;
        std::map<std::wstring, winrt::com_ptr<ID2D1Bitmap1>>
            m_creativeSpriteBitmaps;
        winrt::com_ptr<ID3D11VertexShader> m_creativeVertexShader;
        winrt::com_ptr<ID3D11PixelShader> m_creativePixelShader;
        winrt::com_ptr<ID3D11InputLayout> m_creativeInputLayout;
        winrt::com_ptr<ID3D11Buffer> m_creativeVertexBuffer;
        winrt::com_ptr<ID3D11Texture2D> m_creativeDepthTexture;
        winrt::com_ptr<ID3D11DepthStencilView> m_creativeDepthView;
        winrt::com_ptr<ID3D11DepthStencilState> m_creativeDepthState;
        winrt::com_ptr<ID3D11RasterizerState> m_creativeRasterizer;
        D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_9_1;
        uint32_t m_width = 1280;
        uint32_t m_height = 720;
        mutable std::mutex m_linesMutex;
        std::vector<std::wstring> m_statusLines;
        std::shared_ptr<WorkerCreativeForegroundSnapshot const>
            m_creativeSnapshot;
        std::optional<WorkerCreativeFrameCaptureRequest>
            m_creativeCaptureRequest;
        std::optional<WorkerCreativeFrameCaptureResult>
            m_creativeCaptureResult;
    };
}
