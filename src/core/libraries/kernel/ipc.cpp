// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <span>
#include <system_error>
#include <unordered_map>
#include <nlohmann/json.hpp>

#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/file_sys/backends/host_fs.h"
#include "core/file_sys/fs.h"
#include "core/libraries/kernel/file_system.h"
#include "core/libraries/kernel/ipc.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"

namespace Libraries::Kernel {

namespace {

constexpr size_t ORBIS_KSEM_MAX_NAME = 255;

struct KernelSema {
    explicit KernelSema(u32 init_value) : value{init_value} {}

    std::mutex mutex;
    std::condition_variable cv;
    u32 value;
};

struct ShmEntry {
    std::filesystem::path host_path;
    bool unlinked{false};
};

std::mutex g_ksem_mutex;
u64 g_next_ksem_id = 16;
std::unordered_map<s64, std::shared_ptr<KernelSema>> g_ksem_handles;
std::unordered_map<std::string, std::shared_ptr<KernelSema>> g_ksem_named;

std::mutex g_shm_mutex;
std::unordered_map<std::string, ShmEntry> g_shm_named;
std::unordered_map<std::filesystem::path, u32> g_shm_open_counts;

s64 AllocKsemIdLocked() {
    while (g_next_ksem_id < 16 || g_ksem_handles.contains(static_cast<s64>(g_next_ksem_id))) {
        ++g_next_ksem_id;
    }
    return static_cast<s64>(g_next_ksem_id++);
}

std::shared_ptr<KernelSema> GetKsem(s64 id) {
    std::scoped_lock lk{g_ksem_mutex};
    const auto it = g_ksem_handles.find(id);
    return it == g_ksem_handles.end() ? nullptr : it->second;
}

s32 SetPosixError(s32 err) {
    *__Error() = err;
    return -1;
}

bool ValidateShmName(const char* name, std::string& out_key) {
    if (name == nullptr) {
        SetPosixError(POSIX_EFAULT);
        return false;
    }
    const size_t len = std::strlen(name);
    if (len == 0 || name[0] != '/') {
        SetPosixError(POSIX_EINVAL);
        return false;
    }
    if (len >= ORBIS_MAX_PATH) {
        SetPosixError(POSIX_ENAMETOOLONG);
        return false;
    }
    out_key = name;
    return true;
}

std::filesystem::path MakeShmHostPath(const std::string& name) {
    static std::atomic<u64> seq{0};
    const std::filesystem::path dir =
        Common::FS::GetUserPath(Common::FS::PathType::TempDataDir) / "shm";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const u64 hash = std::hash<std::string>{}(name);
    return dir /
           fmt::format("shm_{:016x}_{}.bin", hash, seq.fetch_add(1, std::memory_order_relaxed));
}

struct ConsoleIdentity {
    std::array<u8, 16> openpsid{};
    std::array<u8, 8> board_id{};
};

void FillRandomBytes(std::span<u8> dst) {
    std::random_device rd;
    size_t offset = 0;
    while (offset < dst.size()) {
        const u32 word = rd();
        const size_t chunk = std::min(sizeof(word), dst.size() - offset);
        std::memcpy(dst.data() + offset, &word, chunk);
        offset += chunk;
    }
}

std::string EncodeHex(std::span<const u8> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const u8 b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0f]);
    }
    return out;
}

bool TryDecodeHex(std::string_view hex, std::span<u8> out) {
    if (hex.size() != out.size() * 2) {
        return false;
    }
    const auto hex_val = [](char c) -> s32 {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return 10 + (c - 'a');
        }
        if (c >= 'A' && c <= 'F') {
            return 10 + (c - 'A');
        }
        return -1;
    };
    for (size_t i = 0; i < out.size(); ++i) {
        const s32 hi = hex_val(hex[i * 2]);
        const s32 lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<u8>((hi << 4) | lo);
    }
    return true;
}

