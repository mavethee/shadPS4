// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/alignment.h"
#include "common/arch.h"
#include "common/assert.h"
#include "common/elf_info.h"
#include "common/logging/formatter.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "common/string_util.h"
#include "common/thread.h"
#include "core/aerolib/aerolib.h"
#include "core/aerolib/stubs.h"
#include "core/devtools/widget/module_list.h"
#include "core/emulator_settings.h"
#include "core/file_sys/backends/host_fs.h"
#include "core/file_sys/fs.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/threads.h"
#include "core/libraries/libc_internal/libc_internal.h"
#include "core/libraries/sysmodule/sysmodule.h"
#include "core/libraries/sysmodule/sysmodule_internal.h"
#include "core/linker.h"
#include "core/memory.h"
#include "core/tls.h"
#include "ipc/ipc.h"

#ifndef _WIN32
#include <signal.h>
#endif

namespace Core {

static PS4_SYSV_ABI void ProgramExitFunc() {
    LOG_ERROR(Core_Linker, "Exit function called");
}

static PS4_SYSV_ABI void* RunMainEntry [[noreturn]](EntryParams* params) {
#ifdef ARCH_X86_64
    // Start shared library modules
    asm volatile("andq $-16, %%rsp\n" // Align to 16 bytes
                 "subq $8, %%rsp\n"   // videoout_basic expects the stack to be misaligned

                 // Kernel also pushes some more things here during process init
                 // at least: environment, auxv, possibly other things

                 "pushq 8(%1)\n" // copy EntryParams to top of stack like the kernel does
                 "pushq 0(%1)\n" // OpenOrbis expects to find it there

                 "movq %1, %%rdi\n" // also pass params and exit func
                 "movq %2, %%rsi\n" // as before

                 "jmp *%0\n" // can't use call here, as that would mangle the prepared stack.
                             // there's no coming back
                 :
                 : "r"(params->entry_addr), "r"(params), "r"(ProgramExitFunc)
                 : "rax", "rsi", "rdi");
    UNREACHABLE();
#else
    UNREACHABLE_MSG("RunMainEntry unimplemented for current architecture.");
#endif
}

Linker::Linker() : memory{Memory::Instance()} {}

Linker::~Linker() = default;

void Linker::Execute(const std::vector<std::string>& args) {
    if (EmulatorSettings.IsDebugDump()) {
        DebugDump();
    }

    // Calculate static TLS size.
    Module* module = GetModule(0);
    static_tls_size = module->tls.offset = module->tls.image_size;

    // Map libSceLibcInternal
    const auto& libc_internal_path =
        EmulatorSettings.GetSysModulesDir() / "libSceLibcInternal.sprx";
    bool has_libcinternal = false;
    if (std::filesystem::exists(libc_internal_path)) {
        LoadModule(libc_internal_path);
        has_libcinternal = true;
    } else {
        // Need to load HLE, LLE isn't present
        LOG_INFO(Core_Linker, "Can't Load libSceLibcInternal.sprx switching to HLE");
        Libraries::LibcInternal::RegisterLib(&GetHLESymbols());
    }

    // Relocate all modules
    RelocateAllImports();

    // libkernel entry is responsible for initializing malloc-related elements of libSceLibcInternal
    // this is done through calling _malloc_init, and sceLibcInternalMemoryMutexEnable.
    static PS4_SYSV_ABI s32 (*malloc_init)() = nullptr;
    static PS4_SYSV_ABI void (*sceLibcInternalMemoryMutexEnable)() = nullptr;

    if (has_libcinternal) {
        for (const s32 handle : m_load_order) {
            const auto* mod = GetModule(handle);
            if (mod && mod->name.contains("libSceLibcInternal.sprx")) {
                malloc_init = reinterpret_cast<PS4_SYSV_ABI s32 (*)()>(
                    const_cast<Module*>(mod)->FindByName("_malloc_init"));
                sceLibcInternalMemoryMutexEnable = reinterpret_cast<PS4_SYSV_ABI void (*)()>(
                    const_cast<Module*>(mod)->FindByName("sceLibcInternalMemoryMutexEnable"));
                break;
            }
        }
    }

    // Configure the direct and flexible memory regions.
    u64 fmem_size = ORBIS_KERNEL_FLEXIBLE_MEMORY_SIZE;
    bool use_extended_mem1 = true, use_extended_mem2 = true;

    const auto* proc_param = GetProcParam();
    ASSERT(proc_param);

    Core::OrbisKernelMemParam mem_param{};
    if (proc_param->size >= offsetof(OrbisProcParam, mem_param) + sizeof(OrbisKernelMemParam*)) {
        if (proc_param->mem_param) {
            mem_param = *proc_param->mem_param;
            if (mem_param.size >=
                offsetof(OrbisKernelMemParam, flexible_memory_size) + sizeof(u64*)) {
                if (const auto* flexible_size = mem_param.flexible_memory_size) {
                    fmem_size = *flexible_size + ORBIS_KERNEL_FLEXIBLE_MEMORY_BASE;
                }
            }
        }
    }

    if (mem_param.size < offsetof(OrbisKernelMemParam, extended_memory_1) + sizeof(u64*)) {
        mem_param.extended_memory_1 = nullptr;
    }
    if (mem_param.size < offsetof(OrbisKernelMemParam, extended_memory_2) + sizeof(u64*)) {
        mem_param.extended_memory_2 = nullptr;
    }

    const u64 sdk_ver = proc_param->sdk_version;
    if (sdk_ver < Common::ElfInfo::FW_500) {
        use_extended_mem1 = mem_param.extended_memory_1 ? *mem_param.extended_memory_1 : false;
        use_extended_mem2 = mem_param.extended_memory_2 ? *mem_param.extended_memory_2 : false;
    }

    memory->SetupMemoryRegions(fmem_size, use_extended_mem1, use_extended_mem2);

    main_thread.Run([this, module, &args, has_libcinternal](std::stop_token) {
        Common::SetCurrentThreadName("Game:Main");

#ifndef _WIN32 // Clear any existing signal mask for game threads.
        sigset_t emptyset;
        sigemptyset(&emptyset);
        pthread_sigmask(SIG_SETMASK, &emptyset, nullptr);
#endif
        if (auto& ipc = IPC::Instance()) {
            ipc.WaitForStart();
        }

        // Load libSceLibcInternal, run malloc_init.
        if (has_libcinternal) {
            LoadLibcInternal();

            if (malloc_init && sceLibcInternalMemoryMutexEnable) {
                // Call _malloc_init
                s32 ret = malloc_init();
                ASSERT_MSG(ret == 0, "malloc_init failed");
                sceLibcInternalMemoryMutexEnable();
            }
        }

        // Have libSceSysmodule preload our libraries.
        Libraries::SysModule::sceSysmodulePreloadModuleForLibkernel();

        // Load and start custom modules from the user directory.
        std::string_view id = Common::ElfInfo::Instance().GameSerial();
        const auto& custom_mod_directory =
            Common::FS::GetUserPath(Common::FS::PathType::CustomModulesDir) / id;
        if (!std::filesystem::exists(custom_mod_directory)) {
            std::filesystem::create_directory(custom_mod_directory);
        }
        for (const auto& entry : std::filesystem::directory_iterator(custom_mod_directory)) {
            if (entry.is_regular_file()) {
                LOG_INFO(Core_Linker, "Loading custom module: {}",
                         fmt::UTF(entry.path().u8string()));
                if (LoadAndStartModule(entry.path(), 0, nullptr, nullptr) == -1) {
                    LOG_ERROR(Core_Linker, "Failed to load custom module: {}",
                              fmt::UTF(entry.path().u8string()));
                }
            }
        }

        // Simulate libSceGnmDriver initialization, which maps a chunk of direct memory.
        // Some games fail without accurately emulating this behavior.
        s64 phys_addr{};
        s32 result = Libraries::Kernel::sceKernelAllocateDirectMemory(
            0, Libraries::Kernel::sceKernelGetDirectMemorySize(), 0x10000, 0x10000, 3, &phys_addr);
        if (result == 0) {
            void* addr{reinterpret_cast<void*>(0xfe0000000)};
            result = Libraries::Kernel::sceKernelMapNamedDirectMemory(
                &addr, 0x10000, 0x13, 0, phys_addr, 0x10000, "SceGnmDriver");
        }
        ASSERT_MSG(result == 0, "Unable to emulate libSceGnmDriver initialization");

        // Add all guest arguments, we will always have the executable path in argv[0]
        EntryParams& params = Libraries::Kernel::entry_params;
        constexpr int MaxArgs = 33;
        params.argc = std::min<int>(args.size(), MaxArgs);
        for (int i = 0; i < params.argc; i++) {
            params.argv[i] = args[i].c_str();
        }
        // Provide empty envp followed by AT_PAGESZ (6) and AT_SECURE (23) auxv entries.
        int tail = params.argc;
        params.argv[tail++] = nullptr;
        params.argv[tail++] = nullptr;
        params.argv[tail++] = reinterpret_cast<const char*>(uintptr_t{6});
        params.argv[tail++] = reinterpret_cast<const char*>(uintptr_t{0x4000});
        params.argv[tail++] = reinterpret_cast<const char*>(uintptr_t{23});
        params.argv[tail++] = reinterpret_cast<const char*>(uintptr_t{1});
        params.argv[tail++] = nullptr;
        params.argv[tail++] = nullptr;

        // Run the game's entry function
        params.entry_addr = module->GetEntryAddress();
        RunMainEntry(&params);
    });
}

s32 Linker::FindByName(const std::filesystem::path& name) const {
    const auto resolved = ResolveModulePath(name);
    for (const s32 handle : m_load_order) {
        const auto* mod = GetModule(handle);
        if (!mod) {
            continue;
        }
        if (name == mod->file || resolved == mod->file) {
            return handle;
        }
    }
    return -1;
}

s32 Linker::FindByGuestOrBaseName(std::string_view name) const {
    for (const s32 handle : m_load_order) {
        const auto* mod = GetModule(handle);
        if (!mod) {
            continue;
        }
        if (mod->name == name || std::string_view{mod->info.name.data()} == name) {
            return handle;
        }
    }
    return -1;
}

void Linker::RelocateAnyImports(Module* m) {
    std::scoped_lock lk{mutex};

    Relocate(m);
    const auto exports = m->GetExportModules();
    for (const auto& export_mod : exports) {
        for (const s32 handle : m_load_order) {
            auto* module = GetModule(handle);
            if (!module) {
                continue;
            }
            const auto imports = module->GetImportModules();
            if (std::ranges::contains(imports, export_mod.name, &ModuleInfo::name)) {
                Relocate(module);
            }
        }
    }
}

void Linker::RelocateAllImports() {
    std::scoped_lock lk{mutex};
    for (const s32 handle : m_load_order) {
        if (auto* module = GetModule(handle)) {
            Relocate(module);
        }
    }
}

std::filesystem::path Linker::ResolveModulePath(const std::filesystem::path& path) const {
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const std::string raw = path.generic_string();
    if (raw.empty()) {
        return path;
    }

    auto exists_in_vfs_or_host = [&](const std::filesystem::path& candidate) -> bool {
        const std::string c = candidate.generic_string();
        if (!c.empty() && c.front() == '/') {
            return mnt->Exists(c) || std::filesystem::exists(candidate);
        }
        return std::filesystem::exists(candidate);
    };

    if (raw.front() == '/') {
        if (exists_in_vfs_or_host(path)) {
            return path;
        }
        if (raw.starts_with("/system/common/lib/") || raw.starts_with("/system/priv/lib/")) {
            const auto sys_candidate = EmulatorSettings.GetSysModulesDir() / path.filename();
            if (std::filesystem::exists(sys_candidate)) {
                return sys_candidate;
            }
        }
        if (raw.starts_with("/app0/lib/")) {
            const std::filesystem::path alt =
                std::filesystem::path{"/app0/sce_module"} / path.filename();
            if (exists_in_vfs_or_host(alt)) {
                return alt;
            }
        } else if (raw.starts_with("/app0/sce_module/")) {
            const std::filesystem::path alt = std::filesystem::path{"/app0/lib"} / path.filename();
            if (exists_in_vfs_or_host(alt)) {
                return alt;
            }
        }
        return path;
    }

    if (!raw.contains('/')) {
        for (const std::string_view dir : {"/app0/sce_module", "/app0/lib", "/app0"}) {
            const std::filesystem::path candidate = std::filesystem::path{dir} / path;
            if (exists_in_vfs_or_host(candidate)) {
                return candidate;
            }
        }
    }
    return path;
}

s32 Linker::AllocateHandle(bool is_dynamic) {
    if (!is_dynamic && !m_modules.contains(0)) {
        return 0;
    }
    while (next_dynamic_handle == 0 || next_dynamic_handle == LibKernelHandle ||
           m_modules.contains(next_dynamic_handle)) {
        next_dynamic_handle++;
        if (next_dynamic_handle > 0x70000000) {
            next_dynamic_handle = 2;
        }
    }
    return next_dynamic_handle++;
}

bool Linker::LoadDependencies(Module* module, std::unordered_set<std::string>& loading) {
    for (const char* needed_name : module->dynamic_info.needed) {
        if (!needed_name || needed_name[0] == '\0') {
            continue;
        }
        const std::filesystem::path dep_path = ResolveModulePath(needed_name);
        const std::string dep_key = dep_path.generic_string();
        if (loading.contains(dep_key)) {
            continue;
        }
        const s32 existing = FindByName(dep_path);
        if (existing > 0) {
            if (auto* dep_mod = GetModule(existing)) {
                dep_mod->AddRef();
                module->dependencies.push_back(existing);
            }
            continue;
        }
        auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
        const bool can_open =
            (!dep_key.empty() && dep_key.front() == '/' && mnt->Exists(dep_key)) ||
            std::filesystem::exists(dep_path);
        if (!can_open) {
            continue;
        }
        loading.insert(dep_key);
        s32 dep_err = ORBIS_OK;
        const s32 dep_handle = LoadAndStartModule(dep_path, 0, nullptr, nullptr, &dep_err);
        loading.erase(dep_key);
        if (dep_handle > 0) {
            module->dependencies.push_back(dep_handle);
        }
    }
    return true;
}

void Linker::UnloadModuleLocked(s32 handle, std::vector<s32>& deps_to_release) {
    const auto it = m_modules.find(handle);
    if (it == m_modules.end()) {
        return;
    }
    auto& mod = it->second;
    deps_to_release = std::move(mod->dependencies);
    if (mod->tls.modid != 0) {
        m_tls_modules.erase(mod->tls.modid);
        AdvanceGenerationCounter();
    }
    std::erase(m_load_order, handle);
    m_modules.erase(it);
}

s32 Linker::LoadModule(const std::filesystem::path& elf_name, bool is_dynamic, s32* out_error) {
    std::scoped_lock lk{mutex};
    const auto set_err = [out_error](s32 err) {
        if (out_error) {
            *out_error = err;
        }
    };

    if (m_modules.size() >= max_modules) {
        set_err(ORBIS_KERNEL_ERROR_ENOMEM);
        return -1;
    }

    const auto resolved_path = ResolveModulePath(elf_name);
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const std::string as_guest = resolved_path.generic_string();
    std::unique_ptr<Core::FileSys::IFile> handle;
    if (!as_guest.empty() && as_guest.front() == '/') {
        handle = mnt->Open(as_guest, /*writable=*/false);
    }
    if (!handle) {
        if (!std::filesystem::exists(resolved_path)) {
            LOG_ERROR(Core_Linker, "Provided file {} does not exist", resolved_path.string());
            set_err(ORBIS_KERNEL_ERROR_ENOENT);
            return -1;
        }
        auto host = std::make_unique<Core::FileSys::HostFile>(
            resolved_path, Common::FS::FileAccessMode::Read, /*read_only=*/true);
        if (!host->IsOpen()) {
            LOG_ERROR(Core_Linker, "Provided file {} could not be opened", resolved_path.string());
            set_err(ORBIS_KERNEL_ERROR_EACCES);
            return -1;
        }
        handle = std::move(host);
    }

    const s32 mod_id = AllocateHandle(is_dynamic);
    auto module =
        std::make_unique<Module>(memory, resolved_path, std::move(handle), max_tls_index, mod_id);
    if (!module->IsValid()) {
        if (!is_dynamic) {
            ASSERT_MSG(
                false,
                "Provided file {} is not valid ELF file. This usually indicated a corrupted dump.",
                resolved_path.string());
        }
        LOG_ERROR(Core_Linker, "Invalid ELF/SELF file {}", resolved_path.string());
        set_err(ORBIS_KERNEL_ERROR_ENOEXEC);
        return -1;
    }

    if (module->tls.modid != 0) {
        m_tls_modules[module->tls.modid] = module.get();
    }
    num_static_modules += !is_dynamic;
    m_modules.emplace(mod_id, std::move(module));
    m_load_order.push_back(mod_id);

    Core::Devtools::Widget::ModuleList::AddModule(resolved_path.filename().string(), resolved_path);
    set_err(ORBIS_OK);
    return mod_id;
}

s32 Linker::LoadAndStartModule(const std::filesystem::path& path, u64 args, const void* argp,
                               int* pRes, s32* out_error) {
    const auto set_err = [out_error](s32 err) {
        if (out_error) {
            *out_error = err;
        }
    };

    std::unique_lock lk{mutex};
    const auto resolved_path = ResolveModulePath(path);
    s32 handle = FindByName(resolved_path);
    if (handle != -1) {
        if (auto* existing = GetModule(handle)) {
            existing->AddRef();
        }
        set_err(ORBIS_OK);
        return handle;
    }

    s32 load_err = ORBIS_OK;
    handle = LoadModule(resolved_path, true, &load_err);
    if (handle == -1) {
        set_err(load_err);
        return -1;
    }

    auto* module = GetModule(handle);
    auto* param = module->GetProcParam<OrbisProcParam*>();
    if ((param && param->size < 0x18) ||
        (module->sdk_version != 0 && (module->sdk_version & 0xffff0000U) > 0x14000000U)) {
        const s32 err = (param && param->size < 0x18) ? ORBIS_KERNEL_ERROR_ENOEXEC
                                                      : ORBIS_KERNEL_ERROR_ESDKVERSION;
        std::vector<s32> deps;
        UnloadModuleLocked(handle, deps);
        set_err(err);
        return -1;
    }

    std::unordered_set<std::string> loading;
    loading.insert(resolved_path.generic_string());
    LoadDependencies(module, loading);

    RelocateAnyImports(module);

    if (module->tls.image_size != 0) {
        AdvanceGenerationCounter();
    }

    lk.unlock();

    const s32 ret = module->Start(args, argp, nullptr);
    if (pRes) {
        *pRes = ret;
    }

    s32 sdk_ver = 0;
    Libraries::Kernel::sceKernelGetCompiledSdkVersion(&sdk_ver);
    if (ret < 0 && sdk_ver >= 0x02000000) {
        module->Stop(0, nullptr, nullptr);
        lk.lock();
        std::vector<s32> deps;
        UnloadModuleLocked(handle, deps);
        lk.unlock();
        for (const s32 dep : deps) {
            StopUnloadModule(dep, 0, nullptr, nullptr);
        }
        const s32 ret_err = (sdk_ver < static_cast<s32>(Common::ElfInfo::FW_1150))
                                ? ORBIS_KERNEL_ERROR_EINVAL
                                : ORBIS_KERNEL_ERROR_ESTART;
        set_err(ret_err);
        return -1;
    }

    set_err(ORBIS_OK);
    return handle;
}

s32 Linker::StopUnloadModule(s32 handle, u64 args, const void* argp, int* pRes) {
    if (handle == 0) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    std::unique_lock lk{mutex};
    auto* module = GetModule(handle);
    if (!module) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }

