#include "PCH.h"
#include "D3D11Compositor.h"
#include "VRCompositorHelper.h"

// Include D3D11 directly for full API access (REX::W32 only has subset)
#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")

namespace VRLoadingScreens
{
    // ========================================================================
    // HLSL shader source (compiled at runtime via d3dcompiler_47.dll)
    // ========================================================================

    static const char* VS_FULLSCREEN_SRC = R"(
struct VS_OUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};
VS_OUT main(uint id : SV_VertexID) {
    VS_OUT o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

    // Flat luminance-key shader:
    // Dark pixels in the game's eye texture are replaced with the background
    // texture sampled at the same UV. Both eyes get identical background content,
    // placing the image at optical infinity (no stereo disparity, no parallax).
    // This matches how the FO4VR title screen renders its background.
    static const char* PS_LUMINANCE_KEY_SRC = R"(
cbuffer Params : register(b0) {
    float threshold;
    float3 _pad;
};

Texture2D gameTex : register(t0);
Texture2D bgTex   : register(t1);
SamplerState samp : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 game = gameTex.Sample(samp, uv);
    float lum = dot(game.rgb, float3(0.299, 0.587, 0.114));
    if (lum > threshold)
        return game;

    float4 bg = bgTex.Sample(samp, uv);
    return float4(bg.rgb, 1.0);
}
)";

    static const char* PS_BACKGROUND_SRC = R"(
Texture2D bgTex     : register(t0);
SamplerState samp   : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return float4(bgTex.Sample(samp, uv).rgb, 1.0);
}
)";

    // Alpha-key shader: dark pixels become transparent, bright pixels (text) stay opaque.
    // Used to strip the black background from the captured loading screen frame.
    // Also masks out the central region where VR controller models appear in the capture.
    static const char* PS_ALPHA_KEY_SRC = R"(
