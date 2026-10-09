// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace Libraries::MusicPlayerService {

struct FlacBitReader {
    const uint8_t* data;
    size_t size;
    size_t bit_pos{0};

    uint32_t read_bits(size_t n) {
        uint32_t val = 0;
        for (size_t i = 0; i < n; ++i) {
            size_t byte_idx = (bit_pos + i) >> 3;
            if (byte_idx >= size)
                return 0;
            size_t bit_idx = 7 - ((bit_pos + i) & 7);
            uint32_t bit = (data[byte_idx] >> bit_idx) & 1;
            val = (val << 1) | bit;
        }
        bit_pos += n;
        return val;
    }

    int32_t read_signed_bits(size_t n) {
        if (n == 0)
            return 0;
        uint32_t v = read_bits(n);
        if (v & (1U << (n - 1))) {
            return static_cast<int32_t>(v | (~0U << n));
        }
        return static_cast<int32_t>(v);
    }

    uint32_t read_unary() {
        uint32_t count = 0;
        while (bit_pos < size * 8) {
            size_t byte_idx = bit_pos >> 3;
            size_t bit_idx = 7 - (bit_pos & 7);
            bit_pos++;
            if ((data[byte_idx] >> bit_idx) & 1) {
                break;
            }
            count++;
        }
        return count;
    }

    uint64_t read_utf8() {
        uint32_t first = read_bits(8);
        if ((first & 0x80) == 0)
            return first;
        int extra = 0;
        uint64_t val = 0;
        if ((first & 0xE0) == 0xC0) {
            extra = 1;
            val = first & 0x1F;
        } else if ((first & 0xF0) == 0xE0) {
            extra = 2;
            val = first & 0x0F;
        } else if ((first & 0xF8) == 0xF0) {
            extra = 3;
            val = first & 0x07;
        } else if ((first & 0xFC) == 0xF8) {
            extra = 4;
            val = first & 0x03;
        } else if ((first & 0xFE) == 0xFC) {
            extra = 5;
            val = first & 0x01;
        } else if (first == 0xFE) {
            extra = 6;
            val = 0;
        }
        for (int i = 0; i < extra; ++i) {
            uint32_t b = read_bits(8);
            val = (val << 6) | (b & 0x3F);
        }
        return val;
    }

    void align_byte() {
        bit_pos = (bit_pos + 7) & ~7ULL;
    }
};

