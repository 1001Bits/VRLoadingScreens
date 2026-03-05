#include "PCH.h"
#include "LoadingScreenManager.h"
#include "VRCompositorHelper.h"
#include "D3D11Compositor.h"

namespace VRLoadingScreens
{
    // Loading loop exit: CMP [RSI+0x68],2 (4 bytes) + JZ back-to-top (6 bytes) = 10 bytes
    static constexpr std::uintptr_t AnimationLoop_CMP_Offset = 0xd07573;

    static constexpr std::uint8_t NOP10[] = {
        0x0F, 0x1F, 0x44, 0x00, 0x00,
        0x0F, 0x1F, 0x44, 0x00, 0x00
    };

    void LoadingScreenManager::Init(bool enableBackgrounds, float renderDelay, int overlayMode,
                                     float overlayAlpha)
    {
        m_backgroundsEnabled = enableBackgrounds;
        m_renderDelaySeconds = renderDelay;
        m_overlayMode = overlayMode;
        m_overlayAlpha = overlayAlpha;

        ScanForTextures();

        if (m_texturePaths.empty()) {
            logger::warn("No loading screen DDS textures found in Data/Textures/LoadingScreens/");
        } else {
            logger::info("Found {} loading screen textures", m_texturePaths.size());
        }

        // Save original CMP+JZ bytes and pre-unprotect the page
        REL::Relocation<std::uintptr_t> animCmp{ REL::Offset(AnimationLoop_CMP_Offset) };
        m_loopAddress = animCmp.address();
        std::memcpy(m_originalLoopBytes, reinterpret_cast<void*>(m_loopAddress), 10);
        m_originalBytesSaved = true;

        DWORD oldProtect;
        VirtualProtect(reinterpret_cast<void*>(m_loopAddress), 10, PAGE_EXECUTE_READWRITE, &oldProtect);

        logger::info("Saved loop bytes at {:x}, page unprotected, render delay={:.1f}s",
            m_loopAddress, m_renderDelaySeconds);

        // Initialize VR compositor + overlay
        VRCompositorHelper::Initialize();
        VRCompositorHelper::InitializeOverlay();

        if (m_backgroundsEnabled) {
            PrepareNextBackground();
        }
    }

