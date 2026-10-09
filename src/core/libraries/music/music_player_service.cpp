// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <queue>

#include <SDL3/SDL.h>
#include <minimp3_ex.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#include "common/support/avdec.h"

#include "flac_decoder.h"

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/file_sys/fs.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "music_player_service.h"

namespace Libraries::MusicPlayerService {

class MusicPlayerManager {
public:
    static MusicPlayerManager& Instance() {
        static MusicPlayerManager s_instance;
        return s_instance;
    }

    void SetTrackList(const std::vector<std::string>& tracks) {
        std::scoped_lock lock(m_mutex);
        m_tracks = tracks;
    }

    void ClearTracks() {
        StopPlayback();
        std::scoped_lock lock(m_mutex);
        m_tracks.clear();
    }

    u32 GetTrackCount() {
        std::scoped_lock lock(m_mutex);
        return static_cast<u32>(m_tracks.size());
    }

    struct MusicEvent {
        u32 event_id;
        u32 track_index;
    };

    void PushEvent(u32 event_id, u32 track_index) {
        std::scoped_lock lock(m_event_mutex);
        m_event_queue.push({event_id, track_index});
        m_event_cv.notify_one();
    }

    bool PopEvent(MusicEvent& ev, u32 timeout_ms = 50) {
        std::unique_lock lock(m_event_mutex);
        if (m_event_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [this] { return !m_event_queue.empty(); })) {
            ev = m_event_queue.front();
            m_event_queue.pop();
            return true;
        }
        return false;
    }

    void Play(u32 track_index, u64 start_time_ms) {
        StopPlayback();

        std::string track_uri;
        {
            std::scoped_lock lock(m_mutex);
            if (track_index < m_tracks.size()) {
                track_uri = m_tracks[track_index];
                m_current_track_idx = track_index;
            } else {
                return;
            }
        }

        m_stop_requested = false;
        m_pause_requested = false;
        m_seek_offset_ms = -1;
        m_current_time_ms = start_time_ms;
        m_total_duration_ms = 0;
        m_play_state = 1;

        m_worker_thread =
            std::thread(&MusicPlayerManager::PlaybackWorker, this, track_uri, start_time_ms);
    }

    void StopPlayback() {
        m_stop_requested = true;
        m_pause_requested = false;
        m_play_state = 0;
        m_current_time_ms = 0;
        m_total_duration_ms = 0;

        if (m_worker_thread.joinable()) {
            m_worker_thread.join();
        }
    }

    void Pause() {
        m_pause_requested = true;
        m_play_state = 2;
    }

    void Unpause() {
        m_pause_requested = false;
        m_play_state = 1;
    }

    void Seek(u64 time_ms) {
        m_seek_offset_ms = static_cast<s64>(time_ms);
        m_current_time_ms = time_ms;
    }

    void NextTrack() {
        u32 count = GetTrackCount();
        if (count > 0) {
            u32 next_idx = (m_current_track_idx.load() + 1) % count;
            Play(next_idx, 0);
        }
    }

    void PreviousTrack() {
        u32 count = GetTrackCount();
        if (count > 0) {
            u32 cur = m_current_track_idx.load();
            u32 prev_idx = (cur > 0) ? (cur - 1) : 0;
            Play(prev_idx, 0);
        }
    }

    void SetVolume(int vol) {
        m_volume = std::clamp(vol, 0, 100);
    }

    int GetVolume() const {
        return m_volume.load();
    }

    void SetRepeatMode(u32 mode) {
        m_repeat_mode = mode;
    }

    void SetShuffle(u32 mode) {
        m_shuffle_mode = mode;
    }

    u32 GetPlayState() const {
        return m_play_state.load();
    }

    u32 GetCurrentTrackIndex() const {
        return m_current_track_idx.load();
    }

    u64 GetCurrentTimeMs() const {
        return m_current_time_ms.load();
    }

    u64 GetTotalDurationMs() const {
        return m_total_duration_ms.load();
    }

    u32 GetRepeatMode() const {
        return m_repeat_mode.load();
    }

    u32 GetShuffleMode() const {
        return m_shuffle_mode.load();
    }

private:
    MusicPlayerManager() = default;
    ~MusicPlayerManager() {
        StopPlayback();
    }