const ConsoleIdentity& GetConsoleIdentity() {
    static ConsoleIdentity cached{};
    static std::once_flag once;
    std::call_once(once, [] {
        const auto id_path =
            Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "console_id.json";
        bool has_psid = false;
        bool has_board = false;
        if (std::filesystem::exists(id_path)) {
            try {
                std::ifstream in(id_path);
                if (in.is_open()) {
                    nlohmann::json j;
                    in >> j;
                    has_psid = j.contains("openpsid") && j["openpsid"].is_string() &&
                               TryDecodeHex(j["openpsid"].get<std::string>(), cached.openpsid);
                    has_board = j.contains("board_id") && j["board_id"].is_string() &&
                                TryDecodeHex(j["board_id"].get<std::string>(), cached.board_id);
                }
            } catch (const std::exception& e) {
                LOG_WARNING(Lib_Kernel, "Failed to parse {}: {}", id_path.string(), e.what());
            }
        }
        if (!has_psid) {
            FillRandomBytes(cached.openpsid);
        }
        if (!has_board) {
            FillRandomBytes(cached.board_id);
        }
        if (!has_psid || !has_board) {
            try {
                const auto tmp_path = id_path.string() + ".tmp";
                nlohmann::json j{
                    {"openpsid", EncodeHex(cached.openpsid)},
                    {"board_id", EncodeHex(cached.board_id)},
                };
                {
                    std::ofstream out(tmp_path, std::ios::trunc);
                    out << j.dump(4) << '\n';
                    out.flush();
                }
                std::error_code ec;
                std::filesystem::rename(tmp_path, id_path, ec);
            } catch (const std::exception& e) {
                LOG_WARNING(Lib_Kernel, "Failed to write {}: {}", id_path.string(), e.what());
            }
        }
    });
    return cached;
}

} // namespace

std::array<u8, 16> GetPersistentOpenPsId() {
    return GetConsoleIdentity().openpsid;
}

std::array<u8, 16> GetPersistentDevKitIdPs(bool is_neo) {
    const auto& id = GetConsoleIdentity();
    std::array<u8, 16> idps{0x00, 0x00, 0x00, 0x01, 0x00, 0x80, 0x00, is_neo ? u8{0x07} : u8{0x01}};
    std::memcpy(idps.data() + 8, id.board_id.data(), id.board_id.size());
    return idps;
}

void OnShmFileClosed(const std::filesystem::path& host_path) {
    if (host_path.empty()) {
        return;
    }
    bool remove_backing = false;
    {
        std::scoped_lock lk{g_shm_mutex};
        const auto cit = g_shm_open_counts.find(host_path);
        if (cit == g_shm_open_counts.end()) {
            return;
        }
        if (--cit->second == 0) {
            g_shm_open_counts.erase(cit);
            remove_backing = true;
            for (const auto& [_, entry] : g_shm_named) {
                if (entry.host_path == host_path && !entry.unlinked) {
                    remove_backing = false;
                    break;
                }
            }
        }
    }
    if (remove_backing) {
        std::error_code ec;
        std::filesystem::remove(host_path, ec);
    }
}

s32 PS4_SYSV_ABI posix_ksem_init(s64* id, u32 value) {
    if (id == nullptr) {
        return SetPosixError(POSIX_EFAULT);
    }
    if (value > static_cast<u32>(std::numeric_limits<s32>::max())) {
        return SetPosixError(POSIX_EINVAL);
    }
    auto sem = std::make_shared<KernelSema>(value);
    std::scoped_lock lk{g_ksem_mutex};
    const s64 handle = AllocKsemIdLocked();
    g_ksem_handles.emplace(handle, std::move(sem));
    *id = handle;
    return 0;
}

s32 PS4_SYSV_ABI posix_ksem_destroy(s64 id) {
    std::scoped_lock lk{g_ksem_mutex};
    if (g_ksem_handles.erase(id) == 0) {
        return SetPosixError(POSIX_EINVAL);
    }
    return 0;
}

