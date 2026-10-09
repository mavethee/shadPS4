// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include "common/logging/log.h"
#include "common/types.h"
#include "core/libraries/libs.h"
#include "ipmi.h"

namespace Libraries::Ipmi {

// Vtable layout expected by guest callers of IPMI::Client:
struct ClientVtable {
    void* destroy;            // 0x00: destroy()
    void* getUserData;        // 0x08: getUserData()
    void* connect;            // 0x10: connect(const void*, size_t, int*)
    void* disconnect;         // 0x18: disconnect()
    void* pollEventFlag;      // 0x20: pollEventFlag(uint, ulong, uint, ulong*)
    void* waitEventFlag;      // 0x28: waitEventFlag(uint, ulong, uint, ulong*, uint*)
    void* getMsg;             // 0x30: getMsg(uint, void*, ulong*, ulong, uint*)
    void* tryGetMsg;          // 0x38: tryGetMsg(uint, void*, ulong*, ulong)
    void* invokeSyncMethod;   // 0x40: invokeSyncMethod(...)
    void* invokeAsyncMethod;  // 0x48: invokeAsyncMethod(...)
    void* tryGetResult;       // 0x50: tryGetResult(...)
    void* invokeSyncMethod2;  // 0x58: invokeSyncMethod(...)
    void* invokeAsyncMethod2; // 0x60: invokeAsyncMethod(...)
    void* tryGetResult2;      // 0x68: tryGetResult(...)
    void* getSessionPid;      // 0x70: getSessionPid(uint*)
    void* terminateConnection;// 0x78: terminateConnection()
};

// Vtable layout expected by guest callers of IPMI::Server:
struct ServerVtable {
    void* destroy;            // 0x00: destroy()
    void* getUserData;        // 0x08: getUserData()
    void* runDispatcher;      // 0x10: runDispatcher(void*, size_t)
    void* tryDispatch;        // 0x18: tryDispatch(void*, size_t)
    void* shutdownDispatcher; // 0x20: shutdownDispatcher()
    void* createSession;      // 0x28: createSession(Session**, void*, void*)
};

// Vtable layout expected by guest callers of IPMI::Session:
struct SessionVtable {
    void* dtor1;              // 0x00: ~Session()
    void* dtor0;              // 0x08: ~Session() delete
    void* destroy;            // 0x10: destroy()
    void* getClientPid;       // 0x18: getClientPid()
    void* getUserData;        // 0x20: getUserData()
    void* getServer;          // 0x28: getServer()
    void* respondSync1;       // 0x30: respondToSyncMethodRequest(i32, BufferInfo*, u32)
    void* respondAsync1;      // 0x38: respondToAsyncMethodRequest(u32, u32, i32, DataInfo*, u32)
    void* setEventFlag;       // 0x40: setEventFlag(u32, u64)
    void* sendMsg;            // 0x48: sendMsg(u32, void*, u64, u32*)
    void* trySendMsg;         // 0x50: trySendMsg(u32, void*, u64)
    void* emptyMsgQueue;      // 0x58: emptyMsgQueue(u32)
    void* respondSync2;       // 0x60: respondToSyncMethodRequest(i32, void*, u64)
    void* respondAsync2;      // 0x68: respondToAsyncMethodRequest(u32, i32, void*, u64)
    void* isPeerPrivileged;   // 0x70: isPeerPrivileged()
    void* getPeerAppId;       // 0x78: getPeerAppId()
    void* getMsg;             // 0x80: getMsg(...)
    void* tryGetMsg;          // 0x88: tryGetMsg(...)
};

static std::atomic<bool> s_dispatcher_running{true};

// Client callbacks
static s32 PS4_SYSV_ABI DummyClientDestroy(void* /*self*/) {
    return 0;
}

static void* PS4_SYSV_ABI DummyClientGetUserData(void* /*self*/) {
    return nullptr;
}

static s32 PS4_SYSV_ABI DummyClientConnect(void* /*self*/, const void* name, size_t /*name_len*/,
                                           int* out_session) {
    if (name) {
        LOG_INFO(Lib, "IPMI::Client::connect called for '{}'", reinterpret_cast<const char*>(name));
    } else {
        LOG_INFO(Lib, "IPMI::Client::connect called");
    }
    if (out_session) {
        *out_session = 1;
    }
    return 0;
}

static s32 PS4_SYSV_ABI DummyClientDisconnect(void* /*self*/) {
    return 0;
}

static s32 PS4_SYSV_ABI DummyClientTerminateConnection(void* /*self*/) {
    return 0;
}

static s32 PS4_SYSV_ABI DummyClientPollEventFlag(void* /*self*/, u32, u64, u32, u64* out_bits) {
    if (out_bits) {
        *out_bits = 0;
    }
    return 0;
}

static s32 PS4_SYSV_ABI DummyClientWaitEventFlag(void* /*self*/, u32, u64, u32, u64* out_bits,
                                                 u32* out_flags) {
    if (out_bits) {
        *out_bits = 0;
    }
    if (out_flags) {
        *out_flags = 0;
    }
    return 0;
}

static s32 PS4_SYSV_ABI DummyClientGetMsg(void* /*self*/, u32, void*, u64* out_size, u64,
                                          u32* out_msg_id) {
    if (out_size) {
        *out_size = 0;
    }
    if (out_msg_id) {
        *out_msg_id = 0;
    }
    return 0;
}

static s32 PS4_SYSV_ABI DummyClientTryGetMsg(void* /*self*/, u32, void*, u64* out_size, u64) {
    if (out_size) {
        *out_size = 0;
    }
    return 0;
}

static s32 PS4_SYSV_ABI DummyClientInvokeSyncMethod(void* /*self*/, u32 method_id, const void*,
                                                    size_t, int* out_ret, void*,
                                                    u64* out_data_size, size_t) {
    LOG_INFO(Lib, "IPMI::Client::invokeSyncMethod called: method_id={}", method_id);
    if (out_ret) {
        *out_ret = 0;
    }
    if (out_data_size) {
        *out_data_size = 0;
    }
    return 0;
}

static s32 PS4_SYSV_ABI DummyClientInvokeAsyncMethod(void* /*self*/, u32 method_id, const void*,
                                                     size_t, u32* out_seq, const void*) {
    LOG_INFO(Lib, "IPMI::Client::invokeAsyncMethod called: method_id={}", method_id);
    if (out_seq) {
        *out_seq = 1;
    }
    return 0;
}

static s32 PS4_SYSV_ABI DummyClientTryGetResult(void* /*self*/, u32, int* out_ret, void*,
                                                u64* out_data_size, size_t) {
    if (out_ret) {
        *out_ret = 0;
    }
    if (out_data_size) {
        *out_data_size = 0;
    }
    return 0;
}

static s32 PS4_SYSV_ABI DummyClientInvokeSyncMethod2(void* /*self*/, u32 method_id, const void*,
                                                     u32, int* out_ret, void*, u32) {
    LOG_INFO(Lib, "IPMI::Client::invokeSyncMethod2 called: method_id={}", method_id);
    if (out_ret) {
        *out_ret = 0;
    }
    return 0;
}

static s32 PS4_SYSV_ABI DummyClientInvokeAsyncMethod2(void* /*self*/, u32 method_id, const void*,
                                                      u32, u32* out_seq, const void*) {
    LOG_INFO(Lib, "IPMI::Client::invokeAsyncMethod2 called: method_id={}", method_id);
    if (out_seq) {
        *out_seq = 1;
    }
    return 0;
}

static s32 PS4_SYSV_ABI DummyClientTryGetResult2(void* /*self*/, u32, u32, int* out_ret, void*,
                                                 u32) {
    if (out_ret) {
        *out_ret = 0;
    }
    return 0;
}

static s32 PS4_SYSV_ABI DummyClientGetSessionPid(void* /*self*/, u32* out_pid) {
    if (out_pid) {
        *out_pid = 1;
    }
    return 0;
}

// Session callbacks
static s32 PS4_SYSV_ABI DummySessionDestroy(void* /*self*/) {
    return 0;
}

static s32 PS4_SYSV_ABI DummySessionGetClientPid(void* /*self*/) {
    return 1;
}

static void* PS4_SYSV_ABI DummySessionGetUserData(void* /*self*/) {
    return nullptr;
}

static void* PS4_SYSV_ABI DummySessionGetServer(void* /*self*/) {
    return nullptr;
}

static s32 PS4_SYSV_ABI DummySessionRespondSync(void* /*self*/, s32, const void*, u64) {
    return 0;
}

static s32 PS4_SYSV_ABI DummySessionRespondAsync(void* /*self*/, u32, s32, const void*, u64) {
    return 0;
}

static s32 PS4_SYSV_ABI DummySessionSetEventFlag(void* /*self*/, u32, u64) {
    return 0;
}

static s32 PS4_SYSV_ABI DummySessionSendMsg(void* /*self*/, u32, const void*, u64, u32* out_seq) {
    if (out_seq) {
        *out_seq = 1;
    }
    return 0;
}

static s32 PS4_SYSV_ABI DummySessionTrySendMsg(void* /*self*/, u32, const void*, u64) {
    return 0;
}

static s32 PS4_SYSV_ABI DummySessionEmptyMsgQueue(void* /*self*/, u32) {
    return 0;
}

static s32 PS4_SYSV_ABI DummySessionIsPeerPrivileged(void* /*self*/) {
    return 1;
}

static s32 PS4_SYSV_ABI DummySessionGetPeerAppId(void* /*self*/) {
    return 1;
}

static s32 PS4_SYSV_ABI DummySessionGetMsg(void* /*self*/, u32, void*, u64* out_size, u64,
                                           u32* out_seq) {
    if (out_size) {
        *out_size = 0;
    }
    if (out_seq) {
        *out_seq = 0;
    }
    return 0;
}

static s32 PS4_SYSV_ABI DummySessionTryGetMsg(void* /*self*/, u32, void*, u64* out_size, u64) {
    if (out_size) {
        *out_size = 0;
    }
    return 0;
}

// Server callbacks
static s32 PS4_SYSV_ABI DummyServerDestroy(void* /*self*/) {
    return 0;
}

static void* PS4_SYSV_ABI DummyServerGetUserData(void* /*self*/) {
    return nullptr;
}

static s32 PS4_SYSV_ABI DummyServerRunDispatcher(void* /*self*/, void*, size_t) {
    LOG_INFO(Lib, "IPMI::Server::runDispatcher started");
    while (s_dispatcher_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    LOG_INFO(Lib, "IPMI::Server::runDispatcher ended");
    return 0;
}

static s32 PS4_SYSV_ABI DummyServerTryDispatch(void* /*self*/, void*, size_t) {
    return 0;
}

static s32 PS4_SYSV_ABI DummyServerShutdownDispatcher(void* /*self*/) {
    LOG_INFO(Lib, "IPMI::Server::shutdownDispatcher called");
    s_dispatcher_running.store(false);
    return 0;
}

static const SessionVtable s_dummy_session_vtable = {
    reinterpret_cast<void*>(&DummySessionDestroy),
    reinterpret_cast<void*>(&DummySessionDestroy),
    reinterpret_cast<void*>(&DummySessionDestroy),
    reinterpret_cast<void*>(&DummySessionGetClientPid),
    reinterpret_cast<void*>(&DummySessionGetUserData),
    reinterpret_cast<void*>(&DummySessionGetServer),
    reinterpret_cast<void*>(&DummySessionRespondSync),
    reinterpret_cast<void*>(&DummySessionRespondAsync),
    reinterpret_cast<void*>(&DummySessionSetEventFlag),
    reinterpret_cast<void*>(&DummySessionSendMsg),
    reinterpret_cast<void*>(&DummySessionTrySendMsg),
    reinterpret_cast<void*>(&DummySessionEmptyMsgQueue),
    reinterpret_cast<void*>(&DummySessionRespondSync),
    reinterpret_cast<void*>(&DummySessionRespondAsync),
    reinterpret_cast<void*>(&DummySessionIsPeerPrivileged),
    reinterpret_cast<void*>(&DummySessionGetPeerAppId),
    reinterpret_cast<void*>(&DummySessionGetMsg),
    reinterpret_cast<void*>(&DummySessionTryGetMsg),
};

static s32 PS4_SYSV_ABI DummyServerCreateSession(void* /*self*/, void** out_session,
                                                 void* /*work_buf*/, void* instance_mem) {
    if (out_session && instance_mem) {
        *reinterpret_cast<const SessionVtable**>(instance_mem) = &s_dummy_session_vtable;
        *reinterpret_cast<s32*>(reinterpret_cast<uintptr_t>(instance_mem) + 8) = 1;
        *out_session = instance_mem;
    }
    return 0;
}

static const ClientVtable s_dummy_client_vtable = {
    reinterpret_cast<void*>(&DummyClientDestroy),
    reinterpret_cast<void*>(&DummyClientGetUserData),
    reinterpret_cast<void*>(&DummyClientConnect),
    reinterpret_cast<void*>(&DummyClientDisconnect),
    reinterpret_cast<void*>(&DummyClientPollEventFlag),
    reinterpret_cast<void*>(&DummyClientWaitEventFlag),
    reinterpret_cast<void*>(&DummyClientGetMsg),
    reinterpret_cast<void*>(&DummyClientTryGetMsg),
    reinterpret_cast<void*>(&DummyClientInvokeSyncMethod),
    reinterpret_cast<void*>(&DummyClientInvokeAsyncMethod),
    reinterpret_cast<void*>(&DummyClientTryGetResult),
    reinterpret_cast<void*>(&DummyClientInvokeSyncMethod2),
    reinterpret_cast<void*>(&DummyClientInvokeAsyncMethod2),
    reinterpret_cast<void*>(&DummyClientTryGetResult2),
    reinterpret_cast<void*>(&DummyClientGetSessionPid),
    reinterpret_cast<void*>(&DummyClientTerminateConnection),
};

static const ServerVtable s_dummy_server_vtable = {
    reinterpret_cast<void*>(&DummyServerDestroy),
    reinterpret_cast<void*>(&DummyServerGetUserData),
    reinterpret_cast<void*>(&DummyServerRunDispatcher),
    reinterpret_cast<void*>(&DummyServerTryDispatch),
    reinterpret_cast<void*>(&DummyServerShutdownDispatcher),
    reinterpret_cast<void*>(&DummyServerCreateSession),
};

static void* PS4_SYSV_ABI IpmiClientImplC1(void* self) {
    if (self) {
        *reinterpret_cast<const ClientVtable**>(self) = &s_dummy_client_vtable;
        *reinterpret_cast<s32*>(reinterpret_cast<uintptr_t>(self) + 8) = -1;
    }
    return self;
}

static s32 PS4_SYSV_ABI IpmiClientImplInitialize(void* /*self*/, const void* /*config*/,
                                                 void* /*work_buf*/) {
    return 0;
}

// Client configs
static void PS4_SYSV_ABI IpmiClientConfigC1(void* config) {
    if (config) {
        std::memset(config, 0, 0x158);
    }
}

static void PS4_SYSV_ABI IpmiSyncMethodInvocationConfigC1(void* config) {
    if (config) {
        std::memset(config, 0, 0x18);
        *reinterpret_cast<u32*>(config) = 1;
    }
}

static void PS4_SYSV_ABI IpmiAsyncMethodInvocationConfigC1(void* config) {
    if (config) {
        std::memset(config, 0, 0x18);
        *reinterpret_cast<u32*>(config) = 1;
    }
}

static void PS4_SYSV_ABI IpmiMsgQueueConfigC1(void* config) {
    if (config) {
        std::memset(config, 0, 0x108);
    }
}

static u64 PS4_SYSV_ABI IpmiClientConfigEstimateClientMemorySize(void* /*config*/) {
    return 0x1000;
}

static u64 PS4_SYSV_ABI IpmiMsgQueueConfigEstimateMsgQueueSize(void* /*config*/, size_t, u32) {
    return 0x1000;
}

static s32 PS4_SYSV_ABI IpmiClientCreate(void** out_client, const void* /*config*/,
                                         void* /*work_buffer*/, void* instance_mem) {
    if (!out_client) {
        return 0x80020016; // SCE_KERNEL_ERROR_EINVAL
    }
    if (!instance_mem) {
        *out_client = nullptr;
        return 0x80020016;
    }
    *reinterpret_cast<const ClientVtable**>(instance_mem) = &s_dummy_client_vtable;
    *reinterpret_cast<s32*>(reinterpret_cast<uintptr_t>(instance_mem) + 8) = -1;
    *out_client = instance_mem;
    return 0;
}

// Server configs
static void PS4_SYSV_ABI IpmiServerConfigC1(void* config) {
    if (config) {
        std::memset(config, 0, 0x38);
        *reinterpret_cast<u64*>(reinterpret_cast<uintptr_t>(config) + 0x00) = 0x0000000000000f00;
        *reinterpret_cast<u64*>(reinterpret_cast<uintptr_t>(config) + 0x08) = 0x0000000000000f00;
    }
}

static u64 PS4_SYSV_ABI IpmiServerConfigEstimateTempWorkingMemorySize(void* /*config*/) {
    return 0x1000;
}

static void PS4_SYSV_ABI IpmiSessionConfigC1(void* config) {
    if (config) {
        std::memset(config, 0, 0x38);
    }
}

static u64 PS4_SYSV_ABI IpmiSessionConfigEstimateSessionMemorySize(void* /*config*/) {
    return 0x1000;
}

static s32 PS4_SYSV_ABI IpmiServerCreate(void** out_server, const void* /*config*/,
                                         void* /*work_buffer*/, void* instance_mem) {
    if (!out_server) {
        return 0x80020016;
    }
    if (!instance_mem) {
        *out_server = nullptr;
        return 0x80020016;
    }
    *reinterpret_cast<const ServerVtable**>(instance_mem) = &s_dummy_server_vtable;
    *reinterpret_cast<s32*>(reinterpret_cast<uintptr_t>(instance_mem) + 8) = -1;
    *out_server = instance_mem;
    return 0;
}

static s32 PS4_SYSV_ABI IpmiUnknownInit(u32 /*thread_id*/, u64 /*flags*/) {
    return 0;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    // Client configuration
    LIB_FUNCTION("O1lQ2+do5r4", "libSceIpmi", 1, "libSceIpmi", IpmiClientConfigC1);
    LIB_FUNCTION("C86F6UUBdFw", "libSceIpmi", 1, "libSceIpmi", IpmiClientConfigC1);
    LIB_FUNCTION("Rih7HnIM3JQ", "libSceIpmi", 1, "libSceIpmi",
                 IpmiClientConfigEstimateClientMemorySize);
    LIB_FUNCTION("yoVdeXZzPck", "libSceIpmi", 1, "libSceIpmi",
                 IpmiMsgQueueConfigEstimateMsgQueueSize);
    LIB_FUNCTION("ZHAqg71nFWw", "libSceIpmi", 1, "libSceIpmi",
                 IpmiSyncMethodInvocationConfigC1);
    LIB_FUNCTION("WxQ-nbumKpM", "libSceIpmi", 1, "libSceIpmi",
                 IpmiSyncMethodInvocationConfigC1);
    LIB_FUNCTION("TA4rU9ZXgFM", "libSceIpmi", 1, "libSceIpmi",
                 IpmiAsyncMethodInvocationConfigC1);
    LIB_FUNCTION("eNDYbSvB334", "libSceIpmi", 1, "libSceIpmi",
                 IpmiAsyncMethodInvocationConfigC1);
    LIB_FUNCTION("yFtfUBc5dsc", "libSceIpmi", 1, "libSceIpmi", IpmiMsgQueueConfigC1);
    LIB_FUNCTION("pbT-iQht85o", "libSceIpmi", 1, "libSceIpmi", IpmiMsgQueueConfigC1);

    // Client lifecycle & factory
    LIB_FUNCTION("jt25u2q3Nvw", "libSceIpmi", 1, "libSceIpmi", IpmiClientImplC1);
    LIB_FUNCTION("N0iXfw1xf1Q", "libSceIpmi", 1, "libSceIpmi", IpmiClientImplC1);
    LIB_FUNCTION("64umtbwiizI", "libSceIpmi", 1, "libSceIpmi", IpmiClientImplInitialize);
    LIB_FUNCTION("0zsTiDhM0nU", "libSceIpmi", 1, "libSceIpmi", IpmiClientCreate);
    LIB_FUNCTION("R9CB6TmSpeU", "libSceIpmi", 1, "libSceIpmi", DummyClientDestroy);
    LIB_FUNCTION("hQkeuEc0BMs", "libSceIpmi", 1, "libSceIpmi", DummyClientDestroy);
    LIB_FUNCTION("QLDcLR7Pzbg", "libSceIpmi", 1, "libSceIpmi", DummyClientDestroy);
    LIB_FUNCTION("9j0AkeNr+qk", "libSceIpmi", 1, "libSceIpmi", DummyClientDestroy);
    LIB_FUNCTION("H2HLRQerr+g", "libSceIpmi", 1, "libSceIpmi", DummyClientDestroy);
    LIB_FUNCTION("lxtXq6qmrC8", "libSceIpmi", 1, "libSceIpmi", DummyClientDestroy);
    LIB_FUNCTION("EsoUVUbGMdg", "libSceIpmi", 1, "libSceIpmi", DummyClientDestroy);

    // Client methods
    LIB_FUNCTION("vogma5QshU4", "libSceIpmi", 1, "libSceIpmi", DummyClientConnect);
    LIB_FUNCTION("nJthG9040U8", "libSceIpmi", 1, "libSceIpmi", DummyClientDisconnect);
    LIB_FUNCTION("cOli1uePSwY", "libSceIpmi", 1, "libSceIpmi", DummyClientTerminateConnection);
    LIB_FUNCTION("X8bM2L-G59E", "libSceIpmi", 1, "libSceIpmi", DummyClientGetUserData);
    LIB_FUNCTION("y29rp3N5qf4", "libSceIpmi", 1, "libSceIpmi", DummyClientPollEventFlag);
    LIB_FUNCTION("eVn6vS9O8EI", "libSceIpmi", 1, "libSceIpmi", DummyClientWaitEventFlag);
    LIB_FUNCTION("rU0UzNMZDIs", "libSceIpmi", 1, "libSceIpmi", DummyClientGetMsg);
    LIB_FUNCTION("k9ixmB0yrP4", "libSceIpmi", 1, "libSceIpmi", DummyClientTryGetMsg);
    LIB_FUNCTION("57XmD42P9gQ", "libSceIpmi", 1, "libSceIpmi", DummyClientInvokeSyncMethod);
    LIB_FUNCTION("lsbUGZCjQy8", "libSceIpmi", 1, "libSceIpmi", DummyClientInvokeSyncMethod2);
    LIB_FUNCTION("8kU11lW12ss", "libSceIpmi", 1, "libSceIpmi", DummyClientInvokeAsyncMethod);
    LIB_FUNCTION("FsiuQCKSE5Q", "libSceIpmi", 1, "libSceIpmi", DummyClientInvokeAsyncMethod2);
    LIB_FUNCTION("pLNZVjiFPKE", "libSceIpmi", 1, "libSceIpmi", DummyClientTryGetResult);
    LIB_FUNCTION("exZt85cnx08", "libSceIpmi", 1, "libSceIpmi", DummyClientTryGetResult2);

    // Server configuration & lifecycle
    LIB_FUNCTION("vo2FiTXAekk", "libSceIpmi", 1, "libSceIpmi", IpmiServerConfigC1);
    LIB_FUNCTION("u5IYJtq4D-4", "libSceIpmi", 1, "libSceIpmi", IpmiServerConfigC1);
    LIB_FUNCTION("OXc1BksfEIg", "libSceIpmi", 1, "libSceIpmi",
                 IpmiServerConfigEstimateTempWorkingMemorySize);
    LIB_FUNCTION("hoWSsRT0ktA", "libSceIpmi", 1, "libSceIpmi", IpmiServerCreate);
    LIB_FUNCTION("P72uwJlJjX4", "libSceIpmi", 1, "libSceIpmi", DummyServerDestroy);
    LIB_FUNCTION("bQOlflF2R5I", "libSceIpmi", 1, "libSceIpmi", DummyServerDestroy);
    LIB_FUNCTION("HkT3ygqjkCg", "libSceIpmi", 1, "libSceIpmi", DummyServerDestroy);
    LIB_FUNCTION("ysfNnf9JPS8", "libSceIpmi", 1, "libSceIpmi", DummyServerDestroy);
    LIB_FUNCTION("w-zWDsN+0bQ", "libSceIpmi", 1, "libSceIpmi", DummyServerDestroy);
    LIB_FUNCTION("Gzah1AMZDS8", "libSceIpmi", 1, "libSceIpmi", DummyServerDestroy);
    LIB_FUNCTION("dPOHeORqhbw", "libSceIpmi", 1, "libSceIpmi", DummyServerDestroy);
    LIB_FUNCTION("w9uM8jBkjhw", "libSceIpmi", 1, "libSceIpmi", DummyServerGetUserData);
    LIB_FUNCTION("P17jonIyIB8", "libSceIpmi", 1, "libSceIpmi", DummyServerRunDispatcher);
    LIB_FUNCTION("GIniJssLcts", "libSceIpmi", 1, "libSceIpmi", DummyServerTryDispatch);
    LIB_FUNCTION("e-OnyPe42tI", "libSceIpmi", 1, "libSceIpmi", DummyServerShutdownDispatcher);
    LIB_FUNCTION("O+Rk6bioKMY", "libSceIpmi", 1, "libSceIpmi", DummyServerCreateSession);

    // Session configuration & lifecycle
    LIB_FUNCTION("+0U0tMljl0I", "libSceIpmi", 1, "libSceIpmi",
                 IpmiSessionConfigEstimateSessionMemorySize);
    LIB_FUNCTION("q090kVFxvro", "libSceIpmi", 1, "libSceIpmi", IpmiSessionConfigC1);
    LIB_FUNCTION("QfbB4i5JCvk", "libSceIpmi", 1, "libSceIpmi", DummySessionDestroy);
    LIB_FUNCTION("AHNKaOCthAE", "libSceIpmi", 1, "libSceIpmi", DummySessionDestroy);
    LIB_FUNCTION("G2tb9MfBnFs", "libSceIpmi", 1, "libSceIpmi", DummySessionDestroy);
    LIB_FUNCTION("WWpgP5pqtZ0", "libSceIpmi", 1, "libSceIpmi", DummySessionDestroy);
    LIB_FUNCTION("pWkKuPgWHR8", "libSceIpmi", 1, "libSceIpmi", DummySessionDestroy);
    LIB_FUNCTION("YWOjuEzZ3Ic", "libSceIpmi", 1, "libSceIpmi", DummySessionDestroy);
    LIB_FUNCTION("vk71LUV2VVw", "libSceIpmi", 1, "libSceIpmi", DummySessionGetClientPid);
    LIB_FUNCTION("bNAnspllfsc", "libSceIpmi", 1, "libSceIpmi", DummySessionGetUserData);
    LIB_FUNCTION("dzgrvhCjExA", "libSceIpmi", 1, "libSceIpmi", DummySessionGetServer);
    LIB_FUNCTION("r0t1NxWUNwI", "libSceIpmi", 1, "libSceIpmi", DummySessionRespondSync);
    LIB_FUNCTION("AYyxvDy-SFU", "libSceIpmi", 1, "libSceIpmi", DummySessionRespondSync);
    LIB_FUNCTION("uqrbCvCIBMU", "libSceIpmi", 1, "libSceIpmi", DummySessionRespondAsync);
    LIB_FUNCTION("niK485vUNSQ", "libSceIpmi", 1, "libSceIpmi", DummySessionRespondAsync);
    LIB_FUNCTION("-S36x8s8CAw", "libSceIpmi", 1, "libSceIpmi", DummySessionSetEventFlag);
    LIB_FUNCTION("ptb571AzPy4", "libSceIpmi", 1, "libSceIpmi", DummySessionSendMsg);
    LIB_FUNCTION("32j2cyCl3Yo", "libSceIpmi", 1, "libSceIpmi", DummySessionTrySendMsg);
    LIB_FUNCTION("03U1u-RK3vg", "libSceIpmi", 1, "libSceIpmi", DummySessionEmptyMsgQueue);
    LIB_FUNCTION("TY5vzD-y6EA", "libSceIpmi", 1, "libSceIpmi", DummySessionIsPeerPrivileged);
    LIB_FUNCTION("nour4-O5aD0", "libSceIpmi", 1, "libSceIpmi", DummySessionGetMsg);
    LIB_FUNCTION("XMR5p-AA5AQ", "libSceIpmi", 1, "libSceIpmi", DummySessionTryGetMsg);

    // Miscellaneous
    LIB_FUNCTION("fjPNqzuUop8", "libSceIpmi", 1, "libSceIpmi", IpmiUnknownInit);
    LIB_FUNCTION("gL4bo5h2nwU", "libSceIpmi", 1, "libSceIpmi", IpmiUnknownInit);
    LIB_FUNCTION("rq3L3xKnt3Y", "libSceIpmi", 1, "libSceIpmi", IpmiUnknownInit);
}

} // namespace Libraries::Ipmi
