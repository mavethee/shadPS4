// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "common/types.h"
#include "core/file_sys/devices/base_device.h"
#include "core/libraries/kernel/file_system.h"
#include "core/libraries/kernel/orbis_error.h"

namespace Core::Devices {

enum class PipeEnd {
    Read,
    Write,
};

struct PipeState {
    static constexpr size_t CAPACITY = 65536;

    std::mutex mutex;
    std::condition_variable cv_read;
    std::condition_variable cv_write;
    std::vector<u8> ring_buffer;
    size_t head = 0;
    size_t tail = 0;
    size_t size = 0;
    size_t readers = 1;
    size_t writers = 1;

    PipeState() : ring_buffer(CAPACITY) {}
};

class PipeDevice final : public BaseDevice {
    std::shared_ptr<PipeState> state;
    PipeEnd end;

public:
    explicit PipeDevice(std::shared_ptr<PipeState> state, PipeEnd end)
        : state(std::move(state)), end(end) {}

    ~PipeDevice() override {
        std::scoped_lock lock{state->mutex};
        if (end == PipeEnd::Read) {
            if (state->readers > 0) {
                state->readers--;
            }
            state->cv_write.notify_all();
        } else {
            if (state->writers > 0) {
                state->writers--;
            }
            state->cv_read.notify_all();
        }
    }

    PipeDevice(const PipeDevice&) = delete;
    PipeDevice& operator=(const PipeDevice&) = delete;

    s64 read(void* buf, u64 nbytes) override {
        if (end != PipeEnd::Read) {
            return ORBIS_KERNEL_ERROR_EBADF;
        }
        if (nbytes == 0) {
            return 0;
        }
        std::unique_lock lock{state->mutex};
        while (state->size == 0) {
            if (state->writers == 0) {
                return 0; // EOF
            }
            state->cv_read.wait(lock);
        }

        size_t to_read = std::min(static_cast<size_t>(nbytes), state->size);
        auto* dst = static_cast<u8*>(buf);
        size_t first_chunk = std::min(to_read, PipeState::CAPACITY - state->head);
        std::memcpy(dst, state->ring_buffer.data() + state->head, first_chunk);
        if (to_read > first_chunk) {
            std::memcpy(dst + first_chunk, state->ring_buffer.data(), to_read - first_chunk);
        }
        state->head = (state->head + to_read) % PipeState::CAPACITY;
        state->size -= to_read;
        state->cv_write.notify_all();
        return static_cast<s64>(to_read);
    }

    s64 write(const void* buf, u64 nbytes) override {
        if (end != PipeEnd::Write) {
            return ORBIS_KERNEL_ERROR_EBADF;
        }
        if (nbytes == 0) {
            return 0;
        }
        std::unique_lock lock{state->mutex};
        if (state->readers == 0) {
            return ORBIS_KERNEL_ERROR_EPIPE;
        }

        const auto* src = static_cast<const u8*>(buf);
        size_t total_written = 0;
        while (total_written < nbytes) {
            while (state->size >= PipeState::CAPACITY) {
                if (state->readers == 0) {
                    return total_written > 0 ? static_cast<s64>(total_written)
                                             : ORBIS_KERNEL_ERROR_EPIPE;
                }
                state->cv_write.wait(lock);
            }
            if (state->readers == 0) {
                return total_written > 0 ? static_cast<s64>(total_written)
                                         : ORBIS_KERNEL_ERROR_EPIPE;
            }

            size_t available = PipeState::CAPACITY - state->size;
            size_t chunk = std::min(available, static_cast<size_t>(nbytes - total_written));
            size_t first_chunk = std::min(chunk, PipeState::CAPACITY - state->tail);
            std::memcpy(state->ring_buffer.data() + state->tail, src + total_written, first_chunk);
            if (chunk > first_chunk) {
                std::memcpy(state->ring_buffer.data(), src + total_written + first_chunk,
                            chunk - first_chunk);
            }
            state->tail = (state->tail + chunk) % PipeState::CAPACITY;
            state->size += chunk;
            total_written += chunk;
            state->cv_read.notify_all();
        }
        return static_cast<s64>(total_written);
    }

