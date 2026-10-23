// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string_view>

#include "common/logging/log.h"
#include "common/types.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/ipmi/ipmi.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"

namespace Libraries::Ipmi {

constexpr u64 DefaultBufferSize = 3840;
constexpr u64 ServerWorkingMemoryOverhead = 0x100;
constexpr u64 ClientMemorySize = 0x1000;

struct DataInfo {
    const void* data = nullptr;
    u64 size = 0;
};

struct BufferInfo {
    void* buffer = nullptr;
    u64 buffer_size = 0;
    u64* out_size = nullptr;
};

struct EventNotifee {
    void* callback = nullptr;
    void* user_data = nullptr;
};

// ============================================================================
// Server & ServerConfig
// ============================================================================

struct ServerConfig {
    u64 buffer_sizes[2];
    void* event_handler;
    u8 flag_0x18;
    char name[25];
    u16 flag_0x32;
};
static_assert(offsetof(ServerConfig, event_handler) == 0x10);
static_assert(offsetof(ServerConfig, name) == 0x19);
static_assert(offsetof(ServerConfig, flag_0x32) == 0x32);
static_assert(sizeof(ServerConfig) == 0x38);

enum ServerState : u8 {
    Idle = 0,
    Running = 1 << 1,
    ShutdownRequested = 1 << 2,
};

struct ServerImpl {
    std::mutex mutex;
    std::condition_variable cv;
    u8 state = Idle;
    void* user_data = nullptr;
};

struct Server {
    void* const* vtable;
    ServerImpl* impl;
};

static s32 PS4_SYSV_ABI ServerDestroy(Server* server) {
    if (server == nullptr || server->impl == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    {
        std::scoped_lock lock{server->impl->mutex};
        if (server->impl->state != Idle) {
            return ORBIS_KERNEL_ERROR_EBUSY;
        }
    }
    delete server->impl;
    server->impl = nullptr;
    return ORBIS_OK;
}

static void* PS4_SYSV_ABI ServerGetUserData(Server* server) {
    return (server != nullptr && server->impl != nullptr) ? server->impl->user_data : nullptr;
}

static s32 PS4_SYSV_ABI ServerRunDispatcher(Server* server, void* memory, u64 size) {
    if (server == nullptr || server->impl == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    ServerImpl* impl = server->impl;
    std::unique_lock lock{impl->mutex};
    if (impl->state != Idle) {
        impl->state &= ~ShutdownRequested;
        return ORBIS_KERNEL_ERROR_EBUSY;
    }
    impl->state = Running;
    impl->cv.wait(lock, [impl] { return (impl->state & ShutdownRequested) != 0; });
    impl->state = Idle;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI ServerTryDispatch(Server* server, void* memory, u64 size) {
    if (server == nullptr || server->impl == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    ServerImpl* impl = server->impl;
    std::scoped_lock lock{impl->mutex};
    if (impl->state != Idle) {
        impl->state &= ~ShutdownRequested;
        return ORBIS_KERNEL_ERROR_EBUSY;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI ServerShutdownDispatcher(Server* server) {
    if (server == nullptr || server->impl == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    ServerImpl* impl = server->impl;
    std::scoped_lock lock{impl->mutex};
    if ((impl->state & ShutdownRequested) != 0) {
        return ORBIS_KERNEL_ERROR_EBUSY;
    }
    impl->state |= ShutdownRequested;
    impl->cv.notify_all();
    return ORBIS_OK;
}

struct Session;

static s32 PS4_SYSV_ABI ServerCreateSession(Server* server, Session** session, void* arg1,
                                            void* arg2);

static void PS4_SYSV_ABI ServerDestructor(Server* server) {
    if (server != nullptr) {
        delete server->impl;
        server->impl = nullptr;
    }
}

static void* const ServerVtable[] = {
    reinterpret_cast<void*>(&ServerDestroy),
    reinterpret_cast<void*>(&ServerGetUserData),
    reinterpret_cast<void*>(&ServerRunDispatcher),
    reinterpret_cast<void*>(&ServerTryDispatch),
    reinterpret_cast<void*>(&ServerShutdownDispatcher),
    reinterpret_cast<void*>(&ServerCreateSession),
    reinterpret_cast<void*>(&ServerDestructor),
    reinterpret_cast<void*>(&ServerDestructor),
};

static void PS4_SYSV_ABI ServerConfigConstructor(ServerConfig* config) {
    if (config == nullptr) {
        return;
    }
    config->buffer_sizes[0] = DefaultBufferSize;
    config->buffer_sizes[1] = DefaultBufferSize;
    config->event_handler = nullptr;
    config->flag_0x18 = 0;
    config->name[0] = '\0';
    config->flag_0x32 = 0;
}

static u64 PS4_SYSV_ABI ServerConfigEstimateTempWorkingMemorySize(const ServerConfig* config) {
    if (config == nullptr) {
        return ServerWorkingMemoryOverhead;
    }
    return std::max(config->buffer_sizes[0], config->buffer_sizes[1]) + ServerWorkingMemoryOverhead;
}

static void PS4_SYSV_ABI ServerConstructor(Server* server) {
    if (server != nullptr) {
        server->vtable = ServerVtable;
        server->impl = nullptr;
    }
}

static s32 PS4_SYSV_ABI ServerCreate(Server** out_server, const ServerConfig* config,
                                     void* user_data, void* memory) {
    if (out_server == nullptr || config == nullptr || memory == nullptr ||
        config->event_handler == nullptr || config->name[0] == '\0') {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    const std::string_view name{config->name, strnlen(config->name, sizeof(config->name))};
    LOG_INFO(Lib_Ipmi, "Creating server '{}'", name);

    auto* server = static_cast<Server*>(memory);
    server->vtable = ServerVtable;
    server->impl = new ServerImpl;
    server->impl->user_data = user_data;
    *out_server = server;
    return ORBIS_OK;
}

// EventHandler callbacks
static void PS4_SYSV_ABI EventHandlerOnSyncMethodDispatch(void*, Session*, u32, void*, u64, u64,
                                                          void*, u64) {}

static void PS4_SYSV_ABI EventHandlerOnSyncMethodDispatchDataInfo(void*, Session*, u32,
                                                                  const DataInfo*, u32, BufferInfo*,
                                                                  u32) {}

static void PS4_SYSV_ABI EventHandlerOnAsyncMethodDispatch(void*, Session*, u32, u32, void*, u64,
                                                           void*, u64) {}

static void PS4_SYSV_ABI EventHandlerOnAsyncMethodDispatchDataInfo(void*, Session*, u32, u32,
                                                                   const DataInfo*, u32) {}

static void PS4_SYSV_ABI EventHandlerOnSessionKilled(void*, Session*) {}

static void PS4_SYSV_ABI EventHandlerDestructor(void*) {}

static void* const EventHandlerVtable[] = {
    nullptr,
    nullptr,
    reinterpret_cast<void*>(&EventHandlerDestructor),
    reinterpret_cast<void*>(&EventHandlerDestructor),
    reinterpret_cast<void*>(&EventHandlerOnSyncMethodDispatchDataInfo),
    reinterpret_cast<void*>(&EventHandlerOnAsyncMethodDispatchDataInfo),
    reinterpret_cast<void*>(&EventHandlerOnSessionKilled),
};

static void* PS4_SYSV_ABI EventHandlerVtableSymbol() {
    return const_cast<void**>(EventHandlerVtable + 2);
}

// ============================================================================
// Session & SessionImpl
// ============================================================================

struct SessionImpl {
    Server* server = nullptr;
    void* user_data = nullptr;
    u32 client_pid = 0;
};

struct Session {
    void* const* vtable;
    SessionImpl* impl;
};

static void PS4_SYSV_ABI SessionImplDestructor(Session* session) {
    if (session != nullptr) {
        delete session->impl;
        session->impl = nullptr;
    }
}

static s32 PS4_SYSV_ABI SessionImplDestroy(Session* session) {
    if (session == nullptr || session->impl == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    delete session->impl;
    session->impl = nullptr;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI SessionImplTryDispatch(Session* session, void* data, u64 size) {
    return ORBIS_OK;
}

static u32 PS4_SYSV_ABI SessionImplGetClientPid(Session* session) {
    if (session != nullptr && session->impl != nullptr) {
        return session->impl->client_pid;
    }
    return 0;
}

static u32 PS4_SYSV_ABI SessionImplGetClientKid(Session* session) {
    return 0;
}

static void* PS4_SYSV_ABI SessionImplGetUserData(Session* session) {
    return (session != nullptr && session->impl != nullptr) ? session->impl->user_data : nullptr;
}

static Server* PS4_SYSV_ABI SessionImplGetServer(Session* session) {
    return (session != nullptr && session->impl != nullptr) ? session->impl->server : nullptr;
}

static s32 PS4_SYSV_ABI SessionImplRespondToSyncMethodRequest(Session* session, int ret_val,
                                                              const BufferInfo* out_buf,
                                                              u32 out_count) {
    LOG_DEBUG(Lib_Ipmi, "SessionImplRespondToSyncMethodRequest called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI SessionImplRespondToSyncMethodRequestRaw(Session* session, int ret_val,
                                                                 const void* out_data,
                                                                 u64 out_size) {
    LOG_DEBUG(Lib_Ipmi, "SessionImplRespondToSyncMethodRequestRaw called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI SessionImplRespondToAsyncMethodRequest(Session* session, u32 seq, u32 unk,
                                                               int ret_val,
                                                               const DataInfo* out_data,
                                                               u32 out_count) {
    LOG_DEBUG(Lib_Ipmi, "SessionImplRespondToAsyncMethodRequest called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI SessionImplRespondToAsyncMethodRequestRaw(Session* session, u32 seq,
                                                                  int ret_val, const void* out_data,
                                                                  u64 out_size) {
    LOG_DEBUG(Lib_Ipmi, "SessionImplRespondToAsyncMethodRequestRaw called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI SessionImplSetEventFlag(Session* session, u32 flag_id, u64 bit_pattern) {
    LOG_DEBUG(Lib_Ipmi, "SessionImplSetEventFlag called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI SessionImplSendMsg(Session* session, u32 queue_id, const void* msg,
                                           u64 size, u32* out_seq) {
    LOG_DEBUG(Lib_Ipmi, "SessionImplSendMsg called");
    if (out_seq != nullptr) {
        *out_seq = 1;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI SessionImplTrySendMsg(Session* session, u32 queue_id, const void* msg,
                                              u64 size) {
    LOG_DEBUG(Lib_Ipmi, "SessionImplTrySendMsg called");
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI SessionImplGetMsg(Session* session, u32 queue_id, void* out_msg,
                                          u64* out_size, u64 max_size, u32* timeout) {
    LOG_DEBUG(Lib_Ipmi, "SessionImplGetMsg called");
    if (out_size != nullptr) {
        *out_size = 0;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI SessionImplTryGetMsg(Session* session, u32 queue_id, void* out_msg,
                                             u64* out_size, u64 max_size) {
    LOG_DEBUG(Lib_Ipmi, "SessionImplTryGetMsg called");
    if (out_size != nullptr) {
        *out_size = 0;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI SessionImplEmptyMsgQueue(Session* session, u32 queue_id) {
    LOG_DEBUG(Lib_Ipmi, "SessionImplEmptyMsgQueue called");
    return ORBIS_OK;
}

static u32 PS4_SYSV_ABI SessionImplIsPeerPrivileged(Session* session) {
    return 1;
}

static void* const SessionVtable[] = {
    reinterpret_cast<void*>(&SessionImplDestructor),
    reinterpret_cast<void*>(&SessionImplDestructor),
    reinterpret_cast<void*>(&SessionImplDestroy),
    reinterpret_cast<void*>(&SessionImplGetClientPid),
    reinterpret_cast<void*>(&SessionImplGetUserData),
    reinterpret_cast<void*>(&SessionImplGetServer),
    reinterpret_cast<void*>(&SessionImplRespondToSyncMethodRequest),
    reinterpret_cast<void*>(&SessionImplRespondToAsyncMethodRequest),
    reinterpret_cast<void*>(&SessionImplSetEventFlag),
    reinterpret_cast<void*>(&SessionImplSendMsg),
    reinterpret_cast<void*>(&SessionImplTrySendMsg),
    reinterpret_cast<void*>(&SessionImplEmptyMsgQueue),
    reinterpret_cast<void*>(&SessionImplRespondToSyncMethodRequestRaw),
    reinterpret_cast<void*>(&SessionImplRespondToAsyncMethodRequestRaw),
    reinterpret_cast<void*>(&SessionImplIsPeerPrivileged),
    reinterpret_cast<void*>(&SessionImplGetClientKid),
    reinterpret_cast<void*>(&SessionImplGetMsg),
    reinterpret_cast<void*>(&SessionImplTryGetMsg),
};

static void PS4_SYSV_ABI SessionConstructor(Session* session) {
    if (session != nullptr) {
        session->vtable = SessionVtable;
        session->impl = nullptr;
    }
}

static s32 PS4_SYSV_ABI ServerCreateSession(Server* server, Session** out_session, void* user_data,
                                            void* memory) {
    if (out_session == nullptr || memory == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    auto* session = static_cast<Session*>(memory);
    session->vtable = SessionVtable;
    session->impl = new SessionImpl;
    session->impl->server = server;
    session->impl->user_data = user_data;
    session->impl->client_pid = 1;
    *out_session = session;
    return ORBIS_OK;
}

static u64 PS4_SYSV_ABI SessionConfigEstimateSessionMemorySize() {
    return 0;
}

// ============================================================================
// Client & ClientConfig
// ============================================================================

struct ClientConfig {
    char name[25];
    u8 flag_0x19;
    u16 unk_0x1a;
    u64 unk_0x1c;
    u64 sync_buffer_sizes[2];
    u32 sync_mode;
    u64 async_buffer_sizes[2];
    u32 async_mode;
    u8 reserved[0x100];
};

struct ClientImpl {
    std::mutex mutex;
    void* user_data = nullptr;
    bool connected = false;
};

struct Client {
    void* const* vtable;
    ClientImpl* impl;
};

static s32 PS4_SYSV_ABI ClientImplDestroy(Client* client) {
    if (client == nullptr || client->impl == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    delete client->impl;
    client->impl = nullptr;
    return ORBIS_OK;
}

static void* PS4_SYSV_ABI ClientImplGetUserData(Client* client) {
    return (client != nullptr && client->impl != nullptr) ? client->impl->user_data : nullptr;
}

static s32 PS4_SYSV_ABI ClientImplConnect(Client* client, const void* name, u64 name_len,
                                          int* out_handle) {
    LOG_INFO(Lib_Ipmi, "ClientImplConnect called");
    if (client == nullptr || client->impl == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    client->impl->connected = true;
    if (out_handle != nullptr) {
        *out_handle = 1;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI ClientImplDisconnect(Client* client) {
    LOG_INFO(Lib_Ipmi, "ClientImplDisconnect called");
    if (client == nullptr || client->impl == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    client->impl->connected = false;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI ClientImplTerminateConnection(Client* client) {
    LOG_INFO(Lib_Ipmi, "ClientImplTerminateConnection called");
    if (client == nullptr || client->impl == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    client->impl->connected = false;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI ClientImplInvokeSyncMethod(Client* client, u32 method_id,
                                                   const void* in_data, u64 in_size, int* out_ret,
                                                   void* out_data, u64* out_size,
                                                   u64 max_out_size) {
    LOG_DEBUG(Lib_Ipmi, "ClientImplInvokeSyncMethod called method_id={}", method_id);
    if (out_ret != nullptr) {
        *out_ret = 0;
    }
    if (out_size != nullptr) {
        *out_size = 0;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI ClientImplInvokeSyncMethodDataInfo(Client* client, u32 method_id,
                                                           const DataInfo* in_data, u32 in_count,
                                                           int* out_ret, BufferInfo* out_buf,
                                                           u32 out_count) {
    LOG_DEBUG(Lib_Ipmi, "ClientImplInvokeSyncMethodDataInfo called method_id={}", method_id);
    if (out_ret != nullptr) {
        *out_ret = 0;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI ClientImplInvokeAsyncMethod(Client* client, u32 method_id,
                                                    const void* in_data, u64 in_size, u32* out_seq,
                                                    const EventNotifee* notifee) {
    LOG_DEBUG(Lib_Ipmi, "ClientImplInvokeAsyncMethod called method_id={}", method_id);
    if (out_seq != nullptr) {
        *out_seq = 1;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI ClientImplInvokeAsyncMethodDataInfo(Client* client, u32 method_id,
                                                            const DataInfo* in_data, u32 in_count,
                                                            u32* out_seq,
                                                            const EventNotifee* notifee) {
    LOG_DEBUG(Lib_Ipmi, "ClientImplInvokeAsyncMethodDataInfo called method_id={}", method_id);
    if (out_seq != nullptr) {
        *out_seq = 1;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI ClientImplTryGetResult(Client* client, u32 seq, int* out_ret,
                                               void* out_data, u64* out_size, u64 max_out_size) {
    LOG_DEBUG(Lib_Ipmi, "ClientImplTryGetResult called seq={}", seq);
    if (out_ret != nullptr) {
        *out_ret = 0;
    }
    if (out_size != nullptr) {
        *out_size = 0;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI ClientImplTryGetResultBufferInfo(Client* client, u32 seq, u32 unk,
                                                         int* out_ret, BufferInfo* out_buf,
                                                         u32 out_count) {
    LOG_DEBUG(Lib_Ipmi, "ClientImplTryGetResultBufferInfo called seq={}", seq);
    if (out_ret != nullptr) {
        *out_ret = 0;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI ClientImplPollEventFlag(Client* client, u32 flag_id, u64 bit_pattern,
                                                u32 wait_mode, u64* out_bits) {
    LOG_DEBUG(Lib_Ipmi, "ClientImplPollEventFlag called");
    if (out_bits != nullptr) {
        *out_bits = bit_pattern;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI ClientImplWaitEventFlag(Client* client, u32 flag_id, u64 bit_pattern,
                                                u32 wait_mode, u64* out_bits, u32* timeout) {
    LOG_DEBUG(Lib_Ipmi, "ClientImplWaitEventFlag called");
    if (out_bits != nullptr) {
        *out_bits = bit_pattern;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI ClientImplGetMsg(Client* client, u32 queue_id, void* out_msg, u64* out_size,
                                         u64 max_size, u32* timeout) {
    LOG_DEBUG(Lib_Ipmi, "ClientImplGetMsg called");
    if (out_size != nullptr) {
        *out_size = 0;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI ClientImplTryGetMsg(Client* client, u32 queue_id, void* out_msg,
                                            u64* out_size, u64 max_size) {
    LOG_DEBUG(Lib_Ipmi, "ClientImplTryGetMsg called");
    if (out_size != nullptr) {
        *out_size = 0;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI ClientImplMethod80(Client* client, void* arg1, void* arg2, void* arg3) {
    return ORBIS_OK;
}

static void PS4_SYSV_ABI ClientImplDestructor(Client* client) {
    if (client != nullptr) {
        delete client->impl;
        client->impl = nullptr;
    }
}

static void* const ClientVtable[] = {
    reinterpret_cast<void*>(&ClientImplDestroy),
    reinterpret_cast<void*>(&ClientImplGetUserData),
    reinterpret_cast<void*>(&ClientImplConnect),
    reinterpret_cast<void*>(&ClientImplDisconnect),
    reinterpret_cast<void*>(&ClientImplPollEventFlag),
    reinterpret_cast<void*>(&ClientImplWaitEventFlag),
    reinterpret_cast<void*>(&ClientImplGetMsg),
    reinterpret_cast<void*>(&ClientImplTryGetMsg),
    reinterpret_cast<void*>(&ClientImplInvokeSyncMethod),
    reinterpret_cast<void*>(&ClientImplInvokeAsyncMethod),
    reinterpret_cast<void*>(&ClientImplTryGetResult),
    reinterpret_cast<void*>(&ClientImplInvokeSyncMethodDataInfo),
    reinterpret_cast<void*>(&ClientImplInvokeAsyncMethodDataInfo),
    reinterpret_cast<void*>(&ClientImplTryGetResultBufferInfo),
    nullptr,
    nullptr,
    reinterpret_cast<void*>(&ClientImplMethod80),
    reinterpret_cast<void*>(&ClientImplTerminateConnection),
    reinterpret_cast<void*>(&ClientImplDestructor),
    reinterpret_cast<void*>(&ClientImplDestructor),
};

static void PS4_SYSV_ABI ClientConfigConstructor(ClientConfig* config) {
    if (config == nullptr) {
        return;
    }
    std::memset(config, 0, sizeof(ClientConfig));
    config->sync_buffer_sizes[0] = DefaultBufferSize;
    config->sync_buffer_sizes[1] = DefaultBufferSize;
    config->sync_mode = 1;
    config->async_buffer_sizes[0] = DefaultBufferSize;
    config->async_buffer_sizes[1] = DefaultBufferSize;
    config->async_mode = 0;
}

static u64 PS4_SYSV_ABI ClientConfigEstimateClientMemorySize() {
    return ClientMemorySize;
}

static void PS4_SYSV_ABI ClientConfigSyncMethodInvocationConfigConstructor(void* config) {}

static void PS4_SYSV_ABI ClientConfigAsyncMethodInvocationConfigConstructor(void* config) {}

static void PS4_SYSV_ABI ClientConfigMsgQueueConfigConstructor(void* config) {}

static u64 PS4_SYSV_ABI ClientConfigMsgQueueConfigEstimateMsgQueueSize(u64 size, u32 count) {
    return static_cast<u64>(count) * (size + 0x10);
}

static void PS4_SYSV_ABI ClientConstructor(Client* client) {
    if (client != nullptr) {
        client->vtable = ClientVtable;
        client->impl = nullptr;
    }
}

static void PS4_SYSV_ABI ClientDestructor(Client* client) {
    if (client != nullptr) {
        delete client->impl;
        client->impl = nullptr;
    }
}

static s32 PS4_SYSV_ABI ClientImplInitialize(Client* client, const ClientConfig* config,
                                             void* user_data) {
    if (client == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    client->vtable = ClientVtable;
    if (client->impl == nullptr) {
        client->impl = new ClientImpl;
    }
    client->impl->user_data = user_data;
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI ClientCreate(Client** out_client, const ClientConfig* config,
                                     void* user_data, void* memory) {
    if (out_client == nullptr || memory == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    auto* client = static_cast<Client*>(memory);
    client->vtable = ClientVtable;
    client->impl = new ClientImpl;
    client->impl->user_data = user_data;
    *out_client = client;
    return ORBIS_OK;
}

static void PS4_SYSV_ABI IpmiSetDebugHook(u32 val, void* ptr) {}

// ============================================================================
// libSceIpmiDbg
// ============================================================================

static s32 PS4_SYSV_ABI sceIpmiMgrGetIpcBlockerPid(u32* out_pid) {
    if (out_pid != nullptr) {
        *out_pid = 0;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceIpmiMgrGetIpcBlockerPidByAppId(u32 app_id, u32* out_pid) {
    if (out_pid != nullptr) {
        *out_pid = 0;
    }
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceIpmiMgrRaiseExceptionToPid(u32 pid, u32 code) {
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceIpmiMgrStartDump(void* param) {
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceIpmiMgrPauseDump(void* param) {
    return ORBIS_OK;
}

// ============================================================================
// RegisterLib
// ============================================================================

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    // Client & ClientConfig & ClientImpl
    LIB_FUNCTION("fjPNqzuUop8", "libSceIpmi", 1, "libSceIpmi", IpmiSetDebugHook);
    LIB_FUNCTION("o8Es+aQYa2Q", "libSceIpmi", 1, "libSceIpmi", ClientConstructor);
    LIB_FUNCTION("N0iXfw1xf1Q", "libSceIpmi", 1, "libSceIpmi", ClientConstructor);
    LIB_FUNCTION("H2HLRQerr+g", "libSceIpmi", 1, "libSceIpmi", ClientImplDestructor);
    LIB_FUNCTION("lxtXq6qmrC8", "libSceIpmi", 1, "libSceIpmi", ClientImplDestructor);
    LIB_FUNCTION("EsoUVUbGMdg", "libSceIpmi", 1, "libSceIpmi", ClientImplDestructor);
    LIB_FUNCTION("64umtbwiizI", "libSceIpmi", 1, "libSceIpmi", ClientImplInitialize);
    LIB_FUNCTION("R9CB6TmSpeU", "libSceIpmi", 1, "libSceIpmi", ClientImplDestroy);
    LIB_FUNCTION("vogma5QshU4", "libSceIpmi", 1, "libSceIpmi", ClientImplConnect);
    LIB_FUNCTION("gL4bo5h2nwU", "libSceIpmi", 1, "libSceIpmi", ClientImplMethod80);
    LIB_FUNCTION("nJthG9040U8", "libSceIpmi", 1, "libSceIpmi", ClientImplDisconnect);
    LIB_FUNCTION("cOli1uePSwY", "libSceIpmi", 1, "libSceIpmi", ClientImplTerminateConnection);
    LIB_FUNCTION("57XmD42P9gQ", "libSceIpmi", 1, "libSceIpmi", ClientImplInvokeSyncMethod);
    LIB_FUNCTION("lsbUGZCjQy8", "libSceIpmi", 1, "libSceIpmi", ClientImplInvokeSyncMethodDataInfo);
    LIB_FUNCTION("8kU11lW12ss", "libSceIpmi", 1, "libSceIpmi", ClientImplInvokeAsyncMethod);
    LIB_FUNCTION("FsiuQCKSE5Q", "libSceIpmi", 1, "libSceIpmi", ClientImplInvokeAsyncMethodDataInfo);
    LIB_FUNCTION("pLNZVjiFPKE", "libSceIpmi", 1, "libSceIpmi", ClientImplTryGetResult);
    LIB_FUNCTION("exZt85cnx08", "libSceIpmi", 1, "libSceIpmi", ClientImplTryGetResultBufferInfo);
    LIB_FUNCTION("y29rp3N5qf4", "libSceIpmi", 1, "libSceIpmi", ClientImplPollEventFlag);
    LIB_FUNCTION("eVn6vS9O8EI", "libSceIpmi", 1, "libSceIpmi", ClientImplWaitEventFlag);
    LIB_FUNCTION("X8bM2L-G59E", "libSceIpmi", 1, "libSceIpmi", ClientImplGetUserData);
    LIB_FUNCTION("rU0UzNMZDIs", "libSceIpmi", 1, "libSceIpmi", ClientImplGetMsg);
    LIB_FUNCTION("k9ixmB0yrP4", "libSceIpmi", 1, "libSceIpmi", ClientImplTryGetMsg);
    LIB_FUNCTION("O1lQ2+do5r4", "libSceIpmi", 1, "libSceIpmi", ClientConfigConstructor);
    LIB_FUNCTION("C86F6UUBdFw", "libSceIpmi", 1, "libSceIpmi", ClientConfigConstructor);
    LIB_FUNCTION("Rih7HnIM3JQ", "libSceIpmi", 1, "libSceIpmi",
                 ClientConfigEstimateClientMemorySize);
    LIB_FUNCTION("ZHAqg71nFWw", "libSceIpmi", 1, "libSceIpmi",
                 ClientConfigSyncMethodInvocationConfigConstructor);
    LIB_FUNCTION("WxQ-nbumKpM", "libSceIpmi", 1, "libSceIpmi",
                 ClientConfigSyncMethodInvocationConfigConstructor);
    LIB_FUNCTION("TA4rU9ZXgFM", "libSceIpmi", 1, "libSceIpmi",
                 ClientConfigAsyncMethodInvocationConfigConstructor);
    LIB_FUNCTION("eNDYbSvB334", "libSceIpmi", 1, "libSceIpmi",
                 ClientConfigAsyncMethodInvocationConfigConstructor);
    LIB_FUNCTION("yFtfUBc5dsc", "libSceIpmi", 1, "libSceIpmi",
                 ClientConfigMsgQueueConfigConstructor);
    LIB_FUNCTION("pbT-iQht85o", "libSceIpmi", 1, "libSceIpmi",
                 ClientConfigMsgQueueConfigConstructor);
    LIB_FUNCTION("yoVdeXZzPck", "libSceIpmi", 1, "libSceIpmi",
                 ClientConfigMsgQueueConfigEstimateMsgQueueSize);
    LIB_FUNCTION("jt25u2q3Nvw", "libSceIpmi", 1, "libSceIpmi", ClientConstructor);
    LIB_FUNCTION("hQkeuEc0BMs", "libSceIpmi", 1, "libSceIpmi", ClientDestructor);
    LIB_FUNCTION("QLDcLR7Pzbg", "libSceIpmi", 1, "libSceIpmi", ClientDestructor);
    LIB_FUNCTION("9j0AkeNr+qk", "libSceIpmi", 1, "libSceIpmi", ClientDestructor);
    LIB_FUNCTION("0zsTiDhM0nU", "libSceIpmi", 1, "libSceIpmi", ClientCreate);

    // Server & ServerConfig & ServerImpl & EventHandler
    LIB_FUNCTION("0qbsT7faz0E", "libSceIpmi", 1, "libSceIpmi", EventHandlerDestructor);
    LIB_FUNCTION("Z5i0--Vqfwg", "libSceIpmi", 1, "libSceIpmi", EventHandlerDestructor);
    LIB_FUNCTION("ExIJ4oFvvzU", "libSceIpmi", 1, "libSceIpmi", EventHandlerDestructor);
    LIB_FUNCTION("RQD5VVIt0Bw", "libSceIpmi", 1, "libSceIpmi",
                 EventHandlerOnSyncMethodDispatchDataInfo);
    LIB_FUNCTION("v7dRprTYrgU", "libSceIpmi", 1, "libSceIpmi",
                 EventHandlerOnAsyncMethodDispatchDataInfo);
    LIB_FUNCTION("WemdQZJ8Ets", "libSceIpmi", 1, "libSceIpmi", EventHandlerOnSessionKilled);
    LIB_FUNCTION("vo2FiTXAekk", "libSceIpmi", 1, "libSceIpmi", ServerConfigConstructor);
    LIB_FUNCTION("u5IYJtq4D-4", "libSceIpmi", 1, "libSceIpmi", ServerConfigConstructor);
    LIB_FUNCTION("iVUJ-VqTtek", "libSceIpmi", 1, "libSceIpmi", EventHandlerOnSyncMethodDispatch);
    LIB_FUNCTION("wQ9pO7tm3DU", "libSceIpmi", 1, "libSceIpmi", EventHandlerOnAsyncMethodDispatch);
    LIB_FUNCTION("OXc1BksfEIg", "libSceIpmi", 1, "libSceIpmi",
                 ServerConfigEstimateTempWorkingMemorySize);
    LIB_FUNCTION("P72uwJlJjX4", "libSceIpmi", 1, "libSceIpmi", ServerConstructor);
    LIB_FUNCTION("bQOlflF2R5I", "libSceIpmi", 1, "libSceIpmi", ServerDestructor);
    LIB_FUNCTION("HkT3ygqjkCg", "libSceIpmi", 1, "libSceIpmi", ServerDestructor);
    LIB_FUNCTION("ysfNnf9JPS8", "libSceIpmi", 1, "libSceIpmi", ServerDestructor);
    LIB_FUNCTION("hoWSsRT0ktA", "libSceIpmi", 1, "libSceIpmi", ServerCreate);
    LIB_FUNCTION("w-zWDsN+0bQ", "libSceIpmi", 1, "libSceIpmi", ServerConstructor);
    LIB_FUNCTION("Gzah1AMZDS8", "libSceIpmi", 1, "libSceIpmi", ServerConstructor);
    LIB_FUNCTION("D1A-mAFwjT4", "libSceIpmi", 1, "libSceIpmi", ServerDestructor);
    LIB_FUNCTION("uMWav34mMq8", "libSceIpmi", 1, "libSceIpmi", ServerDestructor);
    LIB_FUNCTION("2VzR2bMe3iU", "libSceIpmi", 1, "libSceIpmi", ServerDestructor);
    LIB_FUNCTION("dPOHeORqhbw", "libSceIpmi", 1, "libSceIpmi", ServerDestroy);
    LIB_FUNCTION("GIniJssLcts", "libSceIpmi", 1, "libSceIpmi", ServerTryDispatch);
    LIB_FUNCTION("P17jonIyIB8", "libSceIpmi", 1, "libSceIpmi", ServerRunDispatcher);
    LIB_FUNCTION("e-OnyPe42tI", "libSceIpmi", 1, "libSceIpmi", ServerShutdownDispatcher);
    LIB_FUNCTION("O+Rk6bioKMY", "libSceIpmi", 1, "libSceIpmi", ServerCreateSession);
    LIB_FUNCTION("w9uM8jBkjhw", "libSceIpmi", 1, "libSceIpmi", ServerGetUserData);
    LIB_FUNCTION("a1GlhuNPcNc", "libSceIpmi", 1, "libSceIpmi", EventHandlerVtableSymbol);

    // Session & SessionConfig & SessionImpl
    LIB_FUNCTION("+0U0tMljl0I", "libSceIpmi", 1, "libSceIpmi",
                 SessionConfigEstimateSessionMemorySize);
    LIB_FUNCTION("q090kVFxvro", "libSceIpmi", 1, "libSceIpmi", SessionConstructor);
    LIB_FUNCTION("QfbB4i5JCvk", "libSceIpmi", 1, "libSceIpmi", SessionImplDestructor);
    LIB_FUNCTION("AHNKaOCthAE", "libSceIpmi", 1, "libSceIpmi", SessionImplDestructor);
    LIB_FUNCTION("G2tb9MfBnFs", "libSceIpmi", 1, "libSceIpmi", SessionImplDestructor);
    LIB_FUNCTION("WWpgP5pqtZ0", "libSceIpmi", 1, "libSceIpmi", SessionConstructor);
    LIB_FUNCTION("pWkKuPgWHR8", "libSceIpmi", 1, "libSceIpmi", SessionConstructor);
    LIB_FUNCTION("y33KR+d7Ldg", "libSceIpmi", 1, "libSceIpmi", SessionImplDestructor);
    LIB_FUNCTION("X7iya0Mw+To", "libSceIpmi", 1, "libSceIpmi", SessionImplDestructor);
    LIB_FUNCTION("NhaGXEDsWEc", "libSceIpmi", 1, "libSceIpmi", SessionImplDestructor);
    LIB_FUNCTION("YWOjuEzZ3Ic", "libSceIpmi", 1, "libSceIpmi", SessionImplDestroy);
    LIB_FUNCTION("tFY6zzpnlOs", "libSceIpmi", 1, "libSceIpmi", SessionImplTryDispatch);
    LIB_FUNCTION("vk71LUV2VVw", "libSceIpmi", 1, "libSceIpmi", SessionImplGetClientPid);
    LIB_FUNCTION("rq3L3xKnt3Y", "libSceIpmi", 1, "libSceIpmi", SessionImplGetClientKid);
    LIB_FUNCTION("bNAnspllfsc", "libSceIpmi", 1, "libSceIpmi", SessionImplGetUserData);
    LIB_FUNCTION("dzgrvhCjExA", "libSceIpmi", 1, "libSceIpmi", SessionImplGetServer);
    LIB_FUNCTION("AYyxvDy-SFU", "libSceIpmi", 1, "libSceIpmi",
                 SessionImplRespondToSyncMethodRequest);
    LIB_FUNCTION("niK485vUNSQ", "libSceIpmi", 1, "libSceIpmi",
                 SessionImplRespondToAsyncMethodRequest);
    LIB_FUNCTION("r0t1NxWUNwI", "libSceIpmi", 1, "libSceIpmi",
                 SessionImplRespondToSyncMethodRequestRaw);
    LIB_FUNCTION("uqrbCvCIBMU", "libSceIpmi", 1, "libSceIpmi",
                 SessionImplRespondToAsyncMethodRequestRaw);
    LIB_FUNCTION("-S36x8s8CAw", "libSceIpmi", 1, "libSceIpmi", SessionImplSetEventFlag);
    LIB_FUNCTION("ptb571AzPy4", "libSceIpmi", 1, "libSceIpmi", SessionImplSendMsg);
    LIB_FUNCTION("32j2cyCl3Yo", "libSceIpmi", 1, "libSceIpmi", SessionImplTrySendMsg);
    LIB_FUNCTION("nour4-O5aD0", "libSceIpmi", 1, "libSceIpmi", SessionImplGetMsg);
    LIB_FUNCTION("XMR5p-AA5AQ", "libSceIpmi", 1, "libSceIpmi", SessionImplTryGetMsg);
    LIB_FUNCTION("03U1u-RK3vg", "libSceIpmi", 1, "libSceIpmi", SessionImplEmptyMsgQueue);
    LIB_FUNCTION("TY5vzD-y6EA", "libSceIpmi", 1, "libSceIpmi", SessionImplIsPeerPrivileged);

    // libSceIpmiDbg
    LIB_FUNCTION("0RFK6ZS8I9E", "libSceIpmiDbg", 1, "libSceIpmiDbg", sceIpmiMgrGetIpcBlockerPid);
    LIB_FUNCTION("0osoVuiIg38", "libSceIpmiDbg", 1, "libSceIpmiDbg",
                 sceIpmiMgrGetIpcBlockerPidByAppId);
    LIB_FUNCTION("2R+Q+mF1eog", "libSceIpmiDbg", 1, "libSceIpmiDbg", sceIpmiMgrRaiseExceptionToPid);
    LIB_FUNCTION("L4j7nnv2fVU", "libSceIpmiDbg", 1, "libSceIpmiDbg", sceIpmiMgrStartDump);
    LIB_FUNCTION("k+LGes6TQQI", "libSceIpmiDbg", 1, "libSceIpmiDbg", sceIpmiMgrPauseDump);
}

} // namespace Libraries::Ipmi
