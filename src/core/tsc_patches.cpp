// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <climits>
#include <cstring>
#include <Zydis/Zydis.h>
#include "core/tsc_patches.h"

namespace Core {

namespace {

constexpr u64 JmpBytes = 5;
constexpr u64 MaxWindow = 32;
constexpr int MaxInsns = 16;
constexpr u64 DataMargin = 32;

enum class Kind { Unsupported, Plain, Jcc, Jmp, Call, Ret };

struct Insn {
    ZydisDecodedInstruction info;
    Kind kind;
    const u8* address;
};

Kind Classify(const ZydisDecodedInstruction& i) {
    const bool relative = i.raw.imm[0].is_relative;
    switch (i.meta.category) {
    case ZYDIS_CATEGORY_COND_BR:
        return (i.opcode_map == ZYDIS_OPCODE_MAP_DEFAULT && (i.opcode & 0xF0) == 0x70) ||
                       (i.opcode_map == ZYDIS_OPCODE_MAP_0F && (i.opcode & 0xF0) == 0x80)
                   ? Kind::Jcc
                   : Kind::Unsupported;
    case ZYDIS_CATEGORY_UNCOND_BR:
        return relative ? Kind::Jmp : Kind::Unsupported;
    case ZYDIS_CATEGORY_CALL:
        return relative ? Kind::Call : Kind::Unsupported;
    case ZYDIS_CATEGORY_RET:
        return Kind::Ret;
    case ZYDIS_CATEGORY_SYSCALL:
        return Kind::Unsupported;
    default:
        if (i.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) {
            return Kind::Unsupported;
        }
        return (i.attributes & ZYDIS_ATTRIB_IS_RELATIVE) && i.raw.disp.size != 32
                   ? Kind::Unsupported
                   : Kind::Plain;
    }
}

class Patcher {
public:
    Patcher(u8* code, u64 size, u8* trampolines, u64 capacity, u64* used, const u64* multiplier,
            TscPatchVeto veto, std::vector<u64>* skipped_offsets)
        : begin{code}, end{code + size}, size{size}, area{trampolines}, area_capacity{capacity},
          area_used{used}, multiplier{multiplier}, veto{veto}, skipped_offsets{skipped_offsets} {
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    }

    TscPatchReport Run();

private:
    struct Out {
        u8* p;
        u8* end;
        bool ok;
    };

    bool Decode(const u8* p, Insn* out, bool apply_veto) const {
        ZydisDecoderContext context;
        const u64 avail = static_cast<u64>(end - p);
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, &context, p,
                                                        avail < 15 ? avail : 15, &out->info))) {
            return false;
        }
        out->kind = Classify(out->info);
        out->address = p;
        if (apply_veto && veto && veto(p, out->info.length)) {
            out->kind = Kind::Unsupported;
        }
        return true;
    }

    static const u8* BranchTarget(const Insn& i) {
        return i.address + i.info.length + i.info.raw.imm[0].value.s;
    }

    static bool IsPadding(const Insn& i) {
        return i.info.mnemonic == ZYDIS_MNEMONIC_NOP || i.info.mnemonic == ZYDIS_MNEMONIC_INT3;
    }

    static bool TestBit(const std::vector<u8>& bits, u64 i) {
        return (bits[i >> 3] >> (i & 7)) & 1;
    }
    static void SetBit(std::vector<u8>& bits, u64 i) {
        bits[i >> 3] |= static_cast<u8>(1u << (i & 7));
    }

    static void Put(Out* o, const void* bytes, u64 n) {
        if (!o->ok || static_cast<u64>(o->end - o->p) < n) {
            o->ok = false;
            return;
        }
        std::memcpy(o->p, bytes, n);
        o->p += n;
    }
    static void PutU8(Out* o, u8 v) {
        Put(o, &v, 1);
    }
    static void PutRel32(Out* o, const u8* target) {
        const s64 delta = static_cast<s64>(reinterpret_cast<intptr_t>(target) -
                                           reinterpret_cast<intptr_t>(o->p + 4));
        if (delta < INT32_MIN || delta > INT32_MAX) {
            o->ok = false;
            return;
        }
        const s32 rel = static_cast<s32>(delta);
        Put(o, &rel, 4);
    }
    static void PutJmp(Out* o, const u8* target) {
        PutU8(o, 0xE9);
        PutRel32(o, target);
    }

    void PutGuestRdtsc(Out* o) const {
        static constexpr u8 head[] = {0x48, 0x8D, 0x64, 0x24, 0x80,           // lea rsp,[rsp-128]
                                      0x9C,                                   // pushfq
                                      0x0F, 0x31,                             // rdtsc
                                      0x48, 0xC1, 0xE2, 0x20,                 // shl rdx,32
                                      0x48, 0x09, 0xD0,                       // or rax,rdx
                                      0x48, 0xBA};                            // mov rdx,imm64
        static constexpr u8 tail[] = {0x48, 0xF7, 0x22,                       // mul qword [rdx]
                                      0x48, 0x0F, 0xAC, 0xD0, 0x20,           // shrd rax,rdx,32
                                      0x48, 0x89, 0xC2,                       // mov rdx,rax
                                      0x48, 0xC1, 0xEA, 0x20,                 // shr rdx,32
                                      0x89, 0xC0,                             // mov eax,eax
                                      0x9D,                                   // popfq
                                      0x48, 0x8D, 0xA4, 0x24, 0x80, 0, 0, 0}; // lea rsp,[rsp+128]
        const u64 address = static_cast<u64>(reinterpret_cast<uintptr_t>(multiplier));
        Put(o, head, sizeof(head));
        Put(o, &address, 8);
        Put(o, tail, sizeof(tail));
    }

    void EmitWindow(Out* o, const Insn* w, int n, const u8* back) const;

    ZydisDecoder decoder;
    u8* begin;
    u8* end;
    u64 size;
    u8* area;
    u64 area_capacity;
    u64* area_used;
    const u64* multiplier;
    TscPatchVeto veto;
    std::vector<u64>* skipped_offsets;
};