    static bool DecodeAudioWithFFmpeg(const std::string& path, mp3dec_file_info_t* file_info) {
        AVFormatContext* format_ctx = nullptr;
        if (avformat_open_input(&format_ctx, path.c_str(), nullptr, nullptr) != 0) {
            return false;
        }
        if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
            avformat_close_input(&format_ctx);
            return false;
        }
        int audio_stream_idx =
            av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (audio_stream_idx < 0) {
            avformat_close_input(&format_ctx);
            return false;
        }
        AVStream* stream = format_ctx->streams[audio_stream_idx];
        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) {
            avformat_close_input(&format_ctx);
            return false;
        }
        AVCodecContext* codec_ctx = avcodec_alloc_context3(decoder);
        if (!codec_ctx) {
            avformat_close_input(&format_ctx);
            return false;
        }
        if (avcodec_parameters_to_context(codec_ctx, stream->codecpar) < 0 ||
            avcodec_open2(codec_ctx, decoder, nullptr) < 0) {
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&format_ctx);
            return false;
        }

        int out_channels =
            codec_ctx->ch_layout.nb_channels > 0 ? codec_ctx->ch_layout.nb_channels : 2;
        int out_sample_rate = codec_ctx->sample_rate > 0 ? codec_ctx->sample_rate : 44100;
        AVChannelLayout out_ch_layout;
        av_channel_layout_default(&out_ch_layout, out_channels);

        SwrContext* swr = nullptr;
        int swr_res = swr_alloc_set_opts2(&swr, &out_ch_layout, AV_SAMPLE_FMT_S16, out_sample_rate,
                                          &codec_ctx->ch_layout, codec_ctx->sample_fmt,
                                          codec_ctx->sample_rate, 0, nullptr);
        if (swr_res < 0 || !swr || swr_init(swr) < 0) {
            if (swr)
                swr_free(&swr);
            av_channel_layout_uninit(&out_ch_layout);
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&format_ctx);
            return false;
        }

        AVPacket* pkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        std::vector<int16_t> pcm_data;

        while (av_read_frame(format_ctx, pkt) >= 0) {
            if (pkt->stream_index == audio_stream_idx) {
                if (avcodec_send_packet(codec_ctx, pkt) >= 0) {
                    while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                        int max_out_samples =
                            av_rescale_rnd(swr_get_delay(swr, out_sample_rate) + frame->nb_samples,
                                           out_sample_rate, codec_ctx->sample_rate, AV_ROUND_UP);
                        size_t cur_size = pcm_data.size();
                        pcm_data.resize(cur_size + max_out_samples * out_channels);
                        uint8_t* out_ptr = reinterpret_cast<uint8_t*>(pcm_data.data() + cur_size);
                        int converted =
                            swr_convert(swr, &out_ptr, max_out_samples,
                                        (const uint8_t**)frame->extended_data, frame->nb_samples);
                        if (converted > 0) {
                            pcm_data.resize(cur_size + converted * out_channels);
                        } else {
                            pcm_data.resize(cur_size);
                        }
                    }
                }
            }
            av_packet_unref(pkt);
        }

        avcodec_send_packet(codec_ctx, nullptr);
        while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
            int max_out_samples =
                av_rescale_rnd(swr_get_delay(swr, out_sample_rate) + frame->nb_samples,
                               out_sample_rate, codec_ctx->sample_rate, AV_ROUND_UP);
            size_t cur_size = pcm_data.size();
            pcm_data.resize(cur_size + max_out_samples * out_channels);
            uint8_t* out_ptr = reinterpret_cast<uint8_t*>(pcm_data.data() + cur_size);
            int converted = swr_convert(swr, &out_ptr, max_out_samples,
                                        (const uint8_t**)frame->extended_data, frame->nb_samples);
            if (converted > 0) {
                pcm_data.resize(cur_size + converted * out_channels);
            } else {
                pcm_data.resize(cur_size);
            }
        }

        int max_out_samples = swr_get_delay(swr, out_sample_rate);
        if (max_out_samples > 0) {
            size_t cur_size = pcm_data.size();
            pcm_data.resize(cur_size + max_out_samples * out_channels);
            uint8_t* out_ptr = reinterpret_cast<uint8_t*>(pcm_data.data() + cur_size);
            int converted = swr_convert(swr, &out_ptr, max_out_samples, nullptr, 0);
            if (converted > 0) {
                pcm_data.resize(cur_size + converted * out_channels);
            } else {
                pcm_data.resize(cur_size);
            }
        }

        av_frame_free(&frame);
        av_packet_free(&pkt);
        swr_free(&swr);
        av_channel_layout_uninit(&out_ch_layout);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);

        if (pcm_data.empty()) {
            return false;
        }

        file_info->channels = out_channels;
        file_info->hz = out_sample_rate;
        file_info->samples = pcm_data.size();
        file_info->buffer = static_cast<mp3d_sample_t*>(malloc(pcm_data.size() * sizeof(int16_t)));
        if (!file_info->buffer) {
            return false;
        }
        std::memcpy(file_info->buffer, pcm_data.data(), pcm_data.size() * sizeof(int16_t));
        return true;
    }

    void PlaybackWorker(std::string uri, u64 start_time_ms) {
        std::string guest_path = uri;
        auto scheme_pos = guest_path.find("://");
        if (scheme_pos != std::string::npos) {
            guest_path = guest_path.substr(scheme_pos + 3);
        }
        if (!guest_path.starts_with("/")) {
            guest_path = "/" + guest_path;
        }

        std::string decoded_path;
        decoded_path.reserve(guest_path.size());
        for (size_t i = 0; i < guest_path.size(); ++i) {
            if (guest_path[i] == '%' && i + 2 < guest_path.size()) {
                if (std::isxdigit(guest_path[i + 1]) && std::isxdigit(guest_path[i + 2])) {
                    char hex_str[3] = {guest_path[i + 1], guest_path[i + 2], '\0'};
                    int hex_val = static_cast<int>(std::strtol(hex_str, nullptr, 16));
                    decoded_path.push_back(static_cast<char>(hex_val));
                    i += 2;
                    continue;
                }
            }
            decoded_path.push_back(guest_path[i]);
        }
        guest_path = std::move(decoded_path);

        auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
        std::filesystem::path host_path;
        if (mnt) {
            host_path = mnt->GetHostPath(guest_path);
        }
        if (host_path.empty() || !std::filesystem::exists(host_path)) {
            host_path = guest_path;
        }

        if (!std::filesystem::exists(host_path)) {
            LOG_ERROR(Lib_AudioOut, "Music file does not exist: {}", host_path.string());
            m_play_state = 0;
            PushEvent(3, m_current_track_idx.load());
            return;
        }

        mp3dec_file_info_t file_info{};
        bool decoded = false;

        const auto ext = host_path.extension().string();
        if (ext == ".flac" || ext == ".FLAC") {
            int16_t* flac_buf = nullptr;
            int flac_channels = 0;
            int flac_hz = 0;
            size_t flac_samples = 0;
            if (DecodeFlacFile(host_path, &flac_channels, &flac_hz, &flac_samples, &flac_buf)) {
                file_info.channels = flac_channels;
                file_info.hz = flac_hz;
                file_info.samples = flac_samples;
                file_info.buffer = reinterpret_cast<mp3d_sample_t*>(flac_buf);
                decoded = true;
            }
        }

        if (!decoded && (ext == ".mp3" || ext == ".MP3")) {
            mp3dec_t dec;
            mp3dec_init(&dec);
            int res = mp3dec_load(&dec, host_path.string().c_str(), &file_info, nullptr, nullptr);
            if (res == 0 && file_info.samples > 0 && file_info.channels > 0 && file_info.hz > 0) {
                decoded = true;
            }
        }

        if (!decoded) {
            decoded = DecodeAudioWithFFmpeg(host_path.string(), &file_info);
        }

        if (!decoded) {
            LOG_ERROR(Lib_AudioOut, "Failed to decode audio file: {}", host_path.string());
            m_play_state = 0;
            m_current_time_ms = 0;
            m_total_duration_ms = 0;
            PushEvent(2, m_current_track_idx.load());
            return;
        }

        u64 total_duration =
            (static_cast<u64>(file_info.samples) / file_info.channels) * 1000 / file_info.hz;
        m_total_duration_ms = total_duration;

        const SDL_AudioSpec spec = {
            .format = SDL_AUDIO_S16LE,
            .channels = static_cast<u8>(file_info.channels),
            .freq = static_cast<int>(file_info.hz),
        };

        SDL_AudioStream* stream =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream) {
            LOG_ERROR(Lib_AudioOut, "Failed to open SDL audio stream: {}", SDL_GetError());
            free(file_info.buffer);
            m_play_state = 0;
            m_current_time_ms = 0;
            m_total_duration_ms = 0;
            return;
        }

        SDL_ResumeAudioStreamDevice(stream);

        size_t sample_offset = 0;
        if (start_time_ms > 0 && total_duration > 0) {
            sample_offset = (start_time_ms * file_info.hz / 1000) * file_info.channels;
            if (sample_offset > file_info.samples) {
                sample_offset = file_info.samples;
            }
        }

        constexpr size_t chunk_frames = 2048;
        const size_t chunk_samples = chunk_frames * file_info.channels;

        while (!m_stop_requested.load()) {
            s64 seek_target = m_seek_offset_ms.exchange(-1);
            if (seek_target >= 0) {
                sample_offset =
                    (static_cast<u64>(seek_target) * file_info.hz / 1000) * file_info.channels;
                if (sample_offset > file_info.samples) {
                    sample_offset = file_info.samples;
                }
                SDL_ClearAudioStream(stream);
            }

            if (m_pause_requested.load()) {
                SDL_PauseAudioStreamDevice(stream);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            SDL_ResumeAudioStreamDevice(stream);

            float vol = (m_volume.load() / 100.0f) * (EmulatorSettings.GetVolumeSlider() * 0.01f);
            SDL_SetAudioStreamGain(stream, vol);

            int queued = SDL_GetAudioStreamQueued(stream);
            if (sample_offset < file_info.samples) {
                if (queued <
                    static_cast<int>(chunk_frames * file_info.channels * sizeof(int16_t) * 4)) {
                    size_t to_write = std::min(chunk_samples, file_info.samples - sample_offset);
                    SDL_PutAudioStreamData(stream, &file_info.buffer[sample_offset],
                                           static_cast<int>(to_write * sizeof(int16_t)));
                    sample_offset += to_write;

                    u64 cur_ms = (sample_offset / file_info.channels) * 1000 / file_info.hz;
                    m_current_time_ms = cur_ms;
                }
            } else if (queued == 0) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        SDL_DestroyAudioStream(stream);
        free(file_info.buffer);

        if (!m_stop_requested.load()) {
            m_play_state = 0;
            m_current_time_ms = 0;
        }
    }

    std::mutex m_mutex;
    std::vector<std::string> m_tracks;
    std::atomic<u32> m_play_state{0};
    std::atomic<u32> m_current_track_idx{0};
    std::atomic<u64> m_current_time_ms{0};
    std::atomic<u64> m_total_duration_ms{0};
    std::atomic<int> m_volume{100};
    std::atomic<u32> m_repeat_mode{0};
    std::atomic<u32> m_shuffle_mode{0};
    std::atomic<bool> m_stop_requested{false};
    std::atomic<bool> m_pause_requested{false};
    std::atomic<s64> m_seek_offset_ms{-1};
    std::thread m_worker_thread;
    std::mutex m_event_mutex;
    std::condition_variable m_event_cv;
    std::queue<MusicEvent> m_event_queue;
};

