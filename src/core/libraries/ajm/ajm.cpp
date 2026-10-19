// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/libraries/ajm/ajm.h"
#include "core/libraries/ajm/ajm_context.h"
#include "core/libraries/ajm/ajm_error.h"
#include "core/libraries/ajm/ajm_mp3.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"

#include <mutex>
#include <magic_enum/magic_enum.hpp>

namespace Libraries::Ajm {

constexpr int ORBIS_AJM_CHANNELMASK_MONO = 0x0004;
constexpr int ORBIS_AJM_CHANNELMASK_STEREO = 0x0003;
constexpr int ORBIS_AJM_CHANNELMASK_QUAD = 0x0033;
constexpr int ORBIS_AJM_CHANNELMASK_5POINT1 = 0x060F;
constexpr int ORBIS_AJM_CHANNELMASK_7POINT1 = 0x063F;

static std::mutex s_contexts_mutex;
static std::unordered_map<u32, std::unique_ptr<AjmContext>> contexts{};

static AjmContext* GetContext(u32 context_id) {
    std::lock_guard lock(s_contexts_mutex);
    auto it = contexts.find(context_id);
    return it != contexts.end() ? it->second.get() : nullptr;
}

u32 GetChannelMask(u32 num_channels) {
    switch (num_channels) {
    case 1:
        return ORBIS_AJM_CHANNELMASK_MONO;
    case 2:
        return ORBIS_AJM_CHANNELMASK_STEREO;
    case 4:
        return ORBIS_AJM_CHANNELMASK_QUAD;
    case 6:
        return ORBIS_AJM_CHANNELMASK_5POINT1;
    case 8:
        return ORBIS_AJM_CHANNELMASK_7POINT1;
    default:
        UNREACHABLE_MSG("Unexpected number of channels: {}", num_channels);
    }
}

int PS4_SYSV_ABI sceAjmBatchCancel(const u32 context_id, const u32 batch_id) {
    LOG_INFO(Lib_Ajm, "called context_id = {} batch_id = {}", context_id, batch_id);

    auto* context = GetContext(context_id);
    if (!context) {
        return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    }

    return context->BatchCancel(batch_id);
}

void PS4_SYSV_ABI sceAjmBatchErrorDump(const AjmBuffer* batch_buffer,
                                       const AjmBatchError* batch_error) {
    if (!batch_buffer || !batch_error) {
        LOG_ERROR(Lib_Ajm, "** Invalid parameters. **");
        return;
    }

    if (!batch_error->job_addr) {
        LOG_ERROR(Lib_Ajm, "** Impossible to display Batch Error Dump. **");
        return;
    }

    if (batch_error->job_addr < batch_buffer->p_address ||
        batch_error->job_addr >= (batch_buffer->p_address + batch_buffer->size)) {
        LOG_ERROR(Lib_Ajm,
                  "** Impossible to display Batch Error Dump. Make sure that the batch was not "
                  "erased/modified after the error. **");
        return;
    }

    LOG_ERROR(Lib_Ajm,
              "--------------------------Batch Error Dump--------------------------\n"
              "Error Code: {:#x} ({})\n"
              "Job Address: {}\n"
              "Command Offset: {:#x}\n"
              "Job Return Address: {}",
              batch_error->error_code, sceAjmStrError(batch_error->error_code),
              batch_error->job_addr, batch_error->cmd_offset, batch_error->job_ra);
}

void* PS4_SYSV_ABI sceAjmBatchJobControlBufferRa(void* p_buffer, u32 instance_id, u64 flags,
                                                 void* p_sideband_input, size_t sideband_input_size,
                                                 void* p_sideband_output,
                                                 size_t sideband_output_size,
                                                 void* p_return_address) {
    return BatchJobControlBufferRa(p_buffer, instance_id, flags, p_sideband_input,
                                   sideband_input_size, p_sideband_output, sideband_output_size,
                                   p_return_address);
}

void* PS4_SYSV_ABI sceAjmBatchJobInlineBuffer(void* p_buffer, const void* p_data_input,
                                              size_t data_input_size,
                                              const void** pp_batch_address) {
    return BatchJobInlineBuffer(p_buffer, p_data_input, data_input_size, pp_batch_address);
}

void* PS4_SYSV_ABI sceAjmBatchJobRunBufferRa(void* p_buffer, u32 instance_id, u64 flags,
                                             void* p_data_input, size_t data_input_size,
                                             void* p_data_output, size_t data_output_size,
                                             void* p_sideband_output, size_t sideband_output_size,
                                             void* p_return_address) {
    return BatchJobRunBufferRa(p_buffer, instance_id, flags, p_data_input, data_input_size,
                               p_data_output, data_output_size, p_sideband_output,
                               sideband_output_size, p_return_address);
}

void* PS4_SYSV_ABI sceAjmBatchJobRunSplitBufferRa(
    void* p_buffer, u32 instance_id, u64 flags, const AjmBuffer* p_data_input_buffers,
    size_t num_data_input_buffers, const AjmBuffer* p_data_output_buffers,
    size_t num_data_output_buffers, void* p_sideband_output, size_t sideband_output_size,
    void* p_return_address) {
    return BatchJobRunSplitBufferRa(p_buffer, instance_id, flags, p_data_input_buffers,
                                    num_data_input_buffers, p_data_output_buffers,
                                    num_data_output_buffers, p_sideband_output,
                                    sideband_output_size, p_return_address);
}

int PS4_SYSV_ABI sceAjmBatchStartBuffer(u32 context_id, u8* p_batch, u32 batch_size,
                                        const int priority, AjmBatchError* batch_error,
                                        u32* out_batch_id) {
    LOG_TRACE(Lib_Ajm, "called context = {}, batch_size = {:#x}, priority = {}", context_id,
              batch_size, priority);
    if (!out_batch_id) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }
    *out_batch_id = 0xffffffff;
    if (!p_batch || batch_size == 0) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }

    auto* context = GetContext(context_id);
    if (!context) {
        return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    }

    return context->BatchStartBuffer(p_batch, batch_size, priority, batch_error, out_batch_id);
}