void Patcher::EmitWindow(Out* o, const Insn* w, int n, const u8* back) const {
    for (int k = 0; k < n; ++k) {
        const Insn& i = w[k];
        const u8 length = i.info.length;
        switch (i.kind) {
        case Kind::Plain:
            if (i.info.mnemonic == ZYDIS_MNEMONIC_RDTSC) {
                PutGuestRdtsc(o);
            } else if (i.info.attributes & ZYDIS_ATTRIB_IS_RELATIVE) {
                const u8 disp = i.info.raw.disp.offset;
                const u8* target = i.address + length + i.info.raw.disp.value;
                Put(o, i.address, disp);
                const s64 delta =
                    static_cast<s64>(reinterpret_cast<intptr_t>(target) -
                                     reinterpret_cast<intptr_t>(o->p + 4 + (length - disp - 4)));
                if (delta < INT32_MIN || delta > INT32_MAX) {
                    o->ok = false;
                    return;
                }
                const s32 rel = static_cast<s32>(delta);
                Put(o, &rel, 4);
                Put(o, i.address + disp + 4, static_cast<u64>(length - disp - 4));
            } else {
                Put(o, i.address, length);
            }
            break;
        case Kind::Jcc:
            PutU8(o, 0x0F);
            PutU8(o, static_cast<u8>(0x80 | (i.info.opcode & 0x0F)));
            PutRel32(o, BranchTarget(i));
            break;
        case Kind::Jmp:
            PutJmp(o, BranchTarget(i));
            return;
        case Kind::Ret:
            Put(o, i.address, length);
            return;
        case Kind::Call: {
            static constexpr u8 save[] = {0x48, 0x8D, 0x64, 0x24, 0xF8,  // lea rsp,[rsp-8]
                                          0x50,                          // push rax
                                          0x48, 0xB8};                   // mov rax,imm64
            static constexpr u8 store[] = {0x48, 0x89, 0x44, 0x24, 0x08, // mov [rsp+8],rax
                                           0x58};                        // pop rax
            const u64 ret = static_cast<u64>(reinterpret_cast<uintptr_t>(i.address + length));
            Put(o, save, sizeof(save));
            Put(o, &ret, 8);
            Put(o, store, sizeof(store));
            PutJmp(o, BranchTarget(i));
            return;
        }
        case Kind::Unsupported:
            o->ok = false;
            return;
        }
    }
    PutJmp(o, back);
}

