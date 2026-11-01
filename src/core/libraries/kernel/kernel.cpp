// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <random>
#include <span>
#include <thread>
#include <boost/asio/io_context.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "common/va_ctx.h"
#include "core/emulator_settings.h"
#include "core/file_sys/fs.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/coredump/coredump.h"
#include "core/libraries/kernel/debug.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/file_system.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/kernel/threads.h"
#include "core/libraries/kernel/threads/exception.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/libraries/net/net.h"
#include "core/libraries/net/net_kernel.h"

#ifdef _WIN64
#include <Rpc.h>
#else
#include <uuid/uuid.h>
#endif
#include <common/singleton.h>
#include <core/linker.h>
#include "aio.h"

namespace Libraries::Kernel {

static u64 g_stack_chk_guard = 0xDEADBEEF54321ABC; // dummy return
char const* g_environment[64];
static const char* g_progname = "eboot.bin";

boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

Core::EntryParams entry_params{};

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:KernelServiceThread");

    while (!stoken.stop_requested()) {
        HLE_TRACE;
        {
            std::unique_lock lock{m_asio_req};
            Common::CondvarWait(cv_asio_req, lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) {
            break;
        }

        io_context.run();
        io_context.restart();

        asio_requests = 0;
    }
}

static PS4_SYSV_ABI void stack_chk_fail() {
    void* caller = __builtin_return_address(0);
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    auto* mod = linker ? linker->FindByAddress(reinterpret_cast<VAddr>(caller)) : nullptr;
    if (mod) {
        LOG_CRITICAL(Lib_Kernel, "called from {:p} (module: {}, offset: {:#x})", caller, mod->name,
                     reinterpret_cast<VAddr>(caller) - mod->GetBaseAddress());
    } else {
        LOG_CRITICAL(Lib_Kernel, "called from {:p} (unknown module)", caller);
    }
    UNREACHABLE();
}

static thread_local s32 g_posix_errno = 0;

s32* PS4_SYSV_ABI __Error() {
    return &g_posix_errno;
}

void ErrSceToPosix(s32 error) {
    g_posix_errno = error - ORBIS_KERNEL_ERROR_UNKNOWN;
}

s32 ErrnoToSceKernelError(s32 error) {
    return error + ORBIS_KERNEL_ERROR_UNKNOWN;
}

s32 PS4_SYSV_ABI sceKernelError(s32 posix_error) {
    if (posix_error == 0) {
        return 0;
    }
    return posix_error + ORBIS_KERNEL_ERROR_UNKNOWN;
}

s32 NativeToPosixErrno(s32 const e) {
    // Some error numbers are different between supported OSes
    switch (e) {
    case 0:
        return 0;
    case EPERM:
        return POSIX_EPERM;
        break;
    case ENOENT:
        return POSIX_ENOENT;
        break;
    case EINTR:
        return POSIX_EINTR;
        break;
    case EDEADLK:
        return POSIX_EDEADLK;
        break;
    case ENOMEM:
        return POSIX_ENOMEM;
        break;
    case EACCES:
        return POSIX_EACCES;
        break;
    case EFAULT:
        return POSIX_EFAULT;
        break;
    case EINVAL:
        return POSIX_EINVAL;
        break;
    case ENOSPC:
        return POSIX_ENOSPC;
        break;
    case ERANGE:
        return POSIX_ERANGE;
        break;
    case EAGAIN:
        return POSIX_EAGAIN;
        break;
    case ETIMEDOUT:
        return POSIX_ETIMEDOUT;
        break;
    default:
        LOG_WARNING(Kernel, "Unhandled errno {}", e);
        return e;
    }
}

void SetPosixErrno(s32 e) {
    g_posix_errno = NativeToPosixErrno(e);
}

struct OrbisKernelUuid {
    u32 timeLow;
    u16 timeMid;
    u16 timeHiAndVersion;
    u8 clockSeqHiAndReserved;
    u8 clockSeqLow;
    u8 node[6];
};
static_assert(sizeof(OrbisKernelUuid) == 0x10);

s32 PS4_SYSV_ABI sceKernelUuidCreate(OrbisKernelUuid* orbisUuid) {
    if (!orbisUuid) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
#ifdef _WIN64
    UUID uuid;
    if (UuidCreate(&uuid) != RPC_S_OK) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
#else
    uuid_t uuid;
    uuid_generate(uuid);
#endif
    std::memcpy(orbisUuid, &uuid, sizeof(OrbisKernelUuid));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI kernel_ioctl(s32 fd, u64 cmd, VA_ARGS) {
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);
    if (file == nullptr) {
        LOG_INFO(Lib_Kernel, "ioctl: fd = {:X} cmd = {:X} file == nullptr", fd, cmd);
        g_posix_errno = POSIX_EBADF;
        return -1;
    }
    if (file->type != Core::FileSys::FileType::Device) {
        LOG_WARNING(Lib_Kernel, "ioctl: fd = {:X} cmd = {:X} file->type != Device", fd, cmd);
        g_posix_errno = ENOTTY;
        return -1;
    }
    VA_CTX(ctx);
    s32 result = file->device->ioctl(cmd, &ctx);
    LOG_TRACE(Lib_Kernel, "ioctl: fd = {:X} cmd = {:X} result = {}", fd, cmd, result);
    if (result < 0) {
        ErrSceToPosix(result);
        return -1;
    }
    return result;
}

const char* PS4_SYSV_ABI sceKernelGetFsSandboxRandomWord() {
    const char* path = "sys";
    return path;
}

s32 PS4_SYSV_ABI _sigprocmask() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI posix_getpagesize() {
    return 16_KB;
}

// stubbed on non-devkit consoles
s32 PS4_SYSV_ABI sceKernelGetGPI() {
    LOG_DEBUG(Kernel, "called");
    return ORBIS_OK;
}

// stubbed on non-devkit consoles
s32 PS4_SYSV_ABI sceKernelSetGPO() {
    LOG_DEBUG(Kernel, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetAllowedSdkVersionOnSystem(s32* ver) {
    if (ver == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    // Returns the highest game SDK version this PS4 allows.
    *ver = CURRENT_FIRMWARE_VERSION | 0xfff;
    LOG_INFO(Lib_Kernel, "called, returned sw version: {}", *ver);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetSystemSwVersion(SwVersionStruct* ret) {
    if (ret == nullptr) {
        return ORBIS_OK;
    }
    u32 fake_fw = CURRENT_FIRMWARE_VERSION;
    ret->hex_representation = fake_fw;
    std::snprintf(ret->text_representation, 28, "%2x.%03x.%03x", fake_fw >> 0x18,
                  fake_fw >> 0xc & 0xfff, fake_fw & 0xfff);
    LOG_INFO(Lib_Kernel, "called, returned sw version: {}", ret->text_representation);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI getargc() {
    return entry_params.argc;
}

const char** PS4_SYSV_ABI getargv() {
    return entry_params.argv;
}

s32 PS4_SYSV_ABI get_authinfo(s32 pid, AuthInfoData* p2) {
    LOG_WARNING(Lib_Kernel, "(STUBBED) called, pid: {}", pid);
    if (p2 == nullptr) {
        *Kernel::__Error() = POSIX_EPERM;
        return -1;
    }
    if (pid != 0 && pid != GLOBAL_PID) {
        *Kernel::__Error() = POSIX_ESRCH;
        return -1;
    }

    *p2 = {};
    p2->caps[0] = 0x2000000000000000;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetAppInfo(s32 pid, OrbisKernelAppInfo* app_info) {
    LOG_WARNING(Lib_Kernel, "(STUBBED) called, pid: {}", pid);
    if (pid != GLOBAL_PID) {
        return ORBIS_KERNEL_ERROR_EPERM;
    }
    if (app_info == nullptr) {
        return ORBIS_OK;
    }

    auto& game_info = Common::ElfInfo::Instance();
    *app_info = {};
    app_info->has_param_sfo = 1;
    strncpy(app_info->cusa_name, game_info.GameSerial().data(), 10);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelTitleWorkaroundIsEnabled(OrbisKernelTitleWorkaround* tw, s32 bit,
                                                   s32* result) {
    LOG_ERROR(Lib_Kernel, "(STUBBED) called, bit {:#x}", bit);
    if (!tw || !result) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }

    // Maximum bit value is known to change with new firmwares.
    if (bit >= 0x3a) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    // Straight from decompilation, most uses rely on workaround data from sceKernelGetAppInfo.
    *result = ((tw->ids[bit >> 6] >> (bit & 0x3f)) & 1);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetProcessType(s32 pid) {
    LOG_DEBUG(Lib_Kernel, "called, pid: {}", pid);
    if (pid != GLOBAL_PID && pid != -1 && pid != 0) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    return ORBIS_KERNEL_PROCESS_TYPE_BIG_APP;
}

s32 PS4_SYSV_ABI __sys_regmgr_call(u32 op, u32 key, void* result, void* value, u64 len) {
    LOG_ERROR(Lib_Kernel, "(STUBBED) called, op: {:#x}, key: {}, len: {}", op, key, len);
    return ORBIS_OK;
}

namespace {

u8 GetConsoleTargetId() {
    if (EmulatorSettings.IsDevKit()) {
        return 0x80; // Tool / DevKit
    }

    const auto* elf_info = Common::Singleton<Common::ElfInfo>::Instance();
    if (elf_info && elf_info->IsInitialized()) {
        const auto serial = elf_info->GameSerial();
        if (serial.starts_with("PCJS") || serial.starts_with("PLJS") ||
            serial.starts_with("PCJM") || serial.starts_with("PLAS") ||
            serial.starts_with("PCAS")) {
            return 0x81; // Japan
        }
    }

    switch (EmulatorSettings.GetConsoleLanguage()) {
    case 0: // Japanese
        return 0x81;
    case 1:          // English (United States)
    case 17:         // Portuguese (Brazil)
    case 20:         // Spanish (Latin America)
    case 22:         // French (Canada)
        return 0x82; // North America (CEX)
    case 9:          // Korean
        return 0x84; // Korea
    case 10:         // Traditional Chinese
        return 0x88; // Taiwan / Hong Kong
    case 11:         // Simplified Chinese
        return 0x8A; // China
    case 8:          // Russian
        return 0x89; // Russia
    case 27:         // Thai
    case 28:         // Vietnamese
    case 29:         // Indonesian
        return 0x87; // Southeast Asia
    default:
        // Polish (16), UK English (18), German (4), French (2), Spanish (3),
        // Italian (5), Dutch (6), Portuguese (7), Danish (14), Norwegian (15),
        // Swedish (13), Finnish (12), Czech (23), Hungarian (24), Greek (25),
        // Romanian (26), Turkish (19), Arabic (21), Ukrainian (30)
        return 0x85; // Europe / UK / Poland (CEX)
    }
}

OrbisKernelIdPs GetSystemIdPs() {
    OrbisKernelIdPs idps{};
    // Platform ID: 00 00 00 01 (PlayStation 4)
    idps.data[0] = 0x00;
    idps.data[1] = 0x00;
    idps.data[2] = 0x00;
    idps.data[3] = 0x01;

    // Target ID (Region)
    idps.data[4] = 0x00;
    idps.data[5] = GetConsoleTargetId();

    // Model / Chassis: 0x0007 (PS4 Pro / Neo) vs 0x0001 (PS4 Fat / Liverpool)
    const bool is_pro = sceKernelIsNeoMode() != 0;
    idps.data[6] = 0x00;
    idps.data[7] = is_pro ? 0x07 : 0x01;

    // Serial / Board Unique ID (Deterministic hash based on Target ID + Chassis)
    u64 serial_hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < 8; ++i) {
        serial_hash = (serial_hash ^ idps.data[i]) * 0x100000001b3ULL;
    }
    for (size_t i = 8; i < 16; ++i) {
        idps.data[i] = static_cast<u8>(serial_hash >> ((i - 8) * 8));
    }
    return idps;
}

OrbisKernelOpenPsId GetSystemOpenPsId() {
    OrbisKernelOpenPsId id{};
    // Little-endian seed representing 's','h','a','d','P','S','4'
    constexpr u64 kBaseSeed = 0x0034535064616873ULL;
    for (size_t i = 0; i < 7; ++i) {
        id.data[i] = static_cast<u8>(kBaseSeed >> (i * 8));
    }
    id.data[7] = GetConsoleTargetId();

    const u8 chassis_id = (sceKernelIsNeoMode() != 0) ? 0x07 : 0x01;

    u64 hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < 8; ++i) {
        hash = (hash ^ id.data[i]) * 0x100000001b3ULL;
    }
    hash = (hash ^ chassis_id) * 0x100000001b3ULL;

    for (size_t i = 8; i < 16; ++i) {
        id.data[i] = static_cast<u8>(hash >> ((i - 8) * 8));
    }
    return id;
}

constexpr s32 CTL_SYSCTL = 0;
constexpr s32 CTL_KERN = 1;
constexpr s32 CTL_HW = 6;
constexpr s32 CTL_MACHDEP = 7;

constexpr s32 CTL_SYSCTL_NAME2OID = 3;

struct SysctlMibEntry {
    std::string_view name;
    std::vector<s32> mib;
};

static const std::vector<SysctlMibEntry> s_sysctl_mibs = {
    {"machdep.openpsid", {CTL_MACHDEP, 1}},
    {"machdep.openpsid_for_sys", {CTL_MACHDEP, 2}},
    {"machdep.idps", {CTL_MACHDEP, 3}},
    {"machdep.tsc_freq", {CTL_MACHDEP, 4}},
    {"machdep.bootparams.base_ps4_mode", {CTL_MACHDEP, 5}},
    {"machdep.rcmgr_intdev", {CTL_MACHDEP, 6}},
    {"kern.neomode", {CTL_KERN, 100}},
    {"kern.neomode_switch", {CTL_KERN, 101}},
    {"kern.smp.cpus", {CTL_KERN, 102}},
    {"kern.sdk_version", {CTL_KERN, 103}},
    {"kern.rng_pseudo", {CTL_KERN, 104}},
    {"kern.ostype", {CTL_KERN, 1}},
    {"kern.osrelease", {CTL_KERN, 2}},
    {"hw.model", {CTL_HW, 2}},
    {"hw.ncpu", {CTL_HW, 3}},
};

s32 CopySysctl(void* oldp, size_t* oldlenp, const void* src, size_t len) {
    if (oldlenp == nullptr && oldp != nullptr) {
        *__Error() = POSIX_EFAULT;
        return -1;
    }
    if (oldp == nullptr) {
        if (oldlenp != nullptr) {
            *oldlenp = len;
        }
        return 0;
    }
    if (*oldlenp < len) {
        std::memcpy(oldp, src, *oldlenp);
        *oldlenp = len;
        *__Error() = POSIX_ENOMEM;
        return -1;
    }
    std::memcpy(oldp, src, len);
    *oldlenp = len;
    return 0;
}

} // anonymous namespace

s32 PS4_SYSV_ABI sceKernelGetOpenPsId(OrbisKernelOpenPsId* psid) {
    LOG_TRACE(Lib_Kernel, "called, psid = {}", fmt::ptr(psid));
    if (!psid) {
        return POSIX_EINVAL;
    }
    *psid = GetSystemOpenPsId();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetOpenPsIdForSystem(OrbisKernelOpenPsId* psid) {
    LOG_TRACE(Lib_Kernel, "called, psid = {}", fmt::ptr(psid));
    return sceKernelGetOpenPsId(psid);
}

s32 PS4_SYSV_ABI sceKernelGetIdPs(OrbisKernelIdPs* idps) {
    LOG_TRACE(Lib_Kernel, "called, idps = {}", fmt::ptr(idps));
    if (!idps) {
        return POSIX_EINVAL;
    }
    *idps = GetSystemIdPs();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI posix_sysctlbyname(const char* name, void* oldp, size_t* oldlenp, const void* newp,
                                    size_t newlen) {
    if (name == nullptr) {
        *__Error() = POSIX_EFAULT;
        return -1;
    }

    LOG_DEBUG(Lib_Kernel, "sysctlbyname: name = '{}'", name);

    const std::string_view name_sv{name};

    if (name_sv == "machdep.openpsid" || name_sv == "machdep.openpsid_for_sys") {
        const auto psid = GetSystemOpenPsId();
        return CopySysctl(oldp, oldlenp, &psid, sizeof(psid));
    }
    if (name_sv == "machdep.idps") {
        const auto idps = GetSystemIdPs();
        return CopySysctl(oldp, oldlenp, &idps, sizeof(idps));
    }
    if (name_sv == "kern.neomode") {
        const s32 is_neo = sceKernelIsNeoMode();
        return CopySysctl(oldp, oldlenp, &is_neo, sizeof(is_neo));
    }
    if (name_sv == "kern.neomode_switch") {
        const s32 neo_switch = EmulatorSettings.IsNeo() ? 1 : 0;
        return CopySysctl(oldp, oldlenp, &neo_switch, sizeof(neo_switch));
    }
    if (name_sv == "machdep.bootparams.base_ps4_mode") {
        const s32 base_mode = sceKernelIsNeoMode() ? 0 : 1;
        return CopySysctl(oldp, oldlenp, &base_mode, sizeof(base_mode));
    }
    if (name_sv == "machdep.tsc_freq") {
        const u64 freq = sceKernelGetTscFrequency();
        return CopySysctl(oldp, oldlenp, &freq, sizeof(freq));
    }
    if (name_sv == "hw.model") {
        const char* model = sceKernelIsNeoMode() ? "PlayStation 4 Pro" : "PlayStation 4";
        return CopySysctl(oldp, oldlenp, model, std::strlen(model) + 1);
    }
    if (name_sv == "hw.ncpu") {
        const s32 ncpu = 8;
        return CopySysctl(oldp, oldlenp, &ncpu, sizeof(ncpu));
    }
    if (name_sv == "kern.smp.cpus") {
        s32 cpus = 6;
        const auto* elf_info = Common::Singleton<Common::ElfInfo>::Instance();
        if (elf_info && elf_info->IsInitialized()) {
            if (elf_info->GetPSFAttributes().seven_cpu_mode) {
                cpus = 7;
            }
        }
        return CopySysctl(oldp, oldlenp, &cpus, sizeof(cpus));
    }
    if (name_sv == "kern.sdk_version") {
        u32 sdk_ver = 0;
        const auto* elf_info = Common::Singleton<Common::ElfInfo>::Instance();
        if (elf_info && elf_info->IsInitialized()) {
            sdk_ver = elf_info->CompiledSdkVer();
        }
        return CopySysctl(oldp, oldlenp, &sdk_ver, sizeof(sdk_ver));
    }
    if (name_sv == "kern.rng_pseudo") {
        if (oldlenp == nullptr && oldp != nullptr) {
            *__Error() = POSIX_EFAULT;
            return -1;
        }
        if (oldp != nullptr && oldlenp != nullptr) {
            static std::mt19937_64 rng(0x505334);
            u8* out = static_cast<u8*>(oldp);
            for (size_t i = 0; i < *oldlenp; ++i) {
                out[i] = static_cast<u8>(rng());
            }
        }
        return 0;
    }
    if (name_sv == "kern.ostype") {
        constexpr const char* ostype = "FreeBSD";
        return CopySysctl(oldp, oldlenp, ostype, std::strlen(ostype) + 1);
    }
    if (name_sv == "kern.osrelease") {
        constexpr const char* osrel = "9.0-RELEASE";
        return CopySysctl(oldp, oldlenp, osrel, std::strlen(osrel) + 1);
    }
    if (name_sv == "machdep.rcmgr_intdev") {
        const s32 intdev = EmulatorSettings.IsDevKit() ? 1 : 0;
        return CopySysctl(oldp, oldlenp, &intdev, sizeof(intdev));
    }

    LOG_WARNING(Lib_Kernel, "sysctlbyname: unknown mib '{}'", name);
    *__Error() = POSIX_ENOENT;
    return -1;
}

s32 PS4_SYSV_ABI posix_sysctlnametomib(const char* name, s32* mibp, size_t* sizep) {
    if (name == nullptr || mibp == nullptr || sizep == nullptr) {
        *__Error() = POSIX_EFAULT;
        return -1;
    }

    const std::string_view name_sv{name};
    for (const auto& entry : s_sysctl_mibs) {
        if (entry.name == name_sv) {
            if (*sizep < entry.mib.size()) {
                *sizep = entry.mib.size();
                *__Error() = POSIX_ENOMEM;
                return -1;
            }
            std::memcpy(mibp, entry.mib.data(), entry.mib.size() * sizeof(s32));
            *sizep = entry.mib.size();
            return 0;
        }
    }

    LOG_WARNING(Lib_Kernel, "sysctlnametomib: unknown name '{}'", name);
    *__Error() = POSIX_ENOENT;
    return -1;
}

s32 PS4_SYSV_ABI posix_sysctl(const s32* name, u32 namelen, void* oldp, size_t* oldlenp,
                              const void* newp, size_t newlen) {
    if (name == nullptr || namelen == 0) {
        *__Error() = POSIX_EFAULT;
        return -1;
    }

    if (namelen >= 2 && name[0] == CTL_SYSCTL && name[1] == CTL_SYSCTL_NAME2OID) {
        if (newp == nullptr || newlen == 0 || oldp == nullptr || oldlenp == nullptr) {
            *__Error() = POSIX_EFAULT;
            return -1;
        }
        const std::string name_str(static_cast<const char*>(newp), newlen);
        size_t int_count = *oldlenp / sizeof(s32);
        const s32 res =
            posix_sysctlnametomib(name_str.c_str(), static_cast<s32*>(oldp), &int_count);
        *oldlenp = int_count * sizeof(s32);
        return res;
    }

    for (const auto& entry : s_sysctl_mibs) {
        if (entry.mib.size() == namelen &&
            std::memcmp(entry.mib.data(), name, namelen * sizeof(s32)) == 0) {
            return posix_sysctlbyname(entry.name.data(), oldp, oldlenp, newp, newlen);
        }
    }

    std::string mib_str;
    for (u32 i = 0; i < namelen; ++i) {
        if (i > 0) {
            mib_str += ", ";
        }
        mib_str += std::to_string(name[i]);
    }
    LOG_WARNING(Lib_Kernel, "sysctl: unknown mib [{}]", mib_str);
    *__Error() = POSIX_ENOENT;
    return -1;
}

// Nominally: long sysconf(int name);
u64 PS4_SYSV_ABI posix_sysconf(s32 name) {
    switch (name) {
    case 0:
        return 0x20000;
    case POSIX_SC_ARG_MAX:
        return 0x588bc000;
    case POSIX_SC_CHILD_MAX:
        return 0x64;
    case POSIX_SC_CLK_TCK:
        return 0x20;
    case POSIX_SC_NGROUPS_MAX:
        return 0x644;
    case POSIX_SC_OPEN_MAX:
        return -0x1;
    case POSIX_SC_JOB_CONTROL:
        return 0x6;
    case POSIX_SC_SAVED_IDS:
        return 0x1;
    case POSIX_SC_VERSION:
        return 0x1;
    case POSIX_SC_BC_BASE_MAX:
        return 0x31069;
    case POSIX_SC_BC_DIM_MAX:
        return -0x1;
    case POSIX_SC_BC_SCALE_MAX:
        return 0x31069;
    case POSIX_SC_BC_STRING_MAX:
        return 0x31069;
    case POSIX_SC_COLL_WEIGHTS_MAX:
        return -0x1;
    case POSIX_SC_EXPR_NEST_MAX:
        return -0x1;
    case POSIX_SC_LINE_MAX:
        return 0x31069;
    case POSIX_SC_RE_DUP_MAX:
        return 0x31069;
    case POSIX_SC_2_VERSION:
        return 0x31069;
    case POSIX_SC_2_C_BIND:
        return 0x31069;
    case POSIX_SC_2_C_DEV:
        return 0x31069;
    case POSIX_SC_2_CHAR_TERM:
        return 0x31069;
    case POSIX_SC_2_FORT_DEV:
        return 0x31069;
    case POSIX_SC_2_FORT_RUN:
        return 0x31069;
    case POSIX_SC_2_LOCALEDEF:
        return -0x1;
    case POSIX_SC_2_SW_DEV:
        return -0x1;
    case POSIX_SC_2_UPE:
        return 0x0;
    case POSIX_SC_STREAM_MAX:
        return 0x7fffffff;
    case POSIX_SC_TZNAME_MAX:
        return -0x1;
    case POSIX_SC_ASYNCHRONOUS_IO:
        return 0x8000;
    case POSIX_SC_MAPPED_FILES:
        return 0x31069;
    case POSIX_SC_MEMLOCK:
        return 0x4000;
    case POSIX_SC_MEMLOCK_RANGE:
        return 0x1e;
    case POSIX_SC_MEMORY_PROTECTION:
        return 0x100;
    case POSIX_SC_MESSAGE_PASSING:
        return 0x7fffffff;
    case POSIX_SC_PRIORITIZED_IO:
        return -0x1;
    case POSIX_SC_PRIORITY_SCHEDULING:
        return -0x1;
    case POSIX_SC_REALTIME_SIGNALS:
        return 0x63;
    case POSIX_SC_SEMAPHORES:
        return 0x800;
    case POSIX_SC_FSYNC:
        return 0x63;
    case POSIX_SC_SHARED_MEMORY_OBJECTS:
        return 0x3e8;
    case POSIX_SC_SYNCHRONIZED_IO:
        return 0x2;
    case POSIX_SC_THREAD_ATTR_STACKSIZE:
        return 0x1;
    case POSIX_SC_THREAD_CPUTIME:
        return 0x1;
    case POSIX_SC_THREAD_DESTRUCTOR_ITERATIONS:
        return 0x48000;
    case POSIX_SC_THREAD_KEYS_MAX:
        return 0x1a078630b2dd7;
    case POSIX_SC_THREAD_PRIO_INHERIT:
        return -0x1;
    case POSIX_SC_THREAD_PRIO_PROTECT:
        return -0x1;
    case POSIX_SC_THREAD_PRIORITY_SCHEDULING:
        return 0x2bc;
    case POSIX_SC_THREAD_PROCESS_SHARED:
        return 0x2bc;
    case POSIX_SC_THREAD_SAFE_FUNCTIONS:
        return 0x1;
    case POSIX_SC_THREAD_SPORADIC_SERVER:
        return -0x1;
    case POSIX_SC_THREAD_STACK_MIN:
        return 0x1;
    case POSIX_SC_THREAD_THREADS_MAX:
        return 0x1;
    case POSIX_SC_TIMEOUTS:
        return -0x1;
    // Manually specified
    case POSIX_SC_PAGESIZE:
        return posix_getpagesize();
    default:
        LOG_ERROR(Lib_Kernel, "unhandled {}", name);
        return 0;
    }
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    service_thread = std::jthread{KernelServiceThread};

    static char const** kernel_environ = g_environment;

    // Before any socket exists: net ids are file descriptors from the kernel's table.
    Libraries::Net::InstallKernelIntegration();

    Libraries::Kernel::RegisterFileSystem(sym);
    Libraries::Kernel::RegisterTime(sym);
    Libraries::Kernel::RegisterThreads(sym);
    Libraries::Kernel::RegisterKernelEventFlag(sym);
    Libraries::Kernel::RegisterMemory(sym);
    Libraries::Kernel::RegisterEventQueue(sym);
    Libraries::Kernel::RegisterProcess(sym);
    Libraries::Kernel::RegisterException(sym);
    Libraries::Kernel::RegisterAio(sym);
    Libraries::Kernel::RegisterDebug(sym);
    Libraries::Kernel::RegisterCoredump(sym);

    LIB_OBJ("f7uOxY9mM1U", "libkernel", 1, "libkernel", &g_stack_chk_guard);
    LIB_OBJ("+2thxYZ4syk", "libkernel", 1, "libkernel", &kernel_environ);
    LIB_OBJ("djxxOmW6-aw", "libkernel", 1, "libkernel", &g_progname);
    LIB_FUNCTION("D4yla3vx4tY", "libkernel", 1, "libkernel", sceKernelError);
    LIB_FUNCTION("YeU23Szo3BM", "libkernel", 1, "libkernel", sceKernelGetAllowedSdkVersionOnSystem);
    LIB_FUNCTION("Mv1zUObHvXI", "libkernel", 1, "libkernel", sceKernelGetSystemSwVersion);
    LIB_FUNCTION("igMefp4SAv0", "libkernel", 1, "libkernel", get_authinfo);
    LIB_FUNCTION("G-MYv5erXaU", "libkernel", 1, "libkernel", sceKernelGetAppInfo);
    LIB_FUNCTION("1yca4VvfcNA", "libkernel", 1, "libkernel", sceKernelTitleWorkaroundIsEnabled);
    LIB_FUNCTION("+g+UP8Pyfmo", "libkernel", 1, "libkernel", sceKernelGetProcessType);
    LIB_FUNCTION("PfccT7qURYE", "libkernel", 1, "libkernel", kernel_ioctl);
    LIB_FUNCTION("wW+k21cmbwQ", "libkernel", 1, "libkernel", kernel_ioctl);
    LIB_FUNCTION("JGfTMBOdUJo", "libkernel", 1, "libkernel", sceKernelGetFsSandboxRandomWord);
    LIB_FUNCTION("JGfTMBOdUJo", "libkernel_psmkit", 1, "libkernel",
                 sceKernelGetFsSandboxRandomWord);
    LIB_FUNCTION("6xVpy0Fdq+I", "libkernel", 1, "libkernel", _sigprocmask);
    LIB_FUNCTION("Xjoosiw+XPI", "libkernel", 1, "libkernel", sceKernelUuidCreate);
    LIB_FUNCTION("Ou3iL1abvng", "libkernel", 1, "libkernel", stack_chk_fail);
    LIB_FUNCTION("9BcDykPmo1I", "libkernel", 1, "libkernel", __Error);
    LIB_FUNCTION("k+AXqu2-eBc", "libkernel", 1, "libkernel", posix_getpagesize);
    LIB_FUNCTION("k+AXqu2-eBc", "libScePosix", 1, "libkernel", posix_getpagesize);
    LIB_FUNCTION("7NwggrWJ5cA", "libkernel", 1, "libkernel", __sys_regmgr_call);

    LIB_FUNCTION("mkawd0NA9ts", "libkernel", 1, "libkernel", posix_sysconf);
    LIB_FUNCTION("mkawd0NA9ts", "libScePosix", 1, "libkernel", posix_sysconf);

    // network
    LIB_FUNCTION("XVL8So3QJUk", "libkernel", 1, "libkernel", Libraries::Net::sys_connect);
    LIB_FUNCTION("pG70GT5yRo4", "libkernel", 1, "libkernel", Libraries::Net::sys_socketex);
    LIB_FUNCTION("KuOmgKoqCdY", "libkernel", 1, "libkernel", Libraries::Net::sys_bind);
    LIB_FUNCTION("6O8EwYOgH9Y", "libkernel", 1, "libkernel", Libraries::Net::sys_getsockopt);
    LIB_FUNCTION("fFxGkxF2bVo", "libkernel", 1, "libkernel", Libraries::Net::sys_setsockopt);
    LIB_FUNCTION("pxnCmagrtao", "libkernel", 1, "libkernel", Libraries::Net::sys_listen);
    LIB_FUNCTION("3e+4Iv7IJ8U", "libkernel", 1, "libkernel", Libraries::Net::sys_accept);
    LIB_FUNCTION("TUuiYS2kE8s", "libkernel", 1, "libkernel", Libraries::Net::sys_shutdown);
    LIB_FUNCTION("TU-d9PfIHPM", "libkernel", 1, "libkernel", Libraries::Net::sys_socket);
    LIB_FUNCTION("MZb0GKT3mo8", "libkernel", 1, "libkernel", Libraries::Net::sys_socketpair);
    LIB_FUNCTION("MZb0GKT3mo8", "libkernel_ps2emu", 1, "libkernel", Libraries::Net::sys_socketpair);
    LIB_FUNCTION("K1S8oc61xiM", "libkernel", 1, "libkernel", Libraries::Net::sys_htonl);
    LIB_FUNCTION("jogUIsOV3-U", "libkernel", 1, "libkernel", Libraries::Net::sys_htons);
    LIB_FUNCTION("fZOeZIOEmLw", "libkernel", 1, "libkernel", Libraries::Net::sys_send);
    LIB_FUNCTION("oBr313PppNE", "libkernel", 1, "libkernel", Libraries::Net::sys_sendto);
    LIB_FUNCTION("Ez8xjo9UF4E", "libkernel", 1, "libkernel", Libraries::Net::sys_recv);
    LIB_FUNCTION("lUk6wrGXyMw", "libkernel", 1, "libkernel", Libraries::Net::sys_recvfrom);

    LIB_FUNCTION("TUuiYS2kE8s", "libScePosix", 1, "libkernel", Libraries::Net::sys_shutdown);
    LIB_FUNCTION("TU-d9PfIHPM", "libScePosix", 1, "libkernel", Libraries::Net::sys_socket);
    LIB_FUNCTION("fZOeZIOEmLw", "libScePosix", 1, "libkernel", Libraries::Net::sys_send);
    LIB_FUNCTION("oBr313PppNE", "libScePosix", 1, "libkernel", Libraries::Net::sys_sendto);
    LIB_FUNCTION("Ez8xjo9UF4E", "libScePosix", 1, "libkernel", Libraries::Net::sys_recv);
    LIB_FUNCTION("lUk6wrGXyMw", "libScePosix", 1, "libkernel", Libraries::Net::sys_recvfrom);
    LIB_FUNCTION("hI7oVeOluPM", "libScePosix", 1, "libkernel", Libraries::Net::sys_recvmsg);
    LIB_FUNCTION("TXFFFiNldU8", "libScePosix", 1, "libkernel", Libraries::Net::sys_getpeername);
    LIB_FUNCTION("6O8EwYOgH9Y", "libScePosix", 1, "libkernel", Libraries::Net::sys_getsockopt);
    LIB_FUNCTION("fFxGkxF2bVo", "libScePosix", 1, "libkernel", Libraries::Net::sys_setsockopt);
    LIB_FUNCTION("RenI1lL1WFk", "libScePosix", 1, "libkernel", Libraries::Net::sys_getsockname);
    LIB_FUNCTION("KuOmgKoqCdY", "libScePosix", 1, "libkernel", Libraries::Net::sys_bind);
    LIB_FUNCTION("5jRCs2axtr4", "libScePosix", 1, "libkernel",
                 Libraries::Net::sceNetInetNtop); // TODO fix it to sys_ ...
    LIB_FUNCTION("4n51s0zEf0c", "libScePosix", 1, "libkernel",
                 Libraries::Net::sceNetInetPton); // TODO fix it to sys_ ...
    LIB_FUNCTION("XVL8So3QJUk", "libScePosix", 1, "libkernel", Libraries::Net::sys_connect);
    LIB_FUNCTION("3e+4Iv7IJ8U", "libScePosix", 1, "libkernel", Libraries::Net::sys_accept);
    LIB_FUNCTION("aNeavPDNKzA", "libScePosix", 1, "libkernel", Libraries::Net::sys_sendmsg);
    LIB_FUNCTION("pxnCmagrtao", "libScePosix", 1, "libkernel", Libraries::Net::sys_listen);

    LIB_FUNCTION("4oXYe9Xmk0Q", "libkernel", 1, "libkernel", sceKernelGetGPI);
    LIB_FUNCTION("ca7v6Cxulzs", "libkernel", 1, "libkernel", sceKernelSetGPO);
    LIB_FUNCTION("iKJMWrAumPE", "libkernel", 1, "libkernel", getargc);
    LIB_FUNCTION("FJmglmTMdr4", "libkernel", 1, "libkernel", getargv);
    LIB_FUNCTION("DLORcroUqbc", "libkernel", 1, "libkernel", sceKernelGetOpenPsId);
    LIB_FUNCTION("DLORcroUqbc", "libSceOpenPsId", 1, "libkernel", sceKernelGetOpenPsId);
    LIB_FUNCTION("ul57hvm6mBc", "libkernel", 1, "libkernel", sceKernelGetOpenPsIdForSystem);
    LIB_FUNCTION("lZG-WlKxRqQ", "libkernel", 1, "libkernel", sceKernelGetIdPs);
    LIB_FUNCTION("MhC53TKmjVA", "libkernel", 1, "libkernel", posix_sysctlbyname);
    LIB_FUNCTION("MhC53TKmjVA", "libkernel_sys", 1, "libkernel", posix_sysctlbyname);
    LIB_FUNCTION("MhC53TKmjVA", "libkernel_web", 1, "libkernel", posix_sysctlbyname);
    LIB_FUNCTION("DFmMT80xcNI", "libkernel", 1, "libkernel", posix_sysctl);
    LIB_FUNCTION("DFmMT80xcNI", "libkernel_sys", 1, "libkernel", posix_sysctl);
    LIB_FUNCTION("DFmMT80xcNI", "libkernel_web", 1, "libkernel", posix_sysctl);
    LIB_FUNCTION("UkEV4hvMo1E", "libkernel", 1, "libkernel", posix_sysctlnametomib);
    LIB_FUNCTION("UkEV4hvMo1E", "libkernel_sys", 1, "libkernel", posix_sysctlnametomib);
    LIB_FUNCTION("UkEV4hvMo1E", "libkernel_web", 1, "libkernel", posix_sysctlnametomib);
}

} // namespace Libraries::Kernel
