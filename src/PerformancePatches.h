#pragma once

namespace VRLoadingScreens
{
    struct PerformanceConfig
    {
        bool untieSpeedFromFPS = true;
        bool disableiFPSClamp = true;
        bool disableBlackLoadingScreens = false;
        bool disableVSyncWhileLoading = true;
        bool disable3DModel = true;
    };

    class PerformancePatches
    {
    public:
        static void Apply(const PerformanceConfig& config);
        static void ApplyTimerPatches(const PerformanceConfig& config);
        static void OnLoadingMenuOpen();
        static void OnLoadingMenuClose();

    private:
        // VR offsets from module base (Fallout 4 VR 1.2.72)
        // All offsets = (Ghidra static address - 0x140000000)

        // UntieSpeedFromFPS: changes byte from 0x08 to 0x00 in havok timing code
        static constexpr std::uintptr_t UntieSpeedFromFPS_Offset = 0x1b962bb;

        // DisableiFPSClamp: changes byte from 0x08 to 0x38 in FPS clamping code
        static constexpr std::uintptr_t DisableiFPSClamp_Offset = 0x1b96268;

        // DisableBlackLoadingScreens: changes JNZ (0x75) to JMP (0xEB)
        static constexpr std::uintptr_t DisableBlackLoadingScreens_Offset = 0x1313eb6;

        // Disable3DModel: NOP the CALL to SetForegroundModel (5-byte E8 call)
        static constexpr std::uintptr_t SetForegroundModel_Call_Offset = 0x131418b;

        // BSGraphics::RendererData pointer (for VSync toggle)
        static constexpr std::uintptr_t BSGraphics_RendererData_Offset = 0x060f3ce8;
        static constexpr std::uint32_t PresentInterval_Offset = 0x40;

        static inline std::uint32_t s_originalPresentInterval = 0;
        static inline bool s_vsyncEnabled = false;
        static inline std::uint32_t* s_presentIntervalPtr = nullptr;
    };
}
