// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include "common/logging/log.h"
#include "common/types.h"
#include "core/libraries/libs.h"
#include "psm.h"

namespace Libraries::Psm {

static std::atomic<bool> s_psm_running{true};

struct EventListener {
    int event_id;
    void (*callback)(const void* event_data, void* user_data);
    void* user_data;
};

static std::mutex s_event_mutex;
static std::vector<EventListener> s_listeners;
static std::vector<void (*)(const void*, void*)> s_delegators;

static void PS4_SYSV_ABI PsmInitParamC1(void* param) {
    LOG_INFO(Lib, "PsmInitParam::PsmInitParam called");
    if (param) {
        std::memset(param, 0, 0x100);
    }
}

static s32 PS4_SYSV_ABI PsmFrameworkInitialize(const void* /*param*/, int argc, char** /*argv*/) {
    LOG_INFO(Lib, "PsmFramework::Initialize called (argc = {})", argc);
    return 0;
}

static s32 PS4_SYSV_ABI PsmFrameworkRegisterPInvokeCallTable(const void* /*entry*/) {
    LOG_INFO(Lib, "PsmFramework::RegisterPInvokeCallTable called");
    return 0;
}

static s32 PS4_SYSV_ABI PsmFrameworkRegisterInternalCall(const void* /*entry*/) {
    LOG_INFO(Lib, "PsmFramework::RegisterInternalCall called");
    return 0;
}

static s32 PS4_SYSV_ABI InitializePsmFrameworkCsharp() {
    LOG_INFO(Lib, "InitializePsmFrameworkCsharp called");
    return 0;
}

static void PS4_SYSV_ABI SetEnableNativeMethod(bool enable) {
    LOG_INFO(Lib, "PsmFrameworkCsharp::SetEnableNativeMethod called (enable = {})", enable);
}

static s32 PS4_SYSV_ABI TerminatePsmFrameworkCsharp() {
    LOG_INFO(Lib, "TerminatePsmFrameworkCsharp called");
    s_psm_running.store(false);
    return 0;
}

static s32 PS4_SYSV_ABI PsmFrameworkFinalize() {
    LOG_INFO(Lib, "PsmFramework::Finalize called");
    s_psm_running.store(false);
    return 0;
}

static s32 PS4_SYSV_ABI PsmUnknownInit() {
    return 0;
}

static s32 PS4_SYSV_ABI PsmFrameworkRun() {
    LOG_INFO(Lib, "PsmFramework::Run called - entering persistent application event loop");
    s_psm_running.store(true);
    while (s_psm_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    LOG_INFO(Lib, "PsmFramework::Run exiting event loop");
    return 0;
}

// PsmEvent implementation
static s32 PS4_SYSV_ABI PsmEventInitialize() {
    LOG_INFO(Lib, "PsmEvent::Initialize called");
    return 0;
}

static s32 PS4_SYSV_ABI PsmEventFinalize() {
    LOG_INFO(Lib, "PsmEvent::Finalize called");
    return 0;
}

static s32 PS4_SYSV_ABI InitializePsmEventCsharp() {
    LOG_INFO(Lib, "InitializePsmEventCsharp called");
    return 0;
}

static s32 PS4_SYSV_ABI TerminatePsmEventCsharp() {
    LOG_INFO(Lib, "TerminatePsmEventCsharp called");
    return 0;
}

static s32 PS4_SYSV_ABI PsmEventRegister(int event_id, void (*callback)(const void*, void*),
                                        void* user_data) {
    LOG_INFO(Lib, "PsmEvent::Register called: event_id=0x{:x} ({}), callback={:p}, user_data={:p}",
             event_id, event_id, reinterpret_cast<void*>(callback), user_data);
    if (!callback) {
        return 0;
    }
    std::lock_guard lock(s_event_mutex);
    s_listeners.push_back({event_id, callback, user_data});
    return 0;
}

static s32 PS4_SYSV_ABI PsmEventUnregister(int event_id, void (*callback)(const void*, void*),
                                          void* user_data) {
    LOG_INFO(Lib, "PsmEvent::Unregister called: event_id=0x{:x}", event_id);
    std::lock_guard lock(s_event_mutex);
    std::erase_if(s_listeners, [&](const EventListener& l) {
        return l.event_id == event_id && l.callback == callback && l.user_data == user_data;
    });
    return 0;
}

static s32 PS4_SYSV_ABI PsmEventRegisterDelegator(void (*delegator)(const void*, void*),
                                                 void* user_data) {
    LOG_INFO(Lib, "PsmEvent::RegisterDelegator called");
    if (!delegator) {
        return 0;
    }
    std::lock_guard lock(s_event_mutex);
    s_delegators.push_back(delegator);
    return 0;
}

static s32 PS4_SYSV_ABI PsmEventUnregisterDelegator(void (*delegator)(const void*, void*),
                                                   void* user_data) {
    LOG_INFO(Lib, "PsmEvent::UnregisterDelegator called");
    std::lock_guard lock(s_event_mutex);
    std::erase_if(s_delegators, [&](auto d) { return d == delegator; });
    return 0;
}

static s32 PS4_SYSV_ABI PsmEventPost(const void* event_data) {
    if (!event_data) {
        return 0;
    }
    int event_id = *reinterpret_cast<const int*>(event_data);
    LOG_INFO(Lib, "PsmEvent::Post called: event_id=0x{:x} ({})", event_id, event_id);

    std::vector<EventListener> listeners_to_call;
    {
        std::lock_guard lock(s_event_mutex);
        for (const auto& l : s_listeners) {
            if (l.event_id == event_id || l.event_id == -1) {
                listeners_to_call.push_back(l);
            }
        }
    }
    for (const auto& l : listeners_to_call) {
        l.callback(event_data, l.user_data);
    }
    return 0;
}

static s32 PS4_SYSV_ABI PsmEventSend(const void* event_data) {
    return PsmEventPost(event_data);
}

static s32 PS4_SYSV_ABI PsmEventDataSetValue(void* /*self*/, const void* /*json_val*/) {
    return 0;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("FqHN0elWA6E", "libScePsm", 1, "libScePsm", PsmInitParamC1);
    LIB_FUNCTION("lWlBrUu77Kg", "libScePsm", 1, "libScePsm", PsmFrameworkInitialize);
    LIB_FUNCTION("MEuF5zm-r4o", "libScePsm", 1, "libScePsm", PsmFrameworkRegisterPInvokeCallTable);
    LIB_FUNCTION("vNcLSBfLtbk", "libScePsm", 1, "libScePsm", PsmFrameworkRegisterInternalCall);
    LIB_FUNCTION("fFD0QoqKajY", "libScePsm", 1, "libScePsm", InitializePsmFrameworkCsharp);
    LIB_FUNCTION("2eLBy9mcYx4", "libScePsm", 1, "libScePsm", SetEnableNativeMethod);
    LIB_FUNCTION("zEJf7JZDmrM", "libScePsm", 1, "libScePsm", TerminatePsmFrameworkCsharp);
    LIB_FUNCTION("6hSAbrbC3sE", "libScePsm", 1, "libScePsm", PsmFrameworkFinalize);
    LIB_FUNCTION("W8PzsJgkCGU", "libScePsm", 1, "libScePsm", PsmUnknownInit);
    LIB_FUNCTION("co1TwYJ2ybU", "libScePsm", 1, "libScePsm", PsmFrameworkRun);

    // PsmEvent functions
    LIB_FUNCTION("r24IV1PYa1g", "libScePsm", 1, "libScePsm", PsmEventInitialize);
    LIB_FUNCTION("MgGf6CnqHZc", "libScePsm", 1, "libScePsm", PsmEventFinalize);
    LIB_FUNCTION("TGsos8ZCV8c", "libScePsm", 1, "libScePsm", InitializePsmEventCsharp);
    LIB_FUNCTION("MUyZpNLfV3I", "libScePsm", 1, "libScePsm", TerminatePsmEventCsharp);
    LIB_FUNCTION("m3YkcPqYBzQ", "libScePsm", 1, "libScePsm", PsmEventRegister);
    LIB_FUNCTION("cLwb9r8x0+8", "libScePsm", 1, "libScePsm", PsmEventUnregister);
    LIB_FUNCTION("I14Gu-NUIUY", "libScePsm", 1, "libScePsm", PsmEventRegisterDelegator);
    LIB_FUNCTION("yvp6fh6kF6E", "libScePsm", 1, "libScePsm", PsmEventUnregisterDelegator);
    LIB_FUNCTION("7W6jR0LyiSg", "libScePsm", 1, "libScePsm", PsmEventPost);
    LIB_FUNCTION("xmqLf-a8vYY", "libScePsm", 1, "libScePsm", PsmEventSend);
    LIB_FUNCTION("JMA7QKrZvjc", "libScePsm", 1, "libScePsm", PsmEventDataSetValue);
}

} // namespace Libraries::Psm
