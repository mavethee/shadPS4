// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ajm_error.h"
#include "ajm_ffmpeg.h"
#include "ajm_result.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"

#include <magic_enum/magic_enum.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include "common/support/avdec.h"

namespace Libraries::Ajm {

static AVSampleFormat AjmToAVSampleFormat(AjmFormatEncoding format) {
    switch (format) {
    case AjmFormatEncoding::S16:
        return AV_SAMPLE_FMT_S16;
    case AjmFormatEncoding::S32:
        return AV_SAMPLE_FMT_S32;
    case AjmFormatEncoding::Float:
        return AV_SAMPLE_FMT_FLT;
    default:
        return AV_SAMPLE_FMT_S16;
    }
}

AVCodecID AjmFFmpegDecoder::CodecTypeToAVCodecID(AjmCodecType codec_type) const {
    switch (codec_type) {
    case AjmCodecType::Mp3Dec:
        return AV_CODEC_ID_MP3;
    case AjmCodecType::At9Dec:
        return AV_CODEC_ID_ATRAC9;
    case AjmCodecType::M4aacDec:
    case AjmCodecType::M4aacDecNoGame:
        return AV_CODEC_ID_AAC;
    case AjmCodecType::Ac3Dec:
        return AV_CODEC_ID_AC3;
    case AjmCodecType::Mpeg2BcDec:
        return AV_CODEC_ID_MP2;
    case AjmCodecType::VorbisDec:
        return AV_CODEC_ID_VORBIS;
    case AjmCodecType::FlacDec:
        return AV_CODEC_ID_FLAC;
    case AjmCodecType::OpusDec:
    case AjmCodecType::OpusCeltDec:
    case AjmCodecType::OpusSilkDec:
        return AV_CODEC_ID_OPUS;
    default:
        LOG_WARNING(Lib_Ajm, "Unsupported or unknown codec type {}",
                    magic_enum::enum_name(codec_type));
        return AV_CODEC_ID_NONE;
    }
}

AjmFFmpegDecoder::AjmFFmpegDecoder(AjmCodecType codec_type, AjmFormatEncoding format, u64 flags,
                                   u32 channels)
    : m_codec_type(codec_type), m_format(format), m_flags(flags), m_target_channels(channels) {
    const AVCodecID codec_id = CodecTypeToAVCodecID(codec_type);
    if (codec_id == AV_CODEC_ID_NONE) {
        LOG_ERROR(Lib_Ajm, "Unsupported codec type {}", magic_enum::enum_name(codec_type));
        return;
    }

    m_codec = avcodec_find_decoder(codec_id);
    if (!m_codec) {
        LOG_ERROR(Lib_Ajm, "Decoder not found for AVCodecID {}", static_cast<int>(codec_id));
        return;
    }

    m_codec_context = avcodec_alloc_context3(m_codec);
    if (!m_codec_context) {
        LOG_ERROR(Lib_Ajm, "Failed to allocate AVCodecContext");
        return;
    }

    if (m_target_channels > 0) {
        av_channel_layout_default(&m_codec_context->ch_layout, m_target_channels);
    }

    switch (codec_type) {
    case AjmCodecType::Ac3Dec:
        m_frame_samples = 1536;
        break;
    case AjmCodecType::Mpeg2BcDec:
        m_frame_samples = 1152;
        break;
    default:
        m_frame_samples = 1024;
        break;
    }

    int ret = avcodec_open2(m_codec_context, m_codec, nullptr);
    if (ret < 0) {
        LOG_ERROR(Lib_Ajm, "Could not open codec: {}", av_err2str(ret));
        avcodec_free_context(&m_codec_context);
    }
}

AjmFFmpegDecoder::~AjmFFmpegDecoder() {
    if (m_swr_context) {
        swr_free(&m_swr_context);
    }
    av_channel_layout_uninit(&m_swr_in_ch_layout);
    if (m_codec_context) {
        avcodec_free_context(&m_codec_context);
    }
}

void AjmFFmpegDecoder::Reset() {
    if (m_codec_context) {
        avcodec_flush_buffers(m_codec_context);
    }
    if (m_swr_context) {
        swr_free(&m_swr_context);
        m_swr_context = nullptr;
    }
    av_channel_layout_uninit(&m_swr_in_ch_layout);
    m_swr_in_rate = 0;
    m_swr_in_format = -1;
}

void AjmFFmpegDecoder::Initialize(const void* buffer, u32 buffer_size) {
    if (buffer && buffer_size > 0 && m_codec_context) {
        if (m_codec_type == AjmCodecType::M4aacDec ||
            m_codec_type == AjmCodecType::M4aacDecNoGame) {
            if (buffer_size == 8) {
                // SceAjm sideband init parameters: config_type (1 = ADTS, 2 = RAW),
                // sampling_freq_idx
                const u32* params = static_cast<const u32*>(buffer);
                const u32 config_type = params[0];
                const u32 freq_idx = params[1];
                if (config_type == 2 && freq_idx <= 11) { // RAW AAC requires AudioSpecificConfig
                    const u32 channels = m_target_channels > 0 ? m_target_channels : 2;
                    m_codec_context->extradata =
                        static_cast<uint8_t*>(av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE));
                    if (m_codec_context->extradata) {
                        m_codec_context->extradata[0] =
                            static_cast<uint8_t>((2 << 3) | ((freq_idx >> 1) & 0x7));
                        m_codec_context->extradata[1] =
                            static_cast<uint8_t>(((freq_idx & 1) << 7) | ((channels & 0xF) << 3));
                        m_codec_context->extradata_size = 2;
                    }
                }
            } else if (buffer_size >= 2 && !m_codec_context->extradata) {
                m_codec_context->extradata =
                    static_cast<uint8_t*>(av_mallocz(buffer_size + AV_INPUT_BUFFER_PADDING_SIZE));
                if (m_codec_context->extradata) {
                    std::memcpy(m_codec_context->extradata, buffer, buffer_size);
                    m_codec_context->extradata_size = buffer_size;
                }
            }
        }
    }
    Reset();
}

