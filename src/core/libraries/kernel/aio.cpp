// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "aio.h"
#include "common/logging/log.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "file_system.h"

namespace Libraries::Kernel {

static constexpr s32 ORBIS_KERNEL_AIO_MAX_QUEUES = 512;
static constexpr s32 ORBIS_KERNEL_AIO_MAX_REQUESTS = 128;

namespace {

struct AioRequestSlot {
    bool allocated = false;
    bool cancelled = false;
    bool in_flight = false;
    bool is_write = false;
    s32 state = ORBIS_KERNEL_ERROR_ESRCH;
    std::vector<OrbisKernelAioRWRequest> requests;
};

std::mutex g_aio_mutex;
std::condition_variable g_aio_queue_cv;
std::condition_variable g_aio_wait_cv;
std::array<AioRequestSlot, ORBIS_KERNEL_AIO_MAX_QUEUES> g_slots;
std::deque<OrbisKernelAioSubmitId> g_job_queue;
s32 g_id_index = 1;
bool g_aio_initialized = false;
bool g_stop_worker = false;

void AbortRemainingRequests(const AioRequestSlot& slot, size_t start_index) {
    for (size_t i = start_index; i < slot.requests.size(); i++) {
        if (slot.requests[i].result != nullptr) {
            slot.requests[i].result->state = ORBIS_KERNEL_AIO_STATE_ABORTED;
            slot.requests[i].result->returnValue = ORBIS_KERNEL_ERROR_ECANCELED;
        }
    }
}

void AioWorkerLoop() {
    while (true) {
        OrbisKernelAioSubmitId job_id = 0;
        std::vector<OrbisKernelAioRWRequest> reqs;
        bool is_write = false;

        {
            std::unique_lock lk{g_aio_mutex};
            g_aio_queue_cv.wait(lk, [] { return g_stop_worker || !g_job_queue.empty(); });
            if (g_stop_worker && g_job_queue.empty()) {
                return;
            }
            job_id = g_job_queue.front();
            g_job_queue.pop_front();

            auto& slot = g_slots[job_id];
            if (!slot.allocated) {
                continue;
            }
            if (slot.cancelled) {
                AbortRemainingRequests(slot, 0);
                slot.state = ORBIS_KERNEL_AIO_STATE_ABORTED;
                g_aio_wait_cv.notify_all();
                continue;
            }
            slot.in_flight = true;
            reqs = slot.requests;
            is_write = slot.is_write;
        }

        bool all_ok = true;
        bool cancelled_mid_batch = false;
        for (size_t i = 0; i < reqs.size(); i++) {
            {
                std::scoped_lock lk{g_aio_mutex};
                const auto& slot = g_slots[job_id];
                if (!slot.allocated) {
                    cancelled_mid_batch = true;
                    break;
                }
                if (slot.cancelled) {
                    AbortRemainingRequests(slot, i);
                    cancelled_mid_batch = true;
                    break;
                }
            }

            const auto& req = reqs[i];
            const s64 ret = is_write ? sceKernelPwrite(req.fd, req.buf, req.nbyte, req.offset)
                                     : sceKernelPread(req.fd, req.buf, req.nbyte, req.offset);
            if (ret < 0) {
                all_ok = false;
            }
            if (req.result != nullptr) {
                req.result->state =
                    ret < 0 ? ORBIS_KERNEL_AIO_STATE_ABORTED : ORBIS_KERNEL_AIO_STATE_COMPLETED;
                req.result->returnValue = ret;
            }
        }

        {
            std::scoped_lock lk{g_aio_mutex};
            auto& slot = g_slots[job_id];
            slot.in_flight = false;
            if (slot.allocated) {
                const bool completed = all_ok && !cancelled_mid_batch && !slot.cancelled;
                slot.state =
                    completed ? ORBIS_KERNEL_AIO_STATE_COMPLETED : ORBIS_KERNEL_AIO_STATE_ABORTED;
            }
        }
        g_aio_wait_cv.notify_all();
    }
}

struct AioWorkerHost {
    std::thread worker;