Texture2D srcTex    : register(t0);
SamplerState samp   : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 color = srcTex.Sample(samp, uv);
    float lum = dot(color.rgb, float3(0.299, 0.587, 0.114));
    float alpha = smoothstep(0.02, 0.12, lum);

    return float4(color.rgb, alpha);
}
)";

    // D3DCompile function pointer (loaded dynamically)
    using D3DCompileFn = HRESULT(WINAPI*)(
        const void*, SIZE_T, const char*, const void*, void*,
        const char*, const char*, UINT, UINT, void**, void**);
    static D3DCompileFn s_D3DCompile = nullptr;

    // ========================================================================
    // OpenVR struct layouts (matches openvr.h)
    // ========================================================================
    struct VRTexture_t
    {
        void* handle;
        int eType;
        int eColorSpace;
    };

    struct VRTextureWithPose_t
    {
        void* handle;
        int eType;
        int eColorSpace;
        float mDeviceToAbsoluteTracking[3][4];
    };

    // ========================================================================
    // Initialization
    // ========================================================================

    bool D3D11Compositor::Initialize(void* vrCompositor, void* d3dDevice)
    {
        if (m_initialized) return true;
        if (!vrCompositor || !d3dDevice) return false;

        s_instance = this;
        m_device = d3dDevice;

        // Get immediate context
        auto* device = static_cast<ID3D11Device*>(m_device);
        ID3D11DeviceContext* ctx = nullptr;
        device->GetImmediateContext(&ctx);
        if (!ctx) {
            logger::error("D3D11Compositor: failed to get immediate context");
            return false;
        }
        m_context = ctx;

        // Compile shaders
        if (!CompileShaders()) {
            logger::error("D3D11Compositor: shader compilation failed");
            ctx->Release();
            m_context = nullptr;
            return false;
        }

        // Create sampler (bilinear, clamp)
        D3D11_SAMPLER_DESC sampDesc = {};
        sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        ID3D11SamplerState* sampler = nullptr;
        device->CreateSamplerState(&sampDesc, &sampler);
        m_sampler = sampler;

        // Create constant buffer for CompositeParams
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(CompositeParams);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Buffer* cb = nullptr;
        device->CreateBuffer(&cbDesc, nullptr, &cb);
        m_constantBuffer = cb;

        // Blend state (opaque — we're replacing the entire texture)
        D3D11_BLEND_DESC blendDesc = {};
        blendDesc.RenderTarget[0].BlendEnable = FALSE;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ID3D11BlendState* blend = nullptr;
        device->CreateBlendState(&blendDesc, &blend);
        m_blendState = blend;

        // Rasterizer state (no culling, solid fill)
        D3D11_RASTERIZER_DESC rasDesc = {};
        rasDesc.FillMode = D3D11_FILL_SOLID;
        rasDesc.CullMode = D3D11_CULL_NONE;
        ID3D11RasterizerState* ras = nullptr;
        device->CreateRasterizerState(&rasDesc, &ras);
        m_rasterState = ras;

        // Depth stencil state (disabled)
        D3D11_DEPTH_STENCIL_DESC dsDesc = {};
        dsDesc.DepthEnable = FALSE;
        dsDesc.StencilEnable = FALSE;
        ID3D11DepthStencilState* ds = nullptr;
        device->CreateDepthStencilState(&dsDesc, &ds);
        m_depthState = ds;

        // Hook IVRCompositor::Submit at vtable[5]
        void** vtable = *reinterpret_cast<void***>(vrCompositor);
        void* originalSubmitPtr = vtable[5];

        DWORD oldProtect;
        VirtualProtect(&vtable[5], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
        s_originalSubmit = reinterpret_cast<decltype(s_originalSubmit)>(originalSubmitPtr);
        vtable[5] = reinterpret_cast<void*>(&HookedSubmit);
        VirtualProtect(&vtable[5], sizeof(void*), oldProtect, &oldProtect);

        logger::info("D3D11Compositor: Submit hook installed at vtable[5]");

        // Hook ID3D11DeviceContext::ClearRenderTargetView at vtable[50]
        void** ctxVtable = *reinterpret_cast<void***>(ctx);
        void* originalClearPtr = ctxVtable[50];

        VirtualProtect(&ctxVtable[50], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
        s_originalClearRTV = reinterpret_cast<decltype(s_originalClearRTV)>(originalClearPtr);
        ctxVtable[50] = reinterpret_cast<void*>(&HookedClearRTV);
        VirtualProtect(&ctxVtable[50], sizeof(void*), oldProtect, &oldProtect);

        logger::info("D3D11Compositor: ClearRTV hook installed at vtable[50]");

        m_initialized = true;
        logger::info("D3D11Compositor: initialized (mode={})",
            m_mode == CompositeMode::LuminanceKey ? "LuminanceKey" : "ClearIntercept");
        return true;
    }

    // ========================================================================
    // Shader compilation (dynamic d3dcompiler_47.dll)
    // ========================================================================

    bool D3D11Compositor::CompileShaders()
    {
        if (!s_D3DCompile) {
            HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
            if (!compiler) {
                logger::error("D3D11Compositor: d3dcompiler_47.dll not found");
                return false;
            }
            s_D3DCompile = reinterpret_cast<D3DCompileFn>(
                GetProcAddress(compiler, "D3DCompile"));
            if (!s_D3DCompile) {
                logger::error("D3D11Compositor: D3DCompile not found");
                return false;
            }
        }

        auto* device = static_cast<ID3D11Device*>(m_device);
        ID3DBlob* blob = nullptr;
        ID3DBlob* errors = nullptr;
        HRESULT hr;

        // Vertex shader
        hr = s_D3DCompile(VS_FULLSCREEN_SRC, strlen(VS_FULLSCREEN_SRC), "vs_fullscreen",
            nullptr, nullptr, "main", "vs_5_0", 0, 0,
            reinterpret_cast<void**>(&blob), reinterpret_cast<void**>(&errors));
        if (FAILED(hr)) {
            if (errors) {
                logger::error("VS compile: {}", static_cast<const char*>(errors->GetBufferPointer()));
                errors->Release();
            }
            return false;
        }
        ID3D11VertexShader* vs = nullptr;
        device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs);
        blob->Release();
        if (errors) errors->Release();
        m_vsFullscreen = vs;

        // Luminance key pixel shader (flat composite)
        errors = nullptr;
        blob = nullptr;
        hr = s_D3DCompile(PS_LUMINANCE_KEY_SRC, strlen(PS_LUMINANCE_KEY_SRC), "ps_lumkey",
            nullptr, nullptr, "main", "ps_5_0", 0, 0,
            reinterpret_cast<void**>(&blob), reinterpret_cast<void**>(&errors));
        if (FAILED(hr)) {
            if (errors) {
                logger::error("PS lumkey compile: {}", static_cast<const char*>(errors->GetBufferPointer()));
                errors->Release();
            }
            return false;
        }
        ID3D11PixelShader* psLum = nullptr;
        device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &psLum);
        blob->Release();
        if (errors) errors->Release();
        m_psLuminanceKey = psLum;

        // Background-only pixel shader (for ClearRTV mode)
        errors = nullptr;
        blob = nullptr;
        hr = s_D3DCompile(PS_BACKGROUND_SRC, strlen(PS_BACKGROUND_SRC), "ps_bg",
            nullptr, nullptr, "main", "ps_5_0", 0, 0,
            reinterpret_cast<void**>(&blob), reinterpret_cast<void**>(&errors));
        if (FAILED(hr)) {
            if (errors) {
                logger::error("PS bg compile: {}", static_cast<const char*>(errors->GetBufferPointer()));
                errors->Release();
            }
            return false;
        }
        ID3D11PixelShader* psBg = nullptr;
        device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &psBg);
        blob->Release();
        if (errors) errors->Release();
        m_psBackground = psBg;

        // Alpha key pixel shader (black → transparent for captured frame)
        errors = nullptr;
        blob = nullptr;
        hr = s_D3DCompile(PS_ALPHA_KEY_SRC, strlen(PS_ALPHA_KEY_SRC), "ps_alphakey",
            nullptr, nullptr, "main", "ps_5_0", 0, 0,
            reinterpret_cast<void**>(&blob), reinterpret_cast<void**>(&errors));
        if (FAILED(hr)) {
            if (errors) {
                logger::error("PS alphakey compile: {}", static_cast<const char*>(errors->GetBufferPointer()));
                errors->Release();
            }
            return false;
        }
        ID3D11PixelShader* psAlpha = nullptr;
        device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &psAlpha);
        blob->Release();
        if (errors) errors->Release();
        m_psAlphaKey = psAlpha;

        logger::info("D3D11Compositor: all shaders compiled");
        return (m_vsFullscreen && m_psLuminanceKey && m_psBackground && m_psAlphaKey);
    }

    // ========================================================================
    // Background texture management
    // ========================================================================

    void D3D11Compositor::SetBackgroundTexture(void* d3dTexture)
    {
        // Release previous SRV
        if (m_bgSRV) {
            static_cast<ID3D11ShaderResourceView*>(m_bgSRV)->Release();
            m_bgSRV = nullptr;
        }

        m_bgTexture = d3dTexture;
        if (!d3dTexture || !m_device) return;

        auto* device = static_cast<ID3D11Device*>(m_device);
        auto* tex = static_cast<ID3D11Texture2D*>(d3dTexture);

        D3D11_TEXTURE2D_DESC texDesc;
        tex->GetDesc(&texDesc);

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        ID3D11ShaderResourceView* srv = nullptr;
        HRESULT hr = device->CreateShaderResourceView(tex, &srvDesc, &srv);
        if (SUCCEEDED(hr)) {
            m_bgSRV = srv;
            logger::info("D3D11Compositor: background SRV created ({}x{})",
                texDesc.Width, texDesc.Height);
        } else {
            logger::warn("D3D11Compositor: failed to create bg SRV (hr={:x})",
                static_cast<unsigned>(hr));
        }
    }

    void D3D11Compositor::SetEnabled(bool enabled)
    {
        if (enabled) {
            m_clearMatchCount = 0;
            m_submitCompositeCount = 0;

            m_compositeParams.threshold = 0.01f;
        } else {
            if (m_mode == CompositeMode::ClearIntercept) {
                logger::info("D3D11Compositor: ClearRTV matched {} times this load", m_clearMatchCount);
            } else {
                logger::info("D3D11Compositor: Submit composited {} times this load", m_submitCompositeCount);
            }
        }
        m_enabled = enabled;
        logger::info("D3D11Compositor: {}", enabled ? "enabled" : "disabled");
    }

    // ========================================================================
    // Temp texture management
    // ========================================================================

    void D3D11Compositor::EnsureTempTexture(unsigned int width, unsigned int height, unsigned int format)
    {
        if (m_tempTexture && m_tempWidth == width && m_tempHeight == height && m_tempFormat == format) return;

        // Release old
        if (m_tempSRV) {
            static_cast<ID3D11ShaderResourceView*>(m_tempSRV)->Release();
            m_tempSRV = nullptr;
        }
        if (m_tempTexture) {
            static_cast<ID3D11Texture2D*>(m_tempTexture)->Release();
            m_tempTexture = nullptr;
        }

        auto* device = static_cast<ID3D11Device*>(m_device);

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = static_cast<DXGI_FORMAT>(format);
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        ID3D11Texture2D* tex = nullptr;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &tex);
        if (FAILED(hr)) {
            logger::warn("D3D11Compositor: temp texture creation failed ({}x{} fmt={}, hr={:x})",
                width, height, format, static_cast<unsigned>(hr));
            return;
        }
        m_tempTexture = tex;
        m_tempWidth = width;
        m_tempHeight = height;
        m_tempFormat = format;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        ID3D11ShaderResourceView* srv = nullptr;
        device->CreateShaderResourceView(tex, &srvDesc, &srv);
        m_tempSRV = srv;

        logger::info("D3D11Compositor: temp texture created ({}x{} fmt={})", width, height, format);
    }

    // ========================================================================
    // Submit hook — intercepts VR eye texture submission (called once per eye)
    // Signature: int Submit(void* this, EVREye eye, const Texture_t* tex,
    //                       const VRTextureBounds_t* bounds, EVRSubmitFlags flags)
    // ========================================================================

    int __cdecl D3D11Compositor::HookedSubmit(void* compositor, int eye,
        const void* texture, const void* bounds, int flags)
    {
        auto* self = s_instance;
        if (self && texture) {
            auto* vrTex = static_cast<const VRTexture_t*>(texture);

            // Track eye textures (needed for ClearRTV mode)
            if (eye == 0)
                self->m_lastLeftEye = vrTex->handle;
            else
                self->m_lastRightEye = vrTex->handle;

            // Composite background into eye texture before Submit
            if (self->m_enabled && self->m_bgSRV &&
                !self->m_deferredNOPApplied.load() &&
                self->m_mode == CompositeMode::LuminanceKey)
            {
                self->m_submitCompositeCount++;
                if (self->m_submitCompositeCount <= 4) {
                    logger::info("Submit: compositing eye {} (count={})",
                        eye, self->m_submitCompositeCount);
                }
                self->CompositeFrame(vrTex->handle, eye);
            }
        }

        int result = s_originalSubmit(compositor, eye, texture, bounds, flags);

        // Deferred NOP: after right eye Submit returns, capture left eye,
        // show overlays, then apply NOP to break the animation loop.
        if (self && eye == 1 && self->m_deferredNOPPending.load()) {
            // Fade compositor scene to black
            VRCompositorHelper::FadeToColor(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false);

            // Capture and show tip+level overlay if enabled
            if (self->m_showCapturedOverlay) {
                auto* ctx = static_cast<ID3D11DeviceContext*>(self->m_context);
                if (ctx && self->m_lastLeftEye) {
                    auto* leftTex = static_cast<ID3D11Texture2D*>(self->m_lastLeftEye);

                    D3D11_TEXTURE2D_DESC desc;
                    leftTex->GetDesc(&desc);

                    unsigned int halfWidth = desc.Width / 2;
                    unsigned int cropHeight = static_cast<unsigned int>(desc.Height * 0.55f);

                    // Create/recreate frozen texture with cropped dimensions
                    if (self->m_frozenLeftTex) {
                        D3D11_TEXTURE2D_DESC oldDesc;
                        static_cast<ID3D11Texture2D*>(self->m_frozenLeftTex)->GetDesc(&oldDesc);
                        if (oldDesc.Width != halfWidth || oldDesc.Height != cropHeight) {
                            static_cast<ID3D11Texture2D*>(self->m_frozenLeftTex)->Release();
                            self->m_frozenLeftTex = nullptr;
                        }
                    }
                    if (!self->m_frozenLeftTex) {
                        auto* device = static_cast<ID3D11Device*>(self->m_device);
                        D3D11_TEXTURE2D_DESC fd = {};
                        fd.Width = halfWidth;
                        fd.Height = cropHeight;
                        fd.MipLevels = 1;
                        fd.ArraySize = 1;
                        fd.Format = desc.Format;
                        fd.SampleDesc.Count = 1;
                        fd.Usage = D3D11_USAGE_DEFAULT;
                        fd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                        ID3D11Texture2D* newTex = nullptr;
                        HRESULT hr = device->CreateTexture2D(&fd, nullptr, &newTex);
                        if (SUCCEEDED(hr)) {
                            self->m_frozenLeftTex = newTex;
                        } else {
                            logger::error("Failed to create frozen texture (hr={:x})", (unsigned)hr);
                        }
                    }

                    if (self->m_frozenLeftTex) {
                        // GPU-crop: copy left half, top 55% only (excludes controllers)
                        D3D11_BOX srcBox = {};
                        srcBox.left = 0;
                        srcBox.top = 0;
                        srcBox.front = 0;
                        srcBox.right = halfWidth;
                        srcBox.bottom = cropHeight;
                        srcBox.back = 1;
                        ctx->CopySubresourceRegion(
                            static_cast<ID3D11Texture2D*>(self->m_frozenLeftTex), 0, 0, 0, 0,
                            leftTex, 0, &srcBox);
                        ctx->Flush();

                        logger::info("Left eye captured ({}x{} -> {}x{} cropped, fmt={})",
                            desc.Width, desc.Height, halfWidth, cropHeight, (int)desc.Format);

                        // Process and show captured frame
                        self->ProcessCapturedFrame();
                        void* overlayTex = self->m_processedTex ? self->m_processedTex : self->m_frozenLeftTex;
                        VRCompositorHelper::ShowCapturedFrameOverlay(overlayTex, 1);
                        logger::info("Captured overlay shown (alpha-keyed={})", self->m_processedTex != nullptr);
                    }
                }
            }

            // Apply the NOP
            std::memcpy(reinterpret_cast<void*>(self->m_deferredNOPAddress),
                self->m_deferredNOPBytes, self->m_deferredNOPSize);
            self->m_deferredNOPPending.store(false);
            self->m_deferredNOPApplied.store(true);
            logger::info("Deferred NOP applied at {:x} after right eye Submit",
                self->m_deferredNOPAddress);
        }

        return result;
    }

    void D3D11Compositor::RequestDeferredNOP(std::uintptr_t address, const std::uint8_t* bytes, std::size_t size)
    {
        if (size > sizeof(m_deferredNOPBytes)) {
            logger::error("D3D11Compositor: deferred NOP size {} exceeds buffer", size);
            return;
        }
        m_deferredNOPAddress = address;
        std::memcpy(m_deferredNOPBytes, bytes, size);
        m_deferredNOPSize = size;
        m_deferredNOPApplied.store(false);
        m_deferredNOPPending.store(true);
        logger::info("Deferred NOP requested at {:x} ({} bytes)", address, size);
    }

    void D3D11Compositor::ProcessPendingCapture()
    {
        if (!m_pendingCaptureProcess.load()) return;
        m_pendingCaptureProcess.store(false);

        // Fade compositor scene to black (hides stereo frames behind overlays)
        VRCompositorHelper::FadeToColor(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false);

        // Process and show captured tip+level overlay if enabled
        if (m_showCapturedOverlay && m_frozenLeftTex) {
            ProcessCapturedFrame();

            void* overlayTex = m_processedTex ? m_processedTex : m_frozenLeftTex;
            if (overlayTex) {
                VRCompositorHelper::ShowCapturedFrameOverlay(overlayTex, 1);
                logger::info("Captured overlay shown (alpha-keyed={}, deferred from Submit)", m_processedTex != nullptr);
            }
        }
    }

    void D3D11Compositor::ResetDeferredState()
    {
        m_frozen.store(false);
        m_deferredNOPPending.store(false);
        m_deferredNOPApplied.store(false);
        // Note: captured frame overlay is NOT hidden here — it's hidden together
        // with the background overlay in the delayed hide path for synchronised removal.

        // Release processed texture so it gets recreated with correct dimensions
        if (m_processedRTV) {
            static_cast<ID3D11RenderTargetView*>(m_processedRTV)->Release();
            m_processedRTV = nullptr;
        }
        if (m_processedTex) {
            static_cast<ID3D11Texture2D*>(m_processedTex)->Release();
            m_processedTex = nullptr;
        }
        if (m_frozenSRV) {
            static_cast<ID3D11ShaderResourceView*>(m_frozenSRV)->Release();
            m_frozenSRV = nullptr;
        }

        logger::info("D3D11Compositor: deferred state reset (unfrozen)");
    }

    // ========================================================================
    // Alpha-key processing: strip black background from captured loading screen
    // ========================================================================

    void D3D11Compositor::ProcessCapturedFrame()
    {
        if (!m_frozenLeftTex || !m_device || !m_context || !m_psAlphaKey) return;

        auto* device = static_cast<ID3D11Device*>(m_device);
        auto* ctx = static_cast<ID3D11DeviceContext*>(m_context);
        auto* frozenTex = static_cast<ID3D11Texture2D*>(m_frozenLeftTex);

        D3D11_TEXTURE2D_DESC srcDesc;
        frozenTex->GetDesc(&srcDesc);

        // Create SRV for frozen texture input
        if (m_frozenSRV) {
            static_cast<ID3D11ShaderResourceView*>(m_frozenSRV)->Release();
            m_frozenSRV = nullptr;
        }

        DXGI_FORMAT srvFormat = srcDesc.Format;
        // Handle TYPELESS formats (common for VR eye textures)
        if (srvFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS)
            srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        else if (srvFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS)
            srvFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = srvFormat;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        ID3D11ShaderResourceView* frozenSRV = nullptr;
        HRESULT hr = device->CreateShaderResourceView(frozenTex, &srvDesc, &frozenSRV);
        if (FAILED(hr)) {
            logger::warn("ProcessCapturedFrame: failed to create frozen SRV (hr={:x}, fmt={})",
                (unsigned)hr, (int)srcDesc.Format);
            return;
        }
        m_frozenSRV = frozenSRV;

        // Create/recreate processed output texture if size changed
        bool needRecreate = !m_processedTex;
        if (m_processedTex) {
            D3D11_TEXTURE2D_DESC procDesc;
            static_cast<ID3D11Texture2D*>(m_processedTex)->GetDesc(&procDesc);
            if (procDesc.Width != srcDesc.Width || procDesc.Height != srcDesc.Height)
                needRecreate = true;
        }

        if (needRecreate) {
            if (m_processedRTV) {
                static_cast<ID3D11RenderTargetView*>(m_processedRTV)->Release();
                m_processedRTV = nullptr;
            }
            if (m_processedTex) {
                static_cast<ID3D11Texture2D*>(m_processedTex)->Release();
                m_processedTex = nullptr;
            }

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = srcDesc.Width;
            desc.Height = srcDesc.Height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            ID3D11Texture2D* procTex = nullptr;
            hr = device->CreateTexture2D(&desc, nullptr, &procTex);
            if (FAILED(hr)) {
                logger::warn("ProcessCapturedFrame: failed to create processed texture (hr={:x})", (unsigned)hr);
                return;
            }
            m_processedTex = procTex;

            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
            rtvDesc.Format = desc.Format;
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

            ID3D11RenderTargetView* rtv = nullptr;
            hr = device->CreateRenderTargetView(procTex, &rtvDesc, &rtv);
            if (FAILED(hr)) {
                logger::warn("ProcessCapturedFrame: failed to create processed RTV (hr={:x})", (unsigned)hr);
                return;
            }
            m_processedRTV = rtv;

            logger::info("ProcessCapturedFrame: created {}x{} processed texture", srcDesc.Width, srcDesc.Height);
        }

        // Save current pipeline state
        ID3D11RenderTargetView* oldRTV = nullptr;
        ID3D11DepthStencilView* oldDSV = nullptr;
        D3D11_VIEWPORT oldVP = {};
        UINT numVP = 1;
        ctx->OMGetRenderTargets(1, &oldRTV, &oldDSV);
        ctx->RSGetViewports(&numVP, &oldVP);

        // Clear processed texture to fully transparent
        auto* rtv = static_cast<ID3D11RenderTargetView*>(m_processedRTV);
        float clearColor[4] = { 0, 0, 0, 0 };
        s_originalClearRTV(ctx, rtv, clearColor);

        // Render frozen frame through alpha key shader
        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(srcDesc.Width);
        vp.Height = static_cast<float>(srcDesc.Height);
        vp.MaxDepth = 1.0f;

        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);

        ctx->VSSetShader(static_cast<ID3D11VertexShader*>(m_vsFullscreen), nullptr, 0);
        ctx->PSSetShader(static_cast<ID3D11PixelShader*>(m_psAlphaKey), nullptr, 0);

        ctx->PSSetShaderResources(0, 1, &frozenSRV);
        auto* samp = static_cast<ID3D11SamplerState*>(m_sampler);
        ctx->PSSetSamplers(0, 1, &samp);

        ctx->OMSetBlendState(static_cast<ID3D11BlendState*>(m_blendState), nullptr, 0xFFFFFFFF);
        ctx->RSSetState(static_cast<ID3D11RasterizerState*>(m_rasterState));
        ctx->OMSetDepthStencilState(static_cast<ID3D11DepthStencilState*>(m_depthState), 0);

        ctx->Draw(3, 0);

        // Unbind SRV
        ID3D11ShaderResourceView* nullSRV = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSRV);

        // Restore
        ctx->OMSetRenderTargets(1, &oldRTV, oldDSV);
        ctx->RSSetViewports(1, &oldVP);
        if (oldRTV) oldRTV->Release();
        if (oldDSV) oldDSV->Release();

        logger::info("ProcessCapturedFrame: alpha key pass complete ({}x{})", srcDesc.Width, srcDesc.Height);
    }

    // ========================================================================
    // Clear eye texture to solid black (after deferred NOP, hides controllers)
    // ========================================================================

    void D3D11Compositor::ClearEyeToBlack(void* eyeTexture2D)
    {
        if (!m_context || !m_device || !eyeTexture2D) return;

        auto* device = static_cast<ID3D11Device*>(m_device);
        auto* ctx = static_cast<ID3D11DeviceContext*>(m_context);
        auto* eyeTex = static_cast<ID3D11Texture2D*>(eyeTexture2D);

        D3D11_TEXTURE2D_DESC desc;
        eyeTex->GetDesc(&desc);

        // Resolve TYPELESS formats for RTV creation
        DXGI_FORMAT rtvFormat = desc.Format;
        if (rtvFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS)
            rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        else if (rtvFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS)
            rtvFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = rtvFormat;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

        ID3D11RenderTargetView* rtv = nullptr;
        HRESULT hr = device->CreateRenderTargetView(eyeTex, &rtvDesc, &rtv);
        if (FAILED(hr) || !rtv) return;

        float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        s_originalClearRTV(ctx, rtv, black);

        rtv->Release();
    }

    // ========================================================================
    // Flat luminance-key compositing (same image to both eyes)
    // ========================================================================

    void D3D11Compositor::CompositeFrame(void* eyeTexture2D, int eye)
    {
        if (!m_context || !eyeTexture2D || !m_bgSRV) return;

        auto* ctx = static_cast<ID3D11DeviceContext*>(m_context);
        auto* device = static_cast<ID3D11Device*>(m_device);
        auto* eyeTex = static_cast<ID3D11Texture2D*>(eyeTexture2D);

        // Get eye texture dimensions and flags
        D3D11_TEXTURE2D_DESC eyeDesc;
        eyeTex->GetDesc(&eyeDesc);

        // Create RTV on the eye texture
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = eyeDesc.Format;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView* rtv = nullptr;
        HRESULT hr = device->CreateRenderTargetView(eyeTex, &rtvDesc, &rtv);
        if (FAILED(hr) || !rtv) return;

        // Save current pipeline state
        ID3D11RenderTargetView* oldRTV = nullptr;
        ID3D11DepthStencilView* oldDSV = nullptr;
        D3D11_VIEWPORT oldVP = {};
        UINT numVP = 1;
        ID3D11VertexShader* oldVS = nullptr;
        ID3D11PixelShader* oldPS = nullptr;
        ID3D11BlendState* oldBlend = nullptr;
        float oldBlendFactor[4] = {};
        UINT oldSampleMask = 0;
        ID3D11RasterizerState* oldRas = nullptr;
        ID3D11DepthStencilState* oldDS = nullptr;
        UINT oldStencilRef = 0;

        ctx->OMGetRenderTargets(1, &oldRTV, &oldDSV);
        ctx->RSGetViewports(&numVP, &oldVP);
        ctx->VSGetShader(&oldVS, nullptr, nullptr);
        ctx->PSGetShader(&oldPS, nullptr, nullptr);
        ctx->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
        ctx->RSGetState(&oldRas);
        ctx->OMGetDepthStencilState(&oldDS, &oldStencilRef);

        // Fullscreen blit of background image (mono — identical both eyes)
        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(eyeDesc.Width);
        vp.Height = static_cast<float>(eyeDesc.Height);
        vp.MaxDepth = 1.0f;

        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);

        ctx->VSSetShader(static_cast<ID3D11VertexShader*>(m_vsFullscreen), nullptr, 0);
        ctx->PSSetShader(static_cast<ID3D11PixelShader*>(m_psBackground), nullptr, 0);

        auto* bgSRV = static_cast<ID3D11ShaderResourceView*>(m_bgSRV);
        ctx->PSSetShaderResources(0, 1, &bgSRV);

        auto* samp = static_cast<ID3D11SamplerState*>(m_sampler);
        ctx->PSSetSamplers(0, 1, &samp);

        ctx->OMSetBlendState(static_cast<ID3D11BlendState*>(m_blendState), nullptr, 0xFFFFFFFF);
        ctx->RSSetState(static_cast<ID3D11RasterizerState*>(m_rasterState));
        ctx->OMSetDepthStencilState(static_cast<ID3D11DepthStencilState*>(m_depthState), 0);

        ctx->Draw(3, 0);

        // Unbind SRV to avoid hazards
        ID3D11ShaderResourceView* nullSRV = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSRV);

        // Restore previous state
        ctx->OMSetRenderTargets(1, &oldRTV, oldDSV);
        ctx->RSSetViewports(1, &oldVP);
        ctx->VSSetShader(oldVS, nullptr, 0);
        ctx->PSSetShader(oldPS, nullptr, 0);
        ctx->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
        ctx->RSSetState(oldRas);
        ctx->OMSetDepthStencilState(oldDS, oldStencilRef);

        if (oldRTV) oldRTV->Release();
        if (oldDSV) oldDSV->Release();
        if (oldVS) oldVS->Release();
        if (oldPS) oldPS->Release();
        if (oldBlend) oldBlend->Release();
        if (oldRas) oldRas->Release();
        if (oldDS) oldDS->Release();

        rtv->Release();
    }

    // ========================================================================
    // ClearRTV hook — intercepts render target clears to inject background
    // ========================================================================

    void __stdcall D3D11Compositor::HookedClearRTV(void* context, void* rtv,
        const float color[4])
    {
        // Always call original clear first
        s_originalClearRTV(context, rtv, color);

        auto* self = s_instance;
        if (!self || !self->m_enabled || !self->m_bgSRV) return;
        if (self->m_mode != CompositeMode::ClearIntercept) return;

        // Check if this RTV belongs to one of the eye textures
        auto* d3dRTV = static_cast<ID3D11RenderTargetView*>(rtv);
        ID3D11Resource* resource = nullptr;
        d3dRTV->GetResource(&resource);
        if (!resource) return;

        bool isEyeTexture = (resource == self->m_lastLeftEye || resource == self->m_lastRightEye);
        resource->Release();

        if (isEyeTexture) {
            self->m_clearMatchCount++;
            if (self->m_clearMatchCount <= 4) {
                logger::info("ClearRTV: matched eye texture (count={})", self->m_clearMatchCount);
            }
            self->DrawBackgroundOnRTV(context, rtv);
        }
    }

    void D3D11Compositor::DrawBackgroundOnRTV(void* context, void* rtv)
    {
        if (!m_bgSRV || !m_vsFullscreen || !m_psBackground) return;

        auto* ctx = static_cast<ID3D11DeviceContext*>(context);
        auto* d3dRTV = static_cast<ID3D11RenderTargetView*>(rtv);

        // Get dimensions from RTV's resource
        ID3D11Resource* resource = nullptr;
        d3dRTV->GetResource(&resource);
        if (!resource) return;

        ID3D11Texture2D* tex = nullptr;
        resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
        resource->Release();
        if (!tex) return;

        D3D11_TEXTURE2D_DESC desc;
        tex->GetDesc(&desc);
        tex->Release();

        // Save state
        ID3D11RenderTargetView* oldRTV = nullptr;
        ID3D11DepthStencilView* oldDSV = nullptr;
        ctx->OMGetRenderTargets(1, &oldRTV, &oldDSV);

        // Set pipeline for background draw
        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(desc.Width);
        vp.Height = static_cast<float>(desc.Height);
        vp.MaxDepth = 1.0f;

        ctx->OMSetRenderTargets(1, &d3dRTV, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);

        ctx->VSSetShader(static_cast<ID3D11VertexShader*>(m_vsFullscreen), nullptr, 0);
        ctx->PSSetShader(static_cast<ID3D11PixelShader*>(m_psBackground), nullptr, 0);

        auto* bgSRV = static_cast<ID3D11ShaderResourceView*>(m_bgSRV);
        ctx->PSSetShaderResources(0, 1, &bgSRV);

        auto* samp = static_cast<ID3D11SamplerState*>(m_sampler);
        ctx->PSSetSamplers(0, 1, &samp);

        ctx->OMSetBlendState(static_cast<ID3D11BlendState*>(m_blendState), nullptr, 0xFFFFFFFF);
        ctx->RSSetState(static_cast<ID3D11RasterizerState*>(m_rasterState));
        ctx->OMSetDepthStencilState(static_cast<ID3D11DepthStencilState*>(m_depthState), 0);

        // Draw fullscreen triangle
        ctx->Draw(3, 0);

        // Unbind SRV
        ID3D11ShaderResourceView* nullSRV = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSRV);

        // Restore
        ctx->OMSetRenderTargets(1, &oldRTV, oldDSV);
        if (oldRTV) oldRTV->Release();
        if (oldDSV) oldDSV->Release();
    }
}