void AjmFFmpegDecoder::GetInfo(void* out_info) const {
    if (!out_info) {
        return;
    }
    // Generic sideband info fallback
    std::memset(out_info, 0, 32);
}

AjmSidebandFormat AjmFFmpegDecoder::GetFormat() const {
    u32 channels = m_target_channels;
    if (channels == 0 && m_codec_context && m_codec_context->ch_layout.nb_channels > 0) {
        channels = m_codec_context->ch_layout.nb_channels;
    }
    if (channels == 0) {
        channels = 2;
    }

    u32 sample_rate = 48000;
    if (m_codec_context && m_codec_context->sample_rate > 0) {
        sample_rate = m_codec_context->sample_rate;
    }

    u32 bitrate = 0;
    if (m_codec_context && m_codec_context->bit_rate > 0) {
        bitrate = static_cast<u32>(m_codec_context->bit_rate);
    }

    return {
        .num_channels = channels,
        .channel_mask = GetChannelMask(channels),
        .sampl_freq = sample_rate,
        .sample_encoding = m_format,
        .bitrate = bitrate,
    };
}

u32 AjmFFmpegDecoder::GetMinimumInputSize() const {
    return 1;
}

u32 AjmFFmpegDecoder::GetNextFrameSize(const AjmInstanceGapless& gapless) const {
    u32 channels = m_target_channels;
    if (channels == 0 && m_codec_context && m_codec_context->ch_layout.nb_channels > 0) {
        channels = m_codec_context->ch_layout.nb_channels;
    }
    if (channels == 0) {
        channels = 2;
    }

    const auto skip_samples = std::min<u32>(gapless.current.skip_samples, m_frame_samples);
    const auto samples =
        gapless.init.total_samples != 0
            ? std::min<u32>(gapless.current.total_samples, m_frame_samples - skip_samples)
            : m_frame_samples - skip_samples;
    return samples * channels * GetPCMSize(m_format);
}

