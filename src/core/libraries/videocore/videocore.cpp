// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <mutex>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/libraries/videocore/videocore.h"

namespace Libraries::VideoCoreInterface {

constexpr s32 ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT = static_cast<s32>(0x80dc0100);
constexpr s32 ORBIS_VIDEO_CORE_ERROR_NOT_INITIALIZED = static_cast<s32>(0x80dc0200);
constexpr s32 ORBIS_VIDEO_CORE_ERROR_INTERNAL = static_cast<s32>(0x80dc0600);
constexpr s32 ORBIS_VIDEO_CORE_ERROR_SERVER_ERROR = static_cast<s32>(0x80dc0800);
constexpr s32 ORBIS_VIDEO_CORE_ERROR_NOT_SUPPORTED = static_cast<s32>(0x80dc7f00);

static bool s_initialized = false;

struct CanvasContext {
    std::mutex mutex;
    u32 canvas_id = 0;
    u32 canvas_handle = 0;
    void* video_buffer_addr = nullptr;
    u64 video_buffer_size = 0;
    bool is_open = false;
};

struct FrameBufferContext {
    CanvasContext* canvas_ctx = nullptr;
    void* fb_config = nullptr;
    u32 width = 1920;
    u32 height = 1080;
    void* render_buffer = nullptr;
    u32 render_buffer_size = 0;
};

struct VideoCoreOutHandle {
    s32 buffer_index = 0;
    void* buffer_info = nullptr;
    s32 num_buffers = 0;
};

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceLoadModule() {
    LOG_INFO(Lib_VideoCore, "sceVideoCoreInterfaceLoadModule called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceUnloadModule() {
    LOG_INFO(Lib_VideoCore, "sceVideoCoreInterfaceUnloadModule called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceInitializeInterface(void* param) {
    LOG_INFO(Lib_VideoCore, "sceVideoCoreInterfaceInitializeInterface called param={:p}", param);
    if (param == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    s_initialized = true;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceMainLoop(const char* name) {
    LOG_INFO(Lib_VideoCore, "sceVideoCoreInterfaceMainLoop called with name='{}'",
             name ? name : "null");
    if (name == nullptr) {
        return ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceSetFunctionTable(void* table) {
    LOG_INFO(Lib_VideoCore, "sceVideoCoreInterfaceSetFunctionTable called table={:p}", table);
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceSetFunctionTableExt(void* table) {
    LOG_INFO(Lib_VideoCore, "sceVideoCoreInterfaceSetFunctionTableExt called table={:p}", table);
    if (!s_initialized) {
        return ORBIS_VIDEO_CORE_ERROR_NOT_INITIALIZED;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceCreateFrameBufferContext(
    void* fb_config, void* canvas_config, FrameBufferContext** out_fb_ctx) {
    LOG_INFO(Lib_VideoCore, "sceVideoCoreInterfaceCreateFrameBufferContext called");
    if (fb_config == nullptr || canvas_config == nullptr || out_fb_ctx == nullptr) {
        return ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT;
    }
    auto* fb_ctx = new FrameBufferContext;
    fb_ctx->canvas_ctx = new CanvasContext;
    fb_ctx->fb_config = fb_config;
    *out_fb_ctx = fb_ctx;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceCreateCanvasContext(void* canvas_config,
                                                                 CanvasContext** out_canvas_ctx) {
    LOG_INFO(Lib_VideoCore, "sceVideoCoreInterfaceCreateCanvasContext called");
    if (canvas_config == nullptr || out_canvas_ctx == nullptr) {
        return ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT;
    }
    auto* ctx = new CanvasContext;
    *out_canvas_ctx = ctx;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceDestroyFrameBufferContext(FrameBufferContext* fb_ctx) {
    LOG_INFO(Lib_VideoCore, "sceVideoCoreInterfaceDestroyFrameBufferContext called fb_ctx={:p}",
             static_cast<void*>(fb_ctx));
    if (fb_ctx == nullptr) {
        return ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT;
    }
    delete fb_ctx->canvas_ctx;
    delete fb_ctx;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceDestroyCanvasContext(CanvasContext* canvas_ctx) {
    LOG_INFO(Lib_VideoCore, "sceVideoCoreInterfaceDestroyCanvasContext called canvas_ctx={:p}",
             static_cast<void*>(canvas_ctx));
    if (canvas_ctx == nullptr) {
        return ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT;
    }
    delete canvas_ctx;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceGetRenderFrameBuffer(FrameBufferContext* fb_ctx,
                                                                  void* out_render_fb) {
    LOG_DEBUG(Lib_VideoCore, "sceVideoCoreInterfaceGetRenderFrameBuffer called");
    if (fb_ctx == nullptr || out_render_fb == nullptr) {
        return ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT;
    }
    std::memset(out_render_fb, 0, 0x10);
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceFinishRendering(FrameBufferContext* fb_ctx) {
    LOG_DEBUG(Lib_VideoCore, "sceVideoCoreInterfaceFinishRendering called");
    if (fb_ctx == nullptr) {
        return ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceSetCanvasId(CanvasContext* ctx, u32 canvas_id) {
    LOG_INFO(Lib_VideoCore, "sceVideoCoreInterfaceSetCanvasId called canvas_id={}", canvas_id);
    if (ctx == nullptr || canvas_id > 7) {
        return ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(ctx->mutex);
    ctx->canvas_id = canvas_id;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceOpenCanvas(u32 canvas_id) {
    LOG_INFO(Lib_VideoCore, "sceVideoCoreInterfaceOpenCanvas called canvas_id={}", canvas_id);
    if (canvas_id > 7) {
        return ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceCloseCanvas() {
    LOG_INFO(Lib_VideoCore, "sceVideoCoreInterfaceCloseCanvas called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceGetCanvasVideoBufferAddress(void** out_addr) {
    LOG_DEBUG(Lib_VideoCore, "sceVideoCoreInterfaceGetCanvasVideoBufferAddress called");
    if (out_addr == nullptr) {
        return ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT;
    }
    *out_addr = nullptr;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceGetCanvasVideoBufferSize(u64* out_size) {
    LOG_DEBUG(Lib_VideoCore, "sceVideoCoreInterfaceGetCanvasVideoBufferSize called");
    if (out_size == nullptr) {
        return ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT;
    }
    *out_size = 1920 * 1080 * 4;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceSubmitCanvases(const u32* canvas_ids,
                                                            const void** commands, s32 count) {
    LOG_DEBUG(Lib_VideoCore, "sceVideoCoreInterfaceSubmitCanvases called count={}", count);
    if (canvas_ids == nullptr || commands == nullptr || count <= 0) {
        return ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceWaitCanvasFlip() {
    LOG_DEBUG(Lib_VideoCore, "sceVideoCoreInterfaceWaitCanvasFlip called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceAddVblankEvent(u32 bus, void* event) {
    LOG_INFO(Lib_VideoCore, "sceVideoCoreInterfaceAddVblankEvent called bus={}", bus);
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceOpenVideoOut(void** out_handle, u32* out_handle_id) {
    LOG_INFO(Lib_VideoCore, "sceVideoCoreInterfaceOpenVideoOut called");
    if (out_handle == nullptr || out_handle_id == nullptr) {
        return ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT;
    }
    auto* handle = new VideoCoreOutHandle;
    *out_handle = handle;
    *out_handle_id = 1;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceCloseVideoOut(VideoCoreOutHandle* handle) {
    LOG_INFO(Lib_VideoCore, "sceVideoCoreInterfaceCloseVideoOut called handle={:p}",
             static_cast<void*>(handle));
    if (handle == nullptr) {
        return ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT;
    }
    delete handle;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceSetVideoOutBufferInfo(VideoCoreOutHandle* handle,
                                                                   s32 buffer_index,
                                                                   void* buffer_info,
                                                                   s32 num_buffers) {
    LOG_INFO(Lib_VideoCore,
             "sceVideoCoreInterfaceSetVideoOutBufferInfo called buffer_index={} num_buffers={}",
             buffer_index, num_buffers);
    if (handle == nullptr || buffer_index < 0 || buffer_index > 0xf || buffer_info == nullptr ||
        num_buffers <= 0 || num_buffers > 0x10) {
        return ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT;
    }
    handle->buffer_index = buffer_index;
    handle->buffer_info = buffer_info;
    handle->num_buffers = num_buffers;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceFlushVideoOutBuffer(VideoCoreOutHandle* handle,
                                                                 void* flush_param) {
    LOG_INFO(Lib_VideoCore, "sceVideoCoreInterfaceFlushVideoOutBuffer called handle={:p}",
             static_cast<void*>(handle));
    if (handle == nullptr || flush_param == nullptr) {
        return ORBIS_VIDEO_CORE_ERROR_INVALID_ARGUMENT;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVideoCoreInterfaceStubNotSupported() {
    return ORBIS_VIDEO_CORE_ERROR_NOT_SUPPORTED;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("C9E1TPtMAjg", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceLoadModule);
    LIB_FUNCTION("CZRLO-gAmpk", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceUnloadModule);
    LIB_FUNCTION("WNIV7+VKQaA", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceInitializeInterface);
    LIB_FUNCTION("Aj2Ds5L9tmU", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceMainLoop);
    LIB_FUNCTION("uGnFTTL5rzg", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceSetFunctionTable);
    LIB_FUNCTION("H9VH6VZBsSY", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceSetFunctionTableExt);
    LIB_FUNCTION("FyvlwTMC2TI", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceCreateFrameBufferContext);
    LIB_FUNCTION("DdJvHNduw2M", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceCreateCanvasContext);
    LIB_FUNCTION("+2Ed65+55ZI", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceDestroyFrameBufferContext);
    LIB_FUNCTION("LvLdnjxdEVQ", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceDestroyCanvasContext);
    LIB_FUNCTION("IHhl7z1-uFI", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceGetRenderFrameBuffer);
    LIB_FUNCTION("-yexw2HJTr4", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceFinishRendering);
    LIB_FUNCTION("SEU9eYrQFsA", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceSetCanvasId);
    LIB_FUNCTION("GV6hye4Lsjc", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceOpenCanvas);
    LIB_FUNCTION("izssDkN7IAU", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceCloseCanvas);
    LIB_FUNCTION("AJ3xhNOXs-A", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceGetCanvasVideoBufferAddress);
    LIB_FUNCTION("Sgi+fOhrWUQ", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceGetCanvasVideoBufferSize);
    LIB_FUNCTION("fUDokJvA7l0", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceSubmitCanvases);
    LIB_FUNCTION("PfoKAY2M0QE", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceWaitCanvasFlip);
    LIB_FUNCTION("Ik5TMINZINo", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceAddVblankEvent);
    LIB_FUNCTION("4g5vWZV5goU", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceOpenVideoOut);
    LIB_FUNCTION("4IsRgjK5hsU", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceCloseVideoOut);
    LIB_FUNCTION("FdbMnFUqVN0", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceSetVideoOutBufferInfo);
    LIB_FUNCTION("cBl8O2bQwos", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceFlushVideoOutBuffer);
    LIB_FUNCTION("m2qUOTiZRiI", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceStubNotSupported);
    LIB_FUNCTION("-4I1PulG6a4", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceStubNotSupported);
    LIB_FUNCTION("xBEMTDFT+T8", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceStubNotSupported);
    LIB_FUNCTION("mqGUZXETSQc", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceStubNotSupported);
    LIB_FUNCTION("A4Fc6G+tFfQ", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceStubNotSupported);
    LIB_FUNCTION("RFRVneI4Pd4", "libSceVideoCoreInterface", 1, "libSceVideoCoreInterface",
                 sceVideoCoreInterfaceStubNotSupported);
}

} // namespace Libraries::VideoCoreInterface
