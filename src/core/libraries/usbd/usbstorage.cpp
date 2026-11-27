// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "usbstorage.h"

namespace Libraries::UsbStorage {

// PS4 libSceUsbStorage / auto_mounter error codes (module 0x80f4)
constexpr s32 ORBIS_AUTO_MOUNTER_ERROR_INVAL = static_cast<s32>(0x80f40002);
constexpr s32 ORBIS_AUTO_MOUNTER_ERROR_NOENT = static_cast<s32>(0x80f40004);
constexpr s32 ORBIS_AUTO_MOUNTER_ERROR_NOTSUP = static_cast<s32>(0x80f40005);
constexpr s32 ORBIS_AUTO_MOUNTER_ERROR_NO_DEVICE = static_cast<s32>(0x80f40007);
constexpr s32 ORBIS_AUTO_MOUNTER_ERROR_EXCEED_LIMIT = static_cast<s32>(0x80f40015);

// Flags for OrbisUsbStorageDeviceInfo additional_features
constexpr u64 ORBIS_USB_STORAGE_FEATURE_READ_ONLY = 1ull << 4;
constexpr u64 ORBIS_USB_STORAGE_FEATURE_FAT = 1ull << 6;
constexpr u64 ORBIS_USB_STORAGE_FEATURE_EXFAT = 1ull << 7;
constexpr u64 ORBIS_USB_STORAGE_OPTION_LONG_ID = 1ull << 3;
constexpr size_t MAX_ACTIVE_USB_MAPS = 4;

struct OrbisUsbStorageDeviceInfo {
    u64 capacity;            // +0x00
    u64 free_space;          // +0x08
    u16 vendor_id;           // +0x10
    u16 product_id;          // +0x12
    u16 bcd_device;          // +0x14
    char vendor[256];        // +0x16
    u8 pad1[2];              // +0x116
    u64 vendor_len;          // +0x118
    char product[256];       // +0x120
    u64 product_len;         // +0x220
    char serial_number[256]; // +0x228
    u64 serial_number_len;   // +0x328
    u64 additional_features; // +0x330
};
static_assert(offsetof(OrbisUsbStorageDeviceInfo, vendor) == 0x16);
static_assert(offsetof(OrbisUsbStorageDeviceInfo, vendor_len) == 0x118);
static_assert(offsetof(OrbisUsbStorageDeviceInfo, product) == 0x120);
static_assert(offsetof(OrbisUsbStorageDeviceInfo, product_len) == 0x220);
static_assert(offsetof(OrbisUsbStorageDeviceInfo, serial_number) == 0x228);
static_assert(offsetof(OrbisUsbStorageDeviceInfo, serial_number_len) == 0x328);
static_assert(offsetof(OrbisUsbStorageDeviceInfo, additional_features) == 0x330);
static_assert(sizeof(OrbisUsbStorageDeviceInfo) == 0x338);

#pragma pack(push, 1)
struct OrbisUsbStorageDirent {
    u32 d_fileno;
    u16 d_reclen;
    u8 d_type;
    u8 d_namlen;
    char d_name[256];
};
#pragma pack(pop)
static_assert(offsetof(OrbisUsbStorageDirent, d_fileno) == 0);
static_assert(offsetof(OrbisUsbStorageDirent, d_reclen) == 4);
static_assert(offsetof(OrbisUsbStorageDirent, d_type) == 6);
static_assert(offsetof(OrbisUsbStorageDirent, d_namlen) == 7);
static_assert(offsetof(OrbisUsbStorageDirent, d_name) == 8);

struct UsbDirentItem {
    std::string name;
    bool is_directory;
};

struct UsbGetdentsState {
    std::vector<UsbDirentItem> items;
    size_t current_index = 0;
};

struct UsbMapItem {
    u32 device_id = 0;
    std::string directory;
    std::string mount_point;
};

struct UsbCallbackEntry {
    s32 event_type = 0;
    void* callback = nullptr;
    void* arg = nullptr;
};

struct UsbDirCallbackEntry {
    u32 device_id = 0;
    std::string directory;
    void* callback = nullptr;
    void* arg = nullptr;
};

static std::mutex g_usb_getdents_mutex;
static std::unordered_map<s32, UsbGetdentsState> g_usb_getdents_table;
static s32 g_usb_next_fd = 1;

static std::mutex g_usb_state_mutex;
static std::vector<UsbMapItem> g_usb_maps;
static std::vector<UsbCallbackEntry> g_usb_callbacks;
static std::vector<UsbDirCallbackEntry> g_usb_avail_callbacks;
static std::vector<UsbDirCallbackEntry> g_usb_unavail_callbacks;

static bool IsValidUsbSubpath(std::string_view dir) {
    return !dir.empty() && dir.starts_with('/') && dir.find("..") == std::string_view::npos &&
           dir.find("//") == std::string_view::npos;
}

static std::string BuildUsbMountPoint(u32 device_id, std::string_view directory, u64 option_flags) {
    std::string mount_point = (option_flags & ORBIS_USB_STORAGE_OPTION_LONG_ID) != 0
                                  ? fmt::format("/usb{:08x}", device_id)
                                  : fmt::format("/usb{:02d}", device_id);
    std::string suffix{directory};
    std::replace(suffix.begin(), suffix.end(), '/', '_');
    mount_point += suffix;
    return mount_point;
}

static std::filesystem::path ResolveUsbSubpath(const std::filesystem::path& root,
                                               std::string_view directory) {
    std::string_view rel = directory;
    if (rel.starts_with('/')) {
        rel.remove_prefix(1);
    }
    return rel.empty() ? root : (root / rel);
}

static std::optional<std::filesystem::path> ResolveUsbDeviceRoot(u32 device_id) {
    if (device_id != 0) {
        return std::nullopt;
    }
    const auto usb_root = GetUsbMountPath(device_id);
    std::error_code ec;
    if (!std::filesystem::is_directory(usb_root, ec)) {
        return std::nullopt;
    }
    return usb_root;
}

static s32 RegisterDirCallback(std::vector<UsbDirCallbackEntry>& list, u32 device_id,
                               const char* directory, void* callback, void* arg) {
    if (!directory || !callback) {
        return ORBIS_AUTO_MOUNTER_ERROR_INVAL;
    }
    std::scoped_lock lock(g_usb_state_mutex);
    const auto it = std::find_if(list.begin(), list.end(), [&](const UsbDirCallbackEntry& entry) {
        return entry.device_id == device_id && entry.directory == directory &&
               entry.callback == callback;
    });
    if (it != list.end()) {
        return ORBIS_AUTO_MOUNTER_ERROR_INVAL;
    }
    list.push_back({device_id, directory, callback, arg});
    return ORBIS_OK;
}

static s32 UnregisterDirCallback(std::vector<UsbDirCallbackEntry>& list, u32 device_id,
                                 const char* directory, void* callback) {
    if (!directory || !callback) {
        return ORBIS_AUTO_MOUNTER_ERROR_INVAL;
    }
    std::scoped_lock lock(g_usb_state_mutex);
    const auto it = std::find_if(list.begin(), list.end(), [&](const UsbDirCallbackEntry& entry) {
        return entry.device_id == device_id && entry.directory == directory &&
               entry.callback == callback;
    });
    if (it == list.end()) {
        return ORBIS_AUTO_MOUNTER_ERROR_INVAL;
    }
    list.erase(it);
    return ORBIS_OK;
}

std::filesystem::path GetUsbMountPath(u32 device_id) {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "mnt" /
           ("usb" + std::to_string(device_id));
}

static s32 PS4_SYSV_ABI sceUsbStorageInit(void* attr) {
    LOG_DEBUG(Lib_Usbd, "called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageTerm() {
    std::vector<std::string> mounts_to_unmount;
    {
        std::scoped_lock lock(g_usb_state_mutex);
        for (const auto& item : g_usb_maps) {
            mounts_to_unmount.push_back(item.mount_point);
        }
        g_usb_maps.clear();
        g_usb_callbacks.clear();
        g_usb_avail_callbacks.clear();
        g_usb_unavail_callbacks.clear();
    }
    if (auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance()) {
        for (const auto& mp : mounts_to_unmount) {
            mnt->Unmount(mp);
        }
    }
    LOG_DEBUG(Lib_Usbd, "called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageRegisterCallback(s32 event_type, void* callback, void* arg) {
    if (!callback) {
        return ORBIS_AUTO_MOUNTER_ERROR_INVAL;
    }
    if (event_type != 1 && event_type != 2) {
        return ORBIS_AUTO_MOUNTER_ERROR_INVAL;
    }
    std::scoped_lock lock(g_usb_state_mutex);
    const auto it = std::find_if(g_usb_callbacks.begin(), g_usb_callbacks.end(),
                                 [&](const UsbCallbackEntry& e) {
                                     return e.event_type == event_type && e.callback == callback;
                                 });
    if (it != g_usb_callbacks.end()) {
        return ORBIS_AUTO_MOUNTER_ERROR_INVAL;
    }
    g_usb_callbacks.push_back({event_type, callback, arg});
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageUnregisterCallback(s32 event_type, void* callback) {
    if (!callback) {
        return ORBIS_AUTO_MOUNTER_ERROR_INVAL;
    }
    if (event_type != 1 && event_type != 2) {
        return ORBIS_AUTO_MOUNTER_ERROR_INVAL;
    }
    std::scoped_lock lock(g_usb_state_mutex);
    const auto it = std::find_if(g_usb_callbacks.begin(), g_usb_callbacks.end(),
                                 [&](const UsbCallbackEntry& e) {
                                     return e.event_type == event_type && e.callback == callback;
                                 });
    if (it == g_usb_callbacks.end()) {
        return ORBIS_AUTO_MOUNTER_ERROR_INVAL;
    }
    g_usb_callbacks.erase(it);
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageRegisterCallbackForMapAvailable(u32 device_id,
                                                                     const char* directory,
                                                                     void* callback, void* arg) {
    return RegisterDirCallback(g_usb_avail_callbacks, device_id, directory, callback, arg);
}

static s32 PS4_SYSV_ABI sceUsbStorageUnregisterCallbackForMapAvailable(u32 device_id,
                                                                       const char* directory,
                                                                       void* callback) {
    return UnregisterDirCallback(g_usb_avail_callbacks, device_id, directory, callback);
}

static s32 PS4_SYSV_ABI sceUsbStorageRegisterCallbackForMapUnavailable(u32 device_id,
                                                                       const char* directory,
                                                                       void* callback, void* arg) {
    return RegisterDirCallback(g_usb_unavail_callbacks, device_id, directory, callback, arg);
}

static s32 PS4_SYSV_ABI sceUsbStorageUnregisterCallbackForMapUnavailable(u32 device_id,
                                                                         const char* directory,
                                                                         void* callback) {
    return UnregisterDirCallback(g_usb_unavail_callbacks, device_id, directory, callback);
}

static s32 PS4_SYSV_ABI sceUsbStorageGetDeviceList(u32* device_ids, u32* count) {
    if (!device_ids || !count) {
        LOG_ERROR(Lib_Usbd, "device_ids or count pointer is null");
        return ORBIS_AUTO_MOUNTER_ERROR_INVAL;
    }

    if (!ResolveUsbDeviceRoot(0)) {
        *count = 0;
        LOG_DEBUG(Lib_Usbd, "no USB devices found");
        return ORBIS_OK;
    }

    device_ids[0] = 0;
    *count = 1;
    LOG_INFO(Lib_Usbd, "found 1 USB device (id = 0)");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageGetDeviceInfo(u32 device_id, OrbisUsbStorageDeviceInfo* info) {
    if (!info) {
        LOG_ERROR(Lib_Usbd, "info pointer is null");
        return ORBIS_AUTO_MOUNTER_ERROR_INVAL;
    }
    const auto usb_path = ResolveUsbDeviceRoot(device_id);
    if (!usb_path) {
        LOG_ERROR(Lib_Usbd, "USB device {} mount path does not exist", device_id);
        return ORBIS_AUTO_MOUNTER_ERROR_NOENT;
    }

    std::error_code ec;
    const auto space = std::filesystem::space(*usb_path, ec);
    if (ec) {
        LOG_ERROR(Lib_Usbd, "Could not get free space of USB device {} mount path: {}", device_id,
                  ec.message());
        return ORBIS_AUTO_MOUNTER_ERROR_NOENT;
    }

    std::memset(info, 0, sizeof(OrbisUsbStorageDeviceInfo));
    info->capacity = space.capacity;
    info->free_space = space.free;
    info->vendor_id = 0x054c;
    info->product_id = 0x0268;
    info->bcd_device = 0x0100;

    std::strncpy(info->vendor, "shadPS4", sizeof(info->vendor) - 1);
    info->vendor_len = std::strlen(info->vendor);

    std::strncpy(info->product, "Virtual USB Storage", sizeof(info->product) - 1);
    info->product_len = std::strlen(info->product);

    std::strncpy(info->serial_number, "0123456789ABCDEF", sizeof(info->serial_number) - 1);
    info->serial_number_len = std::strlen(info->serial_number);

    info->additional_features = ORBIS_USB_STORAGE_FEATURE_EXFAT;

    LOG_INFO(Lib_Usbd, "device_id = {}, vendor = '{}', product = '{}', serial = '{}'", device_id,
             info->vendor, info->product, info->serial_number);
    return ORBIS_OK;
}

static bool PS4_SYSV_ABI sceUsbStorageIsExist(u32 device_id, const char* directory,
                                              bool* out_exist) {
    if (!directory || !out_exist) {
        return false;
    }
    if (!IsValidUsbSubpath(directory)) {
        *out_exist = false;
        return false;
    }
    const auto usb_root = ResolveUsbDeviceRoot(device_id);
    if (!usb_root) {
        *out_exist = false;
        return false;
    }
    std::error_code ec;
    const auto target = ResolveUsbSubpath(*usb_root, directory);
    *out_exist = std::filesystem::exists(target, ec);
    return true;
}

static s32 PS4_SYSV_ABI sceUsbStorageRequestMapWSB(u32 device_id, const char* directory,
                                                   s32 lock_type, u64 option_flags,
                                                   char* out_mount_path,
                                                   u64* out_additional_features) {
    if (!directory || !out_mount_path) {
        return ORBIS_AUTO_MOUNTER_ERROR_INVAL;
    }

    const auto usb_root = ResolveUsbDeviceRoot(device_id);
    if (!usb_root) {
        return ORBIS_AUTO_MOUNTER_ERROR_NO_DEVICE;
    }

    if (!IsValidUsbSubpath(directory)) {
        return ORBIS_AUTO_MOUNTER_ERROR_INVAL;
    }

    std::error_code ec;
    const auto host_target = ResolveUsbSubpath(*usb_root, directory);
    if (!std::filesystem::is_directory(host_target, ec)) {
        return ORBIS_AUTO_MOUNTER_ERROR_NOENT;
    }

    const std::string mount_point = BuildUsbMountPoint(device_id, directory, option_flags);
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    if (!mnt) {
        return ORBIS_AUTO_MOUNTER_ERROR_NOENT;
    }

    {
        std::scoped_lock lock(g_usb_state_mutex);
        const bool already_mapped =
            std::any_of(g_usb_maps.begin(), g_usb_maps.end(), [&](const UsbMapItem& item) {
                return item.device_id == device_id && item.directory == directory &&
                       item.mount_point == mount_point;
            });
        if (!already_mapped) {
            if (g_usb_maps.size() >= MAX_ACTIVE_USB_MAPS) {
                return ORBIS_AUTO_MOUNTER_ERROR_EXCEED_LIMIT;
            }
            g_usb_maps.push_back({device_id, directory, mount_point});
        }
    }

    mnt->Mount(host_target, mount_point, /*read_only=*/false);

    std::strncpy(out_mount_path, mount_point.c_str(), 127);
    out_mount_path[127] = '\0';
    if (out_additional_features) {
        *out_additional_features = ORBIS_USB_STORAGE_FEATURE_EXFAT;
    }

    LOG_INFO(Lib_Usbd, "mapped device_id {} subpath '{}' -> '{}' (host '{}')", device_id, directory,
             mount_point, host_target.string());
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageRequestMap(u32 device_id, const char* directory, s32 lock_type,
                                                u64 option_flags, char* out_mount_path,
                                                u64* out_additional_features) {
    return sceUsbStorageRequestMapWSB(device_id, directory, lock_type, option_flags, out_mount_path,
                                      out_additional_features);
}

static s32 PS4_SYSV_ABI sceUsbStorageRequestUnmap(u32 device_id, const char* directory) {
    if (!directory) {
        return ORBIS_AUTO_MOUNTER_ERROR_INVAL;
    }

    std::vector<std::string> removed_mounts;
    {
        std::scoped_lock lock(g_usb_state_mutex);
        for (auto it = g_usb_maps.begin(); it != g_usb_maps.end();) {
            if (it->device_id == device_id && it->directory == directory) {
                removed_mounts.push_back(it->mount_point);
                it = g_usb_maps.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance()) {
        for (const auto& mp : removed_mounts) {
            mnt->Unmount(mp);
        }
    }

    LOG_INFO(Lib_Usbd, "unmapped device_id {}, directory '{}'", device_id, directory);
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageGetdentsOpen(u32 device_id, s32* out_fd, u64* out_bufsize) {
    if (!out_fd || !out_bufsize) {
        LOG_ERROR(Lib_Usbd, "null pointer parameter");
        return ORBIS_AUTO_MOUNTER_ERROR_INVAL;
    }
    if (device_id != 0) {
        LOG_ERROR(Lib_Usbd, "invalid device_id {}", device_id);
        return ORBIS_AUTO_MOUNTER_ERROR_NOENT;
    }

    const auto mount_usb_dir = GetUsbMountPath(device_id);
    std::error_code ec;
    if (!std::filesystem::is_directory(mount_usb_dir, ec)) {
        return ORBIS_AUTO_MOUNTER_ERROR_NO_DEVICE;
    }

    UsbGetdentsState state;
    for (const auto& entry : std::filesystem::directory_iterator(mount_usb_dir, ec)) {
        const auto filename = entry.path().filename().string();
        if (filename.empty() || filename[0] == '.') {
            continue;
        }
        const bool is_dir = entry.is_directory(ec);
        state.items.push_back({filename, is_dir});
    }

    std::scoped_lock lock(g_usb_getdents_mutex);
    const s32 fd = g_usb_next_fd++;
    const size_t count = state.items.size();
    g_usb_getdents_table[fd] = std::move(state);

    *out_fd = fd;
    *out_bufsize = 0x8000; // 32 KB buffer

    LOG_INFO(Lib_Usbd, "opened getdents fd = {} for device {}, found {} entries", fd, device_id,
             count);
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageGetdentsRead(s32 fd, void* buffer, u64 bufsize,
                                                  u64* out_bytes_read) {
    if (!buffer || !out_bytes_read) {
        LOG_ERROR(Lib_Usbd, "null buffer or out_bytes_read");
        return ORBIS_AUTO_MOUNTER_ERROR_INVAL;
    }

    std::scoped_lock lock(g_usb_getdents_mutex);
    auto it = g_usb_getdents_table.find(fd);
    if (it == g_usb_getdents_table.end()) {
        LOG_ERROR(Lib_Usbd, "invalid fd {}", fd);
        *out_bytes_read = 0;
        return ORBIS_AUTO_MOUNTER_ERROR_NOTSUP;
    }

    auto& state = it->second;
    u64 bytes_written = 0;

    while (state.current_index < state.items.size()) {
        const auto& item = state.items[state.current_index];
        const u16 namlen = static_cast<u16>(std::min<size_t>(item.name.size(), 255));
        const u16 reclen = static_cast<u16>(Common::AlignUp<size_t>(8 + namlen + 1, 8));

        if (bytes_written + reclen > bufsize) {
            break;
        }

        auto* entry =
            reinterpret_cast<OrbisUsbStorageDirent*>(static_cast<u8*>(buffer) + bytes_written);
        std::memset(entry, 0, reclen);
        entry->d_fileno = static_cast<u32>(state.current_index + 1);
        entry->d_reclen = reclen;
        entry->d_type = item.is_directory ? 4 : 8; // 4 = DT_DIR, 8 = DT_REG
        entry->d_namlen = static_cast<u8>(namlen);
        std::memcpy(entry->d_name, item.name.data(), namlen);
        entry->d_name[namlen] = '\0';

        LOG_TRACE(Lib_Usbd, "entry: '{}', type = {}", item.name, entry->d_type);

        bytes_written += reclen;
        state.current_index++;
    }

    *out_bytes_read = bytes_written;
    LOG_DEBUG(Lib_Usbd, "fd = {}, returned {} bytes ({}/{} entries)", fd, bytes_written,
              state.current_index, state.items.size());
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageGetdentsClose(s32 fd) {
    std::scoped_lock lock(g_usb_getdents_mutex);
    g_usb_getdents_table.erase(fd);
    LOG_DEBUG(Lib_Usbd, "closed fd = {}", fd);
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("BDDZwF5kuTc", "libSceUsbStorage", 1, "libSceUsbStorage", sceUsbStorageInit);
    LIB_FUNCTION("Wp8zHTocS5E", "libSceUsbStorage", 1, "libSceUsbStorage", sceUsbStorageTerm);
    LIB_FUNCTION("vFkdkzJgSpw", "libSceUsbStorage", 1, "libSceUsbStorage",
                 sceUsbStorageRegisterCallback);
    LIB_FUNCTION("+Ib-MHNUf80", "libSceUsbStorage", 1, "libSceUsbStorage",
                 sceUsbStorageUnregisterCallback);
    LIB_FUNCTION("IDYJZSeBgDs", "libSceUsbStorage", 1, "libSceUsbStorage", sceUsbStorageRequestMap);
    LIB_FUNCTION("rx7EcAS2ARk", "libSceUsbStorage", 1, "libSceUsbStorage",
                 sceUsbStorageRequestMapWSB);
    LIB_FUNCTION("rx7EcAS2ARk", "libSceUsbStorageAux", 1, "libSceUsbStorage",
                 sceUsbStorageRequestMapWSB);
    LIB_FUNCTION("fl3roYs7F9U", "libSceUsbStorage", 1, "libSceUsbStorage",
                 sceUsbStorageRequestUnmap);
    LIB_FUNCTION("mryrNITeYvI", "libSceUsbStorage", 1, "libSceUsbStorage",
                 sceUsbStorageGetDeviceList);
    LIB_FUNCTION("-GvBqz54ssU", "libSceUsbStorage", 1, "libSceUsbStorage",
                 sceUsbStorageGetDeviceInfo);
    LIB_FUNCTION("tO8DvyElInw", "libSceUsbStorage", 1, "libSceUsbStorage", sceUsbStorageIsExist);
    LIB_FUNCTION("0rG6xtn7N5Q", "libSceUsbStorage", 1, "libSceUsbStorage",
                 sceUsbStorageRegisterCallbackForMapAvailable);
    LIB_FUNCTION("C3ETNYXsht4", "libSceUsbStorage", 1, "libSceUsbStorage",
                 sceUsbStorageUnregisterCallbackForMapAvailable);
    LIB_FUNCTION("8mpZuu7xfbM", "libSceUsbStorage", 1, "libSceUsbStorage",
                 sceUsbStorageRegisterCallbackForMapUnavailable);
    LIB_FUNCTION("4YMBk1lfUm0", "libSceUsbStorage", 1, "libSceUsbStorage",
                 sceUsbStorageUnregisterCallbackForMapUnavailable);
    LIB_FUNCTION("2LrlpFmBTC8", "libSceUsbStorage", 1, "libSceUsbStorage",
                 sceUsbStorageGetdentsOpen);
    LIB_FUNCTION("2LrlpFmBTC8", "libSceUsbStorageAux", 1, "libSceUsbStorage",
                 sceUsbStorageGetdentsOpen);
    LIB_FUNCTION("OeJdPEmLYX4", "libSceUsbStorage", 1, "libSceUsbStorage",
                 sceUsbStorageGetdentsRead);
    LIB_FUNCTION("OeJdPEmLYX4", "libSceUsbStorageAux", 1, "libSceUsbStorage",
                 sceUsbStorageGetdentsRead);
    LIB_FUNCTION("w1mZOJCxvhQ", "libSceUsbStorage", 1, "libSceUsbStorage",
                 sceUsbStorageGetdentsClose);
    LIB_FUNCTION("w1mZOJCxvhQ", "libSceUsbStorageAux", 1, "libSceUsbStorage",
                 sceUsbStorageGetdentsClose);
}

} // namespace Libraries::UsbStorage