    if (module->GetRefCount() <= 1) {
        for (const s32 other_handle : m_load_order) {
            if (other_handle == handle) {
                continue;
            }
            const auto* other = GetModule(other_handle);
            if (other && std::ranges::contains(other->dependencies, handle)) {
                return ORBIS_KERNEL_ERROR_EBUSY;
            }
        }
    }

    if (module->ReleaseRef() > 0) {
        return ORBIS_OK;
    }

    lk.unlock();
    const s32 stop_ret = module->Stop(args, argp, nullptr);
    if (pRes) {
        *pRes = stop_ret;
    }
    lk.lock();

    std::vector<s32> deps;
    UnloadModuleLocked(handle, deps);
    lk.unlock();

    for (const s32 dep : deps) {
        StopUnloadModule(dep, 0, nullptr, nullptr);
    }
    return ORBIS_OK;
}

Module* Linker::FindByAddress(VAddr address, u32 flags) {
    std::scoped_lock lk{mutex};
    for (const s32 handle : m_load_order) {
        auto* module = GetModule(handle);
        if (!module) {
            continue;
        }
        if (flags != 0) {
            if (module->ContainsSegmentAddress(address, flags)) {
                return module;
            }
        } else {
            const VAddr base = module->GetBaseAddress();
            if (address >= base && address < base + module->aligned_base_size) {
                return module;
            }
        }
    }
    return nullptr;
}