inline bool DecodeFlacFile(const std::filesystem::path& host_path, int* out_channels, int* out_hz,
                           size_t* out_samples, int16_t** out_buffer) {
    if (!out_channels || !out_hz || !out_samples || !out_buffer) {
        return false;
    }
    *out_buffer = nullptr;
    *out_samples = 0;

    std::ifstream file(host_path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    file.seekg(0, std::ios::end);
    size_t file_size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    if (file_size < 42) {
        return false;
    }

    std::vector<uint8_t> file_data(file_size);
    file.read(reinterpret_cast<char*>(file_data.data()), file_size);
    if (!file) {
        return false;
    }

    if (std::memcmp(file_data.data(), "fLaC", 4) != 0) {
        return false;
    }

    size_t idx = 4;
    int sample_rate = 0;
    int channels = 0;
    int bps = 0;
    uint64_t total_samples = 0;

    while (idx + 4 <= file_data.size()) {
        uint8_t header = file_data[idx];
        bool is_last = (header & 0x80) != 0;
        uint8_t btype = header & 0x7F;
        uint32_t length =
            (file_data[idx + 1] << 16) | (file_data[idx + 2] << 8) | file_data[idx + 3];
        idx += 4;
        if (idx + length > file_data.size()) {
            return false;
        }

        if (btype == 0 && length >= 34) { // STREAMINFO
            const uint8_t* si = &file_data[idx];
            sample_rate = (si[10] << 12) | (si[11] << 4) | (si[12] >> 4);
            channels = ((si[12] >> 1) & 7) + 1;
            bps = (((si[12] & 1) << 4) | (si[13] >> 4)) + 1;
            total_samples = (static_cast<uint64_t>(si[13] & 0x0F) << 32) |
                            (static_cast<uint64_t>(si[14]) << 24) |
                            (static_cast<uint64_t>(si[15]) << 16) |
                            (static_cast<uint64_t>(si[16]) << 8) | static_cast<uint64_t>(si[17]);
        }
        idx += length;
        if (is_last)
            break;
    }

    if (channels <= 0 || channels > 8 || sample_rate <= 0) {
        return false;
    }

    std::vector<int16_t> pcm;
    if (total_samples > 0) {
        pcm.reserve(total_samples * channels);
    }

    FlacBitReader reader{file_data.data(), file_data.size(), idx * 8};
    std::vector<int32_t> subframe_samples[8];

    while (reader.bit_pos + 32 < file_data.size() * 8) {
        size_t saved_pos = reader.bit_pos;
        uint32_t sync = reader.read_bits(14);
        if (sync != 0x3FFE) {
            reader.bit_pos = (saved_pos + 8) & ~7ULL;
            bool found = false;
            while (reader.bit_pos + 16 < file_data.size() * 8) {
                if (reader.read_bits(14) == 0x3FFE) {
                    found = true;
                    break;
                }
                reader.bit_pos = (reader.bit_pos - 14 + 8) & ~7ULL;
            }
            if (!found)
                break;
        }

        reader.read_bits(1); // reserved
        reader.read_bits(1); // blocking strategy
        uint32_t bs_code = reader.read_bits(4);
        uint32_t sr_code = reader.read_bits(4);
        uint32_t ch_code = reader.read_bits(4);
        uint32_t bps_code = reader.read_bits(3);
        reader.read_bits(1); // reserved

        uint32_t block_size = 0;
        if (bs_code == 1)
            block_size = 192;
        else if (bs_code >= 2 && bs_code <= 5)
            block_size = 576 << (bs_code - 2);
        else if (bs_code >= 8 && bs_code <= 15)
            block_size = 256 << (bs_code - 8);

        reader.read_utf8(); // frame/sample number

        if (bs_code == 6)
            block_size = reader.read_bits(8) + 1;
        else if (bs_code == 7)
            block_size = reader.read_bits(16) + 1;

        if (sr_code == 12)
            reader.read_bits(8);
        else if (sr_code == 13 || sr_code == 14)
            reader.read_bits(16);

        reader.read_bits(8); // CRC-8

        if (block_size == 0)
            break;

        int frame_channels = channels;
        int ch_mode = 0; // 0: independent, 1: left/side, 2: right/side, 3: mid/side
        if (ch_code < 8) {
            frame_channels = ch_code + 1;
            ch_mode = 0;
        } else if (ch_code == 8) {
            frame_channels = 2;
            ch_mode = 1;
        } else if (ch_code == 9) {
            frame_channels = 2;
            ch_mode = 2;
        } else if (ch_code == 10) {
            frame_channels = 2;
            ch_mode = 3;
        }

        int frame_bps = bps;
        if (bps_code == 1)
            frame_bps = 8;
        else if (bps_code == 2)
            frame_bps = 12;
        else if (bps_code == 4)
            frame_bps = 16;
        else if (bps_code == 5)
            frame_bps = 20;
        else if (bps_code == 6)
            frame_bps = 24;

        for (int ch = 0; ch < frame_channels; ++ch) {
            subframe_samples[ch].resize(block_size);
            int sub_bps = frame_bps;
            if (ch_mode == 1 && ch == 1)
                sub_bps++;
            else if (ch_mode == 2 && ch == 0)
                sub_bps++;
            else if (ch_mode == 3 && ch == 1)
                sub_bps++;

            reader.read_bits(1); // 0 bit
            uint32_t type = reader.read_bits(6);
            uint32_t wasted_bits = 0;
            if (reader.read_bits(1)) {
                wasted_bits = reader.read_unary() + 1;
                sub_bps -= wasted_bits;
            }

            if (type == 0) { // CONSTANT
                int32_t c = reader.read_signed_bits(sub_bps);
                std::fill(subframe_samples[ch].begin(), subframe_samples[ch].end(), c);
            } else if (type == 1) { // VERBATIM
                for (uint32_t i = 0; i < block_size; ++i) {
                    subframe_samples[ch][i] = reader.read_signed_bits(sub_bps);
                }
            } else if (type >= 8 && type <= 12) { // FIXED
                int order = type - 8;
                for (int i = 0; i < order; ++i) {
                    subframe_samples[ch][i] = reader.read_signed_bits(sub_bps);
                }

                uint32_t method = reader.read_bits(2);
                uint32_t part_order = reader.read_bits(4);
                uint32_t num_parts = 1U << part_order;
                uint32_t part_samples = block_size >> part_order;
                uint32_t sample_idx = order;

                for (uint32_t p = 0; p < num_parts; ++p) {
                    uint32_t p_len = (method == 0) ? 4 : 5;
                    uint32_t escape_param = (method == 0) ? 0xF : 0x1F;
                    uint32_t k = reader.read_bits(p_len);
                    uint32_t n_samples = (p == 0) ? (part_samples - order) : part_samples;

                    if (k == escape_param) {
                        uint32_t esc_bits = reader.read_bits(5);
                        for (uint32_t s = 0; s < n_samples; ++s) {
                            subframe_samples[ch][sample_idx++] = reader.read_signed_bits(esc_bits);
                        }
                    } else {
                        for (uint32_t s = 0; s < n_samples; ++s) {
                            uint32_t msb = reader.read_unary();
                            uint32_t lsb = reader.read_bits(k);
                            uint32_t v = (msb << k) | lsb;
                            int32_t res = (v & 1) ? -(static_cast<int32_t>(v >> 1)) - 1
                                                  : static_cast<int32_t>(v >> 1);
                            subframe_samples[ch][sample_idx++] = res;
                        }
                    }
                }

                for (uint32_t i = order; i < block_size; ++i) {
                    int32_t pred = 0;
                    if (order == 1)
                        pred = subframe_samples[ch][i - 1];
                    else if (order == 2)
                        pred = 2 * subframe_samples[ch][i - 1] - subframe_samples[ch][i - 2];
                    else if (order == 3)
                        pred = 3 * subframe_samples[ch][i - 1] - 3 * subframe_samples[ch][i - 2] +
                               subframe_samples[ch][i - 3];
                    else if (order == 4)
                        pred = 4 * subframe_samples[ch][i - 1] - 6 * subframe_samples[ch][i - 2] +
                               4 * subframe_samples[ch][i - 3] - subframe_samples[ch][i - 4];
                    subframe_samples[ch][i] += pred;
                }
            } else if (type >= 32) { // LPC
                int order = (type - 32) + 1;
                for (int i = 0; i < order; ++i) {
                    subframe_samples[ch][i] = reader.read_signed_bits(sub_bps);
                }
                uint32_t qlp_prec = reader.read_bits(4) + 1;
                int32_t qlp_shift = reader.read_signed_bits(5);
                int32_t coeffs[32];
                for (int i = 0; i < order; ++i) {
                    coeffs[i] = reader.read_signed_bits(qlp_prec);
                }

                uint32_t method = reader.read_bits(2);
                uint32_t part_order = reader.read_bits(4);
                uint32_t num_parts = 1U << part_order;
                uint32_t part_samples = block_size >> part_order;
                uint32_t sample_idx = order;

                for (uint32_t p = 0; p < num_parts; ++p) {
                    uint32_t p_len = (method == 0) ? 4 : 5;
                    uint32_t escape_param = (method == 0) ? 0xF : 0x1F;
                    uint32_t k = reader.read_bits(p_len);
                    uint32_t n_samples = (p == 0) ? (part_samples - order) : part_samples;

                    if (k == escape_param) {
                        uint32_t esc_bits = reader.read_bits(5);
                        for (uint32_t s = 0; s < n_samples; ++s) {
                            subframe_samples[ch][sample_idx++] = reader.read_signed_bits(esc_bits);
                        }
                    } else {
                        for (uint32_t s = 0; s < n_samples; ++s) {
                            uint32_t msb = reader.read_unary();
                            uint32_t lsb = reader.read_bits(k);
                            uint32_t v = (msb << k) | lsb;
                            int32_t res = (v & 1) ? -(static_cast<int32_t>(v >> 1)) - 1
                                                  : static_cast<int32_t>(v >> 1);
                            subframe_samples[ch][sample_idx++] = res;
                        }
                    }
                }

                for (uint32_t i = order; i < block_size; ++i) {
                    int64_t sum = 0;
                    for (int j = 0; j < order; ++j) {
                        sum += static_cast<int64_t>(coeffs[j]) * subframe_samples[ch][i - 1 - j];
                    }
                    subframe_samples[ch][i] += static_cast<int32_t>(sum >> qlp_shift);
                }
            }

            if (wasted_bits > 0) {
                for (uint32_t i = 0; i < block_size; ++i) {
                    subframe_samples[ch][i] <<= wasted_bits;
                }
            }
        }

        reader.align_byte();
        reader.read_bits(16); // CRC-16

        // Channel decorrelation
        if (ch_mode == 1) { // left/side
            for (uint32_t i = 0; i < block_size; ++i) {
                subframe_samples[1][i] = subframe_samples[0][i] - subframe_samples[1][i];
            }
        } else if (ch_mode == 2) { // right/side
            for (uint32_t i = 0; i < block_size; ++i) {
                subframe_samples[0][i] = subframe_samples[0][i] + subframe_samples[1][i];
            }
        } else if (ch_mode == 3) { // mid/side
            for (uint32_t i = 0; i < block_size; ++i) {
                int64_t mid = subframe_samples[0][i];
                int64_t side = subframe_samples[1][i];
                mid = (mid << 1) | (side & 1);
                subframe_samples[0][i] = static_cast<int32_t>((mid + side) >> 1);
                subframe_samples[1][i] = static_cast<int32_t>((mid - side) >> 1);
            }
        }

        // Interleave & scale to 16-bit
        for (uint32_t i = 0; i < block_size; ++i) {
            for (int ch = 0; ch < frame_channels; ++ch) {
                int32_t s = subframe_samples[ch][i];
                int16_t sample16 = 0;
                if (frame_bps == 16) {
                    sample16 = static_cast<int16_t>(std::clamp(s, -32768, 32767));
                } else if (frame_bps == 24) {
                    sample16 = static_cast<int16_t>(std::clamp(s >> 8, -32768, 32767));
                } else if (frame_bps == 8) {
                    sample16 = static_cast<int16_t>(s << 8);
                } else if (frame_bps > 16) {
                    sample16 =
                        static_cast<int16_t>(std::clamp(s >> (frame_bps - 16), -32768, 32767));
                } else {
                    sample16 = static_cast<int16_t>(s << (16 - frame_bps));
                }
                pcm.push_back(sample16);
            }
        }
    }

    if (pcm.empty()) {
        return false;
    }

    *out_channels = channels;
    *out_hz = sample_rate;
    *out_samples = pcm.size();

    size_t byte_count = pcm.size() * sizeof(int16_t);
    *out_buffer = static_cast<int16_t*>(std::malloc(byte_count));
    if (!*out_buffer) {
        return false;
    }
    std::memcpy(*out_buffer, pcm.data(), byte_count);
    return true;
}

} // namespace Libraries::MusicPlayerService