s32 PS4_SYSV_ABI posix_ksem_open(s64* id, const char* name, s32 oflag, u16 mode, u32 value) {
    if (id == nullptr || name == nullptr) {
        return SetPosixError(POSIX_EFAULT);
    }
    const size_t name_len = std::strlen(name);
    if (name_len == 0 || name[0] != '/') {
        return SetPosixError(POSIX_EINVAL);
    }
    if (name_len > ORBIS_KSEM_MAX_NAME) {
        return SetPosixError(POSIX_ENAMETOOLONG);
    }
    const s32 allowed_flags = ORBIS_KERNEL_O_CREAT | ORBIS_KERNEL_O_EXCL;
    if ((oflag & ~allowed_flags) != 0) {
        return SetPosixError(POSIX_EINVAL);
    }

    std::scoped_lock lk{g_ksem_mutex};
    const auto it = g_ksem_named.find(name);
    if (it != g_ksem_named.end()) {
        if ((oflag & (ORBIS_KERNEL_O_CREAT | ORBIS_KERNEL_O_EXCL)) ==
            (ORBIS_KERNEL_O_CREAT | ORBIS_KERNEL_O_EXCL)) {
            return SetPosixError(POSIX_EEXIST);
        }
        const s64 handle = AllocKsemIdLocked();
        g_ksem_handles.emplace(handle, it->second);
        *id = handle;
        return 0;
    }

    if ((oflag & ORBIS_KERNEL_O_CREAT) == 0) {
        return SetPosixError(POSIX_ENOENT);
    }
    if (value > static_cast<u32>(std::numeric_limits<s32>::max())) {
        return SetPosixError(POSIX_EINVAL);
    }

    auto sem = std::make_shared<KernelSema>(value);
    g_ksem_named.emplace(name, sem);
    const s64 handle = AllocKsemIdLocked();
    g_ksem_handles.emplace(handle, std::move(sem));
    *id = handle;
    return 0;
}

s32 PS4_SYSV_ABI posix_ksem_close(s64 id) {
    std::scoped_lock lk{g_ksem_mutex};
    if (g_ksem_handles.erase(id) == 0) {
        return SetPosixError(POSIX_EINVAL);
    }
    return 0;
}

s32 PS4_SYSV_ABI posix_ksem_unlink(const char* name) {
    if (name == nullptr) {
        return SetPosixError(POSIX_EFAULT);
    }
    if (name[0] != '/') {
        return SetPosixError(POSIX_EINVAL);
    }
    std::scoped_lock lk{g_ksem_mutex};
    if (g_ksem_named.erase(name) == 0) {
        return SetPosixError(POSIX_ENOENT);
    }
    return 0;
}

s32 PS4_SYSV_ABI posix_ksem_post(s64 id) {
    auto sem = GetKsem(id);
    if (!sem) {
        return SetPosixError(POSIX_EINVAL);
    }
    {
        std::scoped_lock lk{sem->mutex};
        if (sem->value >= static_cast<u32>(std::numeric_limits<s32>::max())) {
            return SetPosixError(POSIX_EOVERFLOW);
        }
        ++sem->value;
    }
    sem->cv.notify_one();
    return 0;
}

s32 PS4_SYSV_ABI posix_ksem_trywait(s64 id) {
    auto sem = GetKsem(id);
    if (!sem) {
        return SetPosixError(POSIX_EINVAL);
    }
    std::scoped_lock lk{sem->mutex};
    if (sem->value == 0) {
        return SetPosixError(POSIX_EAGAIN);
    }
    --sem->value;
    return 0;
}

s32 PS4_SYSV_ABI posix_ksem_wait(s64 id) {
    auto sem = GetKsem(id);
    if (!sem) {
        return SetPosixError(POSIX_EINVAL);
    }
    std::unique_lock lk{sem->mutex};
    sem->cv.wait(lk, [&] { return sem->value > 0; });
    --sem->value;
    return 0;
}

s32 PS4_SYSV_ABI posix_ksem_timedwait(s64 id, const OrbisKernelTimespec* abstime) {
    auto sem = GetKsem(id);
    if (!sem) {
        return SetPosixError(POSIX_EINVAL);
    }
    std::unique_lock lk{sem->mutex};
    if (sem->value > 0) {
        --sem->value;
        return 0;
    }
    if (abstime == nullptr) {
        return SetPosixError(POSIX_EFAULT);
    }
    if (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1'000'000'000) {
        return SetPosixError(POSIX_EINVAL);
    }
    if (!sem->cv.wait_until(lk, abstime->TimePoint(), [&] { return sem->value > 0; })) {
        return SetPosixError(POSIX_ETIMEDOUT);
    }
    --sem->value;
    return 0;
}

