// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <magic_enum/magic_enum.hpp>

#include <stb_image.h>
#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "jpeg_error.h"
#include "jpegenc.h"

namespace Libraries::JpegEnc {

constexpr s32 ORBIS_JPEG_ENC_MINIMUM_MEMORY_SIZE = 0x800;
constexpr u32 ORBIS_JPEG_ENC_MAX_IMAGE_DIMENSION = 0xFFFF;
constexpr u32 ORBIS_JPEG_ENC_MAX_IMAGE_PITCH = 0xFFFFFFF;
constexpr u32 ORBIS_JPEG_ENC_MAX_IMAGE_SIZE = 0x7FFFFFFF;

static s32 ValidateJpegEncCreateParam(const OrbisJpegEncCreateParam* param) {
    if (!param) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_ADDR;
    }
    if (param->size != sizeof(OrbisJpegEncCreateParam)) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_SIZE;
    }
    if (param->attr != ORBIS_JPEG_ENC_ATTRIBUTE_NONE) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    return ORBIS_OK;
}

static s32 ValidateJpegEncMemory(const void* memory, const u32 memory_size) {
    if (!memory) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_ADDR;
    }
    if (memory_size < ORBIS_JPEG_ENC_MINIMUM_MEMORY_SIZE) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_SIZE;
    }
    return ORBIS_OK;
}

static s32 ValidateJpegEncEncodeParam(const OrbisJpegEncEncodeParam* param) {

    // Validate addresses
    if (!param) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_ADDR;
    }
    if (!param->image || (param->pixel_format != ORBIS_JPEG_ENC_PIXEL_FORMAT_Y8 &&
                          !Common::IsAligned(reinterpret_cast<VAddr>(param->image), 4))) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_ADDR;
    }
    if (!param->jpeg) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_ADDR;
    }

    // Validate sizes
    if (param->image_size == 0 || param->jpeg_size == 0) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_SIZE;
    }

    // Validate parameters
    if (param->image_width > ORBIS_JPEG_ENC_MAX_IMAGE_DIMENSION ||
        param->image_height > ORBIS_JPEG_ENC_MAX_IMAGE_DIMENSION) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    if (param->image_pitch == 0 || param->image_pitch > ORBIS_JPEG_ENC_MAX_IMAGE_PITCH ||
        (param->pixel_format != ORBIS_JPEG_ENC_PIXEL_FORMAT_Y8 &&
         !Common::IsAligned(param->image_pitch, 4))) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    const auto calculated_size = param->image_height * param->image_pitch;
    if (calculated_size > ORBIS_JPEG_ENC_MAX_IMAGE_SIZE || calculated_size > param->image_size) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    if (param->encode_mode != ORBIS_JPEG_ENC_ENCODE_MODE_NORMAL &&
        param->encode_mode != ORBIS_JPEG_ENC_ENCODE_MODE_MJPEG) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    if (param->color_space != ORBIS_JPEG_ENC_COLOR_SPACE_YCC &&
        param->color_space != ORBIS_JPEG_ENC_COLOR_SPACE_GRAYSCALE) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    if (param->sampling_type != ORBIS_JPEG_ENC_SAMPLING_TYPE_FULL &&
        param->sampling_type != ORBIS_JPEG_ENC_SAMPLING_TYPE_422 &&
        param->sampling_type != ORBIS_JPEG_ENC_SAMPLING_TYPE_420) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    if (param->restart_interval > ORBIS_JPEG_ENC_MAX_IMAGE_DIMENSION) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    switch (param->pixel_format) {
    case ORBIS_JPEG_ENC_PIXEL_FORMAT_R8G8B8A8:
    case ORBIS_JPEG_ENC_PIXEL_FORMAT_B8G8R8A8:
        if (param->image_pitch >> 2 < param->image_width ||
            param->color_space != ORBIS_JPEG_ENC_COLOR_SPACE_YCC ||
            param->sampling_type == ORBIS_JPEG_ENC_SAMPLING_TYPE_FULL) {
            return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
        }
        break;
    case ORBIS_JPEG_ENC_PIXEL_FORMAT_Y8U8Y8V8:
        if (param->image_pitch >> 1 < Common::AlignUp(param->image_width, 2) ||
            param->color_space != ORBIS_JPEG_ENC_COLOR_SPACE_YCC ||
            param->sampling_type == ORBIS_JPEG_ENC_SAMPLING_TYPE_FULL) {
            return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
        }
        break;
    case ORBIS_JPEG_ENC_PIXEL_FORMAT_Y8:
        if (param->image_pitch < param->image_width ||
            param->color_space != ORBIS_JPEG_ENC_COLOR_SPACE_GRAYSCALE ||
            param->sampling_type != ORBIS_JPEG_ENC_SAMPLING_TYPE_FULL) {
            return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
        }
        break;
    default:
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }

    return ORBIS_OK;
}