    s64 readv(const Libraries::Kernel::OrbisKernelIovec* iov, s32 iovcnt) override {
        if (end != PipeEnd::Read) {
            return ORBIS_KERNEL_ERROR_EBADF;
        }
        if (iovcnt <= 0 || iovcnt > 1024) {
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
        if (!iov) {
            return ORBIS_KERNEL_ERROR_EFAULT;
        }
        s64 total = 0;
        for (s32 i = 0; i < iovcnt; ++i) {
            if (iov[i].iov_len == 0) {
                continue;
            }
            if (!iov[i].iov_base) {
                return total > 0 ? total : ORBIS_KERNEL_ERROR_EFAULT;
            }
            s64 bytes = read(iov[i].iov_base, iov[i].iov_len);
            if (bytes < 0) {
                return total > 0 ? total : bytes;
            }
            total += bytes;
            if (bytes < static_cast<s64>(iov[i].iov_len)) {
                break;
            }
        }
        return total;
    }

    s64 writev(const Libraries::Kernel::OrbisKernelIovec* iov, s32 iovcnt) override {
        if (end != PipeEnd::Write) {
            return ORBIS_KERNEL_ERROR_EBADF;
        }
        if (iovcnt <= 0 || iovcnt > 1024) {
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
        if (!iov) {
            return ORBIS_KERNEL_ERROR_EFAULT;
        }
        s64 total = 0;
        for (s32 i = 0; i < iovcnt; ++i) {
            if (iov[i].iov_len == 0) {
                continue;
            }
            if (!iov[i].iov_base) {
                return total > 0 ? total : ORBIS_KERNEL_ERROR_EFAULT;
            }
            s64 bytes = write(iov[i].iov_base, iov[i].iov_len);
            if (bytes < 0) {
                return total > 0 ? total : bytes;
            }
            total += bytes;
            if (bytes < static_cast<s64>(iov[i].iov_len)) {
                break;
            }
        }
        return total;
    }

    s64 lseek(s64 offset, s32 whence) override {
        return ORBIS_KERNEL_ERROR_ESPIPE;
    }

    s64 pread(void* buf, u64 nbytes, s64 offset) {
        return ORBIS_KERNEL_ERROR_ESPIPE;
    }

    s64 pwrite(const void* buf, u64 nbytes, s64 offset) override {
        return ORBIS_KERNEL_ERROR_ESPIPE;
    }

    s64 preadv(const Libraries::Kernel::OrbisKernelIovec* iov, s32 iovcnt, s64 offset) override {
        return ORBIS_KERNEL_ERROR_ESPIPE;
    }

    s64 pwritev(const Libraries::Kernel::OrbisKernelIovec* iov, s32 iovcnt, s64 offset) override {
        return ORBIS_KERNEL_ERROR_ESPIPE;
    }

    s32 fstat(Libraries::Kernel::OrbisKernelStat* sb) override {
        if (!sb) {
            return ORBIS_KERNEL_ERROR_EFAULT;
        }
        std::memset(sb, 0, sizeof(Libraries::Kernel::OrbisKernelStat));
        sb->st_mode = 0000666u | 0010000u; // S_IFIFO
        sb->st_nlink = 1;
        sb->st_blksize = 4096;
        std::scoped_lock lock{state->mutex};
        sb->st_size = static_cast<s64>(state->size);
        return 0;
    }

    s32 fsync() override {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    s32 ftruncate(s64 length) override {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    s64 getdents(void* buf, u32 nbytes, s64* basep) override {
        return ORBIS_KERNEL_ERROR_ENOTDIR;
    }
};

} // namespace Core::Devices