int PS4_SYSV_ABI sceAjmBatchWait(const u32 context_id, const u32 batch_id, const u32 timeout,
                                 AjmBatchError* const batch_error) {
    LOG_TRACE(Lib_Ajm, "called context = {}, batch_id = {}, timeout = {}", context_id, batch_id,
              timeout);

    auto* context = GetContext(context_id);
    if (!context) {
        return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    }

    return context->BatchWait(batch_id, timeout, batch_error);
}

int PS4_SYSV_ABI sceAjmDecAt9ParseConfigData(const void* config_data,
                                             OrbisAjmDecAt9ConfigData* out_data) {
    if (!config_data || !out_data) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }

    u32 raw = *reinterpret_cast<const u32*>(config_data);
    if ((raw & 0xFF) != 0xFE) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }
    if (((raw >> 8) & 0x80) != 0) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }

    u32 channel_idx = (raw >> 9) & 0x7;
    if (channel_idx > 5) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }

    if ((raw & 0x100) != 0) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }

    u32 sf_idx = (raw >> 27) & 0x3;
    u32 superframe_multiplier = 0;
    if (sf_idx == 0) {
        superframe_multiplier = 1;
    } else if (sf_idx == 2) {
        superframe_multiplier = 4;
    } else {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }

    if ((raw & 0x7000000) != 0) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }

    static constexpr u32 s_sample_rates[8] = {
        11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
    };
    static constexpr u32 s_frame_samples[8] = {
        64, 64, 128, 128, 128, 256, 256, 256,
    };
    static constexpr u32 s_channel_counts[6] = {
        1, 2, 2, 6, 8, 4,
    };

    u32 sr_idx = (raw >> 12) & 0x7;
    u32 channels = s_channel_counts[channel_idx];
    u32 sample_rate = s_sample_rates[sr_idx];
    u32 frame_samples = s_frame_samples[sr_idx];
    u32 frame_bytes_base = ((raw >> 29) + (((raw >> 16) & 0xFF) * 8) + 1);

    out_data->channels = channels;
    out_data->sample_rate = sample_rate;
    out_data->frame_samples = frame_samples;
    out_data->superframe_samples = frame_samples * superframe_multiplier;
    out_data->superframe_bytes = frame_bytes_base * superframe_multiplier;

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAjmDecMp3ParseFrame(const u8* buf, u32 stream_size, int parse_ofl,
                                        AjmDecMp3ParseFrame* frame) {
    return AjmMp3Decoder::ParseMp3Frame(buf, stream_size, parse_ofl, frame);
}

int PS4_SYSV_ABI sceAjmFinalize(u32 context_id) {
    LOG_INFO(Lib_Ajm, "called context = {}", context_id);
    std::lock_guard lock(s_contexts_mutex);
    auto it = contexts.find(context_id);
    if (it == contexts.end()) {
        return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    }
    contexts.erase(it);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAjmInitialize(s64 reserved, u32* p_context_id) {
    LOG_INFO(Lib_Ajm, "called reserved = {}", reserved);
    if (p_context_id == nullptr || reserved != 0) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }
    *p_context_id = 0xffffffff;
    std::lock_guard lock(s_contexts_mutex);
    u32 id = contexts.size() + 1;
    *p_context_id = id;
    contexts.emplace(id, std::make_unique<AjmContext>());
    return ORBIS_OK;
}

AjmCodecType PS4_SYSV_ABI sceAjmInstanceCodecType(u32 instance_id) {
    return static_cast<AjmCodecType>((instance_id >> 14) & 0x1F);
}

