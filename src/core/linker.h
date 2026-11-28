// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <unordered_set>
#include <vector>
#include "common/types.h"
#include "core/libraries/kernel/threads.h"
#include "core/module.h"

namespace Core {

struct DynamicModuleInfo;
class Linker;
class MemoryManager;

struct OrbisKernelMemParam {
    u64 size;
    u64* extended_page_table;
    u64* flexible_memory_size;
    u8* extended_memory_1;
    u64* extended_gpu_page_table;
    u8* extended_memory_2;
    u64* extended_cpu_page_table;
};
static_assert(sizeof(OrbisKernelMemParam) == 0x38);

struct OrbisProcParam {
    u64 size;
    u32 magic;
    u32 entry_count;
    u64 sdk_version;
    char* process_name;
    char* main_thread_name;
    u32* main_thread_prio;
    u32* main_thread_stack_size;
    void* libc_param;
    OrbisKernelMemParam* mem_param;
    void* fs_param;
    u32* process_preload_enable;
    u64 unknown1;
};

using ExitFunc = PS4_SYSV_ABI void (*)();

class Linker;

struct EntryParams {
    int argc;
    u32 padding;
    const char* argv[41];
    VAddr entry_addr;
};

struct HeapAPI {
    PS4_SYSV_ABI void* (*heap_malloc)(u64);
    PS4_SYSV_ABI void (*heap_free)(void*);
    PS4_SYSV_ABI void* (*heap_calloc)(u64, u64);
    PS4_SYSV_ABI void* (*heap_realloc)(void*, u64);
    PS4_SYSV_ABI void* (*heap_memalign)(u64, u64);
    PS4_SYSV_ABI s32 (*heap_posix_memalign)(void**, u64, u64);
    PS4_SYSV_ABI s32 (*heap_reallocalign)(void*, u64, u64);
    PS4_SYSV_ABI s32 (*heap_malloc_stats)(void*);
    PS4_SYSV_ABI s32 (*heap_malloc_stats_fast)(void*);
    PS4_SYSV_ABI u64 (*heap_malloc_usable_size)(void*);
};

using AppHeapAPI = HeapAPI*;

class Linker {
public:
    static constexpr u32 MaxModulesDefault = 0x100;
    static constexpr u32 MaxModulesExtended = 0x300;
    static constexpr s32 LibKernelHandle = 0x2001;

    explicit Linker();
    ~Linker();

    Loader::SymbolsResolver& GetHLESymbols() {
        return m_hle_symbols;
    }

    OrbisProcParam* GetProcParam() const {
        const auto* mod = GetModule(0);
        return mod ? mod->GetProcParam<OrbisProcParam*>() : nullptr;
    }

    Module* GetModule(s32 handle) const {
        const auto it = m_modules.find(handle);
        return it != m_modules.end() ? it->second.get() : nullptr;
    }

    const std::vector<s32>& GetModuleOrder() const noexcept {
        return m_load_order;
    }

    s32 FindByName(const std::filesystem::path& name) const;
    s32 FindByGuestOrBaseName(std::string_view name) const;

    u32 MaxTlsIndex() const {
        return max_tls_index;
    }

    u32 GenerationCounter() const {
        return dtv_generation_counter;
    }

    size_t StaticTlsSize() const noexcept {
        return static_tls_size;
    }

    void RelocateAnyImports(Module* m);
    void RelocateAllImports();

    void LoadLibcInternal() {
        for (const s32 handle : m_load_order) {
            auto* module = GetModule(handle);
            if (module && module->name.contains("libSceLibcInternal")) {
                module->Start(0, nullptr, nullptr);
            }
        }
    }

    void SetHeapAPI(void* func[]) {
        heap_api = reinterpret_cast<AppHeapAPI>(func);
    }

    void AdvanceGenerationCounter() noexcept {
        dtv_generation_counter++;
    }

    void* TlsGetAddr(u64 module_index, u64 offset);
    void* AllocateTlsForThread(bool is_primary);
    void FreeTlsForNonPrimaryThread(void* pointer);

    s32 LoadModule(const std::filesystem::path& elf_name, bool is_dynamic = false,
                   s32* out_error = nullptr);
    s32 LoadAndStartModule(const std::filesystem::path& path, u64 args, const void* argp, int* pRes,
                           s32* out_error = nullptr);
    s32 StopUnloadModule(s32 handle, u64 args, const void* argp, int* pRes);
    Module* FindByAddress(VAddr address, u32 flags = 0);

    void Relocate(Module* module);
    bool Resolve(const std::string& name, Loader::SymbolType sym_type, Module* module,
                 Loader::SymbolRecord* return_info, bool skip_copy_source = false);
    s32 Dlsym(s32 handle, const char* symbol, void** addrp);
    void Execute(const std::vector<std::string>& args = {});
    void DebugDump();

private:
    s32 AllocateHandle(bool is_dynamic);
    std::filesystem::path ResolveModulePath(const std::filesystem::path& path) const;
    bool LoadDependencies(Module* module, std::unordered_set<std::string>& loading);
    void UnloadModuleLocked(s32 handle, std::vector<s32>& deps_to_release);

    MemoryManager* memory;
    Libraries::Kernel::Thread main_thread;
    mutable std::recursive_mutex mutex;
    u32 dtv_generation_counter{1};
    size_t static_tls_size{};
    u32 max_tls_index{};
    u32 max_modules{MaxModulesDefault};
    s32 next_dynamic_handle{2};
    u32 num_static_modules{};
    AppHeapAPI heap_api{};
    std::map<s32, std::unique_ptr<Module>> m_modules;
    std::vector<s32> m_load_order;
    std::map<u32, Module*> m_tls_modules;
    Loader::SymbolsResolver m_hle_symbols{};
};

} // namespace Core