    void LoadingScreenManager::ScanForTextures()
    {
        m_texturePaths.clear();
        std::filesystem::path searchDir = "Data/Textures/LoadingScreens";
        if (!std::filesystem::exists(searchDir)) return;

        for (const auto& entry : std::filesystem::directory_iterator(searchDir)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".dds") {
                m_texturePaths.push_back("LoadingScreens/" + entry.path().filename().string());
            }
        }
        std::sort(m_texturePaths.begin(), m_texturePaths.end());
    }

    std::string LoadingScreenManager::PickRandomTexture()
    {
        if (m_texturePaths.empty()) return "";
        if (m_texturePaths.size() == 1) return m_texturePaths[0];

        int idx;
        do {
            std::uniform_int_distribution<int> dist(0, static_cast<int>(m_texturePaths.size()) - 1);
            idx = dist(m_rng);
        } while (idx == m_lastIndex && m_texturePaths.size() > 1);

        m_lastIndex = idx;
        return m_texturePaths[idx];
    }

    void LoadingScreenManager::PrepareNextBackground()
    {
        if (m_texturePaths.empty()) return;

        // Release previous D3D11 texture
        if (m_currentBgTexture) {
            VRCompositorHelper::ReleaseTexture(m_currentBgTexture);
            m_currentBgTexture = nullptr;
        }

        m_currentTexturePath = PickRandomTexture();
        logger::info("Next background: {}", m_currentTexturePath);

        // Load DDS as D3D11 texture for skybox
        if (m_backgroundsEnabled) {
            std::string fullPath = "Data/Textures/" + m_currentTexturePath;
            m_currentBgTexture = VRCompositorHelper::LoadDDSTexture(fullPath);
            if (m_currentBgTexture) {
                logger::info("Loaded background texture: {}", fullPath);
            }
        }

    }

    void LoadingScreenManager::OnLoadingMenuOpen()
    {
        auto now = std::chrono::steady_clock::now();
        auto sinceLastClose = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastCloseTime).count();
        m_loadStartTime = now;
        m_inLoadingScreen = true;
        m_loopNOPApplied = false;
        m_loadCount++;

        logger::info("LoadingMenu opened (load #{}, mode={}, timingOnly={}, sinceLastClose={}ms)",
            m_loadCount, m_overlayMode, m_timingOnly, sinceLastClose);

        if (m_timingOnly) return;

        m_sinceLastClose = sinceLastClose;

        // After render delay: SuspendRendering → NOP animation loop → show overlays.
        // SuspendRendering tells the compositor to stop reading app textures,
        // which prevents the stereo corruption caused by NOP.
        if (m_originalBytesSaved) {
            static constexpr float kMainMenuFadeSeconds = 0.5f;
            float effectiveDelay = m_renderDelaySeconds;
            if (!m_gameSessionLoaded) {
                effectiveDelay = std::max(effectiveDelay, kMainMenuFadeSeconds);
                logger::info("First load from main menu — using {:.1f}s delay for menu fade", effectiveDelay);
            }

            int delayMs = static_cast<int>(effectiveDelay * 1000.0f);
            std::thread([this, delayMs]() {
                if (delayMs > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                }
                if (!m_inLoadingScreen.load()) return;

                // Request deferred NOP — applied inside Submit hook after right eye
                // so both eyes have valid frames before the loop breaks
                D3D11Compositor::GetSingleton().RequestDeferredNOP(m_loopAddress, NOP10, 10);

                // Show background overlay
                ShowOverlays();

                logger::info("NOP + overlay applied after {}ms delay", delayMs);
            }).detach();
        }
    }

    void LoadingScreenManager::ShowOverlays()
    {
        bool isQuickReload = (m_sinceLastClose < 1000);

        if (VRCompositorHelper::IsOverlayInitialized()) {
            if (!isQuickReload && m_backgroundsEnabled && m_currentBgTexture) {
                VRCompositorHelper::ShowBackgroundOverlay(m_currentBgTexture, m_overlayMode, m_overlayAlpha);
            }
        }
    }

    void LoadingScreenManager::OnLoadingMenuClose()
    {
        m_lastCloseTime = std::chrono::steady_clock::now();
        auto elapsed = m_lastCloseTime - m_loadStartTime;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        m_inLoadingScreen = false;

        logger::info("Loading screen #{} closed — duration: {:.2f}s ({}ms), loopNOPApplied={}, timingOnly={}",
            m_loadCount, ms / 1000.0, ms, m_loopNOPApplied.load(), m_timingOnly);

        if (m_timingOnly) return;

        // Restore animation loop if deferred NOP was applied
        auto& compositor = D3D11Compositor::GetSingleton();
        if (m_originalBytesSaved && compositor.IsDeferredNOPApplied()) {
            std::memcpy(reinterpret_cast<void*>(m_loopAddress), m_originalLoopBytes, 10);
            m_loopNOPApplied = true;  // for log
            logger::info("Restored loop bytes at {:x}", m_loopAddress);
        }
        compositor.ResetDeferredState();

        // Desync fix: only after first save load, consumed once.
        // Check BEFORE hiding overlays — if moveto is about to fire, keep overlays
        // visible to avoid the black flash between close→moveto→reopen.
        bool desyncReloadPending = m_needsDesyncFix;
        if (m_needsDesyncFix) {
            m_needsDesyncFix = false;
            m_pendingDesyncFix = true;
        }

        if (!desyncReloadPending) {
            // Delay hiding overlays by ~200ms so the game renders a clean gameplay frame
            // before we reveal it (avoids brief flash of stereo-mismatched loading content)
            m_needsOverlayHide = true;
            m_overlayHideTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            logger::info("Compositor: overlay hide scheduled in 200ms");
        } else {
            logger::info("Compositor: keeping overlays visible (desync fix pending)");
        }

        // Prepare next background texture (new random image for next load)
        if (m_backgroundsEnabled) {
            PrepareNextBackground();
        }
    }

    void LoadingScreenManager::Update()
    {
        if (m_timingOnly) return;

        // Re-apply overlay transform every frame during loading to keep it "stuck"
        if (m_inLoadingScreen.load()) {
            VRCompositorHelper::UpdateBackgroundOverlay();

            // Process captured frame outside the Submit hook (avoids driver crashes)
            D3D11Compositor::GetSingleton().ProcessPendingCapture();
        }

        // Delayed overlay hide — gives the game time to render clean gameplay frames
        if (m_needsOverlayHide && std::chrono::steady_clock::now() >= m_overlayHideTime) {
            m_needsOverlayHide = false;
            VRCompositorHelper::HideBackgroundOverlay();
            VRCompositorHelper::ClearSkybox();
            logger::info("Compositor: overlays hidden (delayed), skybox cleared");
        }

        if (m_pendingDesyncFix && !m_inLoadingScreen.load()) {
            m_pendingDesyncFix = false;
            RE::Console::ExecuteCommand("player.moveto player");
            logger::info("Executed 'player.moveto player' to fix VR desync");
        }
    }
}
