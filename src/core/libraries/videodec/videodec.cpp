// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "common/alignment.h"
#include "common/logging/log.h"
#include "core/libraries/libs.h"
#include "core/libraries/videodec/videodec.h"
#include "core/libraries/videodec/videodec_error.h"
#include "core/libraries/videodec/videodec_impl.h"

namespace Libraries::Videodec {

static constexpr u64 kFallbackMemorySize = 16_MB;

// Native PS4 libSceVideodec control block magic ("MF049BI\0")
static constexpr u64 kVideodecHeaderMagic = 0x4942393430464dULL;

static std::mutex g_decoder_mutex;
static std::unordered_map<void*, std::unique_ptr<VdecDecoder>> g_decoders;
static std::unordered_map<void*, void*> g_decoder_lookup;

static u64 ComputeFrameSizeBytes(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }

    const u32 aligned_width = Common::AlignUp<u32>((u32)width, 256);
    const u32 aligned_height = Common::AlignUp<u32>((u32)height, 16);

    const u64 pixels = (u64)aligned_width * (u64)aligned_height;
    return (pixels * 3) / 2;
}

static s32 ComputeDpbCount(const OrbisVideodecConfigInfo& cfg) {
    if (cfg.maxDpbFrameCount > 0) {
        return cfg.maxDpbFrameCount;
    }

    return 8;
}

static void ComputeWorstCaseDimensions(const OrbisVideodecConfigInfo& cfg, s32& out_width,
                                       s32& out_height) {
    if (cfg.maxFrameWidth > 0 && cfg.maxFrameHeight > 0) {
        out_width = cfg.maxFrameWidth;
        out_height = cfg.maxFrameHeight;
        return;
    }

    out_width = 1920;
    out_height = 1080;

    if (cfg.maxLevel >= 150) {
        out_width = 3840;
        out_height = 2160;
    }
}

static VdecDecoder* GetDecoder(const OrbisVideodecCtrl* pCtrlIn) {
    if (!pCtrlIn || !pCtrlIn->handle) {
        return nullptr;
    }
    std::scoped_lock lock{g_decoder_mutex};
    auto it = g_decoders.find(pCtrlIn->handle);
    if (it != g_decoders.end()) {
        return it->second.get();
    }
    auto it_lookup = g_decoder_lookup.find(pCtrlIn->handle);
    if (it_lookup != g_decoder_lookup.end()) {
        auto it2 = g_decoders.find(it_lookup->second);
        if (it2 != g_decoders.end()) {
            return it2->second.get();
        }
    }
    return nullptr;
}

