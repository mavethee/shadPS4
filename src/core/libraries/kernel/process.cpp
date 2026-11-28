// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "core/cpu_patches.h"
#include "core/emulator_settings.h"
#include "core/file_sys/fs.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/libs.h"
#include "core/linker.h"

namespace Libraries::Kernel {

static s32 g_neo_mode = -1;

s32 PS4_SYSV_ABI sceKernelIsInSandbox() {
    return 1;
}

s32 PS4_SYSV_ABI sceKernelHasNeoMode() {
    return EmulatorSettings.IsNeo();
}

s32 PS4_SYSV_ABI sceKernelIsAuthenticNeo() {
    return EmulatorSettings.IsNeo();
}

s32 PS4_SYSV_ABI sceKernelIsNeoMode() {
    if (g_neo_mode == -1) {
        g_neo_mode = EmulatorSettings.IsNeo() &&
                     Common::ElfInfo::Instance().GetPSFAttributes().support_neo_mode;
    }
    return g_neo_mode;
}

s32 PS4_SYSV_ABI sceKernelSwitchToNeoMode(u32 flags) {
    LOG_INFO(Lib_Kernel, "called with flags: {:#x}", flags);
    if (!EmulatorSettings.IsNeo()) {
        return ORBIS_KERNEL_ERROR_ENODEV;
    }
    g_neo_mode = 1;
    Core::UpdateGuestTscMultiplier();
    return 0;
}

s32 PS4_SYSV_ABI sceKernelIsDevkit() {
    LOG_INFO(Lib_Kernel, "called, isDevkit: {}", EmulatorSettings.IsDevKit());
    return EmulatorSettings.IsDevKit();
}

s32 PS4_SYSV_ABI sceKernelIsProspero() {
    LOG_INFO(Lib_Kernel, "called: returning false");
    return 0;
}

s32 PS4_SYSV_ABI sceKernelIsCEX() {
    return !sceKernelIsDevkit();
}

s32 PS4_SYSV_ABI sceKernelGetMainSocId() {
    // These hardcoded values are based on hardware observations.
    // Different models of PS4/PS4 Pro likely return slightly different values.
    LOG_DEBUG(Lib_Kernel, "called");
    if (EmulatorSettings.IsNeo()) {
        return 0x740f30;
    }
    return 0x710f10;
}

s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    *ver = Common::ElfInfo::Instance().CompiledSdkVer();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetCpumode() {
    LOG_DEBUG(Lib_Kernel, "called");
    auto& attrs = Common::ElfInfo::Instance().GetPSFAttributes();
    u32 is_cpu6 = attrs.six_cpu_mode.Value();
    u32 is_cpu7 = attrs.seven_cpu_mode.Value();
    if (is_cpu6 == 1 && is_cpu7 == 1) {
        return 2;
    }
    if (is_cpu7 == 1) {
        return 5;
    }
    return 0;
}

s32 PS4_SYSV_ABI sceKernelGetCurrentCpu() {
    LOG_DEBUG(Lib_Kernel, "called");
    return 0;
}

void* PS4_SYSV_ABI sceKernelGetProcParam() {
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    return linker->GetProcParam();
}

s32 PS4_SYSV_ABI sceKernelLoadStartModule(const char* moduleFileName, u64 args, const void* argp,
                                          u32 flags, const void* pOpt, s32* pRes) {
    if (moduleFileName == nullptr || flags != 0 || pOpt != nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    LOG_INFO(Lib_Kernel, "called filename = {}, args = {}", moduleFileName, args);

    auto* linker = Common::Singleton<Core::Linker>::Instance();
    s32 err = ORBIS_OK;
    const s32 handle = linker->LoadAndStartModule(moduleFileName, args, argp, pRes, &err);
    return handle != -1 ? handle : err;
}

s32 PS4_SYSV_ABI sceKernelStopUnloadModule(s32 handle, u64 args, const void* argp, u32 flags,
                                           const void* pOpt, s32* pRes) {
    if (handle == 0 || flags != 0 || pOpt != nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    if (linker == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    return linker->StopUnloadModule(handle, args, argp, pRes);
}

s32 PS4_SYSV_ABI sceKernelDlsym(s32 handle, const char* symbol, void** addrp) {
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    if (linker == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    return linker->Dlsym(handle, symbol, addrp);
}

s32 PS4_SYSV_ABI sceKernelGetModuleInfoForUnwind(VAddr addr, s32 flags,
                                                 OrbisModuleInfoForUnwind* info) {
    if (flags != 1 && flags != 2) {
        if (info != nullptr) {
            std::memset(info, 0, sizeof(OrbisModuleInfoForUnwind));
        }
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (!info) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (info->st_size < sizeof(OrbisModuleInfoForUnwind)) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    LOG_INFO(Lib_Kernel, "called addr = {:#x}, flags = {:#x}", addr, flags);
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    auto* module = linker->FindByAddress(addr, flags);
    if (!module) {
        std::memset(info, 0, sizeof(OrbisModuleInfoForUnwind));
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    const auto mod_info = module->GetModuleInfoEx();

    std::memset(info, 0, sizeof(OrbisModuleInfoForUnwind));
    info->name = mod_info.name;
    info->eh_frame_hdr_addr = mod_info.eh_frame_hdr_addr;
    info->eh_frame_addr = mod_info.eh_frame_addr;
    info->eh_frame_size = mod_info.eh_frame_size;
    info->seg0_addr = mod_info.segments[0].address;
    info->seg0_size = mod_info.segments[0].size;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetModuleInfoFromAddr(VAddr addr, s32 flags,
                                                Core::OrbisKernelModuleInfoEx* info) {
    if (info == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (flags != 1 && flags != 2) {
        std::memset(info, 0, sizeof(Core::OrbisKernelModuleInfoEx));
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    LOG_DEBUG(Lib_Kernel,
              "called addr = {:#x}, flags = {:#x}, caller = {:p}, info = {:p}, st_size = {:#x}",
              addr, flags, __builtin_return_address(0), static_cast<void*>(info), info->st_size);
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    auto* module = linker->FindByAddress(addr, flags);
    if (!module) {
        std::memset(info, 0, sizeof(Core::OrbisKernelModuleInfoEx));
        return ORBIS_KERNEL_ERROR_ESRCH;
    }

    *info = module->GetModuleInfoEx();
    return ORBIS_OK;
}

static Core::OrbisKernelModuleInfo MakeSyntheticLibKernelInfo() {
    Core::OrbisKernelModuleInfo info{};
    constexpr std::string_view kName = "libkernel.sprx";
    std::memcpy(info.name.data(), kName.data(), kName.size());
    info.num_segments = 2;
    return info;
}

static Core::OrbisKernelModuleInfoEx MakeSyntheticLibKernelInfoEx() {
    Core::OrbisKernelModuleInfoEx info{};
    constexpr std::string_view kName = "libkernel.sprx";
    std::memcpy(info.name.data(), kName.data(), kName.size());
    info.id = Core::Linker::LibKernelHandle;
    info.segment_count = 2;
    info.ref_count = 1;
    return info;
}

s32 PS4_SYSV_ABI sceKernelGetModuleInfo(s32 handle, Core::OrbisKernelModuleInfo* info) {
    if (info == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (info->st_size != sizeof(Core::OrbisKernelModuleInfo)) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (handle == Core::Linker::LibKernelHandle) {
        *info = MakeSyntheticLibKernelInfo();
        return ORBIS_OK;
    }

    auto* linker = Common::Singleton<Core::Linker>::Instance();
    auto* module = linker->GetModule(handle);
    if (module == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    *info = module->GetModuleInfo();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetModuleInfo2(s32 handle, Core::OrbisKernelModuleInfo* info) {
    if (info == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (info->st_size != sizeof(Core::OrbisKernelModuleInfo)) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (handle == Core::Linker::LibKernelHandle) {
        return ORBIS_KERNEL_ERROR_EPERM;
    }

    auto* linker = Common::Singleton<Core::Linker>::Instance();
    auto* module = linker->GetModule(handle);
    if (module == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    if (module->IsSystemLib()) {
        return ORBIS_KERNEL_ERROR_EPERM;
    }
    *info = module->GetModuleInfo();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetModuleInfoInternal(s32 handle, Core::OrbisKernelModuleInfoEx* info) {
    if (info == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (info->st_size != sizeof(Core::OrbisKernelModuleInfoEx)) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (handle == Core::Linker::LibKernelHandle) {
        *info = MakeSyntheticLibKernelInfoEx();
        return ORBIS_OK;
    }

    auto* linker = Common::Singleton<Core::Linker>::Instance();
    auto* module = linker->GetModule(handle);
    if (module == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    *info = module->GetModuleInfoEx();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetModuleList2(s32* handles, u64 num_array, u64* out_count) {
    if (out_count == nullptr || (handles == nullptr && num_array > 0)) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }

    auto* linker = Common::Singleton<Core::Linker>::Instance();
    std::vector<s32> matched;
    for (const s32 id : linker->GetModuleOrder()) {
        auto* module = linker->GetModule(id);
        if (module && !module->IsSystemLib()) {
            matched.push_back(id);
        }
    }

    if (matched.size() > num_array) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
    for (size_t i = 0; i < matched.size(); ++i) {
        handles[i] = matched[i];
    }
    *out_count = matched.size();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetModuleListInternal(s32* handles, u64 num_array, u64* out_count) {
    if (out_count == nullptr || (handles == nullptr && num_array > 0)) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }

    auto* linker = Common::Singleton<Core::Linker>::Instance();
    std::vector<s32> matched;
    const auto& order = linker->GetModuleOrder();
    matched.reserve(order.size() + 1);
    for (size_t i = 0; i < order.size(); ++i) {
        matched.push_back(order[i]);
        if (i == 0) {
            matched.push_back(Core::Linker::LibKernelHandle);
        }
    }

    if (matched.size() > num_array) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
    for (size_t i = 0; i < matched.size(); ++i) {
        handles[i] = matched[i];
    }
    *out_count = matched.size();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetModuleList(s32* handles, u64 num_array, u64* out_count) {
    s32 sdk_ver = 0;
    if (sceKernelGetCompiledSdkVersion(&sdk_ver) == ORBIS_OK &&
        sdk_ver >= static_cast<s32>(Common::ElfInfo::FW_500)) {
        return sceKernelGetModuleList2(handles, num_array, out_count);
    }
    return sceKernelGetModuleListInternal(handles, num_array, out_count);
}

s32 PS4_SYSV_ABI sceKernelIsModuleLoaded(const char* name) {
    if (name == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    return linker->FindByGuestOrBaseName(name) != -1 ? ORBIS_OK : ORBIS_KERNEL_ERROR_ESRCH;
}

s32 PS4_SYSV_ABI sceKernelGetExecutableModuleHandle() {
    return 0;
}

u32 PS4_SYSV_ABI posix_getuid() {
    return 1;
}

s32 PS4_SYSV_ABI exit(s32 status) {
    UNREACHABLE_MSG("Exiting with status code {}", status);
    return 0;
}

constexpr s32 ORBIS_KERNEL_ERROR_MDBG_UNAVAILABLE = static_cast<s32>(0x8001ffff);

s32 PS4_SYSV_ABI sceKernelSetProcessName(const char* name) {
    if (name == nullptr) {
        return ORBIS_OK;
    }
    if (std::strlen(name) >= 32) {
        return ORBIS_KERNEL_ERROR_ENAMETOOLONG;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetProcessName(s32 pid, char* name) {
    return ORBIS_KERNEL_ERROR_ESRCH;
}

s32 PS4_SYSV_ABI sceKernelGetThreadName(u32 tid, char* name) {
    return ORBIS_KERNEL_ERROR_ESRCH;
}

s32 PS4_SYSV_ABI sceKernelSetProcessProperty(const char* prop_name, u64 prop_data, size_t prop_size,
                                             u64 unk1, u64 unk2) {
    if (prop_name == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    // Retail mdbg_service rejects process property updates with 0x8001ffff.
    return ORBIS_KERNEL_ERROR_MDBG_UNAVAILABLE;
}

s32 PS4_SYSV_ABI sceKernelSetProcessPropertyString(const char* prop_name, const char* prop_value) {
    if (prop_name == nullptr || prop_value == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    return ORBIS_KERNEL_ERROR_MDBG_UNAVAILABLE;
}

void RegisterProcess(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("8UVYwy0F2Hk", "libkernel", 1, "libkernel", sceKernelSetProcessName);
    LIB_FUNCTION("fUJRLEbJOuQ", "libkernel", 1, "libkernel", sceKernelGetProcessName);
    LIB_FUNCTION("4vkZwDJbMx0", "libkernel", 1, "libkernel", sceKernelGetThreadName);
    LIB_FUNCTION("-W4xI5aVI8w", "libkernel", 1, "libkernel", sceKernelSetProcessProperty);
    LIB_FUNCTION("-W4xI5aVI8w", "libkernel_psmkit", 1, "libkernel", sceKernelSetProcessProperty);
    LIB_FUNCTION("U5HG6wD4smU", "libkernel", 1, "libkernel", sceKernelSetProcessPropertyString);
    LIB_FUNCTION("xeu-pV8wkKs", "libkernel", 1, "libkernel", sceKernelIsInSandbox);
    LIB_FUNCTION("WB66evu8bsU", "libkernel", 1, "libkernel", sceKernelGetCompiledSdkVersion);
    LIB_FUNCTION("WslcK1FQcGI", "libkernel", 1, "libkernel", sceKernelIsNeoMode);
    LIB_FUNCTION("rNRtm1uioyY", "libkernel", 1, "libkernel", sceKernelHasNeoMode);
    LIB_FUNCTION("cbn9Di01j0k", "libkernel", 1, "libkernel", sceKernelIsAuthenticNeo);
    LIB_FUNCTION("+DVcq4YF6Xg", "libkernel", 1, "libkernel", sceKernelSwitchToNeoMode);
    LIB_FUNCTION("QNjGUdj1HPM", "libkernel", 1, "libkernel", sceKernelIsDevkit);
    LIB_FUNCTION("mpxAdqW7dKY", "libkernel", 1, "libkernel", sceKernelIsProspero);
    LIB_FUNCTION("mpxAdqW7dKY", "libkernel_cpumode_platform", 1, "libkernel", sceKernelIsProspero);
    LIB_FUNCTION("8aCOCGoRkUI", "libkernel", 1, "libkernel", sceKernelIsCEX);
    LIB_FUNCTION("0vTn5IDMU9A", "libkernel", 1, "libkernel", sceKernelGetMainSocId);
    LIB_FUNCTION("VOx8NGmHXTs", "libkernel", 1, "libkernel", sceKernelGetCpumode);
    LIB_FUNCTION("g0VTBxfJyu0", "libkernel", 1, "libkernel", sceKernelGetCurrentCpu);
    LIB_FUNCTION("959qrazPIrg", "libkernel", 1, "libkernel", sceKernelGetProcParam);
    LIB_FUNCTION("wzvqT4UqKX8", "libkernel", 1, "libkernel", sceKernelLoadStartModule);
    LIB_FUNCTION("QKd0qM58Qes", "libkernel", 1, "libkernel", sceKernelStopUnloadModule);
    LIB_FUNCTION("LwG8g3niqwA", "libkernel", 1, "libkernel", sceKernelDlsym);
    LIB_FUNCTION("RpQJJVKTiFM", "libkernel", 1, "libkernel", sceKernelGetModuleInfoForUnwind);
    LIB_FUNCTION("f7KBOafysXo", "libkernel", 1, "libkernel", sceKernelGetModuleInfoFromAddr);
    LIB_FUNCTION("f7KBOafysXo", "libkernel_psmkit", 1, "libkernel", sceKernelGetModuleInfoFromAddr);
    LIB_FUNCTION("kUpgrXIrz7Q", "libkernel", 1, "libkernel", sceKernelGetModuleInfo);
    LIB_FUNCTION("QgsKEUfkqMA", "libkernel", 1, "libkernel", sceKernelGetModuleInfo2);
    LIB_FUNCTION("QgsKEUfkqMA", "libkernel_module_info", 1, "libkernel", sceKernelGetModuleInfo2);
    LIB_FUNCTION("HZO7xOos4xc", "libkernel", 1, "libkernel", sceKernelGetModuleInfoInternal);
    LIB_FUNCTION("IuxnUuXk6Bg", "libkernel", 1, "libkernel", sceKernelGetModuleList);
    LIB_FUNCTION("u+mlUkWpU3Y", "libkernel", 1, "libkernel", sceKernelGetModuleListInternal);
    LIB_FUNCTION("ZzzC3ZGVAkc", "libkernel", 1, "libkernel", sceKernelGetModuleList2);
    LIB_FUNCTION("ZzzC3ZGVAkc", "libkernel_module_info", 1, "libkernel", sceKernelGetModuleList2);
    LIB_FUNCTION("21+rb7xOlJk", "libkernel", 1, "libkernel", sceKernelIsModuleLoaded);
    LIB_FUNCTION("21+rb7xOlJk", "libkernel_module_load_check", 1, "libkernel",
                 sceKernelIsModuleLoaded);
    LIB_FUNCTION("wJABa1X4+ec", "libkernel", 1, "libkernel", sceKernelGetExecutableModuleHandle);
    LIB_FUNCTION("wJABa1X4+ec", "libkernel_jvm", 1, "libkernel",
                 sceKernelGetExecutableModuleHandle);
    LIB_FUNCTION("kg4x8Prhfxw", "libkernel", 1, "libkernel", posix_getuid);
    LIB_FUNCTION("6Z83sYWFlA8", "libkernel", 1, "libkernel", exit);
}

} // namespace Libraries::Kernel