AVFrame* AjmFFmpegDecoder::ConvertAudioFrame(AVFrame* frame) {
    const AVSampleFormat target_format = AjmToAVSampleFormat(m_format);

    AVChannelLayout out_ch_layout{};
    if (m_target_channels > 0) {
        av_channel_layout_default(&out_ch_layout, m_target_channels);
    } else if (frame->ch_layout.nb_channels > 0) {
        av_channel_layout_copy(&out_ch_layout, &frame->ch_layout);
    } else {
        av_channel_layout_default(&out_ch_layout, 2);
    }

    const bool format_match = (frame->format == target_format);
    const bool layout_match = (av_channel_layout_compare(&frame->ch_layout, &out_ch_layout) == 0);

    if (format_match && layout_match) {
        av_channel_layout_uninit(&out_ch_layout);
        return frame;
    }

    AVFrame* new_frame = av_frame_alloc();
    new_frame->pts = frame->pts;
    new_frame->pkt_dts = frame->pkt_dts < 0 ? 0 : frame->pkt_dts;
    new_frame->format = target_format;
    av_channel_layout_copy(&new_frame->ch_layout, &out_ch_layout);
    new_frame->sample_rate = frame->sample_rate;

    const bool swr_needs_reinit =
        !m_swr_context || (m_swr_in_rate != frame->sample_rate) ||
        (m_swr_in_format != frame->format) ||
        (av_channel_layout_compare(&m_swr_in_ch_layout, &frame->ch_layout) != 0);

    if (swr_needs_reinit) {
        if (m_swr_context) {
            swr_free(&m_swr_context);
            m_swr_context = nullptr;
        }
        av_channel_layout_uninit(&m_swr_in_ch_layout);
        av_channel_layout_copy(&m_swr_in_ch_layout, &frame->ch_layout);
        m_swr_in_rate = frame->sample_rate;
        m_swr_in_format = frame->format;

        swr_alloc_set_opts2(&m_swr_context, &out_ch_layout, target_format, frame->sample_rate,
                            &frame->ch_layout, static_cast<AVSampleFormat>(frame->format),
                            frame->sample_rate, 0, nullptr);
        if (!m_swr_context || swr_init(m_swr_context) < 0) {
            LOG_ERROR(Lib_Ajm, "Failed to initialize SwrContext");
            av_frame_free(&new_frame);
            av_channel_layout_uninit(&out_ch_layout);
            return frame;
        }
    }

    int ret = swr_convert_frame(m_swr_context, new_frame, frame);
    if (ret < 0) {
        LOG_ERROR(Lib_Ajm, "swr_convert_frame failed: {}", av_err2str(ret));
        av_frame_free(&new_frame);
        av_channel_layout_uninit(&out_ch_layout);
        return frame;
    }

    av_channel_layout_uninit(&out_ch_layout);
    av_frame_free(&frame);
    return new_frame;
}

DecoderResult AjmFFmpegDecoder::ProcessData(std::span<u8>& in_buf, SparseOutputBuffer& output,
                                            AjmInstanceGapless& gapless) {
    DecoderResult result{};
    if (!m_codec_context) {
        result.result |= ORBIS_AJM_RESULT_CODEC_ERROR | ORBIS_AJM_RESULT_FATAL;
        return result;
    }

    if (in_buf.empty()) {
        result.result |= ORBIS_AJM_RESULT_PARTIAL_INPUT;
        return result;
    }

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        result.result |= ORBIS_AJM_RESULT_CODEC_ERROR | ORBIS_AJM_RESULT_FATAL;
        return result;
    }

    pkt->data = in_buf.data();
    pkt->size = static_cast<int>(in_buf.size());
    in_buf = in_buf.subspan(in_buf.size());

    auto drain_frames = [&]() {
        while (true) {
            AVFrame* frame = av_frame_alloc();
            int recv_ret = avcodec_receive_frame(m_codec_context, frame);
            if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF || recv_ret < 0) {
                av_frame_free(&frame);
                break;
            }

            if (frame->nb_samples > 0) {
                m_frame_samples = frame->nb_samples;
            }

            frame = ConvertAudioFrame(frame);
            result.frames_decoded += 1;

            u32 skip_samples = 0;
            if (gapless.current.skip_samples > 0) {
                skip_samples = std::min<u16>(frame->nb_samples, gapless.current.skip_samples);
                gapless.current.skip_samples -= skip_samples;
            }

            const u32 channels = frame->ch_layout.nb_channels;
            const auto max_pcm = gapless.init.total_samples != 0
                                     ? gapless.current.total_samples * channels
                                     : std::numeric_limits<u32>::max();

            u32 pcm_written = 0;
            switch (m_format) {
            case AjmFormatEncoding::S16:
                pcm_written = WriteOutputPCM<s16>(frame, output, skip_samples, max_pcm);
                break;
            case AjmFormatEncoding::S32:
                pcm_written = WriteOutputPCM<s32>(frame, output, skip_samples, max_pcm);
                break;
            case AjmFormatEncoding::Float:
                pcm_written = WriteOutputPCM<float>(frame, output, skip_samples, max_pcm);
                break;
            }

            const auto samples = channels > 0 ? (pcm_written / channels) : 0;
            gapless.current.skipped_samples += frame->nb_samples - samples;
            if (gapless.init.total_samples != 0) {
                gapless.current.total_samples -= samples;
            }
            result.samples_written += samples;

            av_frame_free(&frame);
        }
    };

    int ret = avcodec_send_packet(m_codec_context, pkt);
    if (ret == AVERROR(EAGAIN)) {
        drain_frames();
        ret = avcodec_send_packet(m_codec_context, pkt);
    }
    if (ret < 0 && ret != AVERROR_EOF) {
        LOG_TRACE(Lib_Ajm, "avcodec_send_packet status: {}", av_err2str(ret));
    }

    drain_frames();
    av_packet_free(&pkt);
    return result;
}

} // namespace Libraries::Ajm