void Linker::Relocate(Module* module) {
    if (!module || !module->dynamic_info.symbol_table || !module->dynamic_info.str_table) {
        return;
    }
    const u32 num_relocs =
        static_cast<u32>(module->dynamic_info.relocation_table_size / sizeof(elf_relocation));
    module->ForEachRelocation([&](elf_relocation* rel, u32 i, bool is_jmp_rel) {
        const u32 bit_idx = (is_jmp_rel ? num_relocs : 0) + i;
        if (module->TestRelaBit(bit_idx)) {
            return;
        }
        const auto type = rel->GetType();
        const auto symbol = rel->GetSymbol();
        auto addend = rel->rel_addend;
        auto* symbol_table = module->dynamic_info.symbol_table;
        auto* names_tlb = module->dynamic_info.str_table;

        const VAddr rel_base_virtual_addr = module->GetBaseAddress();
        const VAddr rel_virtual_addr = rel_base_virtual_addr + rel->rel_offset;
        bool rel_is_resolved = false;
        bool mark_done = false;
        u64 rel_value = 0;
        u32 rel_write_size = sizeof(u64);
        Loader::SymbolType rel_sym_type = Loader::SymbolType::Unknown;
        std::string rel_name;

        const u32 num_syms =
            static_cast<u32>(module->dynamic_info.symbol_table_total_size / sizeof(elf_symbol));
        const auto get_sym_type = [](u8 st) -> Loader::SymbolType {
            switch (st) {
            case STT_FUN:
                return Loader::SymbolType::Function;
            case STT_OBJECT:
                return Loader::SymbolType::Object;
            case STT_TLS:
                return Loader::SymbolType::Tls;
            case STT_NOTYPE:
            default:
                return Loader::SymbolType::NoType;
            }
        };

        switch (type) {
        case R_X86_64_NONE:
            module->SetRelaBit(bit_idx);
            return;
        case R_X86_64_RELATIVE:
            rel_value = rel_base_virtual_addr + addend;
            rel_is_resolved = true;
            mark_done = true;
            break;
        case R_X86_64_DTPMOD64: {
            if (symbol == 0 || symbol >= num_syms || symbol_table[symbol].GetBind() == STB_LOCAL ||
                symbol_table[symbol].st_shndx != 0) {
                rel_value = static_cast<u64>(module->tls.modid);
                rel_is_resolved = true;
                mark_done = true;
            } else {
                const auto& sym = symbol_table[symbol];
                rel_name = names_tlb + sym.st_name;
                Loader::SymbolRecord symrec{};
                if (Resolve(rel_name, get_sym_type(sym.GetType()), module, &symrec)) {
                    for (const s32 handle : m_load_order) {
                        const auto* candidate = GetModule(handle);
                        if (candidate && candidate->export_sym.FindSymbol(symrec.symbol)) {
                            rel_value = static_cast<u64>(candidate->tls.modid);
                            rel_is_resolved = true;
                            mark_done = true;
                            break;
                        }
                    }
                }
            }
            rel_sym_type = Loader::SymbolType::Tls;
            break;
        }
        case R_X86_64_DTPOFF64: {
            if (symbol < num_syms) {
                const auto& sym = symbol_table[symbol];
                if (sym.GetBind() == STB_LOCAL || sym.st_shndx != 0) {
                    const u64 sym_off =
                        (sym.GetType() == STT_TLS || sym.st_value < module->tls.image_size)
                            ? sym.st_value
                            : 0;
                    rel_value = sym_off + addend;
                    rel_is_resolved = true;
                    mark_done = true;
                } else {
                    rel_name = names_tlb + sym.st_name;
                    Loader::SymbolRecord symrec{};
                    if (Resolve(rel_name, get_sym_type(sym.GetType()), module, &symrec)) {
                        rel_value = symrec.virtual_address + addend;
                        rel_is_resolved = true;
                        mark_done = true;
                    }
                }
            }
            break;
        }
        case R_X86_64_TPOFF64: {
            if (symbol < num_syms) {
                const auto& sym = symbol_table[symbol];
                rel_value = sym.st_value - module->tls.offset + addend;
                rel_is_resolved = true;
                mark_done = true;
            }
            break;
        }
        case R_X86_64_COPY: {
            if (symbol < num_syms) {
                const auto& sym = symbol_table[symbol];
                rel_name = names_tlb + sym.st_name;
                Loader::SymbolRecord symrec{};
                if (Resolve(rel_name, Loader::SymbolType::Object, module, &symrec, true) &&
                    symrec.virtual_address != 0 && sym.st_size > 0) {
                    std::memcpy(reinterpret_cast<void*>(rel_virtual_addr),
                                reinterpret_cast<const void*>(symrec.virtual_address), sym.st_size);
                    module->SetRelaBit(bit_idx);
                }
            }
            return;
        }
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            addend = 0;
        case R_X86_64_PC32:
        case R_X86_64_64: {
            if (symbol >= num_syms) {
                break;
            }
            const auto sym = symbol_table[symbol];
            const auto sym_bind = sym.GetBind();
            const auto sym_type = sym.GetType();
            u64 symbol_virtual_addr = 0;
            Loader::SymbolRecord symrec{};
            rel_sym_type = get_sym_type(sym_type);

            switch (sym_bind) {
            case STB_LOCAL:
                symbol_virtual_addr = rel_base_virtual_addr + sym.st_value;
                mark_done = true;
                break;
            case STB_GLOBAL:
            case STB_WEAK: {
                rel_name = names_tlb + sym.st_name;
                if (Resolve(rel_name, rel_sym_type, module, &symrec)) {
                    mark_done = true;
                    symbol_virtual_addr = symrec.virtual_address;
                } else if (sym_bind == STB_WEAK) {
                    symbol_virtual_addr = 0;
                    rel_is_resolved = true;
                } else {
                    symbol_virtual_addr = symrec.virtual_address;
                }
                break;
            }
            default:
                UNREACHABLE_MSG("Unknown bind type {}", sym_bind);
            }
            if (symbol_virtual_addr != 0) {
                rel_is_resolved = true;
            }
            if (type == R_X86_64_PC32) {
                rel_value = rel_is_resolved && symbol_virtual_addr != 0
                                ? static_cast<u32>(static_cast<s32>(symbol_virtual_addr + addend -
                                                                    rel_virtual_addr))
                                : 0;
                rel_write_size = sizeof(u32);
            } else {
                rel_value = (symbol_virtual_addr != 0 ? symbol_virtual_addr + addend : 0);
            }
            rel_name = symrec.symbol.name;
            break;
        }
        default:
            LOG_INFO(Core_Linker, "UNK type {:#010x} rel symbol : {:#010x}", type, symbol);
        }

        if (mark_done) {
            module->SetRelaBit(bit_idx);
        }
        if (rel_is_resolved) {
            std::memcpy(reinterpret_cast<void*>(rel_virtual_addr), &rel_value, rel_write_size);
        } else if (rel_sym_type == Loader::SymbolType::Function) {
            LOG_INFO(Core_Linker, "Function not patched! {}", rel_name);
        }
    });
}