TscPatchReport Patcher::Run() {
    TscPatchReport report;
    std::vector<u8> starts(size / 8 + 1), targets(size / 8 + 1);
    std::vector<u64> rdtscs, invalid;

    for (u64 at = 0; at < size;) {
        Insn i;
        if (!Decode(begin + at, &i, false)) {
            invalid.push_back(at++);
            continue;
        }
        SetBit(starts, at);
        if (i.info.mnemonic == ZYDIS_MNEMONIC_RDTSC) {
            rdtscs.push_back(at);
        }
        if (i.kind == Kind::Jcc || i.kind == Kind::Jmp || i.kind == Kind::Call) {
            const u8* t = BranchTarget(i);
            if (t >= begin && t < end) {
                SetBit(targets, static_cast<u64>(t - begin));
            }
        }
        at += i.info.length;
    }
    report.found = static_cast<u32>(rdtscs.size());

    u64 next_free = 0;
    for (const u64 rdtsc : rdtscs) {
        if (rdtsc < next_free) {
            report.moved++;
            continue;
        }
        Insn w[MaxInsns];
        int n = 1;
        if (!Decode(begin + rdtsc, &w[0], false)) {
            continue;
        }
        u64 lo = rdtsc, hi = rdtsc + w[0].info.length;

        bool closed = false;
        while (hi - lo < JmpBytes && hi < size && n < MaxInsns && TestBit(starts, hi) &&
               !TestBit(targets, hi)) {
            Insn next;
            if (!Decode(begin + hi, &next, true) ||
                (closed ? !IsPadding(next) : next.kind == Kind::Unsupported)) {
                break;
            }
            w[n++] = next;
            hi += next.info.length;
            if (next.kind == Kind::Call) {
                break;
            }
            closed = closed || next.kind == Kind::Jmp || next.kind == Kind::Ret;
        }

        while (hi - lo < JmpBytes && lo > next_free && n < MaxInsns && !TestBit(targets, lo)) {
            u64 prev = lo - 1;
            while (prev > next_free && lo - prev < 15 && !TestBit(starts, prev)) {
                --prev;
            }
            Insn insn;
            if (!TestBit(starts, prev) || prev < next_free || !Decode(begin + prev, &insn, true) ||
                prev + insn.info.length != lo ||
                (insn.kind != Kind::Plain && insn.kind != Kind::Jcc)) {
                break;
            }
            std::memmove(&w[1], &w[0], static_cast<size_t>(n) * sizeof(Insn));
            w[0] = insn;
            ++n;
            lo = prev;
        }

        size_t k = 0;
        while (k < invalid.size() && invalid[k] + DataMargin < lo) {
            ++k;
        }
        const bool near_data = k < invalid.size() && invalid[k] < hi + DataMargin;

        Out o{area + *area_used, area + area_capacity, true};
        if (hi - lo >= JmpBytes && hi - lo <= MaxWindow && !near_data) {
            EmitWindow(&o, w, n, begin + hi);
            const s64 delta = static_cast<s64>(reinterpret_cast<intptr_t>(area + *area_used) -
                                               reinterpret_cast<intptr_t>(begin + lo + JmpBytes));
            if (delta < INT32_MIN || delta > INT32_MAX) {
                o.ok = false;
            }
        } else {
            o.ok = false;
        }
        if (!o.ok) {
            report.skipped++;
            if (skipped_offsets) {
                skipped_offsets->push_back(rdtsc);
            }
            continue;
        }

        const u8* trampoline = area + *area_used;
        *area_used = static_cast<u64>(o.p - area);
        u8* site = begin + lo;
        const s32 rel = static_cast<s32>(reinterpret_cast<intptr_t>(trampoline) -
                                         reinterpret_cast<intptr_t>(site + JmpBytes));
        site[0] = 0xE9;
        std::memcpy(site + 1, &rel, 4);
        std::memset(site + JmpBytes, 0x90, static_cast<size_t>(hi - lo - JmpBytes));
        report.windows++;
        next_free = hi;
    }
    return report;
}

} // namespace

TscPatchReport PatchRdtscInSegment(u8* code, u64 size, u8* trampolines, u64 trampoline_capacity,
                                   u64* trampoline_used, const u64* multiplier, TscPatchVeto veto,
                                   std::vector<u64>* skipped_offsets) {
    Patcher patcher{code,       size, trampolines,    trampoline_capacity, trampoline_used,
                    multiplier, veto, skipped_offsets};
    return patcher.Run();
}

} // namespace Core