s32 PS4_SYSV_ABI posix_ksem_getvalue(s64 id, s32* sval) {
    if (sval == nullptr) {
        return SetPosixError(POSIX_EFAULT);
    }
    auto sem = GetKsem(id);
    if (!sem) {
        return SetPosixError(POSIX_EINVAL);
    }
    std::scoped_lock lk{sem->mutex};
    *sval = static_cast<s32>(sem->value);
    return 0;
}

s32 PS4_SYSV_ABI posix_shm_open(const char* path, s32 flags, u16 mode) {
    std::string key;
    if (!ValidateShmName(path, key)) {
        return -1;
    }
    const s32 access = flags & ORBIS_KERNEL_O_ACCMODE;
    if (access != ORBIS_KERNEL_O_RDONLY && access != ORBIS_KERNEL_O_RDWR) {
        return SetPosixError(access == ORBIS_KERNEL_O_WRONLY ? POSIX_EACCES : POSIX_EINVAL);
    }
    const s32 allowed =
        ORBIS_KERNEL_O_ACCMODE | ORBIS_KERNEL_O_CREAT | ORBIS_KERNEL_O_EXCL | ORBIS_KERNEL_O_TRUNC;
    if ((flags & ~allowed) != 0) {
        return SetPosixError(POSIX_EINVAL);
    }

    std::filesystem::path host_path;
    bool create_file = false;
    {
        std::scoped_lock lk{g_shm_mutex};
        const auto it = g_shm_named.find(key);
        if (it != g_shm_named.end()) {
            if ((flags & (ORBIS_KERNEL_O_CREAT | ORBIS_KERNEL_O_EXCL)) ==
                (ORBIS_KERNEL_O_CREAT | ORBIS_KERNEL_O_EXCL)) {
                return SetPosixError(POSIX_EEXIST);
            }
            host_path = it->second.host_path;
        } else {
            if ((flags & ORBIS_KERNEL_O_CREAT) == 0) {
                return SetPosixError(POSIX_ENOENT);
            }
            host_path = MakeShmHostPath(key);
            g_shm_named.emplace(key, ShmEntry{.host_path = host_path, .unlinked = false});
            create_file = true;
        }
        ++g_shm_open_counts[host_path];
    }

    if (create_file) {
        Common::FS::IOFile creator(host_path, Common::FS::FileAccessMode::Create,
                                   Common::FS::FileType::BinaryFile,
                                   Common::FS::FileShareFlag::ShareReadWrite);
        if (!creator.IsOpen()) {
            OnShmFileClosed(host_path);
            return SetPosixError(POSIX_EIO);
        }
    }

    const bool writable = (access == ORBIS_KERNEL_O_RDWR);
    auto backend = std::make_unique<Core::FileSys::HostFile>(
        host_path,
        writable ? Common::FS::FileAccessMode::ReadWrite : Common::FS::FileAccessMode::Read,
        !writable);
    if (!backend->IsOpen()) {
        OnShmFileClosed(host_path);
        return SetPosixError(POSIX_EACCES);
    }
    if ((flags & ORBIS_KERNEL_O_TRUNC) != 0 && writable) {
        if (auto* host_file = backend->GetHostFile()) {
            host_file->SetSize(0);
        }
    }

    auto* table = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    const s32 fd = table->CreateHandle();
    auto* file = table->GetFile(fd);
    file->type = Core::FileSys::FileType::Regular;
    file->is_shm = true;
    file->m_flags = flags;
    file->m_host_name = host_path;
    file->m_guest_name = key;
    file->handle = std::move(backend);
    file->is_opened = true;
    return fd;
}

s32 PS4_SYSV_ABI posix_shm_unlink(const char* path) {
    std::string key;
    if (!ValidateShmName(path, key)) {
        return -1;
    }
    std::filesystem::path host_path;
    bool remove_now = false;
    {
        std::scoped_lock lk{g_shm_mutex};
        const auto it = g_shm_named.find(key);
        if (it == g_shm_named.end()) {
            return SetPosixError(POSIX_ENOENT);
        }
        host_path = it->second.host_path;
        g_shm_named.erase(it);
        remove_now = !g_shm_open_counts.contains(host_path);
    }
    if (remove_now) {
        std::error_code ec;
        std::filesystem::remove(host_path, ec);
    }
    return 0;
}

