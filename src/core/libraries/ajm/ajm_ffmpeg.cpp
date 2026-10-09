// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ajm_error.h"
#include "ajm_ffmpeg.h"
#include "ajm_result.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"

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
    case AjmCodecType::HeaacDec:
        return AV_CODEC_ID_AAC;
    case AjmCodecType::Ac3Dec:
        return AV_CODEC_ID_AC3;
    case AjmCodecType::EAc3Dec:
        return AV_CODEC_ID_EAC3;
    case AjmCodecType::Mpeg2BcDec:
        return AV_CODEC_ID_MP2;
    case AjmCodecType::DtsDec:
    case AjmCodecType::DtsHdDec:
    case AjmCodecType::DtsHdMaDec:
        return AV_CODEC_ID_DTS;
    default:
        LOG_WARNING(Lib_Ajm, "Unknown codec type {}, defaulting to MP3",
                    static_cast<u32>(codec_type));
        return AV_CODEC_ID_MP3;
    }
}

AjmFFmpegDecoder::AjmFFmpegDecoder(AjmCodecType codec_type, AjmFormatEncoding format, u64 flags,
                                   u32 channels)
    : m_codec_type(codec_type), m_format(format), m_flags(flags), m_target_channels(channels) {
    const AVCodecID codec_id = CodecTypeToAVCodecID(codec_type);
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
    case AjmCodecType::EAc3Dec:
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
    }
}

AjmFFmpegDecoder::~AjmFFmpegDecoder() {
    if (m_swr_context) {
        swr_free(&m_swr_context);
    }
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
}

void AjmFFmpegDecoder::Initialize(const void* buffer, u32 buffer_size) {
    if (buffer && buffer_size > 0 && m_codec_context) {
        // Some demuxers pass AudioSpecificConfig or extradata
        if (m_codec_type == AjmCodecType::M4aacDec || m_codec_type == AjmCodecType::HeaacDec) {
            if (buffer_size >= 2 && !m_codec_context->extradata) {
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

    if (!m_swr_context) {
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

    int ret = avcodec_send_packet(m_codec_context, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        LOG_TRACE(Lib_Ajm, "avcodec_send_packet status: {}", av_err2str(ret));
    }

    while (ret >= 0) {
        AVFrame* frame = av_frame_alloc();
        ret = avcodec_receive_frame(m_codec_context, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0) {
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

    av_packet_free(&pkt);
    return result;
}

} // namespace Libraries::Ajm