bool Linker::Resolve(const std::string& name, Loader::SymbolType sym_type, Module* m,
                     Loader::SymbolRecord* return_info, bool skip_copy_source) {
    const auto ids = Common::SplitString(name, '#');
    const LibraryInfo* library;
    const ModuleInfo* module;
    if (ids.size() != 3 && sym_type != Loader::SymbolType::NoType) {
        return_info->virtual_address = 0;
        return_info->symbol.name = ids.at(0);
        LOG_ERROR(Core_Linker, "Not Resolved {}", name);
        return false;
    } else if (ids.size() == 1 && sym_type == Loader::SymbolType::NoType) {
        LOG_DEBUG(Core_Linker, "NoType export {}", name);
        library = m->FindLibrary("");
        module = m->FindModule("");
    } else {
        library = m->FindLibrary(ids[1]);
        module = m->FindModule(ids[2]);
    }

    ASSERT_MSG(library && module, "Unable to find library and module");

    Loader::SymbolResolver sr{};
    sr.name = ids.at(0);
    sr.library = library->name;
    sr.library_version = library->version;
    sr.module = module->name;
    sr.type = sym_type;

    const auto* record = skip_copy_source ? nullptr : m_hle_symbols.FindSymbol(sr);
    if (record) {
        *return_info = *record;
        Core::Devtools::Widget::ModuleList::AddModule(sr.library);
        return true;
    }

    const Loader::SymbolRecord* weak_candidate = nullptr;
    auto check_module = [&](const Module* mod) -> const Loader::SymbolRecord* {
        if (!mod || (skip_copy_source && mod == m)) {
            return nullptr;
        }
        if (!std::ranges::contains(mod->GetExportLibs(), *library) ||
            !std::ranges::contains(mod->GetExportModules(), *module)) {
            return nullptr;
        }
        if (mod->export_sym.GetSize() == 0) {
            return nullptr;
        }
        const auto* rec = mod->export_sym.FindSymbol(sr);
        if (rec && rec->is_weak && !weak_candidate) {
            weak_candidate = rec;
            return nullptr;
        }
        return rec;
    };

    // Prioritize declared dependencies before scanning the global module load order.
    for (const s32 dep_handle : m->dependencies) {
        if (const auto* rec = check_module(GetModule(dep_handle))) {
            *return_info = *rec;
            return true;
        }
    }
    for (const s32 handle : m_load_order) {
        if (const auto* rec = check_module(GetModule(handle))) {
            *return_info = *rec;
            return true;
        }
    }
    if (weak_candidate) {
        *return_info = *weak_candidate;
        return true;
    }

    const auto aeronid = AeroLib::FindByNid(sr.name.c_str());
    if (sym_type == Loader::SymbolType::Object) {
        return_info->symbol.nidName = aeronid ? aeronid->name : "Unknown object";
        return_info->virtual_address = 0;
    } else if (aeronid) {
        return_info->symbol.nidName = aeronid->name;
        return_info->virtual_address = AeroLib::GetStub(aeronid->nid);
    } else {
        return_info->virtual_address = AeroLib::GetStub(sr.name.c_str());
        return_info->symbol.nidName = "Unknown !!!";
    }
    if (library->name != "libc" && library->name != "libSceFios2") {
        LOG_WARNING(Core_Linker, "Linker: Stub resolved {} as {} (lib: {}, mod: {})", sr.name,
                    return_info->symbol.nidName, library->name, module->name);
    } else {
        if (library->name == "libc" && return_info->symbol.nidName == "Need_sceLibc") {
            Libraries::SysModule::g_need_scelibc = true;
        }
        if (library->name == "libSceFios2" && return_info->symbol.nidName == "sceFiosInitialize") {
            Libraries::SysModule::g_need_scelibc = true;
        }
    }
    return false;
}

