// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/enum.h"
#include "common/types.h"
#include "core/libraries/ajm/ajm.h"
#include "core/libraries/ajm/ajm_instance.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

namespace Libraries::Ajm {

class AjmFFmpegDecoder : public AjmCodec {
public:
    explicit AjmFFmpegDecoder(AjmCodecType codec_type, AjmFormatEncoding format, u64 flags,
                              u32 channels);
    ~AjmFFmpegDecoder() override;

    void Reset() override;
    void Initialize(const void* buffer, u32 buffer_size) override;
    void GetInfo(void* out_info) const override;
    AjmSidebandFormat GetFormat() const override;
    u32 GetMinimumInputSize() const override;
    u32 GetNextFrameSize(const AjmInstanceGapless& gapless) const override;
    DecoderResult ProcessData(std::span<u8>& input, SparseOutputBuffer& output,
                              AjmInstanceGapless& gapless) override;

private:
    template <class T>
    size_t WriteOutputPCM(AVFrame* frame, SparseOutputBuffer& output, u32 skipped_samples,
                          u32 max_pcm) {
        const u32 channels = frame->ch_layout.nb_channels;
        std::span<T> pcm_data(reinterpret_cast<T*>(frame->data[0]), frame->nb_samples * channels);
        const size_t skip_items = std::min<size_t>(skipped_samples * channels, pcm_data.size());
        pcm_data = pcm_data.subspan(skip_items);
        return output.Write(pcm_data.subspan(0, std::min<size_t>(pcm_data.size(), max_pcm)));
    }

    AVFrame* ConvertAudioFrame(AVFrame* frame);
    AVCodecID CodecTypeToAVCodecID(AjmCodecType codec_type) const;

    const AjmCodecType m_codec_type;
    const AjmFormatEncoding m_format;
    const u64 m_flags;
    const u32 m_target_channels;
    const AVCodec* m_codec = nullptr;
    AVCodecContext* m_codec_context = nullptr;
    SwrContext* m_swr_context = nullptr;
    u32 m_frame_samples = 1024;
};

} // namespace Libraries::Ajm
