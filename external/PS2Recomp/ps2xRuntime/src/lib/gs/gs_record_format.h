#ifndef PS2X_GS_RECORD_FORMAT_H
#define PS2X_GS_RECORD_FORMAT_H

// Canonical on-disk format of PS2X_GS_RECORD backend recordings (version 2).
// DW5XL-TEST: shared by the runtime recorder (gs_record.cpp) and the offline
// tools (tools/gsreplay: dw5xl_gsrec diff/dump, dw5xl_vif1_replay).
//
// File:    "PS2XGSR2" | u32 version | u32 flags(0)
// Record:  u8 op | u32 payloadBytes | payload
//
// Payloads are serialised field by field (little-endian, bools as 0/1,
// floats/doubles as raw bits, no struct padding), so two recordings of the
// same GS work are byte-identical. Field lists are defined once below and
// are walked by the writer, the reader and the field-level differ.

#include "runtime/gs/gs_types.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace GSRecord
{
    inline constexpr char kMagic[8] = {'P', 'S', '2', 'X', 'G', 'S', 'R', '2'};
    inline constexpr uint32_t kVersion = 2u;

    enum class Op : uint8_t
    {
        Initialize = 1, // u32 size, u64 fnv1a, size bytes of initial VRAM
        Reset,
        Submit,             // i32 FP rounding mode, GSPrimitiveBatch (only vertexCount vertices)
        LoadClut,           // GSTex0Reg, GSTexClutReg
        BeginTransfer,      // GSTransferCommand
        UploadImage,        // u32 size, bytes
        Flush,
        TextureFlush,
        Sync,               // u8 GSSyncReason
        Present,            // GSPresentationRequest (vsyncTick is host-timing only)
        ClearFramebuffer,   // GSContext, u32 rgba
        ConsumeLocalToHost, // u32 maxBytes
        ReadVram,           // u32 psm, base, bw, x, y
        WriteVram,          // u32 psm, base, bw, x, y, value
        SnapshotVram,
    };
    inline constexpr uint8_t kOpCount = 16u; // valid ops are 1..15

    inline const char *opName(uint8_t op)
    {
        static const char *const names[] = {"?", "Initialize", "Reset", "Submit", "LoadClut", "BeginTransfer",
                                            "UploadImage", "Flush", "TextureFlush", "Sync", "Present",
                                            "ClearFramebuffer", "ConsumeLocalToHost", "ReadVram", "WriteVram",
                                            "SnapshotVram"};
        return op < kOpCount ? names[op] : "?";
    }

    inline const char *syncReasonName(uint8_t reason)
    {
        static const char *const names[] = {"Finish", "LocalToHost", "Presentation", "DebugReadback", "Reset"};
        return reason < 5u ? names[reason] : "?";
    }

    inline uint64_t fnv1a(const uint8_t *data, size_t size, uint64_t hash = 0xcbf29ce484222325ull)
    {
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 0x100000001b3ull;
        }
        return hash;
    }

    // ---- field lists (one definition, walked by every visitor) ----------
    template <class V> void fields(V &v, GSVertex &x)
    {
        v("x", x.x); v("y", x.y); v("z", x.z);
        v("r", x.r); v("g", x.g); v("b", x.b); v("a", x.a);
        v("q", x.q); v("s", x.s); v("t", x.t);
        v("u", x.u); v("v", x.v); v("fog", x.fog);
    }
    template <class V> void fields(V &v, GSFrameReg &x) { v("fbp", x.fbp); v("fbw", x.fbw); v("psm", x.psm); v("fbmsk", x.fbmsk); }
    template <class V> void fields(V &v, GSZbufReg &x) { v("zbp", x.zbp); v("psm", x.psm); v("zmask", x.zmask); }
    template <class V> void fields(V &v, GSScissorReg &x) { v("x0", x.x0); v("x1", x.x1); v("y0", x.y0); v("y1", x.y1); }
    template <class V> void fields(V &v, GSTex0Reg &x)
    {
        v("tbp0", x.tbp0); v("tbw", x.tbw); v("psm", x.psm); v("tw", x.tw); v("th", x.th);
        v("tcc", x.tcc); v("tfx", x.tfx); v("cbp", x.cbp); v("cpsm", x.cpsm); v("csm", x.csm);
        v("csa", x.csa); v("cld", x.cld);
    }
    template <class V> void fields(V &v, GSXYOffsetReg &x) { v("ofx", x.ofx); v("ofy", x.ofy); }
    template <class V> void fields(V &v, GSTexaReg &x) { v("ta0", x.ta0); v("aem", x.aem); v("ta1", x.ta1); }
    template <class V> void fields(V &v, GSTexClutReg &x) { v("cbw", x.cbw); v("cou", x.cou); v("cov", x.cov); }
    template <class V> void fields(V &v, GSContext &x)
    {
        v.object("frame", x.frame); v.object("scissor", x.scissor); v.object("tex0", x.tex0);
        v.object("xyoffset", x.xyoffset); v.object("zbuf", x.zbuf);
        v("tex1", x.tex1); v("miptbp1", x.miptbp1); v("miptbp2", x.miptbp2); v("clamp", x.clamp);
        v("alpha", x.alpha); v("test", x.test); v("fba", x.fba);
    }
    template <class V> void fields(V &v, GSPrimReg &x)
    {
        v("type", x.type); v("iip", x.iip); v("tme", x.tme); v("fge", x.fge); v("abe", x.abe);
        v("aa1", x.aa1); v("fst", x.fst); v("ctxt", x.ctxt); v("fix", x.fix);
    }
    template <class V> void fields(V &v, GSBitBltBuf &x)
    {
        v("sbp", x.sbp); v("sbw", x.sbw); v("spsm", x.spsm); v("dbp", x.dbp); v("dbw", x.dbw); v("dpsm", x.dpsm);
    }
    template <class V> void fields(V &v, GSTrxPos &x) { v("ssax", x.ssax); v("ssay", x.ssay); v("dsax", x.dsax); v("dsay", x.dsay); v("dir", x.dir); }
    template <class V> void fields(V &v, GSTrxReg &x) { v("rrw", x.rrw); v("rrh", x.rrh); }
    template <class V> void fields(V &v, GSDrawState &x)
    {
        v.object("context", x.context); v.object("prim", x.prim); v.object("texa", x.texa);
        v.object("texclut", x.texclut); v("pabe", x.pabe); v("scanmsk", x.scanmsk); v("dimx", x.dimx);
        v("dthe", x.dthe); v("colclamp", x.colclamp); v("fogR", x.fogR); v("fogG", x.fogG); v("fogB", x.fogB);
        v("textureWidth", x.textureWidth); v("textureHeight", x.textureHeight); v("linearFilter", x.linearFilter);
    }
    template <class V> void fields(V &v, GSPrimitiveBatch &x)
    {
        v("vertexCount", x.vertexCount);
        const uint8_t count = x.vertexCount < 3u ? x.vertexCount : 3u;
        static const char *const names[] = {"v0", "v1", "v2"};
        for (uint8_t i = 0; i < count; ++i)
            v.object(names[i], x.vertices[i]);
        v.object("state", x.state);
    }
    template <class V> void fields(V &v, GSTransferCommand &x)
    {
        v.object("bitbltbuf", x.bitbltbuf); v.object("trxpos", x.trxpos); v.object("trxreg", x.trxreg);
        v("direction", x.direction);
    }
    template <class V> void fields(V &v, GSPresentationRequest &x)
    {
        v("pmode", x.pmode); v("smode2", x.smode2); v("dispfb1", x.dispfb1); v("display1", x.display1);
        v("dispfb2", x.dispfb2); v("display2", x.display2); v("bgcolor", x.bgcolor); v("vsyncTick", x.vsyncTick);
        v.object("contextFrame0", x.contextFrames[0]); v.object("contextFrame1", x.contextFrames[1]);
        v.object("preferredSource", x.preferredSource); v("preferredDestFbp", x.preferredDestFbp);
        v("hasPreferredSource", x.hasPreferredSource);
    }

    // ---- writer ----------------------------------------------------------
    struct Writer
    {
        std::vector<uint8_t> &out;

        template <class T> void raw(const T &value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            const size_t offset = out.size();
            out.resize(offset + sizeof(T));
            std::memcpy(out.data() + offset, &value, sizeof(T));
        }
        void bytes(const uint8_t *data, size_t size)
        {
            if (size != 0u)
                out.insert(out.end(), data, data + size);
        }
        template <class T> void operator()(const char *, T &value)
        {
            if constexpr (std::is_same_v<T, bool>)
                raw(static_cast<uint8_t>(value ? 1u : 0u));
            else
                raw(value);
        }
        template <class T> void object(const char *, T &value) { fields(*this, value); }
        template <class T> void put(const T &value)
        {
            if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
                raw(value);
            else
                fields(*this, const_cast<T &>(value));
        }
    };

    // ---- reader ----------------------------------------------------------
    struct Reader
    {
        const uint8_t *cur;
        const uint8_t *end;

        template <class T> void raw(T &value)
        {
            if (static_cast<size_t>(end - cur) < sizeof(T))
                throw std::runtime_error("GS record payload truncated");
            std::memcpy(&value, cur, sizeof(T));
            cur += sizeof(T);
        }
        template <class T> void operator()(const char *, T &value)
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                uint8_t b = 0;
                raw(b);
                value = b != 0u;
            }
            else
                raw(value);
        }
        template <class T> void object(const char *, T &value) { fields(*this, value); }
        template <class T> void get(T &value)
        {
            if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
                raw(value);
            else
                fields(*this, value);
        }
    };

    // ---- flattener (name -> printable value, for field-level diffs) ------
    struct FlatField
    {
        std::string name;
        std::string value;
        uint64_t bits = 0;
    };

    struct Flattener
    {
        std::vector<FlatField> &out;
        std::string prefix;

        template <class T> void operator()(const char *name, T &value)
        {
            FlatField f;
            f.name = prefix + name;
            char text[96];
            if constexpr (std::is_same_v<T, float>)
            {
                uint32_t b = 0;
                std::memcpy(&b, &value, 4);
                f.bits = b;
                std::snprintf(text, sizeof(text), "%.9g (0x%08x)", static_cast<double>(value), b);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                std::memcpy(&f.bits, &value, 8);
                std::snprintf(text, sizeof(text), "%.17g (0x%016llx)", value, static_cast<unsigned long long>(f.bits));
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                f.bits = value ? 1u : 0u;
                std::snprintf(text, sizeof(text), "%d", value ? 1 : 0);
            }
            else
            {
                f.bits = static_cast<uint64_t>(value);
                std::snprintf(text, sizeof(text), "%llu (0x%llx)", static_cast<unsigned long long>(f.bits),
                              static_cast<unsigned long long>(f.bits));
            }
            f.value = text;
            out.push_back(std::move(f));
        }
        template <class T> void object(const char *name, T &value)
        {
            Flattener nested{out, prefix + name + "."};
            fields(nested, value);
        }
    };
}

#endif
