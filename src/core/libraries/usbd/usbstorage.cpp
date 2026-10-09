// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "common/alignment.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "usbstorage.h"

namespace Libraries::UsbStorage {

struct SceUsbStorageDeviceInfo {
    u32 device_id;          // +0x00
    u8 pad0[12];            // +0x04
    u16 unk10;              // +0x10
    u16 unk12;              // +0x12
    u16 unk14;              // +0x14
    char vendor[256];       // +0x16
    u64 capacity;           // +0x118
    char product[256];      // +0x120
    u64 status;             // +0x220
    char mount_path[256];   // +0x228
    u64 ready;              // +0x328
    u8 pad1[0x360 - 0x330]; // up to 0x360
};
static_assert(offsetof(SceUsbStorageDeviceInfo, vendor) == 0x16);
static_assert(offsetof(SceUsbStorageDeviceInfo, capacity) == 0x118);
static_assert(offsetof(SceUsbStorageDeviceInfo, product) == 0x120);
static_assert(offsetof(SceUsbStorageDeviceInfo, status) == 0x220);
static_assert(offsetof(SceUsbStorageDeviceInfo, mount_path) == 0x228);
static_assert(offsetof(SceUsbStorageDeviceInfo, ready) == 0x328);

static s32 PS4_SYSV_ABI sceUsbStorageInit() {
    LOG_INFO(Lib_Usbd, "sceUsbStorageInit called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageTerm() {
    LOG_INFO(Lib_Usbd, "sceUsbStorageTerm called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageRegisterCallback(s32 event_type, void* callback, void* arg) {
    LOG_INFO(Lib_Usbd, "sceUsbStorageRegisterCallback called: event_type = {}", event_type);
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageUnregisterCallback(s32 event_type, void* callback) {
    LOG_INFO(Lib_Usbd, "sceUsbStorageUnregisterCallback called: event_type = {}", event_type);
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageRegisterCallbackForMapAvailable(u32 device_id, void* callback,
                                                                     const char* path, void* arg) {
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageUnregisterCallbackForMapAvailable(u32 device_id,
                                                                       void* callback) {
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageRegisterCallbackForMapUnavailable(u32 device_id,
                                                                       void* callback,
                                                                       const char* path,
                                                                       void* arg) {
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageUnregisterCallbackForMapUnavailable(u32 device_id,
                                                                         void* callback) {
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageGetDeviceList(u32* device_ids, u32* count) {
    LOG_INFO(Lib_Usbd, "sceUsbStorageGetDeviceList called");
    if (!count) {
        return 0x80f40002;
    }
    if (device_ids) {
        device_ids[0] = 0;
    }
    *count = 1;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageGetDeviceInfo(u32 device_id, SceUsbStorageDeviceInfo* info) {
    LOG_INFO(Lib_Usbd, "sceUsbStorageGetDeviceInfo called: device_id = {}", device_id);
    if (!info) {
        return 0x80f40002;
    }
    if (device_id != 0) {
        return 0x80f40004;
    }
    std::memset(info, 0, sizeof(SceUsbStorageDeviceInfo));
    info->device_id = 0;
    info->unk10 = 1;
    info->unk12 = 1;
    info->unk14 = 1;
    std::strncpy(info->vendor, "shadPS4", sizeof(info->vendor) - 1);
    info->capacity = 64ULL * 1024 * 1024 * 1024; // 64 GB
    std::strncpy(info->product, "Virtual USB Storage", sizeof(info->product) - 1);
    info->status = 1;
    std::strncpy(info->mount_path, "/mnt/usb0", sizeof(info->mount_path) - 1);
    info->ready = 1;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageIsExist(u32 device_id) {
    LOG_INFO(Lib_Usbd, "sceUsbStorageIsExist called: device_id = {}", device_id);
    return (device_id == 0) ? 1 : 0;
}

static s32 PS4_SYSV_ABI sceUsbStorageRequestMapWSB(u32 device_id, const char* subpath, s32 flags,
                                                   s32 unk, char* out_mount_path, s32* out_status) {
    LOG_INFO(Lib_Usbd,
             "sceUsbStorageRequestMapWSB called: device_id = {}, subpath = '{}', flags = {}",
             device_id, subpath ? subpath : "(null)", flags);

    std::string mount_path = "/mnt/usb" + std::to_string(device_id);
    if (subpath && subpath[0] != '\0') {
        std::string_view sp(subpath);
        if (sp.starts_with("/mnt/usb")) {
            mount_path = std::string(sp);
        } else {
            if (!sp.starts_with("/")) {
                mount_path += "/" + std::string(sp);
            } else {
                mount_path += std::string(sp);
            }
        }
    }

    if (out_mount_path) {
        std::strncpy(out_mount_path, mount_path.c_str(), 127);
        out_mount_path[127] = '\0';
        LOG_INFO(Lib_Usbd, "  out_mount_path = '{}'", out_mount_path);
    }
    if (out_status) {
        *out_status = 0;
    }

    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageRequestMap(u32 device_id, const char* subpath, s32 flags,
                                                s32 unk, char* out_mount_path, s32* out_status) {
    LOG_INFO(Lib_Usbd, "sceUsbStorageRequestMap called: device_id = {}, subpath = '{}'", device_id,
             subpath ? subpath : "(null)");
    return sceUsbStorageRequestMapWSB(device_id, subpath, flags, unk, out_mount_path, out_status);
}

static s32 PS4_SYSV_ABI sceUsbStorageRequestUnmap(u32 device_id, void* b) {
    LOG_INFO(Lib_Usbd, "sceUsbStorageRequestUnmap called: device_id = {}", device_id);
    return ORBIS_OK;
}

#pragma pack(push, 1)
struct SceUsbStorageDirent {
    u32 d_fileno;
    u16 d_reclen;
    u8 d_type;
    u8 d_namlen;
    char d_name[256];
};
#pragma pack(pop)
static_assert(offsetof(SceUsbStorageDirent, d_fileno) == 0);
static_assert(offsetof(SceUsbStorageDirent, d_reclen) == 4);
static_assert(offsetof(SceUsbStorageDirent, d_type) == 6);
static_assert(offsetof(SceUsbStorageDirent, d_namlen) == 7);
static_assert(offsetof(SceUsbStorageDirent, d_name) == 8);

struct UsbDirentItem {
    std::string name;
    bool is_directory;
};

struct UsbGetdentsState {
    std::vector<UsbDirentItem> items;
    size_t current_index = 0;
};

static std::mutex g_usb_getdents_mutex;
static std::unordered_map<s32, UsbGetdentsState> g_usb_getdents_table;
static s32 g_usb_next_fd = 1;

static s32 PS4_SYSV_ABI sceUsbStorageGetdentsOpen(u32 device_id, s32* out_fd, u64* out_bufsize) {
    LOG_INFO(Lib_Usbd, "sceUsbStorageGetdentsOpen called: device_id = {}", device_id);
    if (!out_fd || !out_bufsize) {
        return 0x80f40002;
    }

    const auto mount_usb_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "mnt" /
                               ("usb" + std::to_string(device_id));

    std::error_code ec;
    if (!std::filesystem::exists(mount_usb_dir, ec)) {
        std::filesystem::create_directories(mount_usb_dir, ec);
    }

    // Media Player on PS4 requires files to be inside directories (e.g. Music, Video).
    // Ensure Music and Video exist.
    const auto music_dir = mount_usb_dir / "Music";
    const auto video_dir = mount_usb_dir / "Video";
    const auto photo_dir = mount_usb_dir / "Photo";
    std::filesystem::create_directories(music_dir, ec);
    std::filesystem::create_directories(video_dir, ec);
    std::filesystem::create_directories(photo_dir, ec);

    for (const auto& entry : std::filesystem::directory_iterator(mount_usb_dir, ec)) {
        if (!entry.is_regular_file(ec))
            continue;
        const auto ext = entry.path().extension().string();
        std::string ext_lower;
        for (char c : ext)
            ext_lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (ext_lower == ".mp3" || ext_lower == ".aac" || ext_lower == ".flac" ||
            ext_lower == ".m4a") {
            const auto target = music_dir / entry.path().filename();
            if (!std::filesystem::exists(target, ec)) {
                std::filesystem::create_hard_link(entry.path(), target, ec);
                if (ec) {
                    ec.clear();
                    std::filesystem::create_symlink(entry.path(), target, ec);
                }
            }
        } else if (ext_lower == ".mp4" || ext_lower == ".mkv" || ext_lower == ".avi" ||
                   ext_lower == ".mov" || ext_lower == ".m2ts" || ext_lower == ".ts" ||
                   ext_lower == ".mpg" || ext_lower == ".mpeg" || ext_lower == ".m4v" ||
                   ext_lower == ".mts" || ext_lower == ".srt") {
            const auto target = video_dir / entry.path().filename();
            if (!std::filesystem::exists(target, ec)) {
                std::filesystem::create_hard_link(entry.path(), target, ec);
                if (ec) {
                    ec.clear();
                    std::filesystem::create_symlink(entry.path(), target, ec);
                }
            }
        } else if (ext_lower == ".jpg" || ext_lower == ".jpeg" || ext_lower == ".png" ||
                   ext_lower == ".bmp") {
            const auto target = photo_dir / entry.path().filename();
            if (!std::filesystem::exists(target, ec)) {
                std::filesystem::create_hard_link(entry.path(), target, ec);
                if (ec) {
                    ec.clear();
                    std::filesystem::create_symlink(entry.path(), target, ec);
                }
            }
        }
    }

    UsbGetdentsState state;
    for (const auto& entry : std::filesystem::directory_iterator(mount_usb_dir, ec)) {
        const auto filename = entry.path().filename().string();
        if (filename.empty() || filename[0] == '.') {
            continue;
        }
        bool is_dir = entry.is_directory(ec);
        state.items.push_back({filename, is_dir});
    }

    std::scoped_lock lock(g_usb_getdents_mutex);
    s32 fd = g_usb_next_fd++;
    g_usb_getdents_table[fd] = std::move(state);

    *out_fd = fd;
    *out_bufsize = 0x8000; // 32 KB buffer

    LOG_INFO(Lib_Usbd, "sceUsbStorageGetdentsOpen: opened fd = {}, found {} items", fd,
             g_usb_getdents_table[fd].items.size());
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageGetdentsRead(s32 fd, void* buffer, u64 bufsize,
                                                  u64* out_bytes_read) {
    LOG_INFO(Lib_Usbd, "sceUsbStorageGetdentsRead called: fd = {}, bufsize = {}", fd, bufsize);
    if (!buffer || !out_bytes_read) {
        return 0x80f40002;
    }

    std::scoped_lock lock(g_usb_getdents_mutex);
    auto it = g_usb_getdents_table.find(fd);
    if (it == g_usb_getdents_table.end()) {
        LOG_ERROR(Lib_Usbd, "sceUsbStorageGetdentsRead: invalid fd {}", fd);
        *out_bytes_read = 0;
        return ORBIS_OK;
    }

    auto& state = it->second;
    u64 bytes_written = 0;

    while (state.current_index < state.items.size()) {
        const auto& item = state.items[state.current_index];
        u16 namlen = static_cast<u16>(std::min<size_t>(item.name.size(), 255));
        u16 reclen = static_cast<u16>(Common::AlignUp<size_t>(8 + namlen + 1, 8));

        if (bytes_written + reclen > bufsize) {
            break;
        }

        auto* entry =
            reinterpret_cast<SceUsbStorageDirent*>(static_cast<u8*>(buffer) + bytes_written);
        std::memset(entry, 0, reclen);
        entry->d_fileno = static_cast<u32>(state.current_index + 1);
        entry->d_reclen = reclen;
        entry->d_type = item.is_directory ? 4 : 8; // 4 = DT_DIR, 8 = DT_REG
        entry->d_namlen = static_cast<u8>(namlen);
        std::memcpy(entry->d_name, item.name.data(), namlen);
        entry->d_name[namlen] = '\0';

        LOG_INFO(Lib_Usbd, "  dirent: '{}', type = {}", item.name, entry->d_type);

        bytes_written += reclen;
        state.current_index++;
    }

    *out_bytes_read = bytes_written;
    LOG_INFO(Lib_Usbd, "sceUsbStorageGetdentsRead: returned {} bytes, current_index = {}/{}",
             bytes_written, state.current_index, state.items.size());
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceUsbStorageGetdentsClose(s32 fd) {
    LOG_INFO(Lib_Usbd, "sceUsbStorageGetdentsClose called: fd = {}", fd);
    std::scoped_lock lock(g_usb_getdents_mutex);
    g_usb_getdents_table.erase(fd);
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("BaOKcng8g88", "libSceUsbStorage", 1, "libSceUsbStorage", sceUsbStorageInit);
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