static s32 ValidateJpecEngHandle(OrbisJpegEncHandle handle) {
    if (!handle || !Common::IsAligned(reinterpret_cast<VAddr>(handle), 0x20) ||
        handle->handle != handle) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_HANDLE;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegEncCreate(const OrbisJpegEncCreateParam* param, void* memory,
                                  const u32 memory_size, OrbisJpegEncHandle* handle) {
    if (auto param_ret = ValidateJpegEncCreateParam(param); param_ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid create param");
        return param_ret;
    }
    if (auto memory_ret = ValidateJpegEncMemory(memory, memory_size); memory_ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid memory");
        return memory_ret;
    }
    if (!handle) {
        LOG_ERROR(Lib_Jpeg, "Invalid handle output");
        return ORBIS_JPEG_ENC_ERROR_INVALID_ADDR;
    }

    auto* handle_internal = reinterpret_cast<OrbisJpegEncHandleInternal*>(
        Common::AlignUp(reinterpret_cast<VAddr>(memory), 0x20));
    handle_internal->handle = handle_internal;
    handle_internal->handle_size = sizeof(OrbisJpegEncHandleInternal*);
    *handle = handle_internal;

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegEncDelete(OrbisJpegEncHandle handle) {
    if (auto handle_ret = ValidateJpecEngHandle(handle); handle_ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid handle");
        return handle_ret;
    }
    handle->handle = nullptr;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegEncEncode(OrbisJpegEncHandle handle, const OrbisJpegEncEncodeParam* param,
                                  OrbisJpegEncOutputInfo* output_info) {
    if (auto handle_ret = ValidateJpecEngHandle(handle); handle_ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid handle");
        return handle_ret;
    }
    if (auto param_ret = ValidateJpegEncEncodeParam(param); param_ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid encode param");
        return param_ret;
    }

    LOG_ERROR(Lib_Jpeg,
              "(STUBBED) image_size = {} , jpeg_size = {} , image_width = {} , image_height = {} , "
              "image_pitch = {} , pixel_format = {} , encode_mode = {} , color_space = {} , "
              "sampling_type = {} , compression_ratio = {} , restart_interval = {}",
              param->image_size, param->jpeg_size, param->image_width, param->image_height,
              param->image_pitch, magic_enum::enum_name(param->pixel_format),
              magic_enum::enum_name(param->encode_mode), magic_enum::enum_name(param->color_space),
              magic_enum::enum_name(param->sampling_type), param->compression_ratio,
              param->restart_interval);

    if (output_info) {
        output_info->size = param->jpeg_size;
        output_info->height = param->image_height;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegEncQueryMemorySize(const OrbisJpegEncCreateParam* param) {
    if (auto param_ret = ValidateJpegEncCreateParam(param); param_ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid create param");
        return param_ret;
    }
    return ORBIS_JPEG_ENC_MINIMUM_MEMORY_SIZE;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("K+rocojkr-I", "libSceJpegEnc", 1, "libSceJpegEnc", sceJpegEncCreate);
    LIB_FUNCTION("j1LyMdaM+C0", "libSceJpegEnc", 1, "libSceJpegEnc", sceJpegEncDelete);
    LIB_FUNCTION("QbrU0cUghEM", "libSceJpegEnc", 1, "libSceJpegEnc", sceJpegEncEncode);
    LIB_FUNCTION("o6ZgXfFdWXQ", "libSceJpegEnc", 1, "libSceJpegEnc", sceJpegEncQueryMemorySize);
}

s32 PS4_SYSV_ABI sceJpegDecParseHeader(const void* in_data, void* out_header) {
    if (!in_data || !out_header) {
        return static_cast<s32>(0x80650001);
    }

    struct SceJpegDecParseParam {
        const u8* data;
        u32 size;
        u16 unk_c;
        u16 unk_e;
    };

    auto* param = reinterpret_cast<const SceJpegDecParseParam*>(in_data);
    if (!param->data || param->size < 4) {
        return static_cast<s32>(0x80650002);
    }

    const u8* ptr = param->data;
    const u32 size = param->size;

    if (ptr[0] != 0xFF || ptr[1] != 0xD8) {
        return static_cast<s32>(0x80650002);
    }

    u32 width = 0;
    u32 height = 0;
    u16 num_components = 3;
    u32 pos = 2;
    while (pos + 4 < size) {
        if (ptr[pos] != 0xFF) {
            pos++;
            continue;
        }
        u8 marker = ptr[pos + 1];
        if (marker == 0xD9 || marker == 0xDA) {
            break;
        }
        u16 len = (static_cast<u16>(ptr[pos + 2]) << 8) | ptr[pos + 3];
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            if (pos + 9 < size) {
                height = (static_cast<u32>(ptr[pos + 5]) << 8) | ptr[pos + 6];
                width = (static_cast<u32>(ptr[pos + 7]) << 8) | ptr[pos + 8];
                num_components = ptr[pos + 9];
            }
            break;
        }
        pos += 2 + len;
    }

    auto* out = reinterpret_cast<u8*>(out_header);
    std::memset(out, 0, 0x24);
    *reinterpret_cast<u32*>(out + 0x00) = width;
    *reinterpret_cast<u32*>(out + 0x04) = height;
    *reinterpret_cast<u16*>(out + 0x08) = (num_components == 1) ? 0 : 1;
    *reinterpret_cast<u16*>(out + 0x0a) = num_components;
    out[0x0c] = 1;
    out[0x0d] = 2;
    out[0x0e] = 3;
    out[0x10] = 0x22;
    out[0x11] = 0x11;
    out[0x12] = 0x11;
    *reinterpret_cast<u32*>(out + 0x18) = 1;
    *reinterpret_cast<u32*>(out + 0x1c) = width;
    *reinterpret_cast<u32*>(out + 0x20) = height;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegDecQueryMemorySize(const void* in_param) {
    if (!in_param) {
        return static_cast<s32>(0x80650001);
    }
    const auto* p = reinterpret_cast<const u32*>(in_param);
    if (p[0] != 0xc) {
        return static_cast<s32>(0x80650002);
    }
    u32 mode = p[1];
    if (mode - 1 > 1) {
        return static_cast<s32>(0x80650003);
    }
    u32 width = p[2];
    if (width - 1 > 0xfffe) {
        return static_cast<s32>(0x80650003);
    }
    u32 aligned_w = (width + 0x1f) & ~0x1f;
    aligned_w |= 0x10;
    u32 sz = 0;
    if (mode == 2) {
        sz = (aligned_w << 5) * 5;
    } else if (mode == 1) {
        sz = (aligned_w << 4) * 3;
    }
    return sz + aligned_w * 6 + 0x7360;
}

s32 PS4_SYSV_ABI sceJpegDecCreate(const void* in_param, void* memory, u32 memory_size,
                                  void** handle) {
    if (!in_param || !memory || !handle) {
        return static_cast<s32>(0x80650001);
    }
    s32 req_size = sceJpegDecQueryMemorySize(in_param);
    if (req_size < 0) {
        return req_size;
    }
    if (static_cast<u32>(req_size) > memory_size) {
        return static_cast<s32>(0x80650002);
    }
    uintptr_t aligned_mem =
        (reinterpret_cast<uintptr_t>(memory) + 0x1f) & ~static_cast<uintptr_t>(0x1f);
    auto* dec = reinterpret_cast<void**>(aligned_mem);
    dec[0] = dec;
    *handle = dec;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegDecDelete(void* handle) {
    if (!handle) {
        return static_cast<s32>(0x80650004);
    }
    if ((reinterpret_cast<uintptr_t>(handle) & 0x1f) != 0) {
        return static_cast<s32>(0x80650004);
    }
    auto* dec = reinterpret_cast<void**>(handle);
    if (dec[0] != dec) {
        return static_cast<s32>(0x80650004);
    }
    dec[0] = nullptr;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegDecDecode(void* handle, const void* in_param, void* out_info) {
    if (!handle || !in_param) {
        return static_cast<s32>(0x80650001);
    }
    struct SceJpegDecDecodeParam {
        const u8* in_data;
        u8* out_image;
        const void* aux;
        u32 in_size;
        u32 out_size;
        u32 unk_20;
        u16 mode;
        u16 format;
        u16 pixel_format;
        u16 options;
    };
    static_assert(offsetof(SceJpegDecDecodeParam, in_data) == 0x00);
    static_assert(offsetof(SceJpegDecDecodeParam, out_image) == 0x08);
    static_assert(offsetof(SceJpegDecDecodeParam, in_size) == 0x18);
    static_assert(offsetof(SceJpegDecDecodeParam, out_size) == 0x1c);

    const auto* p = reinterpret_cast<const SceJpegDecDecodeParam*>(in_param);
    if (!p->in_data || !p->out_image || p->in_size == 0) {
        return static_cast<s32>(0x80650001);
    }
    int w = 0, h = 0, ch = 0;
    stbi_uc* decoded =
        stbi_load_from_memory(p->in_data, static_cast<int>(p->in_size), &w, &h, &ch, 4);
    if (!decoded) {
        return static_cast<s32>(0x80650002);
    }
    size_t decode_size = static_cast<size_t>(w * h * 4);
    size_t copy_size = (p->out_size > 0) ? std::min<size_t>(decode_size, p->out_size) : decode_size;
    std::memcpy(p->out_image, decoded, copy_size);
    stbi_image_free(decoded);

    if (out_info) {
        auto* info = reinterpret_cast<u32*>(out_info);
        info[0] = static_cast<u32>(decode_size);
        info[1] = static_cast<u32>(w);
        info[2] = static_cast<u32>(h);
        info[3] = static_cast<u32>(h);
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegDecDecodeWithInputControl(void* handle, const void* in_param,
                                                  void* out_info) {
    return sceJpegDecDecode(handle, in_param, out_info);
}

void RegisterDecLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("uNAUmANZMEw", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecQueryMemorySize);
    LIB_FUNCTION("JPh3Zgg0Zwc", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecCreate);
    LIB_FUNCTION("Hwh11+m5KoI", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecDelete);
    LIB_FUNCTION("1kzQRoWEgSA", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecDecode);
    LIB_FUNCTION("LSinoSQH790", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecParseHeader);
    LIB_FUNCTION("919MhccOiII", "libSceJpegDec_jvm", 1, "libSceJpegDec",
                 sceJpegDecDecodeWithInputControl);
}

} // namespace Libraries::JpegEnc