static s32 PS4_SYSV_ABI sceMusicPlayerServiceInitialize(size_t pool_size, void* pool_addr) {
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceInitialize2(size_t pool_size, void* pool_addr) {
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceInitialize3(size_t pool_size, void* pool_addr,
                                                         u64 unk1, u64 unk2) {
    if (pool_size < 0x200000 || !pool_addr) {
        return static_cast<s32>(0x812f0004);
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceTerminate() {
    MusicPlayerManager::Instance().StopPlayback();
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceSetUsbStorageDeviceInfo(void* a, void* b) {
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceRemoveAllData(s32 player_id) {
    MusicPlayerManager::Instance().ClearTracks();
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceSetTrackList2(s32 player_id, const void* tracks,
                                                           u32 count, u32 flags) {
    if (!tracks || count == 0) {
        return static_cast<s32>(0x812f0004);
    }

    constexpr size_t track_entry_size = 0xc48;
    const auto* base = reinterpret_cast<const u8*>(tracks);
    std::vector<std::string> track_uris;
    track_uris.reserve(count);

    for (u32 i = 0; i < count; i++) {
        const char* uri_str = reinterpret_cast<const char*>(base + i * track_entry_size);
        track_uris.emplace_back(uri_str);
    }

    MusicPlayerManager::Instance().SetTrackList(track_uris);
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceSetTrackList(s32 player_id, const void* tracks,
                                                          u32 count) {
    return sceMusicPlayerServiceSetTrackList2(player_id, tracks, count, 0);
}

static s32 PS4_SYSV_ABI sceMusicPlayerServicePlay(s32 player_id, u32 track_index, s64 start_time_s,
                                                  s32 flags) {
    LOG_INFO(Lib_AudioOut,
             "sceMusicPlayerServicePlay: player_id={}, track_index={}, start_time_s={}, flags={}",
             player_id, track_index, start_time_s, flags);
    MusicPlayerManager::Instance().Play(
        track_index, start_time_s > 0 ? static_cast<u64>(start_time_s * 1000) : 0);
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServicePlayStartByTime(s32 player_id, u32 track_index,
                                                             s64 start_time_s) {
    MusicPlayerManager::Instance().Play(
        track_index, start_time_s > 0 ? static_cast<u64>(start_time_s * 1000) : 0);
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceStop(s32 player_id) {
    MusicPlayerManager::Instance().StopPlayback();
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServicePause(s32 player_id) {
    MusicPlayerManager::Instance().Pause();
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceUnpause(s32 player_id) {
    MusicPlayerManager::Instance().Unpause();
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceNextTrack(s32 player_id) {
    MusicPlayerManager::Instance().NextTrack();
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServicePreviousTrack(s32 player_id) {
    MusicPlayerManager::Instance().PreviousTrack();
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceSeek(s32 player_id, s64 time_s) {
    LOG_INFO(Lib_AudioOut, "sceMusicPlayerServiceSeek: player_id={}, time_s={}", player_id, time_s);
    MusicPlayerManager::Instance().Seek(time_s > 0 ? static_cast<u64>(time_s * 1000) : 0);
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceSetAudioVolume(s32 player_id, s32 volume) {
    MusicPlayerManager::Instance().SetVolume(volume);
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceGetAudioVolume(s32 player_id, s32* volume) {
    if (volume) {
        *volume = MusicPlayerManager::Instance().GetVolume();
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceSetShuffle(s32 player_id, u32 mode) {
    MusicPlayerManager::Instance().SetShuffle(mode);
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceSetRepeatMode(s32 player_id, u32 mode) {
    MusicPlayerManager::Instance().SetRepeatMode(mode);
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI
sceMusicPlayerServiceGetPlayStatusExtension(OrbisMusicPlayerPlayStatusExtension* status) {
    if (!status || status->size != 0x48) {
        return static_cast<s32>(0x812f0004);
    }

    auto& mgr = MusicPlayerManager::Instance();
    status->error_code = 0;
    status->play_state = mgr.GetPlayState();
    status->total_duration_ms = static_cast<u32>(mgr.GetTotalDurationMs() / 1000);
    status->unk14 = 0;
    status->current_time_ms = static_cast<u32>(mgr.GetCurrentTimeMs() / 1000);
    status->unk1c = 0;
    status->volume = static_cast<u32>(mgr.GetVolume());
    status->current_track_index = mgr.GetCurrentTrackIndex();
    status->unk28 = 0;
    status->total_tracks = mgr.GetTrackCount();
    status->unk30 = 0;
    status->repeat_mode = mgr.GetRepeatMode();
    status->shuffle_mode = mgr.GetShuffleMode();
    status->unk3c = 0;
    status->unk40 = 0;
    status->unk44 = 0;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceGetCurrentPlayStatus(void* handle, void* b, void* c,
                                                                  void* d, void* e) {
    auto& mgr = MusicPlayerManager::Instance();
    if (b && c && d && e) {
        *reinterpret_cast<s32*>(b) = static_cast<s32>(mgr.GetPlayState());
        *reinterpret_cast<s32*>(c) = static_cast<s32>(mgr.GetCurrentTrackIndex());
        *reinterpret_cast<s32*>(d) = static_cast<s32>(mgr.GetCurrentTimeMs() / 1000);
        *reinterpret_cast<s32*>(e) = static_cast<s32>(mgr.GetTotalDurationMs() / 1000);
        return ORBIS_OK;
    }
    if (b) {
        u32* s = reinterpret_cast<u32*>(b);
        s[0] = 0;
        s[1] = mgr.GetPlayState();
        s[2] = 0;
        *reinterpret_cast<u64*>(&s[4]) = mgr.GetCurrentTimeMs() / 1000;
        s[6] = static_cast<u32>(mgr.GetTotalDurationMs() / 1000);
        s[7] = static_cast<u32>(mgr.GetVolume());
        return ORBIS_OK;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceReceiveEvent(void* buffer) {
    if (!buffer) {
        return static_cast<s32>(0x812f0004);
    }
    u8* buf = static_cast<u8*>(buffer);
    auto& mgr = MusicPlayerManager::Instance();
    MusicPlayerManager::MusicEvent ev{};
    if (mgr.PopEvent(ev, 50)) {
        LOG_INFO(Lib_AudioOut,
                 "sceMusicPlayerServiceReceiveEvent: delivering event_id={}, track_idx={}",
                 ev.event_id, ev.track_index);
        *reinterpret_cast<u32*>(buf + 0x08) = 1;
        *reinterpret_cast<u32*>(buf + 0x20) = 1;
        *reinterpret_cast<u32*>(buf + 0x24) = ev.event_id;
        *reinterpret_cast<u32*>(buf + 0x28) = ev.track_index;
    } else {
        *reinterpret_cast<u32*>(buf + 0x08) = 0;
        *reinterpret_cast<u32*>(buf + 0x20) = 0;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceGetPlaybackResults(void* a, void* b) {
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceGetTrackInfo(void* a, void* b) {
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceBeginTransaction(s32 a) {
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceEndTransaction(s32 a) {
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceCreateTrackList(void* a, void* b) {
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceRemoveTrackList(void* a) {
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceInsertTrackEntry(void* a, void* b) {
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceRemoveTrackEntry(void* a, void* b) {
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceMusicPlayerServiceGetTrackListVersion(void* a, void* b) {
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("WypjBw-l+pE", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceInitialize);
    LIB_FUNCTION("xT55MQDY9lg", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceInitialize2);
    LIB_FUNCTION("sY8beqtqGv0", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceInitialize3);
    LIB_FUNCTION("uZXjTVsS-hU", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceTerminate);
    LIB_FUNCTION("JlbWOi1Webo", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceSetUsbStorageDeviceInfo);
    LIB_FUNCTION("-JE2KLnsg9I", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceRemoveAllData);
    LIB_FUNCTION("4cuKd4S83xw", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceSetTrackList);
    LIB_FUNCTION("iawJpYIQM7s", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceSetTrackList2);
    LIB_FUNCTION("KblnYfZ5AGM", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServicePlay);
    LIB_FUNCTION("kMb+qpZTQ18", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServicePlayStartByTime);
    LIB_FUNCTION("7Ayp3diNYnk", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceStop);
    LIB_FUNCTION("fIii6-0Adxc", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServicePause);
    LIB_FUNCTION("DLXiDjC1D1A", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceUnpause);
    LIB_FUNCTION("M76dl-GnCs8", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceNextTrack);
    LIB_FUNCTION("NuJT-nmTqrI", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServicePreviousTrack);
    LIB_FUNCTION("5LhFvqzrcug", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceSeek);
    LIB_FUNCTION("tMgpmzMA4Zc", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceSetAudioVolume);
    LIB_FUNCTION("3KChwrxsVPg", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceGetAudioVolume);
    LIB_FUNCTION("DMvAm4HFKg0", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceSetShuffle);
    LIB_FUNCTION("+uCwAp6KYtQ", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceSetRepeatMode);
    LIB_FUNCTION("TVe6T-UKcoc", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceGetPlayStatusExtension);
    LIB_FUNCTION("CNsRsR5a+qc", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceGetCurrentPlayStatus);
    LIB_FUNCTION("Py0XtCoi5IU", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceReceiveEvent);
    LIB_FUNCTION("1+KpQYgv1zk", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceGetPlaybackResults);
    LIB_FUNCTION("wHKiDjNLQwI", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceGetTrackInfo);
    LIB_FUNCTION("AbiIaBA50I0", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceBeginTransaction);
    LIB_FUNCTION("7SOVShNUDXo", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceEndTransaction);
    LIB_FUNCTION("mW3EHwAVHPA", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceCreateTrackList);
    LIB_FUNCTION("EF1lpApBn4s", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceRemoveTrackList);
    LIB_FUNCTION("R1SLegxzCGU", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceInsertTrackEntry);
    LIB_FUNCTION("QEIJRZErmxQ", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceRemoveTrackEntry);
    LIB_FUNCTION("qYbmAxRGuq8", "libSceMusicPlayerService", 1, "libSceMusicPlayerService",
                 sceMusicPlayerServiceGetTrackListVersion);
}

} // namespace Libraries::MusicPlayerService
