// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/libraries/network/http2_error.h"
#include "core/libraries/network/ssl2.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Http2 {

enum OrbisHttp2HttpVersion : u32 {
    ORBIS_HTTP2_VERSION_1_0 = 1,
    ORBIS_HTTP2_VERSION_1_1 = 2,
    ORBIS_HTTP2_VERSION_2_0 = 3,
};

enum OrbisHttp2AsyncEventType : u32 {
    ORBIS_HTTP2_ASYNC_EVENT_SEND_COMPLETE = 0,
    ORBIS_HTTP2_ASYNC_EVENT_READ_COMPLETE = 1,
    ORBIS_HTTP2_ASYNC_EVENT_WS_CONNECTED = 2,
    ORBIS_HTTP2_ASYNC_EVENT_WS_MESSAGE = 3,
    ORBIS_HTTP2_ASYNC_EVENT_WS_CLOSED = 4,
};

struct OrbisHttp2AsyncEvent {
    u32 event_type;
    s32 req_id;
    s32 result;
    u32 reserved0;
    u64 reserved1;
    void* user_arg;
};

// Memory pool statistics
struct OrbisHttp2MemoryPoolStats {
    u64 pool_size;
    u64 max_in_use_size;
    u64 current_in_use_size;
    s32 reserved;
};

// Pre-send callback
struct OrbisHttp2PreSendCallbackData {
    void* unk;
    void* cert;
};
using OrbisHttp2PreSendCallback = PS4_SYSV_ABI s32 (*)(s32 request_id, s32 ssl_id,
                                                       OrbisHttp2PreSendCallbackData* data,
                                                       void* user_arg);

s32 PS4_SYSV_ABI sceHttp2AbortRequest(s32 req_id);
s32 PS4_SYSV_ABI sceHttp2AddCookie(s32 tmpl_or_req_id, const char* url, const char* cookie);
s32 PS4_SYSV_ABI sceHttp2AddRequestHeader(s32 tmpl_or_req_id, const char* name, const char* value,
                                          u32 mode);
s32 PS4_SYSV_ABI sceHttp2AuthCacheFlush(s32 tmpl_or_req_id);
s32 PS4_SYSV_ABI sceHttp2CookieExport(s32 cookie_box_id, void* buf, u64* buf_len);
s32 PS4_SYSV_ABI sceHttp2CookieFlush(s32 cookie_box_id);
s32 PS4_SYSV_ABI sceHttp2CookieImport(s32 cookie_box_id, const void* buf, u64 buf_len);
s32 PS4_SYSV_ABI sceHttp2CreateCookieBox(s32 ctx_id);
s32 PS4_SYSV_ABI sceHttp2CreateRequestWithURL(s32 tmpl_id, const char* method, const char* url,
                                              u64 content_length);
s32 PS4_SYSV_ABI sceHttp2CreateTemplate(s32 ctx_id, const char* user_agent, s32 http_ver,
                                        s32 auto_proxy_conf);
s32 PS4_SYSV_ABI sceHttp2DeleteCookieBox(s32 cookie_box_id);
s32 PS4_SYSV_ABI sceHttp2DeleteRequest(s32 req_id);
s32 PS4_SYSV_ABI sceHttp2DeleteTemplate(s32 tmpl_id);
s32 PS4_SYSV_ABI sceHttp2GetAllResponseHeaders(s32 req_id, char** header, u64* header_size);
s32 PS4_SYSV_ABI sceHttp2GetAllTrailingHeaders(s32 req_id, char** header, u64* header_size);
s32 PS4_SYSV_ABI sceHttp2GetAuthEnabled(s32 tmpl_or_req_id, s32* enable);
s32 PS4_SYSV_ABI sceHttp2GetAutoRedirect(s32 tmpl_or_req_id, s32* enable);
s32 PS4_SYSV_ABI sceHttp2GetCookie(s32 tmpl_or_req_id, const char* url, char* cookie,
                                   u64* cookie_len, u64 prepared = 0);
s32 PS4_SYSV_ABI sceHttp2GetCookieBox(s32 tmpl_or_req_id, s32* cookie_box_id);
s32 PS4_SYSV_ABI sceHttp2GetCookieStats(s32 cookie_box_id, void* stats);
s32 PS4_SYSV_ABI sceHttp2GetMemoryPoolStats(s32 ctx_id, OrbisHttp2MemoryPoolStats* stats);
s32 PS4_SYSV_ABI sceHttp2GetResponseContentLength(s32 req_id, s32* result, u64* content_length);
s32 PS4_SYSV_ABI sceHttp2GetStatusCode(s32 req_id, s32* status_code);
s32 PS4_SYSV_ABI sceHttp2Init(s32 net_id, s32 ssl_id, u64 pool_size, s32 max_requests);
s32 PS4_SYSV_ABI sceHttp2ReadData(s32 req_id, void* data, u64 size);
s32 PS4_SYSV_ABI sceHttp2ReadDataAsync(s32 req_id, void* data, u64 size, void* user_arg,
                                       void* reserved);
s32 PS4_SYSV_ABI sceHttp2RedirectCacheFlush(s32 tmpl_or_req_id);
s32 PS4_SYSV_ABI sceHttp2RemoveRequestHeader(s32 tmpl_or_req_id, const char* name);
s32 PS4_SYSV_ABI sceHttp2SendRequest(s32 req_id, const void* data, u64 size);
s32 PS4_SYSV_ABI sceHttp2SendRequestAsync(s32 req_id, const void* data, u64 size, void* user_arg,
                                          void* reserved);