s32 PS4_SYSV_ABI posix_physhm_open(const char* path, s32 flags, u16 mode) {
    return SetPosixError(POSIX_EINVAL);
}

s32 PS4_SYSV_ABI posix_physhm_unlink(const char* path) {
    return SetPosixError(POSIX_EINVAL);
}

s32 PS4_SYSV_ABI sceKernelGetIpcPath(const char* name, char* out_path) {
    if (name == nullptr || out_path == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    std::snprintf(out_path, 44, "/system_tmp/%.31s", name);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI posix_getppid() {
    return 38;
}

u32 PS4_SYSV_ABI posix_geteuid() {
    return 1;
}

u32 PS4_SYSV_ABI posix_getgid() {
    return 1;
}

u32 PS4_SYSV_ABI posix_getegid() {
    return 1;
}

s32 PS4_SYSV_ABI posix_issetugid() {
    return 1;
}

s32 PS4_SYSV_ABI is_in_sandbox() {
    return 1;
}

void RegisterIpc(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("ePRSOZsroIQ", "libkernel", 1, "libkernel", sceKernelGetIpcPath);
    LIB_FUNCTION("e6ovBo9ZvJc", "libkernel", 1, "libkernel", posix_getppid);
    LIB_FUNCTION("tvpHe5kBO4E", "libkernel", 1, "libkernel", posix_geteuid);
    LIB_FUNCTION("AfuS23bX6kg", "libkernel", 1, "libkernel", posix_getgid);
    LIB_FUNCTION("72rYuYoDTWk", "libkernel", 1, "libkernel", posix_getegid);
    LIB_FUNCTION("AxUhC3zNrhk", "libkernel", 1, "libkernel", posix_issetugid);
    LIB_FUNCTION("WaThXSvAQNc", "libkernel", 1, "libkernel", is_in_sandbox);

    LIB_FUNCTION("TEwqtzAmezo", "libkernel", 1, "libkernel", posix_ksem_init);
    LIB_FUNCTION("asSKL30+heA", "libkernel", 1, "libkernel", posix_ksem_destroy);
    LIB_FUNCTION("k5QQrJEpRYY", "libkernel", 1, "libkernel", posix_ksem_open);
    LIB_FUNCTION("pv9M2jHJ6iw", "libkernel", 1, "libkernel", posix_ksem_close);
    LIB_FUNCTION("tvkQRDe6hNo", "libkernel", 1, "libkernel", posix_ksem_unlink);
    LIB_FUNCTION("s8gWJrY1W-k", "libkernel", 1, "libkernel", posix_ksem_post);
    LIB_FUNCTION("rQvMIxmFD6A", "libkernel", 1, "libkernel", posix_ksem_wait);
    LIB_FUNCTION("gVaofuRvLdU", "libkernel", 1, "libkernel", posix_ksem_trywait);
    LIB_FUNCTION("vE+WlomLSh4", "libkernel", 1, "libkernel", posix_ksem_timedwait);
    LIB_FUNCTION("rOT0UMvtCCA", "libkernel", 1, "libkernel", posix_ksem_getvalue);

    LIB_FUNCTION("QuJYZ2KVGGQ", "libkernel", 1, "libkernel", posix_shm_open);
    LIB_FUNCTION("QuJYZ2KVGGQ", "libScePosix", 1, "libkernel", posix_shm_open);
    LIB_FUNCTION("tPWsbOUGO8k", "libkernel", 1, "libkernel", posix_shm_unlink);
    LIB_FUNCTION("tPWsbOUGO8k", "libScePosix", 1, "libkernel", posix_shm_unlink);
    LIB_FUNCTION("n371J5cP+uo", "libkernel", 1, "libkernel", posix_physhm_open);
    LIB_FUNCTION("AUqJNkobQ1c", "libkernel", 1, "libkernel", posix_physhm_unlink);
}

} // namespace Libraries::Kernel
