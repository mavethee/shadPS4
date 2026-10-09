// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include "common/logging/log.h"
#include "core/libraries/composite/composite.h"
#include "core/libraries/libs.h"

namespace Libraries::Composite {

static std::mutex s_cmd_mutex;

static s32 PS4_SYSV_ABI sceCompositorGetRenderTargetResolution(u16* width, u16* height) {
    LOG_INFO(Lib_VideoOut, "sceCompositorGetRenderTargetResolution: width={:p}, height={:p}",
             static_cast<void*>(width), static_cast<void*>(height));
    if (width) {
        *width = 1920;
    }
    if (height) {
        *height = 1080;
    }
    return 0;
}

static s32 PS4_SYSV_ABI sceCompositorLockCommandBuffer() {
    s_cmd_mutex.lock();
    return 0;
}

static s32 PS4_SYSV_ABI sceCompositorReleaseCommandBuffer() {
    s_cmd_mutex.unlock();
    return 0;
}

static s32 PS4_SYSV_ABI sceCompositorFlush() {
    return 0;
}

static s32 PS4_SYSV_ABI sceCompositorSetFlipCommand(const void* /*cmd*/) {
    return 0;
}

static s32 PS4_SYSV_ABI sceCompositorSetGameSufaceControlCommand(const void* /*cmd*/) {
    return 0;
}

static s32 PS4_SYSV_ABI sceCompositorSetGnmContextCommand(const void* /*cmd*/) {
    return 0;
}

static s32 PS4_SYSV_ABI sceCompositorCheckCrash() {
    return 0;
}

static s32 PS4_SYSV_ABI sceCompositorIsReady4K() {
    return 0;
}

static s32 PS4_SYSV_ABI sceCompositorIsDebugCaptureEnabled() {
    return 0;
}

static s32 PS4_SYSV_ABI sceCompositorInsertThreadTraceMarker(const void* /*marker*/) {
    return 0;
}

static u64 PS4_SYSV_ABI sceCompositorGetSystemAddress() {
    return 0;
}

static u64 PS4_SYSV_ABI sceCompositorGetVideoAddress() {
    return 0;
}

static s32 PS4_SYSV_ABI sceCompsoitorGetRenderingTime() {
    return 0;
}

static s32 PS4_SYSV_ABI sceCompsoitorGetProcessRenderingTime() {
    return 0;
}

static s32 PS4_SYSV_ABI sceCompsoitorGetGpuClock() {
    return 800;
}

static s32 PS4_SYSV_ABI sceCompositorSetZoomCommand(const void* /*cmd*/) {
    return 0;
}

static s32 PS4_SYSV_ABI sceCompositorResetZoomCommand() {
    return 0;
}

static s32 PS4_SYSV_ABI sceCompositorSetPatchCommand(const void* /*cmd*/) {
    return 0;
}

static s32 PS4_SYSV_ABI sceCompositorSetMorpheusState(s32 /*state*/) {
    return 0;
}

static s32 PS4_SYSV_ABI sceCompositorCommandGpuPerfBegin(const void* /*cmd*/) {
    return 0;
}

static s32 PS4_SYSV_ABI sceCompositorCommandGpuPerfEnd(const void* /*cmd*/) {
    return 0;
}

static s32 PS4_SYSV_ABI sceCompositorInit(u32 /*p1*/, u32 /*p2*/, u32 /*p3*/, u32 /*p4*/) {
    return 0;
}

static s32 PS4_SYSV_ABI sceCompositorQuit() {
    return 0;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("twGXom56jw0", "libSceComposite", 1, "libSceComposite",
                 sceCompositorGetRenderTargetResolution);
    LIB_FUNCTION("GgOrwi+9vcA", "libSceComposite", 1, "libSceComposite",
                 sceCompositorLockCommandBuffer);
    LIB_FUNCTION("1OXbuWLRxqI", "libSceComposite", 1, "libSceComposite",
                 sceCompositorReleaseCommandBuffer);
    LIB_FUNCTION("qZNF03+ghLI", "libSceComposite", 1, "libSceComposite", sceCompositorFlush);
    LIB_FUNCTION("1oTrw-ivVpA", "libSceComposite", 1, "libSceComposite",
                 sceCompositorSetFlipCommand);
    LIB_FUNCTION("n2XkxixrY2k", "libSceComposite", 1, "libSceComposite",
                 sceCompositorSetGameSufaceControlCommand);
    LIB_FUNCTION("DhtKelVAIaA", "libSceComposite", 1, "libSceComposite",
                 sceCompositorSetGnmContextCommand);
    LIB_FUNCTION("9M7HkbwUs-E", "libSceComposite", 1, "libSceComposite", sceCompositorCheckCrash);
    LIB_FUNCTION("RSEX1eS1tCI", "libSceComposite", 1, "libSceComposite", sceCompositorIsReady4K);
    LIB_FUNCTION("q+Qw1ESxCj8", "libSceComposite", 1, "libSceComposite",
                 sceCompositorIsDebugCaptureEnabled);
    LIB_FUNCTION("-wIAG588mvg", "libSceComposite", 1, "libSceComposite",
                 sceCompositorInsertThreadTraceMarker);
    LIB_FUNCTION("T6CVkdCDO7o", "libSceComposite", 1, "libSceComposite",
                 sceCompositorGetSystemAddress);
    LIB_FUNCTION("bxt+muwit0w", "libSceComposite", 1, "libSceComposite",
                 sceCompositorGetVideoAddress);
    LIB_FUNCTION("rIH7UTXfho4", "libSceComposite", 1, "libSceComposite",
                 sceCompsoitorGetRenderingTime);
    LIB_FUNCTION("EcTHemmz6n4", "libSceComposite", 1, "libSceComposite",
                 sceCompsoitorGetProcessRenderingTime);
    LIB_FUNCTION("4yWqjTZtvs4", "libSceComposite", 1, "libSceComposite", sceCompsoitorGetGpuClock);
    LIB_FUNCTION("aroIog+umzo", "libSceComposite", 1, "libSceComposite",
                 sceCompositorSetZoomCommand);
    LIB_FUNCTION("1PX0NFNL1BQ", "libSceComposite", 1, "libSceComposite",
                 sceCompositorResetZoomCommand);
    LIB_FUNCTION("YzI2BOoDw+I", "libSceComposite", 1, "libSceComposite",
                 sceCompositorSetPatchCommand);
    LIB_FUNCTION("H4EXZ9L3p2M", "libSceComposite", 1, "libSceComposite",
                 sceCompositorSetMorpheusState);
    LIB_FUNCTION("6bz4VVSSFyg", "libSceComposite", 1, "libSceComposite",
                 sceCompositorCommandGpuPerfBegin);
    LIB_FUNCTION("fH2IStnGK4M", "libSceComposite", 1, "libSceComposite",
                 sceCompositorCommandGpuPerfEnd);
    LIB_FUNCTION("xIkcEpdMxrw", "libSceComposite", 1, "libSceComposite", sceCompositorInit);
    LIB_FUNCTION("y5IJdPxdBrE", "libSceComposite", 1, "libSceComposite", sceCompositorQuit);
}

} // namespace Libraries::Composite