s32 PS4_SYSV_ABI sceHttp2SetAuthEnabled(s32 tmpl_or_req_id, s32 enable);
s32 PS4_SYSV_ABI sceHttp2SetAuthInfoCallback(s32 tmpl_or_req_id, void* cb, void* user_arg);
s32 PS4_SYSV_ABI sceHttp2SetAutoRedirect(s32 tmpl_or_req_id, s32 enable);
s32 PS4_SYSV_ABI sceHttp2SetConnectionWaitTimeOut(s32 tmpl_or_req_id, u32 timeout_us);
s32 PS4_SYSV_ABI sceHttp2SetConnectTimeOut(s32 tmpl_or_req_id, u32 timeout_us);
s32 PS4_SYSV_ABI sceHttp2SetCookieBox(s32 tmpl_or_req_id, s32 cookie_box_id);
s32 PS4_SYSV_ABI sceHttp2SetCookieMaxNum(s32 cookie_box_id, u32 max_num);
s32 PS4_SYSV_ABI sceHttp2SetCookieMaxNumPerDomain(s32 cookie_box_id, u32 max_num);
s32 PS4_SYSV_ABI sceHttp2SetCookieMaxSize(s32 cookie_box_id, u64 max_size);
s32 PS4_SYSV_ABI sceHttp2SetCookieRecvCallback(s32 tmpl_or_req_id, void* cb, void* user_arg);
s32 PS4_SYSV_ABI sceHttp2SetCookieSendCallback(s32 tmpl_or_req_id, void* cb, void* user_arg);
s32 PS4_SYSV_ABI sceHttp2SetInflateGZIPEnabled(s32 tmpl_or_req_id, s32 enable);
s32 PS4_SYSV_ABI sceHttp2SetMinSslVersion(s32 tmpl_or_req_id, u32 version);
s32 PS4_SYSV_ABI sceHttp2SetPreSendCallback(s32 tmpl_id, OrbisHttp2PreSendCallback cb_func,
                                            void* user_arg);
s32 PS4_SYSV_ABI sceHttp2SetRecvTimeOut(s32 tmpl_or_req_id, u32 timeout_us);
s32 PS4_SYSV_ABI sceHttp2SetRedirectCallback(s32 tmpl_or_req_id, void* cb, void* user_arg);
s32 PS4_SYSV_ABI sceHttp2SetRequestContentLength(s32 req_id, u64 content_length);
s32 PS4_SYSV_ABI sceHttp2SetRequestNoContentLength(s32 req_id);
s32 PS4_SYSV_ABI sceHttp2SetResolveRetry(s32 tmpl_or_req_id, s32 retry);
s32 PS4_SYSV_ABI sceHttp2SetResolveTimeOut(s32 tmpl_or_req_id, u32 timeout_us);
s32 PS4_SYSV_ABI sceHttp2SetSendTimeOut(s32 tmpl_or_req_id, u32 timeout_us);
s32 PS4_SYSV_ABI sceHttp2SetSslCallback(s32 tmpl_or_req_id, void* cb, void* user_arg);
s32 PS4_SYSV_ABI sceHttp2SetTimeOut(s32 tmpl_or_req_id, u32 timeout_us);
s32 PS4_SYSV_ABI sceHttp2SslDisableOption(s32 tmpl_or_req_id, u32 option);
s32 PS4_SYSV_ABI sceHttp2SslEnableOption(s32 tmpl_or_req_id, u32 option);
s32 PS4_SYSV_ABI sceHttp2Term(s32 ctx_id);
s32 PS4_SYSV_ABI sceHttp2WaitAsync(s32 ctx_id, OrbisHttp2AsyncEvent* event, u32 timeout,
                                   void* reserved);

// WebSocket functions
s32 PS4_SYSV_ABI sceHttp2WebSocketCreateRequest(s32 tmpl_id, const char* url, const char* protocol,
                                                void* opt, void* user_arg, void* extra);
s32 PS4_SYSV_ABI sceHttp2WebSocketSendTextMessage(s32 req_id, const char* text, u64 size);
s32 PS4_SYSV_ABI sceHttp2WebSocketSendTextMessageAsync(s32 req_id, const char* text, u64 size,
                                                       void* user_arg, void* reserved);
s32 PS4_SYSV_ABI sceHttp2WebSocketSendDataMessage(s32 req_id, const void* data, u64 size);
s32 PS4_SYSV_ABI sceHttp2WebSocketSendDataMessageAsync(s32 req_id, const void* data, u64 size,
                                                       void* user_arg, void* reserved);
s32 PS4_SYSV_ABI sceHttp2WebSocketClose(s32 req_id, u16 code, const char* reason, u64 reason_len);
s32 PS4_SYSV_ABI sceHttp2WebSocketCloseAsync(s32 req_id, u16 code, const char* reason,
                                             u64 reason_len, void* user_arg, void* reserved);
s32 PS4_SYSV_ABI sceHttp2WebSocketSetPingInterval(s32 req_id, u32 interval_ms);
s32 PS4_SYSV_ABI sceHttp2WebSocketSetPingTimeout(s32 req_id, u32 timeout_ms);

// Reverse-engineered internal/unknown SPRX exports
s32 PS4_SYSV_ABI sceHttp2Unknown_zx9s1Lk(s32 id, const char* domain);
s32 PS4_SYSV_ABI sceHttp2Unknown_mMWZm0(s32 id, u32 val);
s32 PS4_SYSV_ABI sceHttp2Unknown_yYVB1y(s32 id, void* ptr);
s32 PS4_SYSV_ABI sceHttp2Unknown_fkKvmz(s32 id, void* ptr);
s32 PS4_SYSV_ABI sceHttp2Unknown_4PUc7B(s32 id, u32 val);
void PS4_SYSV_ABI dummy();

void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Http2