int PS4_SYSV_ABI sceVideodecCreateDecoder(const OrbisVideodecConfigInfo* pCfgInfoIn,
                                          const OrbisVideodecResourceInfo* pRsrcInfoIn,
                                          OrbisVideodecCtrl* pCtrlOut) {
    LOG_INFO(Lib_Videodec, "called");

    if (!pCfgInfoIn || !pRsrcInfoIn || !pCtrlOut) {
        LOG_ERROR(Lib_Videodec, "Invalid arguments");
        return ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER;
    }
    if ((pCfgInfoIn->thisSize != sizeof(OrbisVideodecConfigInfo) && pCfgInfoIn->thisSize != 0x20) ||
        pRsrcInfoIn->thisSize != sizeof(OrbisVideodecResourceInfo) ||
        pCtrlOut->thisSize != sizeof(OrbisVideodecCtrl)) {
        LOG_ERROR(Lib_Videodec, "Invalid struct size (cfg: {:#x}, rsrc: {:#x}, ctrl: {:#x})",
                  pCfgInfoIn->thisSize, pRsrcInfoIn->thisSize, pCtrlOut->thisSize);
        return ORBIS_VIDEODEC_ERROR_STRUCT_SIZE;
    }

    auto decoder = std::make_unique<VdecDecoder>(*pCfgInfoIn, *pRsrcInfoIn);
    VdecDecoder* decoder_ptr = decoder.get();

    void* handle = pRsrcInfoIn->pCpuMemory ? pRsrcInfoIn->pCpuMemory : decoder_ptr;

    if (pRsrcInfoIn->pCpuMemory) {
        u8* cpu_mem = static_cast<u8*>(pRsrcInfoIn->pCpuMemory);
        const size_t init_size = std::min<size_t>(pRsrcInfoIn->cpuMemorySize, 0x1000);
        std::memset(cpu_mem, 0, init_size);

        // Native libSceVideodec header: magic "MF049BI\0" at +0x00 and internal decoder pointer at
        // +0x50
        *reinterpret_cast<u64*>(cpu_mem) = kVideodecHeaderMagic;
        *reinterpret_cast<void**>(cpu_mem + 0x50) = decoder_ptr;
    }

    {
        std::scoped_lock lock{g_decoder_mutex};
        g_decoders[handle] = std::move(decoder);
        if (handle != decoder_ptr) {
            g_decoder_lookup[decoder_ptr] = handle;
        }
    }

    pCtrlOut->thisSize = sizeof(OrbisVideodecCtrl);
    pCtrlOut->handle = handle;
    pCtrlOut->version = 0;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceVideodecDecode(OrbisVideodecCtrl* pCtrlIn,
                                   const OrbisVideodecInputData* pInputDataIn,
                                   OrbisVideodecFrameBuffer* pFrameBufferInOut,
                                   OrbisVideodecPictureInfo* pPictureInfoOut) {
    LOG_TRACE(Lib_Videodec, "called");
    if (!pCtrlIn || !pInputDataIn || !pPictureInfoOut || !pFrameBufferInOut) {
        LOG_ERROR(Lib_Videodec, "Invalid arguments");
        return ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER;
    }
    if (pCtrlIn->thisSize != sizeof(OrbisVideodecCtrl) ||
        (pFrameBufferInOut->thisSize != sizeof(OrbisVideodecFrameBuffer) &&
         pFrameBufferInOut->thisSize != 0x20)) {
        LOG_ERROR(Lib_Videodec, "Invalid struct size (ctrl: {:#x}, fb: {:#x})", pCtrlIn->thisSize,
                  pFrameBufferInOut->thisSize);
        return ORBIS_VIDEODEC_ERROR_STRUCT_SIZE;
    }

    VdecDecoder* decoder = GetDecoder(pCtrlIn);
    if (!decoder) {
        LOG_ERROR(Lib_Videodec, "Invalid decoder handle: {}", (void*)pCtrlIn->handle);
        return ORBIS_VIDEODEC_ERROR_HANDLE;
    }
    return decoder->Decode(*pInputDataIn, *pFrameBufferInOut, *pPictureInfoOut);
}

int PS4_SYSV_ABI sceVideodecDeleteDecoder(OrbisVideodecCtrl* pCtrlIn) {
    LOG_INFO(Lib_Videodec, "called");

    if (!pCtrlIn) {
        LOG_ERROR(Lib_Videodec, "Invalid arguments");
        return ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER;
    }
    if (pCtrlIn->thisSize != sizeof(OrbisVideodecCtrl)) {
        LOG_ERROR(Lib_Videodec, "Invalid struct size: {:#x}", pCtrlIn->thisSize);
        return ORBIS_VIDEODEC_ERROR_STRUCT_SIZE;
    }

    std::scoped_lock lock{g_decoder_mutex};
    auto it = g_decoders.find(pCtrlIn->handle);
    if (it != g_decoders.end()) {
        g_decoder_lookup.erase(it->second.get());
        g_decoders.erase(it);
        pCtrlIn->handle = nullptr;
        return ORBIS_OK;
    }

    auto it_lookup = g_decoder_lookup.find(pCtrlIn->handle);
    if (it_lookup != g_decoder_lookup.end()) {
        auto it2 = g_decoders.find(it_lookup->second);
        if (it2 != g_decoders.end()) {
            g_decoders.erase(it2);
        }
        g_decoder_lookup.erase(it_lookup);
        pCtrlIn->handle = nullptr;
        return ORBIS_OK;
    }

    LOG_ERROR(Lib_Videodec, "Invalid decoder handle: {}", (void*)pCtrlIn->handle);
    return ORBIS_VIDEODEC_ERROR_HANDLE;
}

int PS4_SYSV_ABI sceVideodecFlush(OrbisVideodecCtrl* pCtrlIn,
                                  OrbisVideodecFrameBuffer* pFrameBufferInOut,
                                  OrbisVideodecPictureInfo* pPictureInfoOut) {
    LOG_INFO(Lib_Videodec, "called");

    if (!pCtrlIn || !pFrameBufferInOut || !pPictureInfoOut) {
        LOG_ERROR(Lib_Videodec, "Invalid arguments");
        return ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER;
    }
    if (pCtrlIn->thisSize != sizeof(OrbisVideodecCtrl) ||
        (pFrameBufferInOut->thisSize != sizeof(OrbisVideodecFrameBuffer) &&
         pFrameBufferInOut->thisSize != 0x20)) {
        LOG_ERROR(Lib_Videodec, "Invalid struct size (ctrl: {:#x}, fb: {:#x})", pCtrlIn->thisSize,
                  pFrameBufferInOut->thisSize);
        return ORBIS_VIDEODEC_ERROR_STRUCT_SIZE;
    }

    VdecDecoder* decoder = GetDecoder(pCtrlIn);
    if (!decoder) {
        LOG_ERROR(Lib_Videodec, "Invalid decoder handle: {}", (void*)pCtrlIn->handle);
        return ORBIS_VIDEODEC_ERROR_HANDLE;
    }
    return decoder->Flush(*pFrameBufferInOut, *pPictureInfoOut);
}

int PS4_SYSV_ABI sceVideodecMapMemory(OrbisVideodecCtrl* pCtrlIn, const void* pMemInfoIn) {
    LOG_INFO(Lib_Videodec, "called");
    if (!pCtrlIn || !pMemInfoIn) {
        LOG_ERROR(Lib_Videodec, "Invalid arguments");
        return ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER;
    }
    if (pCtrlIn->thisSize != sizeof(OrbisVideodecCtrl)) {
        LOG_ERROR(Lib_Videodec, "Invalid struct size: {:#x}", pCtrlIn->thisSize);
        return ORBIS_VIDEODEC_ERROR_STRUCT_SIZE;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceVideodecQueryResourceInfo(const OrbisVideodecConfigInfo* pCfgInfoIn,
                                              OrbisVideodecResourceInfo* pRsrcInfoOut) {
    LOG_INFO(Lib_Videodec, "called");

    if (!pCfgInfoIn || !pRsrcInfoOut) {
        LOG_ERROR(Lib_Videodec, "Invalid arguments");
        return ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER;
    }
    if ((pCfgInfoIn->thisSize != sizeof(OrbisVideodecConfigInfo) && pCfgInfoIn->thisSize != 0x20) ||
        pRsrcInfoOut->thisSize != sizeof(OrbisVideodecResourceInfo)) {
        LOG_ERROR(Lib_Videodec, "Invalid struct size (cfg: {:#x}, rsrc: {:#x})",
                  pCfgInfoIn->thisSize, pRsrcInfoOut->thisSize);
        return ORBIS_VIDEODEC_ERROR_STRUCT_SIZE;
    }

    s32 width = 0;
    s32 height = 0;
    ComputeWorstCaseDimensions(*pCfgInfoIn, width, height);

    const u64 frame_size = ComputeFrameSizeBytes(width, height);
    const s32 dpb_count = ComputeDpbCount(*pCfgInfoIn);

    u64 cpu_gpu_size = 0;
    u64 cpu_size = 0;
    u64 max_frame_buffer = 0;

    if (frame_size == 0) {
        cpu_gpu_size = kFallbackMemorySize;
        cpu_size = kFallbackMemorySize;
        max_frame_buffer = kFallbackMemorySize;
    } else {
        const u64 padded_frame = Common::AlignUp<u64>(frame_size, 256) + 0x4000;
        const u64 surfaces = (u64)dpb_count + 2;

        max_frame_buffer = padded_frame;

        cpu_gpu_size = (padded_frame * surfaces) + 8_MB;
        cpu_size = 16_MB;
    }

    pRsrcInfoOut->thisSize = sizeof(OrbisVideodecResourceInfo);
    pRsrcInfoOut->pCpuMemory = nullptr;
    pRsrcInfoOut->pCpuGpuMemory = nullptr;

    pRsrcInfoOut->cpuGpuMemorySize = cpu_gpu_size;
    pRsrcInfoOut->cpuMemorySize = cpu_size;

    pRsrcInfoOut->maxFrameBufferSize = max_frame_buffer;
    pRsrcInfoOut->frameBufferAlignment = 0x100;

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceVideodecReset(OrbisVideodecCtrl* pCtrlIn) {
    LOG_INFO(Lib_Videodec, "called");

    if (!pCtrlIn) {
        LOG_ERROR(Lib_Videodec, "Invalid arguments");
        return ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER;
    }
    if (pCtrlIn->thisSize != sizeof(OrbisVideodecCtrl)) {
        LOG_ERROR(Lib_Videodec, "Invalid struct size: {:#x}", pCtrlIn->thisSize);
        return ORBIS_VIDEODEC_ERROR_STRUCT_SIZE;
    }

    VdecDecoder* decoder = GetDecoder(pCtrlIn);
    if (!decoder) {
        LOG_ERROR(Lib_Videodec, "Invalid decoder handle: {}", (void*)pCtrlIn->handle);
        return ORBIS_VIDEODEC_ERROR_HANDLE;
    }

    // Console firmware resets state fields in pCpuMemory if handle points to it
    if (pCtrlIn->handle) {
        u8* mem = static_cast<u8*>(pCtrlIn->handle);
        *reinterpret_cast<u64*>(mem + 0x08) = 0;
        *reinterpret_cast<u32*>(mem + 0x38) = 0;
        *reinterpret_cast<u64*>(mem + 0x40) = 0;
    }

    return decoder->Reset();
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("qkgRiwHyheU", "libSceVideodec", 1, "libSceVideodec", sceVideodecCreateDecoder);
    LIB_FUNCTION("q0W5GJMovMs", "libSceVideodec", 1, "libSceVideodec", sceVideodecDecode);
    LIB_FUNCTION("U0kpGF1cl90", "libSceVideodec", 1, "libSceVideodec", sceVideodecDeleteDecoder);
    LIB_FUNCTION("jeigLlKdp5I", "libSceVideodec", 1, "libSceVideodec", sceVideodecFlush);
    LIB_FUNCTION("kg+lH0V61hM", "libSceVideodec", 1, "libSceVideodec", sceVideodecMapMemory);
    LIB_FUNCTION("leCAscipfFY", "libSceVideodec", 1, "libSceVideodec",
                 sceVideodecQueryResourceInfo);
    LIB_FUNCTION("f8AgDv-1X8A", "libSceVideodec", 1, "libSceVideodec", sceVideodecReset);
};

} // namespace Libraries::Videodec
