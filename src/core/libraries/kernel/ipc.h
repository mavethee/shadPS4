// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Kernel {

template <typename T>
class NamedKernelRegistry {
public:
    explicit NamedKernelRegistry(u64 first_handle = 1) : m_next_handle{first_handle} {}

    u64 Create(const std::string& name, bool openable, const std::shared_ptr<T>& obj) {
        std::unique_lock lk{m_mutex};
        const u64 handle = AllocHandleLocked();
        m_handles.emplace(handle, obj);
        if (openable) {
            m_named.insert_or_assign(name, obj);
        }
        return handle;
    }

    u64 Open(const std::string& name) {
        std::unique_lock lk{m_mutex};
        const auto it = m_named.find(name);
        if (it == m_named.end()) {
            return 0;
        }
        const u64 handle = AllocHandleLocked();
        m_handles.emplace(handle, it->second);
        return handle;
    }

    std::shared_ptr<T> Get(u64 handle) const {
        if (handle == 0) {
            return nullptr;
        }
        std::shared_lock lk{m_mutex};
        const auto it = m_handles.find(handle);
        return it == m_handles.end() ? nullptr : it->second;
    }

    bool Close(u64 handle) {
        if (handle == 0) {
            return false;
        }
        std::unique_lock lk{m_mutex};
        return m_handles.erase(handle) > 0;
    }

    std::shared_ptr<T> Delete(u64 handle) {
        if (handle == 0) {
            return nullptr;
        }
        std::unique_lock lk{m_mutex};
        const auto it = m_handles.find(handle);
        if (it == m_handles.end()) {
            return nullptr;
        }
        auto obj = std::move(it->second);
        m_handles.erase(it);
        for (auto nit = m_named.begin(); nit != m_named.end();) {
            if (nit->second == obj) {
                nit = m_named.erase(nit);
            } else {
                ++nit;
            }
        }
        return obj;
    }

private:
    u64 AllocHandleLocked() {
        while (m_next_handle == 0 || m_handles.contains(m_next_handle)) {
            ++m_next_handle;
        }
        return m_next_handle++;
    }

    mutable std::shared_mutex m_mutex;
    u64 m_next_handle;
    std::unordered_map<u64, std::shared_ptr<T>> m_handles;
    std::unordered_map<std::string, std::shared_ptr<T>> m_named;
};

void OnShmFileClosed(const std::filesystem::path& host_path);

std::array<u8, 16> GetPersistentOpenPsId();
std::array<u8, 16> GetPersistentDevKitIdPs(bool is_neo);

void RegisterIpc(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Kernel
