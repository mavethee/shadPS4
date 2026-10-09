// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/logging/log.h"
#include "common/types.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "regmgr.h"

namespace Libraries::RegMgr {

static std::mutex s_reg_mutex;
static std::unordered_map<u32, s32> s_reg_ints;
static std::unordered_map<u32, std::string> s_reg_strs;
static std::unordered_map<u32, std::vector<u8>> s_reg_bins;

s32 PS4_SYSV_ABI sceRegMgrGetVersion(u32* out_ver) {
    LOG_DEBUG(Lib, "sceRegMgrGetVersion called");
    if (out_ver) {
        *out_ver = 1;
    }
    return 1;
}

s32 PS4_SYSV_ABI sceRegMgrIsInitOK() {
    LOG_DEBUG(Lib, "sceRegMgrIsInitOK called");
    return 1;
}

s32 PS4_SYSV_ABI sceRegMgrSrvGetRegion() {
    LOG_DEBUG(Lib, "sceRegMgrSrvGetRegion called (returning 2/US)");
    return 2; // North America / US
}

s32 PS4_SYSV_ABI sceRegMgrSrvGetMachineType() {
    LOG_DEBUG(Lib, "sceRegMgrSrvGetMachineType called (returning 0)");
    return 0; // Standard PS4
}

s32 PS4_SYSV_ABI sceRegMgrSrvGetRealMachineType() {
    LOG_DEBUG(Lib, "sceRegMgrSrvGetRealMachineType called (returning 0)");
    return 0;
}

s32 PS4_SYSV_ABI sceRegMgrSrvGetQAFforReg() {
    LOG_DEBUG(Lib, "sceRegMgrSrvGetQAFforReg called");
    return 0;
}

s32 PS4_SYSV_ABI sceRegMgrSrvGetRegionStr(char* out_buf, size_t size) {
    LOG_DEBUG(Lib, "sceRegMgrSrvGetRegionStr called");
    if (!out_buf || size == 0) {
        return 0x80020016;
    }
    const char* region = "us";
    size_t copy_len = std::min(size - 1, std::strlen(region));
    std::memcpy(out_buf, region, copy_len);
    out_buf[copy_len] = '\0';
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrSrvCnvRegionStr(s32 /*region*/, char* out_buf, size_t size) {
    LOG_DEBUG(Lib, "sceRegMgrSrvCnvRegionStr called");
    if (!out_buf || size == 0) {
        return 0x80020016;
    }
    const char* region = "us";
    size_t copy_len = std::min(size - 1, std::strlen(region));
    std::memcpy(out_buf, region, copy_len);
    out_buf[copy_len] = '\0';
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrSrvCnvRegionInt(const char* /*region_str*/) {
    LOG_DEBUG(Lib, "sceRegMgrSrvCnvRegionInt called");
    return 2;
}

s32 PS4_SYSV_ABI sceRegMgrCheckError(s32 /*err*/) {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrNonSysCheckError(s32 /*err*/) {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrGetInt(u32 reg_id, s32* out_val) {
    if (!out_val) {
        return 0x80020016;
    }

    std::lock_guard lock(s_reg_mutex);
    auto it = s_reg_ints.find(reg_id);
    if (it != s_reg_ints.end()) {
        *out_val = it->second;
    } else {
        switch (reg_id) {
        case 0x09400100: // TTS / Voice Guide
        case 0x02020000:
        case 0x05020000:
            *out_val = 1;
            break;
        case 0x09c04100:
        case 0x09c04200:
            *out_val = 0;
            break;
        default:
            *out_val = 1;
            break;
        }
    }
    LOG_DEBUG(Lib, "sceRegMgrGetInt(0x{:08x}) -> {}", reg_id, *out_val);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrSetInt(u32 reg_id, s32 val) {
    LOG_DEBUG(Lib, "sceRegMgrSetInt(0x{:08x}, {})", reg_id, val);
    std::lock_guard lock(s_reg_mutex);
    s_reg_ints[reg_id] = val;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrGetStr(u32 reg_id, char* out_buf, size_t max_size) {
    if (!out_buf || max_size == 0) {
        return 0x80020016;
    }
    out_buf[0] = '\0';

    std::lock_guard lock(s_reg_mutex);
    auto it = s_reg_strs.find(reg_id);
    if (it != s_reg_strs.end()) {
        size_t copy_len = std::min(max_size - 1, it->second.size());
        std::memcpy(out_buf, it->second.data(), copy_len);
        out_buf[copy_len] = '\0';
    }
    LOG_DEBUG(Lib, "sceRegMgrGetStr(0x{:08x}) -> '{}'", reg_id, out_buf);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrSetStr(u32 reg_id, const char* str) {
    LOG_DEBUG(Lib, "sceRegMgrSetStr(0x{:08x}, '{}')", reg_id, str ? str : "");
    if (!str) {
        return 0x80020016;
    }
    std::lock_guard lock(s_reg_mutex);
    s_reg_strs[reg_id] = str;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrGetBin(u32 reg_id, void* out_buf, size_t size) {
    if (!out_buf) {
        return 0x80020016;
    }
    std::memset(out_buf, 0, size);

    std::lock_guard lock(s_reg_mutex);
    auto it = s_reg_bins.find(reg_id);
    if (it != s_reg_bins.end()) {
        size_t copy_len = std::min(size, it->second.size());
        std::memcpy(out_buf, it->second.data(), copy_len);
    }
    LOG_DEBUG(Lib, "sceRegMgrGetBin(0x{:08x}, size={})", reg_id, size);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrSetBin(u32 reg_id, const void* buf, size_t size) {
    LOG_DEBUG(Lib, "sceRegMgrSetBin(0x{:08x}, size={})", reg_id, size);
    if (!buf) {
        return 0x80020016;
    }
    std::lock_guard lock(s_reg_mutex);
    auto& vec = s_reg_bins[reg_id];
    vec.resize(size);
    std::memcpy(vec.data(), buf, size);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrGetIntInitVal(u32 reg_id, s32* out_val) {
    return sceRegMgrGetInt(reg_id, out_val);
}

s32 PS4_SYSV_ABI sceRegMgrGetStrInitVal(u32 reg_id, char* out_buf, size_t max_size) {
    return sceRegMgrGetStr(reg_id, out_buf, max_size);
}

s32 PS4_SYSV_ABI sceRegMgrGetBinInitVal(u32 reg_id, void* out_buf, size_t size) {
    return sceRegMgrGetBin(reg_id, out_buf, size);
}

s32 PS4_SYSV_ABI sceRegMgrSetInitLevel(s32 level) {
    LOG_DEBUG(Lib, "sceRegMgrSetInitLevel({})", level);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrResetVal(u32 reg_id, s32 mode) {
    LOG_DEBUG(Lib, "sceRegMgrResetVal(0x{:08x}, {})", reg_id, mode);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrIsChange(u32 /*reg_id*/, s32 /*val*/) {
    return 0;
}

s32 PS4_SYSV_ABI sceRegMgrEvtGetCnt(u32 /*reg_id*/) {
    return 0;
}

s32 PS4_SYSV_ABI sceRegMgrEvtGetRegId(u32 reg_id, u32* out_id) {
    if (out_id) {
        *out_id = reg_id;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrNonSysGetInt(const char* name, u32 reg_id, s32* out_val) {
    LOG_DEBUG(Lib, "sceRegMgrNonSysGetInt('{}', 0x{:08x})", name ? name : "", reg_id);
    if (!out_val) {
        return 0x800d0202;
    }
    *out_val = 0;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrNonSysSetInt(const char* name, u32 reg_id, s32 val) {
    LOG_DEBUG(Lib, "sceRegMgrNonSysSetInt('{}', 0x{:08x}, {})", name ? name : "", reg_id, val);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrNonSysGetStr(const char* name, u32 reg_id, char* out_buf, size_t max_size) {
    LOG_DEBUG(Lib, "sceRegMgrNonSysGetStr('{}', 0x{:08x}, max_size={})", name ? name : "", reg_id,
              max_size);
    if (!out_buf) {
        return 0x800d0202;
    }
    if (max_size > 0x800) {
        return 0x800d0208;
    }
    out_buf[0] = '\0';
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrNonSysSetStr(const char* name, u32 reg_id, const char* str) {
    LOG_DEBUG(Lib, "sceRegMgrNonSysSetStr('{}', 0x{:08x}, '{}')", name ? name : "", reg_id,
              str ? str : "");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrNonSysGetBin(const char* name, u32 reg_id, void* out_buf, size_t size) {
    LOG_DEBUG(Lib, "sceRegMgrNonSysGetBin('{}', 0x{:08x}, size={})", name ? name : "", reg_id, size);
    if (!out_buf) {
        return 0x800d0202;
    }
    std::memset(out_buf, 0, size);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrNonSysSetBin(const char* name, u32 reg_id, const void* /*buf*/, size_t size) {
    LOG_DEBUG(Lib, "sceRegMgrNonSysSetBin('{}', 0x{:08x}, size={})", name ? name : "", reg_id, size);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrBackupNeedMem(void* /*info*/) {
    return 0x1000;
}

s32 PS4_SYSV_ABI sceRegMgrBackupPullData(void* /*buf*/, size_t /*size*/) {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrBackupPushData(const void* /*buf*/, size_t /*size*/) {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrCntlStart() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrCntlDeleteReg(u32 /*reg_id*/) {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrCntlGetFilesCount(u32* out_count) {
    if (out_count) {
        *out_count = 0;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrCntlGetFileName(u32 /*idx*/, char* out_name, size_t size) {
    if (out_name && size > 0) {
        out_name[0] = '\0';
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrCntSetInitReinst() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrRecoverRegNvs() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrToolGetEntryCnt() {
    return 0;
}

s32 PS4_SYSV_ABI sceRegMgrToolGetUpdateCnt() {
    return 0;
}

s32 PS4_SYSV_ABI sceRegMgrToolDataCheckGet() {
    return 0;
}

s32 PS4_SYSV_ABI sceRegMgrToolGetInfo() {
    return 0;
}

s32 PS4_SYSV_ABI sceRegMgrDrvGetEntCnt() {
    return 0;
}

s32 PS4_SYSV_ABI sceRegMgrDrvGetUpdateCnt() {
    return 0;
}

s32 PS4_SYSV_ABI sceRegMgrDrvDataOpen() {
    return 0;
}

s32 PS4_SYSV_ABI sceRegMgrDrvDataCheckGet() {
    return 0;
}

s32 PS4_SYSV_ABI sceRegMgrDrvDataClose() {
    return 0;
}

s32 PS4_SYSV_ABI sceRegMgrLogStart() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrLogPull() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrCntlStopHdd() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrPrintInfo() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceRegMgrUnknown20() {
    return 0;
}

s32 PS4_SYSV_ABI sceRegMgrUnknown1F() {
    return 0;
}

s32 PS4_SYSV_ABI sceRegMgrGetInitError() {
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("q+lPgqZniW4", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrGetVersion);
    LIB_FUNCTION("7yIpFl9KknY", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrGetVersion);
    LIB_FUNCTION("xhrI4zhlBuA", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrIsChange);
    LIB_FUNCTION("khaYelw1Ytc", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrSetInt);
    LIB_FUNCTION("mPYKD12UDQI", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrGetInt);
    LIB_FUNCTION("ffInidSqRss", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrSetStr);
    LIB_FUNCTION("CTplLrrndUg", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrGetStr);
    LIB_FUNCTION("eXxrXkrLXa4", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrSetBin);
    LIB_FUNCTION("NqxMleeTiLs", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrGetBin);
    LIB_FUNCTION("i64ig3BF6f8", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrGetIntInitVal);
    LIB_FUNCTION("jqyhb1oMgHw", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrGetStrInitVal);
    LIB_FUNCTION("dhuH8HjNhUY", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrGetBinInitVal);
    LIB_FUNCTION("YRPHqS8TcnI", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrSetInitLevel);
    LIB_FUNCTION("eL7sMZaIdac", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrResetVal);
    LIB_FUNCTION("LZBANaFbPqM", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrCheckError);
    LIB_FUNCTION("grCFSFvMxgU", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrEvtGetCnt);
    LIB_FUNCTION("qIB-HHTKOCs", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrEvtGetCnt);
    LIB_FUNCTION("djHSzoTfixE", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrEvtGetRegId);
    LIB_FUNCTION("mXTBaSe0p6E", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrEvtGetRegId);
    LIB_FUNCTION("sywg-RnhZMA", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrSrvGetRegion);
    LIB_FUNCTION("xCxMQ7Efh4k", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrSrvGetRegionStr);
    LIB_FUNCTION("PwmjM-dGut4", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrSrvCnvRegionStr);
    LIB_FUNCTION("CUSk0qEDj7s", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrSrvCnvRegionInt);
    LIB_FUNCTION("04zv+0zNJkg", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrSrvGetMachineType);
    LIB_FUNCTION("wQlp94zzrWg", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrSrvGetQAFforReg);
    LIB_FUNCTION("x0oTVOySvTU", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrSrvGetRealMachineType);
    LIB_FUNCTION("9BhoVC0q85k", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrBackupNeedMem);
    LIB_FUNCTION("K2XscH2j2Hk", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrBackupNeedMem);
    LIB_FUNCTION("f-qy0soAkfA", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrBackupPullData);
    LIB_FUNCTION("TjURD46vx-k", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrBackupPullData);
    LIB_FUNCTION("gqPjTzsu9vU", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrBackupPushData);
    LIB_FUNCTION("rducUH7Y62g", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrBackupPushData);
    LIB_FUNCTION("tqHQdMPde4E", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrCntlStart);
    LIB_FUNCTION("BJi3VSFP+8s", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrCntlDeleteReg);
    LIB_FUNCTION("Ena7T6bVG5s", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrCntlGetFilesCount);
    LIB_FUNCTION("18ZCDYD5xjI", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrCntlGetFileName);
    LIB_FUNCTION("HuVpMjidh7c", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrCntSetInitReinst);
    LIB_FUNCTION("rebo0q4yREE", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrIsInitOK);
    LIB_FUNCTION("Xf2JqV9Xj2w", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrRecoverRegNvs);
    LIB_FUNCTION("voOYmhF1vsQ", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrToolGetEntryCnt);
    LIB_FUNCTION("jQ0656do1V4", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrToolGetUpdateCnt);
    LIB_FUNCTION("VmQkpRjBp3s", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrToolDataCheckGet);
    LIB_FUNCTION("hXNEjSwZO9E", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrUnknown20);
    LIB_FUNCTION("qy++51QQzko", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrUnknown1F);
    LIB_FUNCTION("lwQpbDXKpbg", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrDrvGetEntCnt);
    LIB_FUNCTION("NwwkHSVFYBA", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrDrvGetUpdateCnt);
    LIB_FUNCTION("okzABktskwU", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrDrvDataOpen);
    LIB_FUNCTION("Ds-BHoPDTHY", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrDrvDataCheckGet);
    LIB_FUNCTION("HCUhvdmryuQ", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrDrvDataClose);
    LIB_FUNCTION("0QcqLS+gFiQ", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrToolGetInfo);
    LIB_FUNCTION("HGxgXwGSAzQ", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrLogStart);
    LIB_FUNCTION("yfNwXqOshk0", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrLogPull);
    LIB_FUNCTION("Pj8CwRqcOIk", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrCntlStopHdd);
    LIB_FUNCTION("hApQ4NmZ0IU", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrPrintInfo);
    LIB_FUNCTION("PPRCIaNpJJY", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrNonSysSetInt);
    LIB_FUNCTION("dKeshzt29G4", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrNonSysGetInt);
    LIB_FUNCTION("AxUlf9UUxCg", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrNonSysSetStr);
    LIB_FUNCTION("DKWSr89zMsI", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrNonSysGetStr);
    LIB_FUNCTION("6yU1KEvOl14", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrNonSysSetBin);
    LIB_FUNCTION("k9LC1z8kh-E", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrNonSysGetBin);
    LIB_FUNCTION("Eq+fDFaK3i0", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrNonSysCheckError);
    LIB_FUNCTION("nfAfS7f2gR0", "libSceRegMgr", 1, "libSceRegMgr", sceRegMgrGetInitError);
}

} // namespace Libraries::RegMgr
