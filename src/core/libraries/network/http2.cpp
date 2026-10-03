// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/network/http.h"
#include "core/libraries/network/http2.h"
#include "core/libraries/network/http2_error.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <fmt/format.h>

namespace Libraries::Http2 {

struct Http2RequestInfo {
    s32 ctx_id{-1};
    s32 tmpl_id{-1};
    s32 conn_id{-1};
    s32 req_id{-1};
    bool is_websocket{false};
    void* user_arg{nullptr};
};

struct Http2Context {
    explicit Http2Context(s32 id) : ctx_id(id) {}
    s32 ctx_id;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<OrbisHttp2AsyncEvent> event_queue;
};

std::mutex g_http2_mutex;
std::map<s32, s32> templates_to_contexts{};
std::map<s32, s32> requests_to_connections{};
std::map<s32, Http2RequestInfo> requests_info{};
std::map<s32, std::shared_ptr<Http2Context>> g_http2_contexts{};
static std::atomic<s32> g_next_ws_req_id{0x6000};

s32 PS4_SYSV_ABI sceHttp2AbortRequest(s32 req_id) {
    LOG_INFO(Lib_Http2, "called req_id={}", req_id);
    s32 result = Libraries::Http::sceHttpAbortRequest(req_id);
    if (result < 0) {
        LOG_ERROR(Lib_Http2, "Failed to abort HTTP request, error = {:#x}", result);
    }
    return result;
}

s32 PS4_SYSV_ABI sceHttp2AddCookie(s32 tmpl_or_req_id, const char* url, const char* cookie) {
    LOG_INFO(Lib_Http2, "(STUBBED) called id={}, url={}", tmpl_or_req_id, url ? url : "null");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2AddRequestHeader(s32 template_or_req_id, const char* name,
                                          const char* value, u32 mode) {
    LOG_INFO(Lib_Http2, "called id={}, name={}, value={}, mode={}", template_or_req_id,
             name ? name : "null", value ? value : "null", mode);
    s32 result = Libraries::Http::sceHttpAddRequestHeader(template_or_req_id, name, value, mode);
    if (result < 0) {
        LOG_ERROR(Lib_Http2, "Failed to add HTTP request header, error = {:#x}", result);
    }
    return result;
}

s32 PS4_SYSV_ABI sceHttp2AuthCacheFlush(s32 tmpl_or_req_id) {
    LOG_INFO(Lib_Http2, "(STUBBED) called id={}", tmpl_or_req_id);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2CookieExport(s32 cookie_box_id, void* buf, u64* buf_len) {
    LOG_INFO(Lib_Http2, "(STUBBED) called cookie_box_id={}", cookie_box_id);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2CookieFlush(s32 cookie_box_id) {
    LOG_INFO(Lib_Http2, "called cookie_box_id={}", cookie_box_id);
    return Libraries::Http::sceHttpCookieFlush(cookie_box_id);
}

s32 PS4_SYSV_ABI sceHttp2CookieImport(s32 cookie_box_id, const void* buf, u64 buf_len) {
    LOG_INFO(Lib_Http2, "called cookie_box_id={}, buf_len={}", cookie_box_id, buf_len);
    return Libraries::Http::sceHttpCookieImport(cookie_box_id, buf, buf_len);
}

s32 PS4_SYSV_ABI sceHttp2CreateCookieBox(s32 ctx_id) {
    LOG_INFO(Lib_Http2, "(STUBBED) called ctx_id={}", ctx_id);
    return 1; // Return dummy valid cookie box ID
}

s32 PS4_SYSV_ABI sceHttp2CreateRequestWithURL(s32 tmpl_id, const char* method, const char* url,
                                              u64 content_length) {
    LOG_INFO(Lib_Http2, "called tmpl_id={}, method={}, url={}, content_length={}", tmpl_id,
             method ? method : "null", url ? url : "null", content_length);
    std::lock_guard<std::mutex> lock(g_http2_mutex);

    s32 ctx_id = -1;
    if (auto it = templates_to_contexts.find(tmpl_id); it != templates_to_contexts.end()) {
        ctx_id = it->second;
    }

    s32 conn_id = Libraries::Http::sceHttpCreateConnectionWithURL(tmpl_id, url, true);
    if (conn_id < 0) {
        LOG_ERROR(Lib_Http2, "Failed to create HTTP connection, error = {:#x}", conn_id);
        return conn_id;
    }
    s32 req_id =
        Libraries::Http::sceHttpCreateRequestWithURL2(conn_id, method, url, content_length);
    if (req_id < 0) {
        LOG_ERROR(Lib_Http2, "Failed to create HTTP request, error = {:#x}", req_id);
        const s32 del_result = Libraries::Http::sceHttpDeleteConnection(conn_id);
        if (del_result < 0) {
            LOG_ERROR(Lib_Http2, "Failed to clean up HTTP connection {}, error = {:#x}", conn_id,
                      del_result);
        }
        return req_id;
    }

    auto& info = requests_info[req_id];
    info.ctx_id = ctx_id;
    info.tmpl_id = tmpl_id;
    info.conn_id = conn_id;
    info.req_id = req_id;
    info.is_websocket = false;
    requests_to_connections[req_id] = conn_id;

    return req_id;
}

s32 PS4_SYSV_ABI sceHttp2CreateTemplate(s32 ctx_id, const char* user_agent, s32 http_ver,
                                        s32 auto_proxy_conf) {
    LOG_INFO(Lib_Http2, "called ctx_id={}, user_agent={}, http_ver={}, auto_proxy={}", ctx_id,
             user_agent ? user_agent : "null", http_ver, auto_proxy_conf);
    s32 tmpl_id =
        Libraries::Http::sceHttpCreateTemplate(ctx_id, user_agent, http_ver, auto_proxy_conf);
    if (tmpl_id < 0) {
        LOG_ERROR(Lib_Http2, "Failed to create HTTP template, error = {:#x}", tmpl_id);
        return tmpl_id;
    }

    std::lock_guard<std::mutex> lock(g_http2_mutex);
    templates_to_contexts[tmpl_id] = ctx_id;
    return tmpl_id;
}

s32 PS4_SYSV_ABI sceHttp2DeleteCookieBox(s32 cookie_box_id) {
    LOG_INFO(Lib_Http2, "(STUBBED) called cookie_box_id={}", cookie_box_id);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2DeleteRequest(s32 req_id) {
    LOG_INFO(Lib_Http2, "called req_id={}", req_id);
    std::lock_guard<std::mutex> lock(g_http2_mutex);

    requests_info.erase(req_id);

    const s32 result = Libraries::Http::sceHttpDeleteRequest(req_id);
    if (result < 0) {
        LOG_ERROR(Lib_Http2, "Failed to delete HTTP request, error = {:#x}", result);
        return result;
    }
    const auto it = requests_to_connections.find(req_id);
    if (it == requests_to_connections.end()) {
        LOG_DEBUG(Lib_Http2, "No connection tracked for request {}", req_id);
        return ORBIS_OK;
    }
    const s32 conn_id = it->second;
    requests_to_connections.erase(it);

    const s32 conn_result = Libraries::Http::sceHttpDeleteConnection(conn_id);
    if (conn_result < 0) {
        LOG_ERROR(Lib_Http2, "Failed to delete HTTP connection, error = {:#x}", conn_result);
        return conn_result;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2DeleteTemplate(s32 tmpl_id) {
    LOG_INFO(Lib_Http2, "called tmpl_id={}", tmpl_id);
    {
        std::lock_guard<std::mutex> lock(g_http2_mutex);
        templates_to_contexts.erase(tmpl_id);
    }
    s32 result = Libraries::Http::sceHttpDeleteTemplate(tmpl_id);
    if (result < 0) {
        LOG_ERROR(Lib_Http2, "Failed to delete HTTP template, error = {:#x}", result);
    }
    return result;
}

s32 PS4_SYSV_ABI sceHttp2GetAllResponseHeaders(s32 req_id, char** header, u64* header_size) {
    LOG_INFO(Lib_Http2, "called req_id={}", req_id);
    s32 result = Libraries::Http::sceHttpGetAllResponseHeaders(req_id, header, header_size);
    if (result < 0) {
        LOG_ERROR(Lib_Http2, "Failed to get HTTP response headers, error = {:#x}", result);
    }
    return result;
}

s32 PS4_SYSV_ABI sceHttp2GetAllTrailingHeaders(s32 req_id, char** header, u64* header_size) {
    LOG_INFO(Lib_Http2, "called req_id={}", req_id);
    if (header) {
        *header = nullptr;
    }
    if (header_size) {
        *header_size = 0;
    }
    return ORBIS_HTTP2_ERROR_NOT_FOUND;
}

s32 PS4_SYSV_ABI sceHttp2GetAuthEnabled(s32 tmpl_or_req_id, s32* enable) {
    LOG_INFO(Lib_Http2, "called id={}", tmpl_or_req_id);
    return Libraries::Http::sceHttpGetAuthEnabled(tmpl_or_req_id, enable);
}

s32 PS4_SYSV_ABI sceHttp2GetAutoRedirect(s32 tmpl_or_req_id, s32* enable) {
    LOG_INFO(Lib_Http2, "called id={}", tmpl_or_req_id);
    return Libraries::Http::sceHttpGetAutoRedirect(tmpl_or_req_id, enable);
}

s32 PS4_SYSV_ABI sceHttp2GetCookie(s32 tmpl_or_req_id, const char* url, char* cookie,
                                   u64* cookie_len, u64 prepared) {
    LOG_INFO(Lib_Http2, "called id={}, url={}", tmpl_or_req_id, url ? url : "null");
    return Libraries::Http::sceHttpGetCookie(tmpl_or_req_id, url, cookie, cookie_len, prepared, 0);
}

s32 PS4_SYSV_ABI sceHttp2GetCookieBox(s32 tmpl_or_req_id, s32* cookie_box_id) {
    LOG_INFO(Lib_Http2, "(STUBBED) called id={}", tmpl_or_req_id);
    if (cookie_box_id) {
        *cookie_box_id = 1;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2GetCookieStats(s32 cookie_box_id, void* stats) {
    LOG_INFO(Lib_Http2, "called cookie_box_id={}", cookie_box_id);
    return Libraries::Http::sceHttpGetCookieStats(
        cookie_box_id, reinterpret_cast<Libraries::Http::OrbisHttpCookieStats*>(stats));
}

s32 PS4_SYSV_ABI sceHttp2GetMemoryPoolStats(s32 ctx_id, OrbisHttp2MemoryPoolStats* stats) {
    LOG_INFO(Lib_Http2, "called ctx_id={}", ctx_id);
    return Libraries::Http::sceHttpGetMemoryPoolStats(
        ctx_id, reinterpret_cast<Libraries::Http::OrbisHttpMemoryPoolStats*>(stats));
}

s32 PS4_SYSV_ABI sceHttp2GetResponseContentLength(s32 req_id, s32* result, u64* content_length) {
    LOG_INFO(Lib_Http2, "called req_id={}", req_id);
    s32 res = Libraries::Http::sceHttpGetResponseContentLength(
        req_id, reinterpret_cast<int*>(result), content_length);
    if (res < 0) {
        LOG_ERROR(Lib_Http2, "Failed to get HTTP response content length, error = {:#x}", res);
    }
    return res;
}

s32 PS4_SYSV_ABI sceHttp2GetStatusCode(s32 req_id, s32* status_code) {
    LOG_INFO(Lib_Http2, "called req_id={}", req_id);
    s32 result = Libraries::Http::sceHttpGetStatusCode(req_id, status_code);
    if (result < 0) {
        LOG_ERROR(Lib_Http2, "Failed to get HTTP status code, error = {:#x}", result);
    }
    return result;
}

s32 PS4_SYSV_ABI sceHttp2Init(s32 net_id, s32 ssl_id, u64 pool_size, s32 max_requests) {
    LOG_INFO(Lib_Http2, "called net_id={}, ssl_id={}, pool_size={}, max_requests={}", net_id,
             ssl_id, pool_size, max_requests);
    s32 ctx_id = Libraries::Http::sceHttpInit(net_id, ssl_id, pool_size);
    if (ctx_id < 0) {
        LOG_ERROR(Lib_Http2, "Failed to init HTTP context, error = {:#x}", ctx_id);
        return ctx_id;
    }
    std::lock_guard<std::mutex> lock(g_http2_mutex);
    g_http2_contexts[ctx_id] = std::make_shared<Http2Context>(ctx_id);
    return ctx_id;
}

s32 PS4_SYSV_ABI sceHttp2ReadData(s32 req_id, void* data, u64 size) {
    LOG_INFO(Lib_Http2, "called req_id={}, size={}", req_id, size);
    s32 result = Libraries::Http::sceHttpReadData(req_id, data, size);
    if (result < 0) {
        LOG_ERROR(Lib_Http2, "Failed to read HTTP data, error = {:#x}", result);
    }
    return result;
}

s32 PS4_SYSV_ABI sceHttp2ReadDataAsync(s32 req_id, void* data, u64 size, void* user_arg,
                                       void* reserved) {
    LOG_INFO(Lib_Http2, "called req_id={}, data={}, size={}, user_arg={}", req_id, fmt::ptr(data),
             size, fmt::ptr(user_arg));
    std::shared_ptr<Http2Context> ctx;
    {
        std::lock_guard<std::mutex> lock(g_http2_mutex);
        auto it = requests_info.find(req_id);
        if (it != requests_info.end()) {
            if (auto cit = g_http2_contexts.find(it->second.ctx_id);
                cit != g_http2_contexts.end()) {
                ctx = cit->second;
            }
        }
    }

    if (!ctx) {
        LOG_ERROR(Lib_Http2, "No context found for request {}", req_id);
        return ORBIS_HTTP2_ERROR_INVALID_ID;
    }

    std::thread([ctx, req_id, data, size, user_arg]() {
        s32 read_bytes = Libraries::Http::sceHttpReadData(req_id, data, size);

        OrbisHttp2AsyncEvent evt{};
        evt.event_type = ORBIS_HTTP2_ASYNC_EVENT_READ_COMPLETE;
        evt.req_id = req_id;
        evt.result = read_bytes;
        evt.user_arg = user_arg;

        std::lock_guard<std::mutex> lk(ctx->queue_mutex);
        ctx->event_queue.push_back(evt);
        ctx->queue_cv.notify_one();
    }).detach();

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2RedirectCacheFlush(s32 tmpl_or_req_id) {
    LOG_INFO(Lib_Http2, "called id={}", tmpl_or_req_id);
    return Libraries::Http::sceHttpRedirectCacheFlush(tmpl_or_req_id);
}

s32 PS4_SYSV_ABI sceHttp2RemoveRequestHeader(s32 tmpl_or_req_id, const char* name) {
    LOG_INFO(Lib_Http2, "called id={}, name={}", tmpl_or_req_id, name ? name : "null");
    return Libraries::Http::sceHttpRemoveRequestHeader(tmpl_or_req_id, name);
}

s32 PS4_SYSV_ABI sceHttp2SendRequest(s32 req_id, const void* data, u64 size) {
    LOG_INFO(Lib_Http2, "called req_id={}, size={}", req_id, size);
    s32 result = Libraries::Http::sceHttpSendRequest(req_id, data, size);
    if (result < 0) {
        LOG_ERROR(Lib_Http2, "Failed to send HTTP request, error = {:#x}", result);
    }
    return result;
}

s32 PS4_SYSV_ABI sceHttp2SendRequestAsync(s32 req_id, const void* data, u64 size, void* user_arg,
                                          void* reserved) {
    LOG_INFO(Lib_Http2, "called req_id={}, data={}, size={}, user_arg={}", req_id, fmt::ptr(data),
             size, fmt::ptr(user_arg));
    std::shared_ptr<Http2Context> ctx;
    {
        std::lock_guard<std::mutex> lock(g_http2_mutex);
        auto it = requests_info.find(req_id);
        if (it != requests_info.end()) {
            it->second.user_arg = user_arg;
            if (auto cit = g_http2_contexts.find(it->second.ctx_id);
                cit != g_http2_contexts.end()) {
                ctx = cit->second;
            }
        }
    }

    s32 result = Libraries::Http::sceHttpSendRequest(req_id, data, size);
    if (result < 0) {
        LOG_ERROR(Lib_Http2, "sceHttpSendRequest failed, error = {:#x}", result);
        return result;
    }

    if (ctx) {
        std::thread([ctx, req_id, user_arg]() {
            s32 status = 0;
            s32 poll_res = Libraries::Http::sceHttpGetStatusCode(req_id, &status);

            OrbisHttp2AsyncEvent evt{};
            evt.event_type = ORBIS_HTTP2_ASYNC_EVENT_SEND_COMPLETE;
            evt.req_id = req_id;
            evt.result = (poll_res >= 0) ? ORBIS_OK : ORBIS_HTTP2_ERROR_NETWORK;
            evt.reserved0 = 0;
            evt.reserved1 = 0;
            evt.user_arg = user_arg;

            std::lock_guard<std::mutex> lk(ctx->queue_mutex);
            ctx->event_queue.push_back(evt);
            ctx->queue_cv.notify_one();
        }).detach();
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2SetAuthEnabled(s32 tmpl_or_req_id, s32 enable) {
    LOG_INFO(Lib_Http2, "called id={}, enable={}", tmpl_or_req_id, enable);
    return Libraries::Http::sceHttpSetAuthEnabled(tmpl_or_req_id, enable);
}

s32 PS4_SYSV_ABI sceHttp2SetAuthInfoCallback(s32 tmpl_or_req_id, void* cb, void* user_arg) {
    LOG_INFO(Lib_Http2, "called id={}, cb={}, user_arg={}", tmpl_or_req_id, fmt::ptr(cb),
             fmt::ptr(user_arg));
    return Libraries::Http::sceHttpSetAuthInfoCallback(
        tmpl_or_req_id, reinterpret_cast<Libraries::Http::OrbisHttpAuthInfoCallback>(cb), user_arg);
}

s32 PS4_SYSV_ABI sceHttp2SetAutoRedirect(s32 tmpl_or_req_id, s32 enable) {
    LOG_INFO(Lib_Http2, "called id={}, enable={}", tmpl_or_req_id, enable);
    return Libraries::Http::sceHttpSetAutoRedirect(tmpl_or_req_id, enable);
}

s32 PS4_SYSV_ABI sceHttp2SetConnectionWaitTimeOut(s32 tmpl_or_req_id, u32 timeout_us) {
    LOG_INFO(Lib_Http2, "(STUBBED) called id={}, timeout_us={}", tmpl_or_req_id, timeout_us);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2SetConnectTimeOut(s32 tmpl_or_req_id, u32 timeout_us) {
    LOG_INFO(Lib_Http2, "called id={}, timeout_us={}", tmpl_or_req_id, timeout_us);
    return Libraries::Http::sceHttpSetConnectTimeOut(tmpl_or_req_id, timeout_us);
}

s32 PS4_SYSV_ABI sceHttp2SetCookieBox(s32 tmpl_or_req_id, s32 cookie_box_id) {
    LOG_INFO(Lib_Http2, "(STUBBED) called id={}, cookie_box_id={}", tmpl_or_req_id, cookie_box_id);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2SetCookieMaxNum(s32 cookie_box_id, u32 max_num) {
    LOG_INFO(Lib_Http2, "called cookie_box_id={}, max_num={}", cookie_box_id, max_num);
    return Libraries::Http::sceHttpSetCookieMaxNum(cookie_box_id, max_num);
}

s32 PS4_SYSV_ABI sceHttp2SetCookieMaxNumPerDomain(s32 cookie_box_id, u32 max_num) {
    LOG_INFO(Lib_Http2, "called cookie_box_id={}, max_num={}", cookie_box_id, max_num);
    return Libraries::Http::sceHttpSetCookieMaxNumPerDomain(cookie_box_id, max_num);
}

s32 PS4_SYSV_ABI sceHttp2SetCookieMaxSize(s32 cookie_box_id, u64 max_size) {
    LOG_INFO(Lib_Http2, "called cookie_box_id={}, max_size={}", cookie_box_id, max_size);
    return Libraries::Http::sceHttpSetCookieMaxSize(cookie_box_id, static_cast<u32>(max_size));
}

s32 PS4_SYSV_ABI sceHttp2SetCookieRecvCallback(s32 tmpl_or_req_id, void* cb, void* user_arg) {
    LOG_INFO(Lib_Http2, "called id={}, cb={}, user_arg={}", tmpl_or_req_id, fmt::ptr(cb),
             fmt::ptr(user_arg));
    return Libraries::Http::sceHttpSetCookieRecvCallback(
        tmpl_or_req_id, reinterpret_cast<Libraries::Http::OrbisHttpCookieRecvCallback>(cb),
        user_arg);
}

s32 PS4_SYSV_ABI sceHttp2SetCookieSendCallback(s32 tmpl_or_req_id, void* cb, void* user_arg) {
    LOG_INFO(Lib_Http2, "called id={}, cb={}, user_arg={}", tmpl_or_req_id, fmt::ptr(cb),
             fmt::ptr(user_arg));
    return Libraries::Http::sceHttpSetCookieSendCallback(
        tmpl_or_req_id, reinterpret_cast<Libraries::Http::OrbisHttpCookieSendCallback>(cb),
        user_arg);
}

s32 PS4_SYSV_ABI sceHttp2SetInflateGZIPEnabled(s32 tmpl_or_req_id, s32 enable) {
    LOG_INFO(Lib_Http2, "called id={}, enable={}", tmpl_or_req_id, enable);
    return Libraries::Http::sceHttpSetInflateGZIPEnabled(tmpl_or_req_id, enable);
}

s32 PS4_SYSV_ABI sceHttp2SetMinSslVersion(s32 tmpl_or_req_id, u32 version) {
    LOG_INFO(Lib_Http2, "called id={}, version={}", tmpl_or_req_id, version);
    return Libraries::Http::sceHttpsSetMinSslVersion(tmpl_or_req_id, version);
}

s32 PS4_SYSV_ABI sceHttp2SetPreSendCallback(s32 template_id, OrbisHttp2PreSendCallback cb_func,
                                            void* user_arg) {
    LOG_INFO(Lib_Http2, "(STUBBED) called template_id={}", template_id);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2SetRecvTimeOut(s32 tmpl_or_req_id, u32 timeout_us) {
    LOG_INFO(Lib_Http2, "called id={}, timeout_us={}", tmpl_or_req_id, timeout_us);
    return Libraries::Http::sceHttpSetRecvTimeOut(tmpl_or_req_id, timeout_us);
}

s32 PS4_SYSV_ABI sceHttp2SetRedirectCallback(s32 tmpl_or_req_id, void* cb, void* user_arg) {
    LOG_INFO(Lib_Http2, "called id={}, cb={}, user_arg={}", tmpl_or_req_id, fmt::ptr(cb),
             fmt::ptr(user_arg));
    return Libraries::Http::sceHttpSetRedirectCallback(
        tmpl_or_req_id, reinterpret_cast<Libraries::Http::OrbisHttpRedirectCallback>(cb), user_arg);
}

s32 PS4_SYSV_ABI sceHttp2SetRequestContentLength(s32 req_id, u64 content_length) {
    LOG_INFO(Lib_Http2, "called req_id={}, content_length={}", req_id, content_length);
    s32 result = Libraries::Http::sceHttpSetRequestContentLength(req_id, content_length);
    if (result < 0) {
        LOG_ERROR(Lib_Http2, "Failed to set HTTP request content length, error = {:#x}", result);
    }
    return result;
}

s32 PS4_SYSV_ABI sceHttp2SetRequestNoContentLength(s32 req_id) {
    LOG_INFO(Lib_Http2, "called req_id={}", req_id);
    return Libraries::Http::sceHttpSetRequestContentLength(req_id, 0);
}

s32 PS4_SYSV_ABI sceHttp2SetResolveRetry(s32 tmpl_or_req_id, s32 retry) {
    LOG_INFO(Lib_Http2, "called id={}, retry={}", tmpl_or_req_id, retry);
    return Libraries::Http::sceHttpSetResolveRetry(tmpl_or_req_id, retry);
}

s32 PS4_SYSV_ABI sceHttp2SetResolveTimeOut(s32 tmpl_or_req_id, u32 timeout_us) {
    LOG_INFO(Lib_Http2, "called id={}, timeout_us={}", tmpl_or_req_id, timeout_us);
    return Libraries::Http::sceHttpSetResolveTimeOut(tmpl_or_req_id, timeout_us);
}

s32 PS4_SYSV_ABI sceHttp2SetSendTimeOut(s32 tmpl_or_req_id, u32 timeout_us) {
    LOG_INFO(Lib_Http2, "called id={}, timeout_us={}", tmpl_or_req_id, timeout_us);
    return Libraries::Http::sceHttpSetSendTimeOut(tmpl_or_req_id, timeout_us);
}

s32 PS4_SYSV_ABI sceHttp2SetSslCallback(s32 tmpl_or_req_id, void* cb, void* user_arg) {
    LOG_INFO(Lib_Http2, "called id={}, cb={}, user_arg={}", tmpl_or_req_id, fmt::ptr(cb),
             fmt::ptr(user_arg));
    return Libraries::Http::sceHttpsSetSslCallback(
        tmpl_or_req_id, reinterpret_cast<Libraries::Http::OrbisHttpsCallback>(cb), user_arg);
}

s32 PS4_SYSV_ABI sceHttp2SetTimeOut(s32 tmpl_or_req_id, u32 timeout_us) {
    LOG_INFO(Lib_Http2, "called id={}, timeout_us={}", tmpl_or_req_id, timeout_us);
    Libraries::Http::sceHttpSetConnectTimeOut(tmpl_or_req_id, timeout_us);
    Libraries::Http::sceHttpSetSendTimeOut(tmpl_or_req_id, timeout_us);
    return Libraries::Http::sceHttpSetRecvTimeOut(tmpl_or_req_id, timeout_us);
}

s32 PS4_SYSV_ABI sceHttp2SslDisableOption(s32 tmpl_or_req_id, u32 option) {
    LOG_INFO(Lib_Http2, "called id={}, option={:#x}", tmpl_or_req_id, option);
    return Libraries::Http::sceHttpsDisableOption(tmpl_or_req_id, option);
}

s32 PS4_SYSV_ABI sceHttp2SslEnableOption(s32 tmpl_or_req_id, u32 option) {
    LOG_INFO(Lib_Http2, "called id={}, option={:#x}", tmpl_or_req_id, option);
    return Libraries::Http::sceHttpsEnableOption(tmpl_or_req_id, option);
}

s32 PS4_SYSV_ABI sceHttp2Term(s32 ctx_id) {
    LOG_INFO(Lib_Http2, "called ctx_id={}", ctx_id);
    {
        std::lock_guard<std::mutex> lock(g_http2_mutex);
        g_http2_contexts.erase(ctx_id);
    }
    const s32 result = Libraries::Http::sceHttpTerm(ctx_id);
    if (result < 0) {
        LOG_ERROR(Lib_Http2, "Failed to terminate HTTP context {}, error = {:#x}", ctx_id, result);
    }
    return result;
}

s32 PS4_SYSV_ABI sceHttp2WaitAsync(s32 ctx_id, OrbisHttp2AsyncEvent* event, u32 timeout,
                                   void* reserved) {
    if (!event) {
        return ORBIS_HTTP2_ERROR_INVALID_POINTER;
    }
    if (reserved != nullptr) {
        return ORBIS_HTTP2_ERROR_INVALID_VALUE;
    }

    std::shared_ptr<Http2Context> ctx;
    {
        std::lock_guard<std::mutex> lock(g_http2_mutex);
        auto it = g_http2_contexts.find(ctx_id);
        if (it == g_http2_contexts.end()) {
            LOG_ERROR(Lib_Http2, "Invalid ctx_id={}", ctx_id);
            return ORBIS_HTTP2_ERROR_INVALID_ID;
        }
        ctx = it->second;
    }

    std::unique_lock<std::mutex> lk(ctx->queue_mutex);
    if (ctx->event_queue.empty()) {
        if (timeout == 0) {
            return ORBIS_HTTP2_ERROR_TIMED_OUT;
        }
        bool acquired = ctx->queue_cv.wait_for(lk, std::chrono::microseconds(timeout),
                                               [&] { return !ctx->event_queue.empty(); });
        if (!acquired || ctx->event_queue.empty()) {
            return ORBIS_HTTP2_ERROR_TIMED_OUT;
        }
    }

    *event = ctx->event_queue.front();
    ctx->event_queue.pop_front();
    LOG_DEBUG(Lib_Http2, "Delivered event type={}, req_id={}, result={:#x}", event->event_type,
              event->req_id, event->result);
    return ORBIS_OK;
}

// WebSocket functions
s32 PS4_SYSV_ABI sceHttp2WebSocketCreateRequest(s32 tmpl_id, const char* url, const char* protocol,
                                                void* opt, void* user_arg, void* extra) {
    LOG_INFO(Lib_Http2, "called tmpl_id={}, url={}, protocol={}, user_arg={}", tmpl_id,
             url ? url : "null", protocol ? protocol : "null", fmt::ptr(user_arg));
    if (!url) {
        return ORBIS_HTTP2_ERROR_INVALID_VALUE;
    }

    if (!EmulatorSettings.IsConnectedToNetwork()) {
        LOG_INFO(Lib_Http2, "Network is offline, returning ORBIS_HTTP2_ERROR_NETWORK");
        return ORBIS_HTTP2_ERROR_NETWORK;
    }

    std::lock_guard<std::mutex> lock(g_http2_mutex);
    s32 ctx_id = -1;
    if (auto it = templates_to_contexts.find(tmpl_id); it != templates_to_contexts.end()) {
        ctx_id = it->second;
    }

    s32 req_id = g_next_ws_req_id.fetch_add(1);
    auto& info = requests_info[req_id];
    info.ctx_id = ctx_id;
    info.tmpl_id = tmpl_id;
    info.req_id = req_id;
    info.is_websocket = true;
    info.user_arg = user_arg;

    return req_id;
}

s32 PS4_SYSV_ABI sceHttp2WebSocketSendTextMessage(s32 req_id, const char* text, u64 size) {
    LOG_INFO(Lib_Http2, "(STUBBED) called req_id={}, size={}", req_id, size);
    return ORBIS_HTTP2_ERROR_NETWORK;
}

s32 PS4_SYSV_ABI sceHttp2WebSocketSendTextMessageAsync(s32 req_id, const char* text, u64 size,
                                                       void* user_arg, void* reserved) {
    LOG_INFO(Lib_Http2, "(STUBBED) called req_id={}, size={}, user_arg={}", req_id, size,
             fmt::ptr(user_arg));
    return ORBIS_HTTP2_ERROR_NETWORK;
}

s32 PS4_SYSV_ABI sceHttp2WebSocketSendDataMessage(s32 req_id, const void* data, u64 size) {
    LOG_INFO(Lib_Http2, "(STUBBED) called req_id={}, size={}", req_id, size);
    return ORBIS_HTTP2_ERROR_NETWORK;
}

s32 PS4_SYSV_ABI sceHttp2WebSocketSendDataMessageAsync(s32 req_id, const void* data, u64 size,
                                                       void* user_arg, void* reserved) {
    LOG_INFO(Lib_Http2, "(STUBBED) called req_id={}, size={}, user_arg={}", req_id, size,
             fmt::ptr(user_arg));
    return ORBIS_HTTP2_ERROR_NETWORK;
}

s32 PS4_SYSV_ABI sceHttp2WebSocketClose(s32 req_id, u16 code, const char* reason, u64 reason_len) {
    LOG_INFO(Lib_Http2, "(STUBBED) called req_id={}, code={}", req_id, code);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2WebSocketCloseAsync(s32 req_id, u16 code, const char* reason,
                                             u64 reason_len, void* user_arg, void* reserved) {
    LOG_INFO(Lib_Http2, "called req_id={}, code={}, user_arg={}", req_id, code, fmt::ptr(user_arg));
    std::shared_ptr<Http2Context> ctx;
    {
        std::lock_guard<std::mutex> lock(g_http2_mutex);
        auto it = requests_info.find(req_id);
        if (it != requests_info.end()) {
            if (auto cit = g_http2_contexts.find(it->second.ctx_id);
                cit != g_http2_contexts.end()) {
                ctx = cit->second;
            }
        }
    }

    if (ctx) {
        OrbisHttp2AsyncEvent evt{};
        evt.event_type = ORBIS_HTTP2_ASYNC_EVENT_WS_CLOSED;
        evt.req_id = req_id;
        evt.result = ORBIS_OK;
        evt.user_arg = user_arg;

        std::lock_guard<std::mutex> lk(ctx->queue_mutex);
        ctx->event_queue.push_back(evt);
        ctx->queue_cv.notify_one();
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2WebSocketSetPingInterval(s32 req_id, u32 interval_ms) {
    LOG_INFO(Lib_Http2, "called req_id={}, interval_ms={}", req_id, interval_ms);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2WebSocketSetPingTimeout(s32 req_id, u32 timeout_ms) {
    LOG_INFO(Lib_Http2, "called req_id={}, timeout_ms={}", req_id, timeout_ms);
    return ORBIS_OK;
}

// Reverse-engineered internal/unknown SPRX exports
s32 PS4_SYSV_ABI sceHttp2Unknown_zx9s1Lk(s32 id, const char* domain) {
    LOG_INFO(Lib_Http2, "(STUBBED) called id={}, domain={}", id, domain ? domain : "null");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2Unknown_mMWZm0(s32 id, u32 val) {
    LOG_INFO(Lib_Http2, "(STUBBED) called id={}, val={}", id, val);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2Unknown_yYVB1y(s32 id, void* ptr) {
    LOG_INFO(Lib_Http2, "(STUBBED) called id={}, ptr={}", id, fmt::ptr(ptr));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2Unknown_fkKvmz(s32 id, void* ptr) {
    LOG_INFO(Lib_Http2, "(STUBBED) called id={}, ptr={}", id, fmt::ptr(ptr));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttp2Unknown_4PUc7B(s32 id, u32 val) {
    LOG_INFO(Lib_Http2, "(STUBBED) called id={}, val={}", id, val);
    return ORBIS_OK;
}

void PS4_SYSV_ABI dummy() {
    LOG_INFO(Lib_Http2, "(STUBBED) dummy called");
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("AS45QoYHjc4", "libSceHttp2", 1, "libSceHttp2", dummy);
    LIB_FUNCTION("3JCe3lCbQ8A", "libSceHttp2", 1, "libSceHttp2", sceHttp2Init);
    LIB_FUNCTION("YiBUtz-pGkc", "libSceHttp2", 1, "libSceHttp2", sceHttp2Term);
    LIB_FUNCTION("otUQuZa-mv0", "libSceHttp2", 1, "libSceHttp2", sceHttp2GetMemoryPoolStats);
    LIB_FUNCTION("mmyOCxQMVYQ", "libSceHttp2", 1, "libSceHttp2", sceHttp2CreateRequestWithURL);
    LIB_FUNCTION("o0DBQpFE13o", "libSceHttp2", 1, "libSceHttp2", sceHttp2GetResponseContentLength);
    LIB_FUNCTION("+wCt7fCijgk", "libSceHttp2", 1, "libSceHttp2", sceHttp2CreateTemplate);
    LIB_FUNCTION("N4UfjvWJsMw", "libSceHttp2", 1, "libSceHttp2", sceHttp2CreateCookieBox);
    LIB_FUNCTION("O9ync3F-JVI", "libSceHttp2", 1, "libSceHttp2", sceHttp2DeleteCookieBox);
    LIB_FUNCTION("jrVHsKCXA0g", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetCookieBox);
    LIB_FUNCTION("IX23slKvtQI", "libSceHttp2", 1, "libSceHttp2", sceHttp2GetCookieBox);
    LIB_FUNCTION("pDom5-078DA", "libSceHttp2", 1, "libSceHttp2", sceHttp2DeleteTemplate);
    LIB_FUNCTION("c8D9qIjo8EY", "libSceHttp2", 1, "libSceHttp2", sceHttp2DeleteRequest);
    LIB_FUNCTION("EWcwMpbr5F8", "libSceHttp2", 1, "libSceHttp2", sceHttp2SslEnableOption);
    LIB_FUNCTION("B37SruheQ5Y", "libSceHttp2", 1, "libSceHttp2", sceHttp2SslDisableOption);
    LIB_FUNCTION("YrWX+DhPHQY", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetSslCallback);
    LIB_FUNCTION("9XYJwCf3lEA", "libSceHttp2", 1, "libSceHttp2", sceHttp2GetStatusCode);
    LIB_FUNCTION("-rdXUi2XW90", "libSceHttp2", 1, "libSceHttp2", sceHttp2GetAllResponseHeaders);
    LIB_FUNCTION("CjxqMyx2-pU", "libSceHttp2", 1, "libSceHttp2", sceHttp2GetAllTrailingHeaders);
    LIB_FUNCTION("nrPfOE8TQu0", "libSceHttp2", 1, "libSceHttp2", sceHttp2AddRequestHeader);
    LIB_FUNCTION("rbqZig38AT8", "libSceHttp2", 1, "libSceHttp2", sceHttp2SendRequest);
    LIB_FUNCTION("A+NVAFu4eCg", "libSceHttp2", 1, "libSceHttp2", sceHttp2SendRequestAsync);
    LIB_FUNCTION("QygCNNmbGss", "libSceHttp2", 1, "libSceHttp2", sceHttp2ReadData);
    LIB_FUNCTION("bGN-6zbo7ms", "libSceHttp2", 1, "libSceHttp2", sceHttp2ReadDataAsync);
    LIB_FUNCTION("MOp-AUhdfi8", "libSceHttp2", 1, "libSceHttp2", sceHttp2WaitAsync);
    LIB_FUNCTION("IZ-qjhRqvjk", "libSceHttp2", 1, "libSceHttp2", sceHttp2AbortRequest);
    LIB_FUNCTION("jHdP0CS4ZlA", "libSceHttp2", 1, "libSceHttp2", sceHttp2RemoveRequestHeader);
    LIB_FUNCTION("FSAFOzi0FpM", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetRequestContentLength);
    LIB_FUNCTION("bEegosRhgM0", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetRequestNoContentLength);
    LIB_FUNCTION("zx9s1Lk+O90", "libSceHttp2", 1, "libSceHttp2", sceHttp2Unknown_zx9s1Lk);
    LIB_FUNCTION("uRosf8GQbHQ", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetInflateGZIPEnabled);
    LIB_FUNCTION("mMWZm0+7XHM", "libSceHttp2", 1, "libSceHttp2", sceHttp2Unknown_mMWZm0);
    LIB_FUNCTION("zdtXKn9X7no", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetCookieRecvCallback);
    LIB_FUNCTION("McYmUpQ3-DY", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetCookieSendCallback);
    LIB_FUNCTION("GQFGj0rYX+A", "libSceHttp2", 1, "libSceHttp2", sceHttp2GetCookie);
    LIB_FUNCTION("flPxnowtvWY", "libSceHttp2", 1, "libSceHttp2", sceHttp2AddCookie);
    LIB_FUNCTION("JlFGR4v50Kw", "libSceHttp2", 1, "libSceHttp2", sceHttp2CookieExport);
    LIB_FUNCTION("B5ibZI5UlzU", "libSceHttp2", 1, "libSceHttp2", sceHttp2CookieImport);
    LIB_FUNCTION("5VlQSzXW-SQ", "libSceHttp2", 1, "libSceHttp2", sceHttp2CookieFlush);
    LIB_FUNCTION("6a0N6GPD7RM", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetCookieMaxSize);
    LIB_FUNCTION("mPKVhQqh2Es", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetCookieMaxNum);
    LIB_FUNCTION("o7+WXe4WadE", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetCookieMaxNumPerDomain);
    LIB_FUNCTION("eij7UzkUqK8", "libSceHttp2", 1, "libSceHttp2", sceHttp2GetCookieStats);
    LIB_FUNCTION("Wwj6HbB2mOo", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetAuthInfoCallback);
    LIB_FUNCTION("jjFahkBPCYs", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetAuthEnabled);
    LIB_FUNCTION("m-OL13q8AI8", "libSceHttp2", 1, "libSceHttp2", sceHttp2GetAuthEnabled);
    LIB_FUNCTION("WeuDjj5m4YU", "libSceHttp2", 1, "libSceHttp2", sceHttp2AuthCacheFlush);
    LIB_FUNCTION("BJgi0CH7al4", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetRedirectCallback);
    LIB_FUNCTION("b9AvoIaOuHI", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetAutoRedirect);
    LIB_FUNCTION("od5QCZhZSfw", "libSceHttp2", 1, "libSceHttp2", sceHttp2GetAutoRedirect);
    LIB_FUNCTION("klwUy2Wg+q8", "libSceHttp2", 1, "libSceHttp2", sceHttp2RedirectCacheFlush);
    LIB_FUNCTION("VYMxTcBqSE0", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetTimeOut);
    LIB_FUNCTION("ACjtE27aErY", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetResolveTimeOut);
    LIB_FUNCTION("Gcjh+CisAZM", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetResolveRetry);
    LIB_FUNCTION("-HIO4VT87v8", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetConnectTimeOut);
    LIB_FUNCTION("XPtW45xiLHk", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetSendTimeOut);
    LIB_FUNCTION("izvHhqgDt44", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetRecvTimeOut);
    LIB_FUNCTION("n8hMLe31OPA", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetConnectionWaitTimeOut);
    LIB_FUNCTION("UL4Fviw+IAM", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetPreSendCallback);
    LIB_FUNCTION("09tk+kIA1Ns", "libSceHttp2", 1, "libSceHttp2", sceHttp2SetMinSslVersion);
    LIB_FUNCTION("yYVB1yGq35k", "libSceHttp2", 1, "libSceHttp2", sceHttp2Unknown_yYVB1y);
    LIB_FUNCTION("fkKvmzFNBe8", "libSceHttp2", 1, "libSceHttp2", sceHttp2Unknown_fkKvmz);
    LIB_FUNCTION("-0wUiGX74GQ", "libSceHttp2", 1, "libSceHttp2", sceHttp2WebSocketCreateRequest);
    LIB_FUNCTION("jSHMX1oSE+E", "libSceHttp2", 1, "libSceHttp2", sceHttp2WebSocketSendTextMessage);
    LIB_FUNCTION("WYszaHaIkWs", "libSceHttp2", 1, "libSceHttp2",
                 sceHttp2WebSocketSendTextMessageAsync);
    LIB_FUNCTION("D1IAo-CNswA", "libSceHttp2", 1, "libSceHttp2", sceHttp2WebSocketSendDataMessage);
    LIB_FUNCTION("ksn0O9Iilb0", "libSceHttp2", 1, "libSceHttp2",
                 sceHttp2WebSocketSendDataMessageAsync);
    LIB_FUNCTION("v2hFBiyS13w", "libSceHttp2", 1, "libSceHttp2", sceHttp2WebSocketClose);
    LIB_FUNCTION("BlZ8niq3nHU", "libSceHttp2", 1, "libSceHttp2", sceHttp2WebSocketCloseAsync);
    LIB_FUNCTION("-MU9L6S3Fj4", "libSceHttp2", 1, "libSceHttp2", sceHttp2WebSocketSetPingTimeout);
    LIB_FUNCTION("YfaXPnc8PJE", "libSceHttp2", 1, "libSceHttp2", sceHttp2WebSocketSetPingInterval);
    LIB_FUNCTION("4PUc7B6Zr+c", "libSceHttp2", 1, "libSceHttp2", sceHttp2Unknown_4PUc7B);
}

} // namespace Libraries::Http2