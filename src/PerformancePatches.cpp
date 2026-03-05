#include "PCH.h"
#include "PerformancePatches.h"

namespace VRLoadingScreens
{
    void PerformancePatches::Apply(const PerformanceConfig& config)
    {
        // Non-timer patches applied at startup (safe during loading)

        // 1. Disable3DModel: NOP the 5-byte CALL to SetForegroundModel
        if (config.disable3DModel) {
            REL::Relocation<std::uintptr_t> target{ REL::Offset(SetForegroundModel_Call_Offset) };
            const std::uint8_t nop5[] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
            REL::safe_write(target.address(), nop5, sizeof(nop5));
            logger::info("Disable3DModel: NOPed SetForegroundModel call at {:x}", target.address());
        }

        // 2. DisableBlackLoadingScreens: patch JNZ (0x75) to JMP (0xEB)
        if (config.disableBlackLoadingScreens) {
            REL::Relocation<std::uintptr_t> target{ REL::Offset(DisableBlackLoadingScreens_Offset) };
            const std::uint8_t patch[] = { 0xEB };
            REL::safe_write(target.address(), patch, sizeof(patch));
            logger::info("DisableBlackLoadingScreens: patched at {:x}", target.address());
        }

        // 2. DisableVSyncWhileLoading: resolve renderer data pointer and cache presentInterval location
        if (config.disableVSyncWhileLoading) {
            REL::Relocation<void**> rendererDataPtr{ REL::Offset(BSGraphics_RendererData_Offset) };
            void* rendererData = *rendererDataPtr;
            if (rendererData) {
                s_presentIntervalPtr = reinterpret_cast<std::uint32_t*>(
                    reinterpret_cast<std::uintptr_t>(rendererData) + PresentInterval_Offset);
                s_originalPresentInterval = *s_presentIntervalPtr;
                s_vsyncEnabled = true;
                logger::info("DisableVSyncWhileLoading: initialized, presentInterval={}", s_originalPresentInterval);
            } else {
                logger::warn("DisableVSyncWhileLoading: BSGraphics::RendererData not available");
            }
        }
    }

    void PerformancePatches::ApplyTimerPatches(const PerformanceConfig& config)
    {
        // Timer patches deferred until after first save load to prevent VR camera desync.
        // These modify BSTimer::Update's delta clamping, which can cause the Havok
        // character controller to desync from the VR headset during save load.

        static bool s_applied = false;
        if (s_applied) return;
        s_applied = true;

        if (config.untieSpeedFromFPS) {
            REL::Relocation<std::uintptr_t> target{ REL::Offset(UntieSpeedFromFPS_Offset) };
            const std::uint8_t patch[] = { 0x00 };
            REL::safe_write(target.address(), patch, sizeof(patch));
            logger::info("UntieSpeedFromFPS: patched at {:x}", target.address());
        }

        if (config.disableiFPSClamp) {
            REL::Relocation<std::uintptr_t> target{ REL::Offset(DisableiFPSClamp_Offset) };
            const std::uint8_t patch[] = { 0x38 };
            REL::safe_write(target.address(), patch, sizeof(patch));
            logger::info("DisableiFPSClamp: patched at {:x}", target.address());

            if (auto* setting = RE::GetINISetting("iFPSClamp:General")) {
                setting->SetInt(0);
                logger::info("DisableiFPSClamp: iFPSClamp:General set to 0");
            }
        }
    }

    void PerformancePatches::OnLoadingMenuOpen()
    {
        if (!s_vsyncEnabled || !s_presentIntervalPtr) return;

        s_originalPresentInterval = *s_presentIntervalPtr;
        *s_presentIntervalPtr = 0;
        logger::info("VSync disabled for loading (was {})", s_originalPresentInterval);
    }

    void PerformancePatches::OnLoadingMenuClose()
    {
        if (!s_vsyncEnabled || !s_presentIntervalPtr) return;

        *s_presentIntervalPtr = s_originalPresentInterval;
        logger::info("VSync restored to {}", s_originalPresentInterval);
    }
}