int PS4_SYSV_ABI sceAjmInstanceCreate(u32 context_id, AjmCodecType codec_type,
                                      AjmInstanceFlags flags, u32* out_instance) {
    LOG_INFO(Lib_Ajm, "called context = {}, codec_type = {}, flags = {:#x}", context_id,
             magic_enum::enum_name(codec_type), flags.raw);
    if (!out_instance) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }
    *out_instance = 0xffffffff;
    if (std::to_underlying(codec_type) > 0x16) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }
    if ((flags.raw & 0x380) > 0x17f) {
        return ORBIS_AJM_ERROR_FLAG_NOT_SUPPORTED;
    }

    auto* context = GetContext(context_id);
    if (!context) {
        return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    }

    return context->InstanceCreate(codec_type, flags, out_instance);
}

int PS4_SYSV_ABI sceAjmInstanceDestroy(u32 context_id, u32 instance_id) {
    LOG_INFO(Lib_Ajm, "called context = {}, instance = {}", context_id, instance_id);

    auto* context = GetContext(context_id);
    if (!context) {
        return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    }

    return context->InstanceDestroy(instance_id);
}

int PS4_SYSV_ABI sceAjmInstanceExtend(u32 context_id, AjmCodecType codec_type,
                                      AjmInstanceFlags flags, u32 instance_id) {
    LOG_INFO(Lib_Ajm, "called context = {}, codec_type = {}, flags = {:#x}, instance = {}",
             context_id, magic_enum::enum_name(codec_type), flags.raw, instance_id);
    if (std::to_underlying(codec_type) > 0x16) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }
    if ((flags.raw & 0x380) > 0x17f) {
        return ORBIS_AJM_ERROR_FLAG_NOT_SUPPORTED;
    }

    auto* context = GetContext(context_id);
    if (!context) {
        return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAjmInstanceSwitch(u32 context_id, AjmCodecType codec_type,
                                      AjmInstanceFlags flags, u32 instance_id) {
    LOG_INFO(Lib_Ajm, "called context = {}, codec_type = {}, flags = {:#x}, instance = {}",
             context_id, magic_enum::enum_name(codec_type), flags.raw, instance_id);
    if (std::to_underlying(codec_type) > 0x16) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }
    if ((flags.raw & 0x380) > 0x17f) {
        return ORBIS_AJM_ERROR_FLAG_NOT_SUPPORTED;
    }

    auto* context = GetContext(context_id);
    if (!context) {
        return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAjmMemoryRegister(u32 context_id, void* ptr, size_t num_pages) {
    LOG_TRACE(Lib_Ajm, "called context = {}, ptr = {}, num_pages = {}", context_id, ptr, num_pages);
    if (!ptr || num_pages == 0) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }

    auto* context = GetContext(context_id);
    if (!context) {
        return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAjmMemoryUnregister(u32 context_id, void* ptr) {
    LOG_TRACE(Lib_Ajm, "called context = {}, ptr = {}", context_id, ptr);
    if (!ptr) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }

    auto* context = GetContext(context_id);
    if (!context) {
        return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAjmModuleRegister(u32 context_id, AjmCodecType codec_type, s64 reserved) {
    LOG_INFO(Lib_Ajm, "called context = {}, codec_type = {}", context_id,
             magic_enum::enum_name(codec_type));
    if (reserved != 0 || std::to_underlying(codec_type) > 0x16) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }

    auto* context = GetContext(context_id);
    if (!context) {
        return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    }

    return context->ModuleRegister(codec_type);
}

int PS4_SYSV_ABI sceAjmModuleUnregister(u32 context_id, AjmCodecType codec_type) {
    LOG_INFO(Lib_Ajm, "called context = {}, codec_type = {}", context_id,
             magic_enum::enum_name(codec_type));
    if (std::to_underlying(codec_type) > 0x16) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }

    auto* context = GetContext(context_id);
    if (!context) {
        return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    }

    return context->ModuleUnregister(codec_type);
}

const char* PS4_SYSV_ABI sceAjmStrError(int error_code) {
    switch (error_code) {
    case ORBIS_OK:
        return "ok";
    case ORBIS_AJM_ERROR_INVALID_CONTEXT:
        return "context does not exist";
    case ORBIS_AJM_ERROR_INVALID_INSTANCE:
        return "instance does not exist";
    case ORBIS_AJM_ERROR_INVALID_BATCH:
        return "batch does not exist";
    case ORBIS_AJM_ERROR_INVALID_PARAMETER:
        return "invalid parameter";
    case ORBIS_AJM_ERROR_OUT_OF_MEMORY:
        return "not enough memory";
    case ORBIS_AJM_ERROR_OUT_OF_RESOURCES:
        return "not enough resources";
    case ORBIS_AJM_ERROR_CODEC_NOT_SUPPORTED:
        return "codec is not supported";
    case ORBIS_AJM_ERROR_CODEC_ALREADY_REGISTERED:
        return "codec is already registered in the context";
    case ORBIS_AJM_ERROR_CODEC_NOT_REGISTERED:
        return "codec is not registered in the context";
    case ORBIS_AJM_ERROR_WRONG_REVISION_FLAG:
        return "wrong revision in the flags";
    case ORBIS_AJM_ERROR_FLAG_NOT_SUPPORTED:
        return "flag is not supported";
    case ORBIS_AJM_ERROR_BUSY:
        return "busy";
    case ORBIS_AJM_ERROR_BAD_PRIORITY:
        return "priority is invalid";
    case ORBIS_AJM_ERROR_IN_PROGRESS:
        return "in progress";
    case ORBIS_AJM_ERROR_RETRY:
        return "operation cannot be performed at the moment";
    case ORBIS_AJM_ERROR_MALFORMED_BATCH:
        return "malformed batch";
    case ORBIS_AJM_ERROR_JOB_CREATION:
        return "error at job creation";
    case ORBIS_AJM_ERROR_INVALID_OPCODE:
        return "invalid opcode";
    case ORBIS_AJM_ERROR_PRIORITY_VIOLATION:
        return "priority violation";
    case ORBIS_AJM_ERROR_BUFFER_TOO_BIG:
        return "buffer is too big";
    case ORBIS_AJM_ERROR_INVALID_ADDRESS:
        return "invalid address";
    case ORBIS_AJM_ERROR_CANCELLED:
        return "batch was cancelled";
    default:
        return "(unknown)";
    }
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("NVDXiUesSbA", "libSceAjm", 1, "libSceAjm", sceAjmBatchCancel);
    LIB_FUNCTION("WfAiBW8Wcek", "libSceAjm", 1, "libSceAjm", sceAjmBatchErrorDump);
    LIB_FUNCTION("dmDybN--Fn8", "libSceAjm", 1, "libSceAjm", sceAjmBatchJobControlBufferRa);
    LIB_FUNCTION("stlghnic3Jc", "libSceAjm", 1, "libSceAjm", sceAjmBatchJobInlineBuffer);
    LIB_FUNCTION("ElslOCpOIns", "libSceAjm", 1, "libSceAjm", sceAjmBatchJobRunBufferRa);
    LIB_FUNCTION("7jdAXK+2fMo", "libSceAjm", 1, "libSceAjm", sceAjmBatchJobRunSplitBufferRa);
    LIB_FUNCTION("fFFkk0xfGWs", "libSceAjm", 1, "libSceAjm", sceAjmBatchStartBuffer);
    LIB_FUNCTION("-qLsfDAywIY", "libSceAjm", 1, "libSceAjm", sceAjmBatchWait);
    LIB_FUNCTION("1t3ixYNXyuc", "libSceAjm", 1, "libSceAjm", sceAjmDecAt9ParseConfigData);
    LIB_FUNCTION("eDFeTyi+G3Y", "libSceAjm", 1, "libSceAjm", sceAjmDecMp3ParseFrame);
    LIB_FUNCTION("MHur6qCsUus", "libSceAjm", 1, "libSceAjm", sceAjmFinalize);
    LIB_FUNCTION("dl+4eHSzUu4", "libSceAjm", 1, "libSceAjm", sceAjmInitialize);
    LIB_FUNCTION("diXjQNiMu-s", "libSceAjm", 1, "libSceAjm", sceAjmInstanceCodecType);
    LIB_FUNCTION("AxoDrINp4J8", "libSceAjm", 1, "libSceAjm", sceAjmInstanceCreate);
    LIB_FUNCTION("RbLbuKv8zho", "libSceAjm", 1, "libSceAjm", sceAjmInstanceDestroy);
    LIB_FUNCTION("YDFR0dDVGAg", "libSceAjm", 1, "libSceAjm", sceAjmInstanceExtend);
    LIB_FUNCTION("rgLjmfdXocI", "libSceAjm", 1, "libSceAjm", sceAjmInstanceSwitch);
    LIB_FUNCTION("bkRHEYG6lEM", "libSceAjm", 1, "libSceAjm", sceAjmMemoryRegister);
    LIB_FUNCTION("pIpGiaYkHkM", "libSceAjm", 1, "libSceAjm", sceAjmMemoryUnregister);
    LIB_FUNCTION("Q3dyFuwGn64", "libSceAjm", 1, "libSceAjm", sceAjmModuleRegister);
    LIB_FUNCTION("Wi7DtlLV+KI", "libSceAjm", 1, "libSceAjm", sceAjmModuleUnregister);
    LIB_FUNCTION("AxhcqVv5AYU", "libSceAjm", 1, "libSceAjm", sceAjmStrError);
};

} // namespace Libraries::Ajm