s32 Linker::Dlsym(s32 handle, const char* symbol, void** addrp) {
    if (!addrp) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (!symbol || symbol[0] == '\0') {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }

    std::scoped_lock lk{mutex};
    if (handle == LibKernelHandle) {
        const std::string nid = Module::StringToNid(symbol);
        for (const auto& rec : m_hle_symbols.GetSymbols()) {
            if ((rec.symbol.module == "libkernel" || rec.symbol.library == "libkernel" ||
                 rec.symbol.library == "libkernel_psmkit" || rec.symbol.library == "libScePosix") &&
                (rec.symbol.name == nid || rec.symbol.nidName == symbol ||
                 rec.symbol.name == symbol)) {
                *addrp = reinterpret_cast<void*>(rec.virtual_address);
                return ORBIS_OK;
            }
        }
        return ORBIS_KERNEL_ERROR_ESRCH;
    }

    auto* module = GetModule(handle);
    if (!module) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }

    void* resolved = module->FindByName(symbol);
    if (!resolved) {
        resolved = module->FindByNid(symbol);
    }
    if (!resolved) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }

    *addrp = resolved;
    return ORBIS_OK;
}

void* Linker::TlsGetAddr(u64 module_index, u64 offset) {
    std::scoped_lock lk{mutex};

    DtvEntry* dtv_table = GetTcbBase()->tcb_dtv;
    if (dtv_table[0].counter != dtv_generation_counter) {
        const u32 old_num_dtvs = static_cast<u32>(dtv_table[1].counter);
        const u32 new_num_dtvs = std::max(old_num_dtvs, max_tls_index);
        DtvEntry* new_dtv_table = new DtvEntry[new_num_dtvs + 2]{};
        for (u32 i = 1; i <= old_num_dtvs; ++i) {
            u8* ptr = dtv_table[i + 1].pointer;
            if (!ptr) {
                continue;
            }
            if (i <= max_tls_index && m_tls_modules.contains(i)) {
                new_dtv_table[i + 1].pointer = ptr;
            } else {
                const u8* tcb_ptr = reinterpret_cast<const u8*>(GetTcbBase());
                const u8* tls_base = tcb_ptr - static_tls_size;
                if (ptr < tls_base || ptr >= tcb_ptr) {
                    FreeTlsForNonPrimaryThread(ptr);
                }
            }
        }
        new_dtv_table[0].counter = dtv_generation_counter;
        new_dtv_table[1].counter = new_num_dtvs;
        delete[] dtv_table;

        GetTcbBase()->tcb_dtv = new_dtv_table;
        dtv_table = new_dtv_table;
    }

    const auto mod_it = m_tls_modules.find(static_cast<u32>(module_index));
    if (mod_it == m_tls_modules.end() || module_index == 0 || module_index > dtv_table[1].counter) {
        return nullptr;
    }

    u8* addr = dtv_table[module_index + 1].pointer;
    Module* module = mod_it->second;
    if (!addr) {
        const size_t alloc_size = std::max<size_t>(1, module->tls.image_size);
        u8* dest{};
        if (heap_api && heap_api->heap_malloc) {
            dest = reinterpret_cast<u8*>(heap_api->heap_malloc(alloc_size));
        } else {
            dest = reinterpret_cast<u8*>(std::malloc(alloc_size));
        }
        const u32 init_image_size = module->tls.init_image_size;
        if (init_image_size > 0 && module->tls.image_virtual_addr != 0) {
            const u8* src = reinterpret_cast<const u8*>(module->tls.image_virtual_addr);
            std::memcpy(dest, src, init_image_size);
        }
        if (alloc_size > init_image_size) {
            std::memset(dest + init_image_size, 0, alloc_size - init_image_size);
        }
        dtv_table[module_index + 1].pointer = dest;
        addr = dest;
    }
    return addr + offset;
}

