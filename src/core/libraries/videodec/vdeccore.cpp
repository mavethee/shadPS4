// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "vdeccore.h"

#include <atomic>
#include <vector>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "video_utils.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace Libraries::VdecCore {

struct SceVdecCoreConfigInfo {
    u32 resource_type;
    u32 codec_type;
    u32 profile;
    u32 max_level;
    u32 max_dpb_frame_count;
    u32 max_frame_width;
    u32 max_frame_height;
    u32 flags;
    void* compute_queue;
};

struct SceVdecCoreFrameBufferInfo {
    u32 frame_buffer_size;
    u32 frame_buffer_alignment;
    u32 min_frame_buffer_count;
    u32 reserved0;
    u32 reserved1;
    u32 reserved2;
};

struct SceVdecCoreFrameBuffer {
    void* frame_buffer;
    u64 frame_buffer_size;
};

struct SceVdecCoreInputData {
    void* au_data;
    u64 au_size;
    u64 pts;
    u64 dts;
    u64 user_data;
    u32 flags;
    u32 stream_id;
};

struct SceVdecCorePicture {
    u32 flags;
    u32 reserved0;
    u64 pts;
    u64 dts;
    u64 user_data;
    void* picture_info;
    void* frame_buffer;
    u32 frame_buffer_size;
    u32 frame_pitch;
    u64 reserved1;
};

struct SceVdecCoreOutputInfo {
    u32 codec_type;
    u32 frame_width;
    u32 frame_height;
    u32 picture_count;
    void* frame_buffer;
    u32 frame_format;
    u32 reserved0;
    SceVdecCorePicture pictures[2];
};

struct SceVdecCoreMemoryBlock {
    void* addr;
    u64 size;
    u32 type;
    u32 reserved;
};

struct VdecCoreDecoder {
    AVCodecContext* codec_context = nullptr;
    AVCodecParserContext* parser = nullptr;
    SwsContext* sws_context = nullptr;
    AVFrame* decoded_frame = nullptr;

    AVCodecID codec_id = AV_CODEC_ID_NONE;
    u32 codec_type = 0;
    u32 max_width = 1920;
    u32 max_height = 1080;
    u32 max_dpb = 16;
    u32 frame_buffer_size = 0;

    std::vector<SceVdecCoreMemoryBlock> mapped_blocks;
    std::vector<void*> frame_buffer_slots;
    u32 current_slot_index = 0;

    std::vector<u8> fallback_frame_buffer;
    u64 last_pts = 0;
    u64 last_dts = 0;
    u64 last_user_data = 0;
};

