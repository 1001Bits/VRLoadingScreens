#pragma once

#include <atomic>

namespace VRLoadingScreens
{
    enum class CompositeMode
    {
        LuminanceKey = 0,  // Hook Submit, composite bg behind game content
        ClearIntercept = 1 // Hook ClearRTV, draw bg before game renders
    };

    class D3D11Compositor
    {
    public:
        static D3D11Compositor& GetSingleton()
        {
            static D3D11Compositor instance;
            return instance;
        }

        // Initialize with IVRCompositor and D3D11 device
        bool Initialize(void* vrCompositor, void* d3dDevice);

        // Set the background texture (ID3D11Texture2D*)
        void SetBackgroundTexture(void* d3dTexture);

        // Enable/disable compositing (call on loading menu open/close)
        void SetEnabled(bool enabled);
        bool IsEnabled() const { return m_enabled; }
        bool IsInitialized() const { return m_initialized; }

        // Compositing mode
        void SetMode(CompositeMode mode) { m_mode = mode; }
        CompositeMode GetMode() const { return m_mode; }

        // Whether to capture and show the tip+level overlay
        void SetShowCapturedOverlay(bool show) { m_showCapturedOverlay = show; }

        // Deferred NOP: applied inside Submit hook after right eye completes
        // (guarantees both eyes have valid frames before the loop breaks)
        void RequestDeferredNOP(std::uintptr_t address, const std::uint8_t* bytes, std::size_t size);
        bool IsDeferredNOPApplied() const { return m_deferredNOPApplied.load(); }
        void ResetDeferredState();

        // Called from Update() to process captured frame outside the Submit hook
        // (avoids Nvidia driver crashes from D3D11/OpenVR calls during Submit)
        void ProcessPendingCapture();


    private:
        D3D11Compositor() = default;

        // Shader compilation
        bool CompileShaders();

        // Submit hook (mode: LuminanceKey) — called once per eye
        static int __cdecl HookedSubmit(void* compositor, int eye,
            const void* texture, const void* bounds, int flags);
        void CompositeFrame(void* eyeTexture2D, int eye);

        // ClearRTV hook (mode: ClearIntercept)
        static void __stdcall HookedClearRTV(void* context, void* rtv,
            const float color[4]);
        void DrawBackgroundOnRTV(void* context, void* rtv);

        // Process captured frame: render through alpha key shader (black → transparent)
        void ProcessCapturedFrame();

        // Clear eye texture to solid black (hides controllers after deferred NOP)
        void ClearEyeToBlack(void* eyeTexture2D);

        // Ensure temp texture matches eye texture size and format
        void EnsureTempTexture(unsigned int width, unsigned int height, unsigned int format);

        // State
        bool m_initialized = false;
        bool m_enabled = false;
        bool m_showCapturedOverlay = true;
        CompositeMode m_mode = CompositeMode::LuminanceKey;

        // D3D11 objects (stored as void* to avoid d3d11.h in header)
        void* m_device = nullptr;
        void* m_context = nullptr;

        // Shaders
        void* m_vsFullscreen = nullptr;    // ID3D11VertexShader*
        void* m_psLuminanceKey = nullptr;  // ID3D11PixelShader*
        void* m_psBackground = nullptr;    // ID3D11PixelShader*
        void* m_psAlphaKey = nullptr;      // ID3D11PixelShader*

        // Resources
        void* m_sampler = nullptr;         // ID3D11SamplerState*
        void* m_constantBuffer = nullptr;  // ID3D11Buffer*
        void* m_blendState = nullptr;      // ID3D11BlendState*
        void* m_rasterState = nullptr;     // ID3D11RasterizerState*
        void* m_depthState = nullptr;      // ID3D11DepthStencilState*

        // Background texture SRV
        void* m_bgTexture = nullptr;       // ID3D11Texture2D* (not owned)
        void* m_bgSRV = nullptr;           // ID3D11ShaderResourceView*

        // Temp texture for compositing (copy game texture here as input)
        void* m_tempTexture = nullptr;     // ID3D11Texture2D*
        void* m_tempSRV = nullptr;         // ID3D11ShaderResourceView*
        unsigned int m_tempWidth = 0;
        unsigned int m_tempHeight = 0;
        unsigned int m_tempFormat = 0;

        // Eye texture tracking from Submit
        void* m_lastLeftEye = nullptr;
        void* m_lastRightEye = nullptr;

        int m_clearMatchCount = 0;
        int m_submitCompositeCount = 0;

        // Deferred NOP — applied inside Submit hook after right eye completes
        std::atomic<bool> m_deferredNOPPending{ false };
        std::atomic<bool> m_deferredNOPApplied{ false };
        std::uintptr_t m_deferredNOPAddress = 0;
        std::uint8_t m_deferredNOPBytes[16] = {};
        std::size_t m_deferredNOPSize = 0;

        // Frozen eye textures — snapshot taken when deferred NOP fires,
        // submitted in place of live textures so post-NOP code can't corrupt them
        std::atomic<bool> m_frozen{ false };
        std::atomic<bool> m_pendingCaptureProcess{ false };  // Set in Submit, processed in Update
        void* m_frozenLeftTex = nullptr;   // ID3D11Texture2D*
        void* m_frozenRightTex = nullptr;  // ID3D11Texture2D*

        // Alpha-keyed captured frame (black background → transparent)
        void* m_processedTex = nullptr;    // ID3D11Texture2D* (RGBA with alpha)
        void* m_processedRTV = nullptr;    // ID3D11RenderTargetView*
        void* m_frozenSRV = nullptr;       // ID3D11ShaderResourceView*

        // GPU constant buffer layout (must match HLSL cbuffer)
        struct CompositeParams
        {
            float threshold;
            float pad[3];
        };
        CompositeParams m_compositeParams = {};

        // Original function pointers
        static inline decltype(&HookedSubmit) s_originalSubmit = nullptr;
        static inline decltype(&HookedClearRTV) s_originalClearRTV = nullptr;
        static inline D3D11Compositor* s_instance = nullptr;
    };
}