void* Linker::AllocateTlsForThread(bool is_primary) {
    static constexpr size_t TcbSize = 0x40;
    static constexpr size_t TlsAllocAlign = 0x20;
    const size_t total_tls_size = Common::AlignUp(static_tls_size, TlsAllocAlign) + TcbSize;

    // If sceKernelMapNamedFlexibleMemory is being called from libkernel and addr = 0
    // it automatically places mappings in system reserved area instead of managed.
    // Since the system reserved area already has a mapping in it, this address is slightly higher.
    static constexpr VAddr KernelAllocBase = 0x881000000ULL;

    // The kernel module has a few different paths for TLS allocation.
    // For SDK < 1.7 it allocates both main and secondary thread blocks using libc mspace/malloc.
    // In games compiled with newer SDK, the main thread gets mapped from flexible memory,
    // with addr = 0, so system managed area. Here we will only implement the latter.
    void* addr_out{reinterpret_cast<void*>(KernelAllocBase)};
    if (is_primary) {
        const size_t tls_aligned = Common::AlignUp(total_tls_size, 16_KB);
        const int ret = Libraries::Kernel::sceKernelMapNamedFlexibleMemory(
            &addr_out, tls_aligned, 3, 0, "SceKernelPrimaryTcbTls");
        ASSERT_MSG(ret == 0, "Unable to allocate TLS+TCB for the primary thread");
    } else {
        if (heap_api && heap_api->heap_malloc) {
            addr_out = heap_api->heap_malloc(total_tls_size);
        } else {
            addr_out = std::malloc(total_tls_size);
        }
    }
    return addr_out;
}

void Linker::FreeTlsForNonPrimaryThread(void* pointer) {
    if (heap_api && heap_api->heap_free) {
        heap_api->heap_free(pointer);
    } else {
        std::free(pointer);
    }
}

void Linker::DebugDump() {
    const auto& log_dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
    const std::filesystem::path debug(log_dir / "debugdump");
    std::filesystem::create_directory(debug);
    for (const s32 handle : m_load_order) {
        Module* module = GetModule(handle);
        if (!module) {
            continue;
        }
        auto& elf = module->elf;
        const std::filesystem::path filepath(debug / module->file.stem());
        std::filesystem::create_directory(filepath);
        module->import_sym.DebugDump(filepath / "imports.txt");
        module->export_sym.DebugDump(filepath / "exports.txt");
        if (elf.IsSelfFile()) {
            elf.SelfHeaderDebugDump(filepath / "selfHeader.txt");
            elf.SelfSegHeaderDebugDump(filepath / "selfSegHeaders.txt");
        }
        elf.ElfHeaderDebugDump(filepath / "elfHeader.txt");
        elf.PHeaderDebugDump(filepath / "elfPHeaders.txt");
    }
}

} // namespace Core