static AVFrame* ConvertNV12Frame(VdecCoreDecoder* dec, AVFrame& frame) {
    AVFrame* nv12_frame = av_frame_alloc();
    nv12_frame->format = AV_PIX_FMT_NV12;
    nv12_frame->width = frame.width;
    nv12_frame->height = frame.height;
    nv12_frame->pts = frame.pts;
    nv12_frame->pkt_dts = frame.pkt_dts;
    nv12_frame->opaque = frame.opaque;
    av_frame_get_buffer(nv12_frame, 0);

    dec->sws_context = sws_getCachedContext(
        dec->sws_context, frame.width, frame.height, static_cast<AVPixelFormat>(frame.format),
        frame.width, frame.height, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
    sws_scale(dec->sws_context, frame.data, frame.linesize, 0, frame.height, nv12_frame->data,
              nv12_frame->linesize);
    return nv12_frame;
}

static s32 PS4_SYSV_ABI sceVdecCoreQueryInstanceSize(const SceVdecCoreConfigInfo* config,
                                                     u64* instance_size, u64* compute_size,
                                                     u64* reserved_size) {
    LOG_DEBUG(Lib_Videodec, "sceVdecCoreQueryInstanceSize called");
    if (instance_size) {
        *instance_size = 0x40000;
    }
    if (compute_size) {
        *compute_size = 0x10000;
    }
    if (reserved_size) {
        *reserved_size = 0;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVdecCoreQueryFrameBufferInfo(const SceVdecCoreConfigInfo* config,
                                                        SceVdecCoreFrameBufferInfo* fb_info) {
    if (!fb_info) {
        return 0x80c00001;
    }
    u32 width = config ? config->max_frame_width : 1920;
    u32 height = config ? config->max_frame_height : 1080;
    if (width == 0 || width == 0xffffffff)
        width = 1920;
    if (height == 0 || height == 0xffffffff)
        height = 1080;

    u32 dpb = config ? config->max_dpb_frame_count : 16;
    if (dpb == 0 || dpb == 0xffffffff) {
        dpb = (config && config->codec_type == 1) ? 4 : 16;
    }

    u32 pitch = Common::AlignUp<u32>(width, 64);
    u32 aligned_height = Common::AlignUp<u32>(height, 16);
    u32 size = (pitch * aligned_height * 3) / 2;

    fb_info->frame_buffer_size = Common::AlignUp<u32>(size + 0x1000, 0x10000);
    fb_info->frame_buffer_alignment = 256;
    fb_info->min_frame_buffer_count = dpb + 1;
    fb_info->reserved0 = 0;
    fb_info->reserved1 = 0;
    fb_info->reserved2 = 0;

    LOG_INFO(Lib_Videodec,
             "sceVdecCoreQueryFrameBufferInfo: fb_size={:#x}, align={}, min_count={}, w={}, h={}",
             fb_info->frame_buffer_size, fb_info->frame_buffer_alignment,
             fb_info->min_frame_buffer_count, width, height);
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVdecCoreCreateDecoder(const void* config, const void* mem_info,
                                                 void* compute_queue,
                                                 VdecCoreDecoder** out_decoder) {
    LOG_INFO(Lib_Videodec, "sceVdecCoreCreateDecoder called");
    if (!config || !out_decoder) {
        return 0x80c00001;
    }

    u32 codec_type = 0;
    u32 width = 1920;
    u32 height = 1080;
    u32 dpb = 16;

    const u64* ptr64 = reinterpret_cast<const u64*>(config);
    if (*ptr64 == 0x48) {
        // OrbisVideodec2DecoderConfigInfo layout
        codec_type = *reinterpret_cast<const u32*>(reinterpret_cast<const u8*>(config) + 0xc);
        width = *reinterpret_cast<const u32*>(reinterpret_cast<const u8*>(config) + 0x18);
        height = *reinterpret_cast<const u32*>(reinterpret_cast<const u8*>(config) + 0x1c);
        dpb = *reinterpret_cast<const u32*>(reinterpret_cast<const u8*>(config) + 0x20);
    } else {
        const auto* c = reinterpret_cast<const SceVdecCoreConfigInfo*>(config);
        codec_type = c->codec_type;
        dpb = c->max_dpb_frame_count;
        width = c->max_frame_width;
        height = c->max_frame_height;
    }

    if (width == 0 || width == 0xffffffff)
        width = 1920;
    if (height == 0 || height == 0xffffffff)
        height = 1080;
    if (dpb == 0 || dpb == 0xffffffff)
        dpb = (codec_type == 1) ? 4 : 16;

    LOG_INFO(Lib_Videodec, "VdecCore: config codec_type={}, width={}, height={}, dpb={}",
             codec_type, width, height, dpb);

    auto* dec = new VdecCoreDecoder();
    dec->codec_type = codec_type;
    dec->max_width = width;
    dec->max_height = height;
    dec->max_dpb = dpb;

    AVCodecID codec_id = AV_CODEC_ID_H264;
    switch (codec_type) {
    case 0:
        codec_id = AV_CODEC_ID_H264;
        break;
    case 1:
        codec_id = AV_CODEC_ID_MPEG2VIDEO;
        break;
    case 2:
    case 3:
    case 4:
        codec_id = AV_CODEC_ID_MPEG4;
        break;
    case 974921:
        codec_id = AV_CODEC_ID_HEVC;
        break;
    default:
        codec_id = AV_CODEC_ID_H264;
        break;
    }

    dec->codec_id = codec_id;
    LOG_INFO(Lib_Videodec, "VdecCore: creating decoder codec_type={}, ffmpeg_codec_id={}",
             codec_type, static_cast<int>(codec_id));
    const AVCodec* codec = avcodec_find_decoder(codec_id);
    if (!codec) {
        LOG_WARNING(Lib_Videodec, "VdecCore: codec not found, falling back to H264");
        codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    }
    ASSERT(codec);

    dec->codec_context = avcodec_alloc_context3(codec);
    ASSERT(dec->codec_context);
    dec->codec_context->width = dec->max_width;
    dec->codec_context->height = dec->max_height;
    dec->codec_context->flags |= AV_CODEC_FLAG_COPY_OPAQUE;

    int ret = avcodec_open2(dec->codec_context, codec, nullptr);
    if (ret < 0) {
        LOG_ERROR(Lib_Videodec, "VdecCore: avcodec_open2 failed: {}", ret);
    }

    dec->parser = av_parser_init(codec->id);

    dec->decoded_frame = av_frame_alloc();

    u32 pitch = Common::AlignUp<u32>(dec->max_width, 64);
    u32 aligned_height = Common::AlignUp<u32>(dec->max_height, 16);
    dec->frame_buffer_size =
        Common::AlignUp<u32>((pitch * aligned_height * 3) / 2 + 0x1000, 0x10000);

    *out_decoder = dec;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVdecCoreDeleteDecoder(VdecCoreDecoder* decoder) {
    LOG_INFO(Lib_Videodec, "sceVdecCoreDeleteDecoder called");
    if (!decoder) {
        return 0x80c00001;
    }
    if (decoder->parser) {
        av_parser_close(decoder->parser);
    }
    if (decoder->decoded_frame) {
        av_frame_free(&decoder->decoded_frame);
    }
    if (decoder->codec_context) {
        avcodec_free_context(&decoder->codec_context);
    }
    if (decoder->sws_context) {
        sws_freeContext(decoder->sws_context);
    }
    delete decoder;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVdecCoreResetDecoder(VdecCoreDecoder* decoder, void* reset_param) {
    LOG_INFO(Lib_Videodec, "sceVdecCoreResetDecoder called");
    if (!decoder) {
        return 0x80c00001;
    }
    if (decoder->codec_context) {
        avcodec_flush_buffers(decoder->codec_context);
    }
    if (decoder->parser) {
        av_parser_close(decoder->parser);
        decoder->parser = av_parser_init(decoder->codec_id);
    }
    decoder->current_slot_index = 0;
    return ORBIS_OK;
}

static std::atomic<u64> s_vdec_input_packets{0};
static std::atomic<u64> s_vdec_output_frames{0};

static s32 PS4_SYSV_ABI sceVdecCoreSetDecodeInput(VdecCoreDecoder* decoder,
                                                  const SceVdecCoreInputData* input_data,
                                                  void* reserved0, void* reserved1) {
    if (!decoder || !input_data) {
        return 0x80c00001;
    }
    if (reserved1) {
        *reinterpret_cast<u64*>(reserved1) = 1;
    }
    if (!input_data->au_data || input_data->au_size == 0) {
        return 0x80c00001;
    }

    u64 in_idx = ++s_vdec_input_packets;
    if (in_idx <= 25 || in_idx % 120 == 0) {
        LOG_INFO(Lib_Videodec,
                 "VdecCore: SetDecodeInput #{} size={}, pts={}, dts={}, user_data={:#x}", in_idx,
                 input_data->au_size, input_data->pts, input_data->dts, input_data->user_data);
    }

    decoder->last_pts = input_data->pts;
    decoder->last_dts = input_data->dts;
    decoder->last_user_data = input_data->user_data;

    const s64 pts = (input_data->pts == 0xffffffffffffffffULL) ? AV_NOPTS_VALUE
                                                               : static_cast<s64>(input_data->pts);
    const s64 dts = (input_data->dts == 0xffffffffffffffffULL) ? AV_NOPTS_VALUE
                                                               : static_cast<s64>(input_data->dts);

    if (decoder->parser) {
        const u8* cur = static_cast<const u8*>(input_data->au_data);
        int cur_size = static_cast<int>(input_data->au_size);

        while (cur_size > 0) {
            u8* pout = nullptr;
            int pout_size = 0;
            int len = av_parser_parse2(decoder->parser, decoder->codec_context, &pout, &pout_size,
                                       cur, cur_size, pts, dts, 0);
            if (len <= 0) {
                break;
            }
            cur += len;
            cur_size -= len;

            if (pout_size > 0) {
                AVPacket* packet = av_packet_alloc();
                if (packet) {
                    packet->data = pout;
                    packet->size = pout_size;
                    packet->pts =
                        (decoder->parser->pts != AV_NOPTS_VALUE) ? decoder->parser->pts : pts;
                    packet->dts =
                        (decoder->parser->dts != AV_NOPTS_VALUE) ? decoder->parser->dts : dts;
                    packet->opaque = reinterpret_cast<void*>(input_data->user_data);

                    int ret = avcodec_send_packet(decoder->codec_context, packet);
                    if (ret == AVERROR_EOF) {
                        avcodec_flush_buffers(decoder->codec_context);
                        ret = avcodec_send_packet(decoder->codec_context, packet);
                    }
                    av_packet_free(&packet);
                    if (ret < 0 && ret != AVERROR(EAGAIN)) {
                        LOG_WARNING(Lib_Videodec, "VdecCore: avcodec_send_packet warning: {}", ret);
                    }
                }
            }
        }
    } else {
        AVPacket* packet = av_packet_alloc();
        if (packet) {
            packet->data = static_cast<u8*>(input_data->au_data);
            packet->size = static_cast<int>(input_data->au_size);
            packet->pts = pts;
            packet->dts = dts;
            packet->opaque = reinterpret_cast<void*>(input_data->user_data);

            int ret = avcodec_send_packet(decoder->codec_context, packet);
            if (ret == AVERROR_EOF) {
                avcodec_flush_buffers(decoder->codec_context);
                ret = avcodec_send_packet(decoder->codec_context, packet);
            }
            av_packet_free(&packet);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                LOG_WARNING(Lib_Videodec, "VdecCore: avcodec_send_packet warning: {}", ret);
            }
        }
    }

    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVdecCoreSyncDecode(VdecCoreDecoder* decoder, void* reserved) {
    if (!decoder) {
        return 0x80c00001;
    }
    return ORBIS_OK;
}

static std::atomic<u64> s_vdec_get_output_calls{0};

static s32 PS4_SYSV_ABI sceVdecCoreGetDecodeOutput(VdecCoreDecoder* decoder,
                                                   SceVdecCoreFrameBuffer* frame_buf, u32 flags,
                                                   SceVdecCoreOutputInfo* output_info,
                                                   void* reserved) {
    if (!decoder || !output_info) {
        return 0x80c00001;
    }

    u64 call_idx = ++s_vdec_get_output_calls;
    int ret = avcodec_receive_frame(decoder->codec_context, decoder->decoded_frame);
    if (call_idx <= 25 || call_idx % 120 == 0) {
        LOG_INFO(Lib_Videodec, "VdecCore: GetDecodeOutput call #{} avcodec_receive_frame ret={}",
                 call_idx, ret);
    }
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        output_info->picture_count = 0;
        output_info->codec_type = decoder->codec_type;
        output_info->frame_width = decoder->max_width;
        output_info->frame_height = decoder->max_height;
        output_info->frame_buffer = nullptr;
        output_info->pictures[0].frame_buffer = nullptr;
        output_info->pictures[1].frame_buffer = nullptr;
        return 0x80c00010; // SCE_VDEC_ERROR_NO_PICTURE
    }
    if (ret < 0) {
        LOG_ERROR(Lib_Videodec, "VdecCore: avcodec_receive_frame error: {}", ret);
        output_info->picture_count = 0;
        return 0x80c00001;
    }

    AVFrame* frame = decoder->decoded_frame;
    bool needs_free = false;
    if (frame->format != AV_PIX_FMT_NV12) {
        frame = ConvertNV12Frame(decoder, *frame);
        needs_free = true;
    }

    u32 width = frame->width;
    u32 height = frame->height;
    u32 pitch = Common::AlignUp<u32>(width, 64);
    u32 aligned_height = Common::AlignUp<u32>(height, 16);
    u32 fb_size = (pitch * aligned_height * 3) / 2;

    void* dest_buf = nullptr;
    if (frame_buf && frame_buf->frame_buffer) {
        dest_buf = frame_buf->frame_buffer;
    } else if (!decoder->frame_buffer_slots.empty()) {
        dest_buf = decoder->frame_buffer_slots[decoder->current_slot_index %
                                               decoder->frame_buffer_slots.size()];
        decoder->current_slot_index++;
    } else {
        if (decoder->fallback_frame_buffer.size() < fb_size) {
            decoder->fallback_frame_buffer.resize(fb_size);
        }
        dest_buf = decoder->fallback_frame_buffer.data();
    }

    Videodec::CopyNV12Data(static_cast<u8*>(dest_buf), fb_size, *frame);

    output_info->codec_type = decoder->codec_type;
    output_info->frame_width = width;
    output_info->frame_height = height;
    output_info->picture_count = 1;
    output_info->frame_buffer = dest_buf;
    output_info->frame_format = 0; // NV12
    output_info->pictures[0].flags = 0;
    output_info->pictures[0].pts = (frame->pts != AV_NOPTS_VALUE) ? frame->pts : decoder->last_pts;
    output_info->pictures[0].dts =
        (frame->pkt_dts != AV_NOPTS_VALUE) ? frame->pkt_dts : decoder->last_dts;
    output_info->pictures[0].user_data = reinterpret_cast<u64>(frame->opaque);
    output_info->pictures[0].picture_info = nullptr;
    output_info->pictures[0].frame_buffer = dest_buf;
    output_info->pictures[0].frame_buffer_size = fb_size;
    output_info->pictures[0].frame_pitch = pitch;

    u64 out_idx = ++s_vdec_output_frames;
    if (out_idx <= 25 || out_idx % 120 == 0) {
        LOG_INFO(Lib_Videodec, "VdecCore: GetDecodeOutput #{} frame {}x{}, pts={}, dest_buf={:p}",
                 out_idx, width, height, output_info->pictures[0].pts, dest_buf);
    }

    if (needs_free) {
        av_frame_free(&frame);
    }

    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVdecCoreFlushDecodeOutput(VdecCoreDecoder* decoder) {
    LOG_INFO(Lib_Videodec, "sceVdecCoreFlushDecodeOutput called");
    if (!decoder) {
        return 0x80c00001;
    }
    if (decoder->codec_context) {
        avcodec_flush_buffers(decoder->codec_context);
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVdecCoreMapMemoryBlock(VdecCoreDecoder* decoder,
                                                  const SceVdecCoreMemoryBlock* blocks, u32 count) {
    LOG_INFO(Lib_Videodec, "sceVdecCoreMapMemoryBlock called with count={}", count);
    if (!decoder) {
        return 0x80c00001;
    }
    if (blocks && count > 0) {
        for (u32 i = 0; i < count; ++i) {
            decoder->mapped_blocks.push_back(blocks[i]);
            u8* base = reinterpret_cast<u8*>(blocks[i].addr);
            u64 size = blocks[i].size;
            LOG_INFO(Lib_Videodec, "VdecCore: mapped block[{}] addr={:p}, size={:#x}", i,
                     static_cast<void*>(base), size);
            if (base && size >= decoder->frame_buffer_size) {
                // Check if buffer starts with 0x120000 offset for VNEE frame buffers
                u64 start_offset = (size >= 0x120000 + decoder->frame_buffer_size) ? 0x120000 : 0;
                u32 num_slots =
                    static_cast<u32>((size - start_offset) / decoder->frame_buffer_size);
                for (u32 s = 0; s < num_slots; ++s) {
                    void* slot_addr = base + start_offset + s * decoder->frame_buffer_size;
                    decoder->frame_buffer_slots.push_back(slot_addr);
                    LOG_DEBUG(Lib_Videodec, "VdecCore: created frame buffer slot[{}] at {:p}", s,
                              slot_addr);
                }
            }
        }
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVdecCoreAnalyzeStream(void* reserved) {
    LOG_DEBUG(Lib_Videodec, "sceVdecCoreAnalyzeStream stub called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVdecCoreQueryComputeResourceInfo(void* info) {
    LOG_DEBUG(Lib_Videodec, "sceVdecCoreQueryComputeResourceInfo stub called");
    if (info) {
        *reinterpret_cast<u64*>(info) = 0x10000;
        *(reinterpret_cast<u64*>(info) + 1) = 0x10000;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVdecCoreInitializeComputeResource(void* res, void* reserved) {
    LOG_DEBUG(Lib_Videodec, "sceVdecCoreInitializeComputeResource stub called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVdecCoreFinalizeComputeResource(void* res) {
    LOG_DEBUG(Lib_Videodec, "sceVdecCoreFinalizeComputeResource stub called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVdecCoreSetDecodeOutputSw(void* decoder, void* out) {
    LOG_DEBUG(Lib_Videodec, "sceVdecCoreSetDecodeOutputSw stub called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVdecCoreSyncDecodeOutputSw(void* decoder, void* out) {
    LOG_DEBUG(Lib_Videodec, "sceVdecCoreSyncDecodeOutputSw stub called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVdecCoreTrySyncDecodeOutputSw(void* decoder, void* out) {
    LOG_DEBUG(Lib_Videodec, "sceVdecCoreTrySyncDecodeOutputSw stub called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVdecCoreSyncDecodeWptr(void* decoder, void* out) {
    LOG_DEBUG(Lib_Videodec, "sceVdecCoreSyncDecodeWptr stub called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVdecCoreTrySyncDecode(void* decoder, void* out) {
    LOG_DEBUG(Lib_Videodec, "sceVdecCoreTrySyncDecode stub called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceVdecCoreTrySyncDecodeWptr(void* decoder, void* out) {
    LOG_DEBUG(Lib_Videodec, "sceVdecCoreTrySyncDecodeWptr stub called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("Zsh1K8YTD1E", "libSceVdecCore", 1, "libSceVdecCore",
                 sceVdecCoreQueryInstanceSize);
    LIB_FUNCTION("UxV5adxfBFg", "libSceVdecCore", 1, "libSceVdecCore",
                 sceVdecCoreQueryFrameBufferInfo);
    LIB_FUNCTION("wnWMkCYv0m4", "libSceVdecCore", 1, "libSceVdecCore", sceVdecCoreCreateDecoder);
    LIB_FUNCTION("D0RWslNmK9s", "libSceVdecCore", 1, "libSceVdecCore", sceVdecCoreDeleteDecoder);
    LIB_FUNCTION("nFn-3CWQEyo", "libSceVdecCore", 1, "libSceVdecCore", sceVdecCoreResetDecoder);
    LIB_FUNCTION("F4nrEfi-M84", "libSceVdecCore", 1, "libSceVdecCore", sceVdecCoreSetDecodeInput);
    LIB_FUNCTION("OyxRC7GT4Es", "libSceVdecCore", 1, "libSceVdecCore", sceVdecCoreSyncDecode);
    LIB_FUNCTION("K4sH-0WUfuc", "libSceVdecCore", 1, "libSceVdecCore", sceVdecCoreGetDecodeOutput);
    LIB_FUNCTION("b0xbD0x+02M", "libSceVdecCore", 1, "libSceVdecCore",
                 sceVdecCoreFlushDecodeOutput);
    LIB_FUNCTION("7YW9rloMLYo", "libSceVdecCore", 1, "libSceVdecCore", sceVdecCoreMapMemoryBlock);

    LIB_FUNCTION("bWPuW6TnMjk", "libSceVdecCore", 1, "libSceVdecCore", sceVdecCoreAnalyzeStream);
    LIB_FUNCTION("zwcpjJ7WUgQ", "libSceVdecCore", 1, "libSceVdecCore",
                 sceVdecCoreQueryComputeResourceInfo);
    LIB_FUNCTION("HPpz-oyX4vM", "libSceVdecCore", 1, "libSceVdecCore",
                 sceVdecCoreInitializeComputeResource);
    LIB_FUNCTION("DHQXNKiY-Dw", "libSceVdecCore", 1, "libSceVdecCore",
                 sceVdecCoreFinalizeComputeResource);
    LIB_FUNCTION("hF8Pz0rtPLU", "libSceVdecCore", 1, "libSceVdecCore",
                 sceVdecCoreSetDecodeOutputSw);
    LIB_FUNCTION("1hE5Sdn4U3U", "libSceVdecCore", 1, "libSceVdecCore",
                 sceVdecCoreSyncDecodeOutputSw);
    LIB_FUNCTION("WMhw0IVNNC8", "libSceVdecCore", 1, "libSceVdecCore",
                 sceVdecCoreTrySyncDecodeOutputSw);
    LIB_FUNCTION("JFcSDC6AD+k", "libSceVdecCore", 1, "libSceVdecCore", sceVdecCoreSyncDecodeWptr);
    LIB_FUNCTION("iQRVLJQ+OYs", "libSceVdecCore", 1, "libSceVdecCore", sceVdecCoreTrySyncDecode);
    LIB_FUNCTION("T9yCBYaGDew", "libSceVdecCore", 1, "libSceVdecCore",
                 sceVdecCoreTrySyncDecodeWptr);
}

} // namespace Libraries::VdecCore