    void EnsureStarted() {
        if (!worker.joinable()) {
            g_stop_worker = false;
            worker = std::thread(AioWorkerLoop);
        }
    }

    ~AioWorkerHost() {
        {
            std::scoped_lock lk{g_aio_mutex};
            g_stop_worker = true;
        }
        g_aio_queue_cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }
};

AioWorkerHost g_worker_host;

s32 AllocateSlotLocked(std::vector<OrbisKernelAioRWRequest> reqs, bool is_write,
                       OrbisKernelAioSubmitId& out_id) {
    for (s32 attempt = 0; attempt < ORBIS_KERNEL_AIO_MAX_QUEUES - 1; attempt++) {
        const s32 candidate = g_id_index;
        g_id_index = (g_id_index + 1) % ORBIS_KERNEL_AIO_MAX_QUEUES;
        if (g_id_index == 0) {
            g_id_index = 1;
        }
        auto& slot = g_slots[candidate];
        if (slot.allocated || slot.in_flight) {
            continue;
        }
        slot.allocated = true;
        slot.cancelled = false;
        slot.is_write = is_write;
        slot.state = ORBIS_KERNEL_AIO_STATE_PROCESSING;
        slot.requests = std::move(reqs);
        out_id = candidate;
        return ORBIS_OK;
    }
    return ORBIS_KERNEL_ERROR_EAGAIN;
}

bool IsValidSlotId(OrbisKernelAioSubmitId id) {
    return id > 0 && id < ORBIS_KERNEL_AIO_MAX_QUEUES;
}

s32 PollAioRequestsImpl(const OrbisKernelAioSubmitId* id, s32 num, s32* state) {
    if (state == nullptr || id == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    std::scoped_lock lk{g_aio_mutex};
    for (s32 i = 0; i < num; i++) {
        if (!IsValidSlotId(id[i]) || !g_slots[id[i]].allocated) {
            state[i] = ORBIS_KERNEL_ERROR_ESRCH;
        } else {
            state[i] = g_slots[id[i]].state;
        }
    }
    return ORBIS_OK;
}

s32 CancelAioRequestsImpl(const OrbisKernelAioSubmitId* id, s32 num, s32* state) {
    if (state == nullptr || id == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    bool woke_waiters = false;
    {
        std::scoped_lock lk{g_aio_mutex};
        for (s32 i = 0; i < num; i++) {
            if (!IsValidSlotId(id[i]) || !g_slots[id[i]].allocated) {
                state[i] = ORBIS_KERNEL_ERROR_ESRCH;
                continue;
            }
            auto& slot = g_slots[id[i]];
            if (slot.state == ORBIS_KERNEL_AIO_STATE_PROCESSING) {
                slot.cancelled = true;
                const auto it = std::find(g_job_queue.begin(), g_job_queue.end(), id[i]);
                if (it != g_job_queue.end()) {
                    g_job_queue.erase(it);
                    AbortRemainingRequests(slot, 0);
                    slot.state = ORBIS_KERNEL_AIO_STATE_ABORTED;
                    woke_waiters = true;
                }
            }
            state[i] = slot.state;
        }
    }
    if (woke_waiters) {
        g_aio_wait_cv.notify_all();
    }
    return ORBIS_OK;
}

s32 DeleteAioRequestsImpl(const OrbisKernelAioSubmitId* id, s32 num, s32* ret) {
    if (ret == nullptr || id == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    {
        std::scoped_lock lk{g_aio_mutex};
        for (s32 i = 0; i < num; i++) {
            if (!IsValidSlotId(id[i]) || !g_slots[id[i]].allocated) {
                ret[i] = ORBIS_KERNEL_ERROR_ESRCH;
                continue;
            }
            auto& slot = g_slots[id[i]];
            std::erase(g_job_queue, id[i]);
            slot.allocated = false;
            slot.cancelled = true;
            slot.state = ORBIS_KERNEL_ERROR_ESRCH;
            slot.requests.clear();
            ret[i] = ORBIS_OK;
        }
    }
    g_aio_wait_cv.notify_all();
    return ORBIS_OK;
}

s32 SubmitAioCommandsImpl(OrbisKernelAioRWRequest req[], s32 size, bool is_write, bool multiple,
                          OrbisKernelAioSubmitId id[]) {
    if (req == nullptr || id == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (size < 1 || size > ORBIS_KERNEL_AIO_MAX_REQUESTS) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    for (s32 i = 0; i < size; i++) {
        if (req[i].result == nullptr) {
            return ORBIS_KERNEL_ERROR_EFAULT;
        }
    }

    {
        std::scoped_lock lk{g_aio_mutex};
        g_aio_initialized = true;
        g_worker_host.EnsureStarted();

        if (!multiple) {
            std::vector<OrbisKernelAioRWRequest> batch(req, req + size);
            OrbisKernelAioSubmitId assigned = 0;
            const s32 rc = AllocateSlotLocked(std::move(batch), is_write, assigned);
            if (rc != ORBIS_OK) {
                return rc;
            }
            id[0] = assigned;
            g_job_queue.push_back(assigned);
        } else {
            std::vector<OrbisKernelAioSubmitId> assigned_ids;
            assigned_ids.reserve(size);
            for (s32 i = 0; i < size; i++) {
                std::vector<OrbisKernelAioRWRequest> single{req[i]};
                OrbisKernelAioSubmitId assigned = 0;
                const s32 rc = AllocateSlotLocked(std::move(single), is_write, assigned);
                if (rc != ORBIS_OK) {
                    for (const auto rollback_id : assigned_ids) {
                        g_slots[rollback_id].allocated = false;
                        g_slots[rollback_id].state = ORBIS_KERNEL_ERROR_ESRCH;
                        g_slots[rollback_id].requests.clear();
                    }
                    return rc;
                }
                assigned_ids.push_back(assigned);
            }
            for (s32 i = 0; i < size; i++) {
                id[i] = assigned_ids[i];
                g_job_queue.push_back(assigned_ids[i]);
            }
        }
    }

    g_aio_queue_cv.notify_all();
    return ORBIS_OK;
}

s32 WaitAioRequestsImpl(const OrbisKernelAioSubmitId* id, s32 num, s32* state, u32 mode,
                        const u32* usec) {
    if (state == nullptr || id == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    for (s32 i = 0; i < num; i++) {
        if (id[i] < 0 || id[i] >= ORBIS_KERNEL_AIO_MAX_QUEUES) {
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
    }

    const auto is_done = [mode](OrbisKernelAioSubmitId req_id) {
        const auto& slot = g_slots[req_id];
        if (!slot.allocated) {
            return true;
        }
        if (mode == 2) {
            return slot.state == ORBIS_KERNEL_AIO_STATE_COMPLETED ||
                   slot.state == ORBIS_KERNEL_AIO_STATE_ABORTED;
        }
        return slot.state != ORBIS_KERNEL_AIO_STATE_PROCESSING;
    };

    const auto all_or_any_done = [&]() {
        return (mode == 2) ? std::any_of(id, id + num, is_done)
                           : std::all_of(id, id + num, is_done);
    };

    std::unique_lock lk{g_aio_mutex};
    bool timed_out = false;
    if (usec == nullptr) {
        g_aio_wait_cv.wait(lk, all_or_any_done);
    } else if (*usec == 0) {
        timed_out = !all_or_any_done();
    } else {
        timed_out = !g_aio_wait_cv.wait_for(lk, std::chrono::microseconds(*usec), all_or_any_done);
    }

    for (s32 i = 0; i < num; i++) {
        state[i] = g_slots[id[i]].allocated ? g_slots[id[i]].state : ORBIS_KERNEL_ERROR_ESRCH;
    }

    return timed_out ? ORBIS_KERNEL_ERROR_ETIMEDOUT : ORBIS_OK;
}

} // namespace

s32 PS4_SYSV_ABI sceKernelAioInitializeImpl(void* p, s32 size) {
    if (p == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    std::scoped_lock lk{g_aio_mutex};
    if (g_aio_initialized) {
        return ORBIS_KERNEL_ERROR_EBUSY;
    }
    if (size != static_cast<s32>(sizeof(OrbisKernelAioParam))) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    g_aio_initialized = true;
    g_worker_host.EnsureStarted();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelAioDeleteRequest(OrbisKernelAioSubmitId id, s32* ret) {
    return DeleteAioRequestsImpl(&id, 1, ret);
}

s32 PS4_SYSV_ABI sceKernelAioDeleteRequests(OrbisKernelAioSubmitId id[], s32 num, s32 ret[]) {
    if (num < 1 || num > ORBIS_KERNEL_AIO_MAX_REQUESTS) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    return DeleteAioRequestsImpl(id, num, ret);
}

s32 PS4_SYSV_ABI sceKernelAioPollRequest(OrbisKernelAioSubmitId id, s32* state) {
    return PollAioRequestsImpl(&id, 1, state);
}

s32 PS4_SYSV_ABI sceKernelAioPollRequests(OrbisKernelAioSubmitId id[], s32 num, s32 state[]) {
    if (num < 1 || num > ORBIS_KERNEL_AIO_MAX_REQUESTS) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    return PollAioRequestsImpl(id, num, state);
}

s32 PS4_SYSV_ABI sceKernelAioCancelRequest(OrbisKernelAioSubmitId id, s32* state) {
    return CancelAioRequestsImpl(&id, 1, state);
}

s32 PS4_SYSV_ABI sceKernelAioCancelRequests(OrbisKernelAioSubmitId id[], s32 num, s32 state[]) {
    if (num < 1 || num > ORBIS_KERNEL_AIO_MAX_REQUESTS) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    return CancelAioRequestsImpl(id, num, state);
}

s32 PS4_SYSV_ABI sceKernelAioWaitRequest(OrbisKernelAioSubmitId id, s32* state, u32* usec) {
    return WaitAioRequestsImpl(&id, 1, state, 1, usec);
}

s32 PS4_SYSV_ABI sceKernelAioWaitRequests(OrbisKernelAioSubmitId id[], s32 num, s32 state[],
                                          u32 mode, u32* usec) {
    if (num < 1 || num > ORBIS_KERNEL_AIO_MAX_REQUESTS || (mode != 1 && mode != 2)) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    return WaitAioRequestsImpl(id, num, state, mode, usec);
}

s32 PS4_SYSV_ABI sceKernelAioSubmitReadCommands(OrbisKernelAioRWRequest req[], s32 size, s32 prio,
                                                OrbisKernelAioSubmitId* id) {
    return SubmitAioCommandsImpl(req, size, /*is_write=*/false, /*multiple=*/false, id);
}

s32 PS4_SYSV_ABI sceKernelAioSubmitReadCommandsMultiple(OrbisKernelAioRWRequest req[], s32 size,
                                                        s32 prio, OrbisKernelAioSubmitId id[]) {
    return SubmitAioCommandsImpl(req, size, /*is_write=*/false, /*multiple=*/true, id);
}

s32 PS4_SYSV_ABI sceKernelAioSubmitWriteCommands(OrbisKernelAioRWRequest req[], s32 size, s32 prio,
                                                 OrbisKernelAioSubmitId* id) {
    return SubmitAioCommandsImpl(req, size, /*is_write=*/true, /*multiple=*/false, id);
}

s32 PS4_SYSV_ABI sceKernelAioSubmitWriteCommandsMultiple(OrbisKernelAioRWRequest req[], s32 size,
                                                         s32 prio, OrbisKernelAioSubmitId id[]) {
    return SubmitAioCommandsImpl(req, size, /*is_write=*/true, /*multiple=*/true, id);
}

s32 PS4_SYSV_ABI sceKernelAioSetParam(OrbisKernelAioParam* param, u32 prio_mid, u32 prio_high,
                                      u32 enable_split, u32 read_split_size, u32 write_split_size) {
    if (param == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (prio_mid > 0x80 || prio_high > 0x80) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (enable_split == 0) {
        read_split_size = 0;
        write_split_size = 0;
    } else if (enable_split == 1) {
        if (read_split_size == 0 || read_split_size > 0x1000000 || write_split_size == 0 ||
            write_split_size > 0x1000000) {
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
    } else {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    param->queues[0] = {prio_mid, prio_high, enable_split, read_split_size, write_split_size};
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelAioInitializeParam(OrbisKernelAioParam* param) {
    if (param == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    static constexpr OrbisKernelAioParam DefaultParam = {{
        {0x20, 0x20, 1, 0x100000, 0x100000},
        {0x20, 0x20, 1, 0x100000, 0x100000},
        {0x20, 0x20, 0, 0, 0},
    }};
    *param = DefaultParam;
    return ORBIS_OK;
}

void RegisterAio(Core::Loader::SymbolsResolver* sym) {
    {
        std::scoped_lock lk{g_aio_mutex};
        g_id_index = 1;
        g_aio_initialized = false;
        g_job_queue.clear();
        for (auto& slot : g_slots) {
            slot = AioRequestSlot{};
        }
    }

    LIB_FUNCTION("fR521KIGgb8", "libkernel", 1, "libkernel", sceKernelAioCancelRequest);
    LIB_FUNCTION("3Lca1XBrQdY", "libkernel", 1, "libkernel", sceKernelAioCancelRequests);
    LIB_FUNCTION("5TgME6AYty4", "libkernel", 1, "libkernel", sceKernelAioDeleteRequest);
    LIB_FUNCTION("Ft3EtsZzAoY", "libkernel", 1, "libkernel", sceKernelAioDeleteRequests);
    LIB_FUNCTION("vYU8P9Td2Zo", "libkernel", 1, "libkernel", sceKernelAioInitializeImpl);
    LIB_FUNCTION("nu4a0-arQis", "libkernel", 1, "libkernel", sceKernelAioInitializeParam);
    LIB_FUNCTION("2pOuoWoCxdk", "libkernel", 1, "libkernel", sceKernelAioPollRequest);
    LIB_FUNCTION("o7O4z3jwKzo", "libkernel", 1, "libkernel", sceKernelAioPollRequests);
    LIB_FUNCTION("9WK-vhNXimw", "libkernel", 1, "libkernel", sceKernelAioSetParam);
    LIB_FUNCTION("HgX7+AORI58", "libkernel", 1, "libkernel", sceKernelAioSubmitReadCommands);
    LIB_FUNCTION("lXT0m3P-vs4", "libkernel", 1, "libkernel",
                 sceKernelAioSubmitReadCommandsMultiple);
    LIB_FUNCTION("XQ8C8y+de+E", "libkernel", 1, "libkernel", sceKernelAioSubmitWriteCommands);
    LIB_FUNCTION("xT3Cpz0yh6Y", "libkernel", 1, "libkernel",
                 sceKernelAioSubmitWriteCommandsMultiple);
    LIB_FUNCTION("KOF-oJbQVvc", "libkernel", 1, "libkernel", sceKernelAioWaitRequest);
    LIB_FUNCTION("lgK+oIWkJyA", "libkernel", 1, "libkernel", sceKernelAioWaitRequests);
}

} // namespace Libraries::Kernel
