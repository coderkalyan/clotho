// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// VCD dumping: variable registry, change records and the writer thread.

#include "RuntimeInternal.h"

using namespace rt_internal;

namespace {

// -- Value-change dump (VCD, LRM §21.7; design in docs/VCD.md) --
//
// The sim thread is the sole producer: at each settled instant rt_vcd_flush reads
// the planes of the variables the dirty set marked, diffs them against a
// per-variable shadow, and appends backend-neutral records to a buffer. A writer
// thread formats whole buffers into VCD text and owns the file. Storage (var
// table, shadows, record buffers, the 1 MiB output buffer) is allocated once at
// rt_vcd_start; the steady state performs no heap allocation. docs/VCD.md is the
// design of record; what is still open there (multithreaded formatting, io_uring,
// crash-handler durability, FST / viewer sinks) all sits behind this same
// producer/record/sink split.

// 64-bit words needed to hold `width` bits.
inline uint32_t vcd_nwords(uint32_t width) {
    return (width + 63) / 64;
}

// One dumped scalar/vector variable: its live plane pointers into design state,
// its VCD identifier, and the last value emitted (shadow) for change suppression.
// Planes are multi-word to support arbitrary width; a value is `nwords` 64-bit
// words per plane (little-endian, low word first).
struct VcdVar {
    const uint8_t* mm = nullptr;   // packed must|may plane (i(2W)), or the iW word when narrow
    const uint8_t* z  = nullptr;   // z plane (iW), or null for a never-z / narrow variable
    uint32_t       width       = 0;
    uint32_t       nwords      = 0;   // words per plane
    uint32_t       mm_bytes    = 0;   // significant bytes of the mm slot field (for the raw diff)
    uint32_t       z_bytes     = 0;   // significant bytes of the z field (0 for never-z / narrow)
    bool           four_state  = false;
    bool           narrow      = false;   // 2-state `{ iW }` slot: mm is a bare iW value word
    // real / shortreal: the value word is an IEEE-754 bit pattern, dumped as
    // `r<decimal>` rather than as bits (LRM §21.7.2.1 vector_value_change).
    bool           is_real     = false;
    // named event (LRM 15.5): no value, only occurrences. `mm` points at the
    // event's 64-bit last-trigger tick, so the ordinary shadow diff detects a
    // trigger, and the emitted change is the bare pulse `1<id>`.
    bool           is_event    = false;
    bool           seen        = false;   // shadow valid (a value has been dumped)
    // Shadow of the last dumped *raw* slot: mm_bytes of the mm field, then z_bytes
    // of the z field. The diff is a memcmp of these against the live slot; the
    // planes are assembled (vcd_read_planes) only when they differ.
    std::vector<uint8_t> shadow;
    // VCD identifier code, and the precomputed line suffix: idcode+"\n" for a
    // scalar, " "+idcode+"\n" for a vector (appended after the value in one copy).
    std::string idcode;
    std::string suffix;
};

// The producer -> sink record stream is a byte buffer of self-describing records
// (docs/VCD.md §4), so a value of any width travels inline and the sink stays
// stateless. A record is a tag byte followed by:
//   tag 1 (time):   u64 tick
//   tag 0 (change): u32 idx, u32 nwords, then nwords u64 each of must, may, z
// Keeping the planes in the record (not a shadow reference) is what lets the sink
// run on another thread or format an alternate backend (FST, viewer).

// The single global VCD engine (the simulator is single-threaded, one dump per
// run). Reset by rt_vcd_close so the next simulation in the same process starts
// clean.
struct VcdEngine {
    std::FILE*            file    = nullptr;
    char*                 iobuf   = nullptr;   // owned output buffer (rendered VCD bytes)
    size_t                out_len = 0;         // bytes pending in iobuf
    rt_wheel*             wheel   = nullptr;    // cached for the close-time flush
    std::string           path;
    std::vector<VcdVar>   vars;
    std::vector<uint8_t>  recbuf;    // producer -> sink record byte stream for pending instants
    std::vector<uint64_t> scratch;   // scan buffer: 3*max_nwords words (must | may | z)
    // Activity-driven dirty set: `epoch[i]` is set by rt_vcd_touch when var i is
    // written this instant, including per-element for an unpacked array. A dense
    // byte array parallel to `vars`, so the flush's per-instant index scan reads
    // one byte per variable and never the fat VcdVar struct.
    std::vector<uint8_t> epoch;
    // Bytes already handed to the FILE. With the pending iobuf they are the file's
    // size, which the writer tests against the $dumplimit cap (vcd_limit_reached).
    uint64_t written = 0;
    uint32_t max_nwords = 1;    // widest variable, in words (sizes scratch / buffers)
    uint32_t next_id    = 0;    // next VCD identifier index (assigned per registration)
    bool started        = false;   // header finished ($enddefinitions written)
    bool active         = false;   // dumping (cleared by $dumpoff)
    bool force_all      = false;   // next flush emits every var ($dumpall / $dumpon / initial)
    bool pending_off    = false;   // next flush emits an all-x checkpoint, then deactivates

    static constexpr size_t iobuf_size = 1u << 20;   // 1 MiB
};

VcdEngine g_vcd;

// All VCD text is rendered into g_vcd.iobuf and written in bulk. This avoids the
// per-character locked stdio (std::fputc takes the FILE lock on every call), which
// dominated the formatter's cost. The buffer is written by whichever thread is
// appending (the sim thread for the header before the writer spawns, the writer
// thread for the body afterwards) -- never concurrently.
void vcd_flush_out() {
    if (g_vcd.out_len != 0) {
        std::fwrite(g_vcd.iobuf, 1, g_vcd.out_len, g_vcd.file);
        g_vcd.written += g_vcd.out_len;
        g_vcd.out_len = 0;
    }
}

inline void vcd_put(char c) {
    if (g_vcd.out_len >= VcdEngine::iobuf_size) {
        vcd_flush_out();
    }
    g_vcd.iobuf[g_vcd.out_len++] = c;
}

inline void vcd_puts(std::string_view s) {
    if (g_vcd.out_len + s.size() > VcdEngine::iobuf_size) {
        vcd_flush_out();
        if (s.size() > VcdEngine::iobuf_size) {   // a single string larger than the buffer
            std::fwrite(s.data(), 1, s.size(), g_vcd.file);
            g_vcd.written += s.size();
            return;
        }
    }
    std::memcpy(g_vcd.iobuf + g_vcd.out_len, s.data(), s.size());
    g_vcd.out_len += s.size();
}

void vcd_put_dec(uint64_t v) {
    char t[20];
    int  n = 0;
    if (v == 0) {
        vcd_put('0');
        return;
    }
    while (v != 0) {
        t[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    }
    while (n != 0) {
        vcd_put(t[--n]);
    }
}

// Dense VCD identifier from a variable index: base-94 over the printable range
// 33..126. Uniqueness is all VCD requires; the exact spelling is unspecified.
std::string vcd_idcode(uint32_t n) {
    std::string s;
    do {
        s.push_back(static_cast<char>(33 + n % 94));
        n = n / 94;
    } while (n-- != 0);
    return s;
}

// Decompose femtoseconds-per-tick into a VCD `<magnitude><unit>` timescale. The
// design precision is a power of ten, so the magnitude lands on 1 / 10 / 100.
void vcd_write_timescale(uint64_t tick_fs) {
    struct Unit {
        uint64_t    fs;
        const char* suffix;
    };
    static constexpr Unit units[] = { { 1000000000000000ull, "s" }, { 1000000000000ull, "ms" },
                                      { 1000000000ull, "us" },       { 1000000ull, "ns" },
                                      { 1000ull, "ps" },             { 1ull, "fs" } };
    if (tick_fs == 0) {
        tick_fs = 1;
    }
    for (const Unit& u : units) {
        if (tick_fs % u.fs == 0) {
            vcd_puts("$timescale\n\t");
            vcd_put_dec(tick_fs / u.fs);
            vcd_puts(u.suffix);
            vcd_puts("\n$end\n");
            return;
        }
    }
    vcd_puts("$timescale\n\t");
    vcd_put_dec(tick_fs);
    vcd_puts("fs\n$end\n");
}

// Mask keeping the significant bits of a plane's top word (all bits when the width
// is a multiple of 64).
inline uint64_t vcd_topmask(uint32_t width) {
    const uint32_t r = width % 64;
    return r == 0 ? ~0ull : ((1ull << r) - 1);
}

// dst[0..dstw) = (src[0..srcw) as a bignum) >> shift. Words are little-endian.
void vcd_shr(const uint64_t* src, uint32_t srcw, uint32_t shift, uint64_t* dst, uint32_t dstw) {
    const uint32_t ws = shift / 64;
    const uint32_t bs = shift % 64;
    for (uint32_t k = 0; k < dstw; ++k) {
        const uint64_t lo  = ws + k < srcw ? src[ws + k] : 0;
        uint64_t       val = lo >> bs;
        if (bs != 0) {
            const uint64_t hi = ws + k + 1 < srcw ? src[ws + k + 1] : 0;
            val |= hi << (64 - bs);
        }
        dst[k] = val;
    }
}

// Read a variable's live planes into `out`, laid out must | may | z, each
// `v.nwords` little-endian 64-bit words. The must|may plane is the packed i(2W)
// slot field (bits [0,W) must, [W,2W) may); a narrow slot is a bare iW value.
void vcd_read_planes(const VcdVar& v, uint64_t* out) {
    if (v.nwords == 1) {
        // Fast path for the common width <= 64: three word stores, no memset / no
        // bignum shift. The 2W-bit slot is <= 128 bits, read once via __int128.
        const uint64_t tm = vcd_topmask(v.width);
        if (v.narrow) {
            uint64_t val = 0;
            std::memcpy(&val, v.mm, (v.width + 7) / 8);
            val    = val & tm;
            out[0] = val;   // must
            out[1] = val;   // may == must
            out[2] = 0;     // z
            return;
        }
        unsigned __int128 mm = 0;
        std::memcpy(&mm, v.mm, (2 * v.width + 7) / 8);
        out[0] = static_cast<uint64_t>(mm) & tm;             // must (low W)
        out[1] = static_cast<uint64_t>(mm >> v.width) & tm;  // may  (bits [W,2W))
        uint64_t zz = 0;
        if (v.z != nullptr) {
            std::memcpy(&zz, v.z, (v.width + 7) / 8);
            zz &= tm;
        }
        out[2] = zz;
        return;
    }
    const uint32_t w    = v.width;
    const uint32_t nw   = v.nwords;
    uint64_t*      must = out;
    uint64_t*      may  = out + nw;
    uint64_t*      z    = out + 2 * nw;
    const uint64_t tm   = vcd_topmask(w);
    std::memset(out, 0, static_cast<size_t>(3) * nw * 8);
    if (v.narrow) {
        // A 2-state `{ iW }` slot: the single value word is fully known (may==must).
        std::memcpy(must, v.mm, (w + 7) / 8);
        must[nw - 1] &= tm;
        std::memcpy(may, must, static_cast<size_t>(nw) * 8);
        return;   // z stays 0
    }
    // Copy the 2W-bit must|may field into a zeroed scratch, then split it: must is
    // the low W bits, may is the field shifted right by W.
    const uint32_t rw  = vcd_nwords(2 * w);
    uint64_t*      raw = g_vcd.scratch.data() + 3 * g_vcd.max_nwords;   // scratch tail, reserved
    std::memset(raw, 0, static_cast<size_t>(rw) * 8);
    std::memcpy(raw, v.mm, (2 * w + 7) / 8);
    std::memcpy(must, raw, static_cast<size_t>(nw) * 8);
    must[nw - 1] &= tm;
    vcd_shr(raw, rw, w, may, nw);
    may[nw - 1] &= tm;
    if (v.z != nullptr) {
        std::memcpy(z, v.z, (w + 7) / 8);
        z[nw - 1] &= tm;
    }
}

// Emit a real's value change: `r<decimal> <id>` (LRM §21.7.2.1). The slot holds
// the IEEE-754 bit pattern in its value word -- 64 bits for `real`, 32 for
// `shortreal`, which widens to double so the file carries one representation.
// The decimal is the shortest round-tripping form (std::to_chars), so a reader's
// strtod recovers the exact bits; iverilog's %.16g can lose the last ulp.
// A real has no x/z, so the only way `may` can differ from `must` is the all-x
// $dumpoff checkpoint (rt_vcd_flush), which prints as NaN -- iverilog's spelling,
// and the one value no real can otherwise round-trip to.
void vcd_emit_real(const VcdVar& v, const uint64_t* must, const uint64_t* may) {
    char   buf[40];
    size_t len = 0;
    if (must[0] != may[0]) {
        std::memcpy(buf, "NaN", 3);
        len = 3;
    } else {
        const double d = v.width == 32
                           ? static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(must[0])))
                           : std::bit_cast<double>(must[0]);
        len            = shortest_double(buf, d);
    }
    vcd_put('r');
    vcd_puts(std::string_view(buf, len));
    vcd_puts(v.suffix);
}

// Emit one variable's value-change line for the given planes (each `v.nwords`
// little-endian words). Scalars print the value char then the id; vectors print
// `b<bits> <id>` with the LRM §21.7.2.1 asymmetric leading-bit compression
// (leftmost printed digit sets the reader's left-extension: 0->0, x->x, z->z, but
// 1->0, so a 1-MSB cannot be compressed). A real prints `r<decimal> <id>`, and a
// named event the bare pulse `1<id>`.
void vcd_emit(const VcdVar& v, const uint64_t* must, const uint64_t* may, const uint64_t* z) {
    const auto bitc = [&](uint32_t i) -> char {
        const uint64_t m = 1ull << (i & 63);
        const uint32_t w = i >> 6;
        if ((z[w] & m) != 0) {
            return 'z';
        }
        if ((may[w] & m) == 0) {
            return '0';
        }
        return (must[w] & m) != 0 ? '1' : 'x';
    };
    if (v.is_real) {
        vcd_emit_real(v, must, may);
        return;
    }
    if (v.is_event) {
        // An event occurrence has no value and no complement: VCD writes `1<id>`
        // at the instant it triggers and nothing at any other instant. Repeated
        // triggers within one instant collapse into this single pulse, which is
        // all the format can express (LRM §21.7.2.1 declares the `event` var
        // type; iverilog writes the same line).
        vcd_put('1');
        vcd_puts(v.suffix);
        return;
    }
    // One capacity check for the whole line: 'b' + up to width digits + suffix. Then
    // write through a local pointer, no per-character bounds check.
    const size_t need = static_cast<size_t>(v.width) + v.suffix.size() + 2;
    if (g_vcd.out_len + need > VcdEngine::iobuf_size) {
        vcd_flush_out();
    }
    char* wp = g_vcd.iobuf + g_vcd.out_len;
    if (v.width == 1) {
        *wp++ = bitc(0);
    } else {
        *wp++          = 'b';
        const char msb = bitc(v.width - 1);
        if (msb == '0') {
            int hi = -1;
            for (int i = static_cast<int>(v.width) - 1; i >= 0; --i) {
                if (bitc(static_cast<uint32_t>(i)) != '0') {
                    hi = i;
                    break;
                }
            }
            if (hi < 0) {
                *wp++ = '0';
            } else {
                for (int i = hi; i >= 0; --i) {
                    *wp++ = bitc(static_cast<uint32_t>(i));
                }
            }
        } else if (msb == '1') {
            for (int i = static_cast<int>(v.width) - 1; i >= 0; --i) {
                *wp++ = bitc(static_cast<uint32_t>(i));
            }
        } else {
            // x / z MSB: one marker char forces the reader's left-extension, then the
            // bits below the leading run (every bit above `hi` equals the marker).
            *wp++  = msb;
            int hi = -1;
            for (int i = static_cast<int>(v.width) - 2; i >= 0; --i) {
                if (bitc(static_cast<uint32_t>(i)) != msb) {
                    hi = i;
                    break;
                }
            }
            for (int i = hi; i >= 0; --i) {
                *wp++ = bitc(static_cast<uint32_t>(i));
            }
        }
    }
    // Precomputed suffix: scalar idcode+"\n", vector " "+idcode+"\n".
    std::memcpy(wp, v.suffix.data(), v.suffix.size());
    wp += v.suffix.size();
    g_vcd.out_len = static_cast<size_t>(wp - g_vcd.iobuf);
}

// -- Producer / sink seam --
//
// The producer (rt_vcd_flush, on the sim thread) diffs against the shadow and
// appends records; the sink (vcd_drain) turns records into VCD text. Splitting the
// two here is what lets a later step move vcd_drain onto a writer thread, or swap
// in an FST / viewer sink, without touching the producer.

// Grow recbuf by `n` and return a pointer to the fresh region. recbuf is reserved
// to a whole handoff batch at start, so resize never reallocates -- far cheaper
// than std::vector::insert's range machinery, which dominated the producer's cost
// in profiling.
inline uint8_t* vcd_rec_grow(size_t n) {
    const size_t o = g_vcd.recbuf.size();
    g_vcd.recbuf.resize(o + n);
    return g_vcd.recbuf.data() + o;
}

void vcd_rec_time(uint64_t tick) {
    uint8_t* d = vcd_rec_grow(1 + sizeof(tick));
    d[0]       = 1;
    std::memcpy(d + 1, &tick, sizeof(tick));
}

// Append a value-change record: idx, nwords, then the must | may | z planes
// (3*nwords words, contiguous at `planes`).
void vcd_rec_change(uint32_t idx, const uint64_t* planes, uint32_t nwords) {
    const size_t pbytes = static_cast<size_t>(3) * nwords * 8;
    uint8_t*     d      = vcd_rec_grow(9 + pbytes);
    d[0]                = 0;
    std::memcpy(d + 1, &idx, 4);
    std::memcpy(d + 5, &nwords, 4);
    std::memcpy(d + 9, planes, pbytes);
}

// Diff variable `i` against its shadow; if changed (or `force`), append a change
// record and update the shadow. Returns true when a record was appended.
bool vcd_scan_var(uint32_t i, bool force) {
    VcdVar&  v  = g_vcd.vars[i];
    uint8_t* sh = v.shadow.data();
    // Cheap diff on the raw slot bytes (no plane assembly). Assemble the planes
    // only when the value actually changed.
    if (!force && v.seen && std::memcmp(v.mm, sh, v.mm_bytes) == 0
        && (v.z_bytes == 0 || std::memcmp(v.z, sh + v.mm_bytes, v.z_bytes) == 0)) {
        return false;
    }
    uint64_t* scan = g_vcd.scratch.data();
    vcd_read_planes(v, scan);
    vcd_rec_change(i, scan, v.nwords);
    std::memcpy(sh, v.mm, v.mm_bytes);
    if (v.z_bytes != 0) {
        std::memcpy(sh + v.mm_bytes, v.z, v.z_bytes);
    }
    v.seen = true;
    return true;
}

// Writer-thread scratch for the aligned plane copy (only the writer touches it).
std::vector<uint64_t> vcd_writer_scratch;

// -- $dumplimit (LRM §21.7.1.5) --
//
// The cap and the "cap reached" edge are the only VCD state both threads see: the
// sim thread sets the cap at the $dumplimit call and reads the edge at the next
// flush; the writer, which is the only thread that knows how many bytes the text
// actually takes, tests the cap and publishes the edge. Both are reset at close.
std::atomic<uint64_t> vcd_limit{ 0 };
std::atomic<bool>     vcd_limit_hit{ false };

// Writer-side test, run at instant boundaries only, so the dump never ends
// mid-instant: the file keeps the last whole instant that fit, then the comment
// the LRM asks for. Everything after it is dropped. The console notice goes out
// here, the one place the edge is taken -- a small dump reaches its cap only when
// the writer drains at close, too late for the sim thread to report it.
bool vcd_limit_reached() {
    const uint64_t cap = vcd_limit.load(std::memory_order_relaxed);
    if (cap == 0 || g_vcd.written + g_vcd.out_len < cap) {
        return false;
    }
    vcd_puts("$comment Dump file limit (");
    vcd_put_dec(cap);
    vcd_puts(" bytes) reached. $end\n");
    vcd_limit_hit.store(true, std::memory_order_release);
    std::fprintf(stderr, "$dumplimit: dump file limit (%" PRIu64 " bytes) reached; dumping stopped\n", cap);
    return true;
}

// Format one record byte buffer to the output file. Reads only the record bytes
// and the read-only var table (no diff / shadow state), so it runs on the writer
// thread while the sim thread fills the next buffer.
void vcd_write_records(const std::vector<uint8_t>& buf) {
    size_t p = 0;
    while (p < buf.size()) {
        const uint8_t tag = buf[p++];
        if (tag == 1) {
            if (vcd_limit_hit.load(std::memory_order_relaxed) || vcd_limit_reached()) {
                return;   // capped: this instant and everything after it is dropped
            }
            uint64_t tick = 0;
            std::memcpy(&tick, &buf[p], sizeof(tick));
            p += sizeof(tick);
            vcd_put('#');
            vcd_put_dec(tick);
            vcd_put('\n');
        } else {
            uint32_t idx = 0, nw = 0;
            std::memcpy(&idx, &buf[p], sizeof(idx));
            p += sizeof(idx);
            std::memcpy(&nw, &buf[p], sizeof(nw));
            p += sizeof(nw);
            // Copy the (unaligned) planes into the aligned scratch (reserved to
            // 3*max_nwords at start, so no resize) before formatting.
            std::memcpy(vcd_writer_scratch.data(), &buf[p], static_cast<size_t>(3) * nw * 8);
            p += static_cast<size_t>(3) * nw * 8;
            const uint64_t* planes = vcd_writer_scratch.data();
            vcd_emit(g_vcd.vars[idx], planes, planes + nw, planes + 2 * nw);
        }
    }
}

// -- Writer thread --
//
// The sim thread fills g_vcd.recbuf and, at instant boundaries, hands a full
// buffer to the writer thread through a bounded pool: the sim thread never blocks
// on formatting or the disk unless the writer falls a whole pool behind
// (backpressure). Buffers are recycled, so steady state does not allocate. The
// var table and file are the only shared state: the table is read-only after
// rt_vcd_start, and after the thread spawns the file is written by the writer
// alone (the header is written earlier, on the sim thread, before the spawn).

constexpr size_t kVcdBuffers      = 8;             // record buffers in the pool
constexpr size_t kVcdHandoffBytes = 64u * 1024u;   // hand off once a buffer holds this many bytes

clotho::rt::OsThread               vcd_thread;
clotho::rt::OsMutex                vcd_mu;
clotho::rt::OsCond                 vcd_cv_ready;   // writer waits for filled buffers
clotho::rt::OsCond                 vcd_cv_free;    // sim waits for a recycled buffer
clotho::rt::OsCond                 vcd_cv_idle;    // $dumpflush waits for the writer to catch up
std::queue<std::vector<uint8_t>>   vcd_ready;      // filled buffers awaiting write
std::vector<std::vector<uint8_t>>  vcd_pool;       // recycled empty buffers
size_t                             vcd_inflight = 0;  // buffers handed off but not yet written
bool                               vcd_stop     = false;

void vcd_writer_loop() {
    for (;;) {
        std::vector<uint8_t> buf;
        {
            clotho::rt::OsLock lk(vcd_mu);
            vcd_cv_ready.wait(lk, [] { return !vcd_ready.empty() || vcd_stop; });
            if (vcd_ready.empty()) {
                return;   // stop requested and nothing left to write
            }
            buf = std::move(vcd_ready.front());
            vcd_ready.pop();
        }
        vcd_write_records(buf);   // format + write outside the lock
        {
            clotho::rt::OsLock lk(vcd_mu);
            buf.clear();
            vcd_pool.push_back(std::move(buf));
            --vcd_inflight;
            vcd_cv_free.notify_one();
            if (vcd_ready.empty() && vcd_inflight == 0) {
                vcd_cv_idle.notify_all();
            }
        }
    }
}

// Hand the current record buffer to the writer and take a recycled one. Blocks
// only if every pooled buffer is in flight (backpressure). A no-op on an empty
// buffer.
void vcd_handoff() {
    if (g_vcd.recbuf.empty()) {
        return;
    }
    clotho::rt::OsLock lk(vcd_mu);
    vcd_cv_free.wait(lk, [] { return !vcd_pool.empty(); });
    std::vector<uint8_t> next = std::move(vcd_pool.back());
    vcd_pool.pop_back();
    next.clear();
    vcd_ready.push(std::move(g_vcd.recbuf));
    ++vcd_inflight;
    g_vcd.recbuf = std::move(next);
    vcd_cv_ready.notify_one();
}

// Sim-thread side of the $dumplimit cap: has the writer reported the file full?
// It switches dumping off for the rest of the run -- a later $dumpon / $dumpall /
// $dumpvars does not restart it, since the file already carries its cutoff
// comment -- and drops the records still queued behind the cutoff.
bool vcd_capped() {
    if (!vcd_limit_hit.load(std::memory_order_acquire)) {
        return false;
    }
    g_vcd.active      = false;
    rt_vcd_active     = 0;
    g_vcd.pending_off = false;
    g_vcd.force_all   = false;
    g_vcd.recbuf.clear();
    return true;
}

} // namespace

extern "C" void rt_vcd_open(const char* path_buf, uint32_t path_len, uint32_t forward) {
    g_vcd.path = forward != 0 ? std::string(path_buf, path_len) : unpack_ascii(path_buf, path_len);
}

extern "C" void rt_vcd_begin(void* wheel, uint64_t tick_fs, int32_t units) {
    (void)units;
    if (g_vcd.file != nullptr) {
        return;   // a repeat $dumpvars: header already written (rt_vcd_start re-arms)
    }
    if (g_vcd.path.empty()) {
        g_vcd.path = "dump.vcd";   // LRM default when $dumpfile is omitted
    }
    g_vcd.file = std::fopen(g_vcd.path.c_str(), "wb");
    if (g_vcd.file == nullptr) {
        std::fprintf(stderr, "$dumpvars: cannot open '%s'\n", g_vcd.path.c_str());
        return;
    }
    g_vcd.iobuf   = static_cast<char*>(std::malloc(VcdEngine::iobuf_size));
    g_vcd.out_len = 0;
    // We buffer output ourselves in iobuf; make the FILE unbuffered so a bulk
    // fwrite of the rendered bytes goes straight to write() with no second copy.
    std::setvbuf(g_vcd.file, nullptr, _IONBF, 0);
    g_vcd.wheel = static_cast<rt_wheel*>(wheel);

    vcd_puts("$date\n\tClotho simulation\n$end\n");
    vcd_puts("$version\n\tClotho VCD dump\n$end\n");
    vcd_write_timescale(tick_fs);
}

extern "C" void rt_vcd_scope(const char* name, uint32_t name_len) {
    if (g_vcd.file == nullptr || g_vcd.started) {
        return;
    }
    vcd_puts("$scope module ");
    vcd_puts(std::string_view(name, name_len));
    vcd_puts(" $end\n");
}

extern "C" void rt_vcd_upscope(void) {
    if (g_vcd.file == nullptr || g_vcd.started) {
        return;
    }
    vcd_puts("$upscope $end\n");
}

// Write one $var line and add the variable to the scan table. Shared by the
// scalar (rt_vcd_var) and per-element array (rt_vcd_array) registration; every
// variable it adds is instrumented with rt_vcd_touch at its write sites, so the
// flush needs no per-variable scan policy.
static void vcd_register(std::string_view ref, uint32_t width, uint32_t flags, const uint8_t* mm,
                         const uint8_t* z) {
    VcdVar v;
    v.mm         = mm;
    v.z          = z;
    v.width      = width;
    v.nwords     = vcd_nwords(width);
    v.four_state = (flags & RT_VCD_FLAG_FOUR_STATE) != 0;
    v.narrow     = (flags & RT_VCD_FLAG_NARROW) != 0;
    v.is_real    = (flags & RT_VCD_FLAG_REAL) != 0;
    v.is_event   = (flags & RT_VCD_FLAG_EVENT) != 0;
    v.idcode     = vcd_idcode(g_vcd.next_id++);
    v.mm_bytes   = v.narrow ? (width + 7) / 8 : (2 * width + 7) / 8;
    v.z_bytes    = z != nullptr ? (width + 7) / 8 : 0;
    if (v.is_event) {
        // `mm` is the event's 64-bit last-trigger tick, not a value slot: diff all
        // eight bytes, and seed the shadow with the same never-triggered sentinel
        // the design state holds, so a trigger at tick 0 still reads as a change.
        // `seen` starts true for the same reason -- there is no power-on value to
        // dump, only occurrences.
        v.mm_bytes = sizeof(uint64_t);
        v.z_bytes  = 0;
        v.seen     = true;
        v.shadow.assign(v.mm_bytes, 0xFF);
    } else {
        v.shadow.assign(static_cast<size_t>(v.mm_bytes) + v.z_bytes, 0);
    }
    v.suffix     = width == 1 ? v.idcode + "\n" : " " + v.idcode + "\n";
    if (v.nwords > g_vcd.max_nwords) {
        g_vcd.max_nwords = v.nwords;
    }
    // A real declares var type `real`; its size field is meaningless to a reader
    // (the value is a decimal, not a bit string), so it is always 64 -- what
    // Verilator writes, and what a shortreal's widened double matches.
    // An event declares `event`, size 1: it has occurrences, not a value.
    const char* vtype = v.is_real                          ? "real"
                        : v.is_event                       ? "event"
                        : (flags & RT_VCD_FLAG_NET) != 0   ? "wire"
                                                           : "reg";
    vcd_puts("$var ");
    vcd_puts(vtype);
    vcd_put(' ');
    vcd_put_dec(v.is_real ? 64 : width);
    vcd_put(' ');
    vcd_puts(v.idcode);
    vcd_put(' ');
    vcd_puts(ref);
    vcd_puts(" $end\n");
    g_vcd.vars.push_back(std::move(v));
}

// Mirror of g_vcd.active exposed as a plain external byte (global scope so the
// symbol matches the JIT's external reference) so the JIT can inline the "is VCD
// dumping active?" check and skip the rt_vcd_touch call entirely when it is not.
extern "C" {
unsigned char rt_vcd_active = 0;
}

extern "C" void rt_vcd_var(const char* name, uint32_t name_len, uint32_t width, uint32_t flags,
                           const uint8_t* mm, const uint8_t* z) {
    if (g_vcd.file == nullptr || g_vcd.started) {
        return;
    }
    vcd_register(std::string_view(name, name_len), width, flags, mm, z);
}

extern "C" void rt_vcd_touch(uint32_t idx) {
    // idx >= epoch.size() covers a not-dumped NBA target passed as (uint32_t)-1.
    // Setting the flag is idempotent, so no dedup / list bookkeeping is needed.
    if (g_vcd.active && idx < g_vcd.epoch.size()) {
        g_vcd.epoch[idx] = 1;
    }
}

extern "C" void rt_vcd_touch_range(uint32_t base, uint32_t count) {
    if (!g_vcd.active) {
        return;
    }
    uint32_t end = base + count;
    if (end > g_vcd.epoch.size()) {
        end = static_cast<uint32_t>(g_vcd.epoch.size());
    }
    for (uint32_t i = base; i < end; ++i) {
        g_vcd.epoch[i] = 1;
    }
}

extern "C" void rt_vcd_array(const char* name, uint32_t name_len, uint32_t count, uint32_t width, uint32_t flags,
                             const uint8_t* mm_base, const uint8_t* z_base, uint32_t stride, int64_t left,
                             int64_t right) {
    if (g_vcd.file == nullptr || g_vcd.started) {
        return;
    }
    const std::string_view leaf(name, name_len);
    for (uint32_t k = 0; k < count; ++k) {
        // Storage index k maps to declared address left+k (ascending range) or
        // left-k (descending), matching the element-select normalization.
        const int64_t     addr = left <= right ? left + static_cast<int64_t>(k) : left - static_cast<int64_t>(k);
        const std::string ref  = std::string(leaf) + "[" + fmt::to_string(addr) + "]";
        const uint8_t*    mm   = mm_base + static_cast<size_t>(k) * stride;
        const uint8_t*    z    = z_base != nullptr ? z_base + static_cast<size_t>(k) * stride : nullptr;
        vcd_register(ref, width, flags, mm, z);
    }
}

extern "C" void rt_vcd_start(void) {
    if (g_vcd.file == nullptr) {
        return;
    }
    if (!g_vcd.started) {
        vcd_puts("$enddefinitions $end\n");
        // The scan scratch holds one variable's 3*nwords planes plus, in its tail,
        // the raw 2W-bit must|may read buffer (up to 2*max_nwords+1 words).
        g_vcd.scratch.assign(static_cast<size_t>(5) * g_vcd.max_nwords + 2, 0);
        // Dirty set: one epoch flag per var, set by rt_vcd_touch and scanned +
        // cleared in index order at flush.
        g_vcd.epoch.assign(g_vcd.vars.size(), 0);
        vcd_writer_scratch.reserve(static_cast<size_t>(3) * g_vcd.max_nwords);
        // Size every record buffer to hold a full handoff batch plus one more whole
        // instant, so the producer never allocates and never splits an instant
        // across buffers. A worst-case instant is a full-dump change for every
        // variable plus one time marker. Spawn the writer only after the header is
        // fully written on this (sim) thread, so file access never overlaps.
        size_t instant_bytes = 1 + sizeof(uint64_t);   // one time marker
        for (const VcdVar& v : g_vcd.vars) {
            instant_bytes += 1 + 2 * sizeof(uint32_t) + static_cast<size_t>(3) * v.nwords * 8;
        }
        const size_t cap = kVcdHandoffBytes + instant_bytes;
        g_vcd.recbuf.reserve(cap);
        vcd_pool.clear();
        for (size_t k = 0; k < kVcdBuffers; ++k) {
            std::vector<uint8_t> b;
            b.reserve(cap);
            vcd_pool.push_back(std::move(b));
        }
        vcd_stop      = false;
        vcd_inflight  = 0;
        if (!vcd_thread.start(vcd_writer_loop)) {
            format_fatal("$dumpvars: cannot start the VCD writer thread");
        }
        g_vcd.started = true;
    }
    if (vcd_capped()) {
        return;   // the cap was already reached: $dumpvars does not restart the dump
    }
    g_vcd.active    = true;
    rt_vcd_active   = 1;
    g_vcd.force_all = true;   // the first flush emits every variable's initial value
}

extern "C" void rt_vcd_flush(uint64_t tick) {
    if (g_vcd.file == nullptr || vcd_capped()) {
        return;
    }
    uint64_t* const scan  = g_vcd.scratch.data();
    const uint32_t  nvars = static_cast<uint32_t>(g_vcd.vars.size());
    if (g_vcd.pending_off) {
        // $dumpoff checkpoint: mark every variable all-x at this instant. An event
        // is skipped -- it has no value to make unknown, and `1<id>` here would
        // read as a trigger that never happened.
        const size_t cmark = g_vcd.recbuf.size();
        vcd_rec_time(tick);
        bool checkpointed = false;
        for (uint32_t i = 0; i < nvars; ++i) {
            VcdVar& v = g_vcd.vars[i];
            if (v.is_event) {
                g_vcd.epoch[i] = 0;
                continue;
            }
            const uint32_t nw = v.nwords;
            std::memset(scan, 0, static_cast<size_t>(3) * nw * 8);   // must = 0, z = 0
            for (uint32_t k = 0; k < nw; ++k) {
                scan[nw + k] = ~0ull;   // may = all ones (x)
            }
            scan[2 * nw - 1] &= vcd_topmask(v.width);
            vcd_rec_change(i, scan, nw);
            checkpointed = true;
            // The shadow (raw slot) is left stale: $dumpon re-arms a full dump
            // (force_all), which re-emits every variable and rewrites the shadow.
            v.seen         = true;
            g_vcd.epoch[i] = 0;
        }
        if (!checkpointed) {
            // Nothing to make unknown (a design whose only dumped variables are
            // events): drop the marker rather than write a bare `#tick`.
            g_vcd.recbuf.resize(cmark);
        }
        g_vcd.pending_off = false;
        g_vcd.active      = false;
        rt_vcd_active     = 0;
        vcd_handoff();
        return;
    }
    if (!g_vcd.active) {
        return;
    }
    // Produce a time marker followed by the changed variables; drop the marker if
    // nothing changed (no-change instants emit nothing). Scan in index order,
    // reading + diffing only the variables the dirty set marked. No per-instant
    // sort: index order is byte-identical to a full scan for free, and the epoch
    // check skips the expensive read/diff for the rest.
    const size_t mark = g_vcd.recbuf.size();
    vcd_rec_time(tick);
    bool any = false;
    if (g_vcd.force_all) {
        for (uint32_t i = 0; i < nvars; ++i) {
            const bool touched = g_vcd.epoch[i] != 0;
            g_vcd.epoch[i]     = 0;
            VcdVar& v          = g_vcd.vars[i];
            if (v.is_event) {
                // A forced dump ($dumpvars / $dumpall / $dumpon) states current
                // values, and an event has none -- it is emitted only when it
                // actually triggers. So pulse only if it triggered this instant,
                // and otherwise resync the shadow, which keeps a trigger that
                // happened while dumping was off from surfacing as a pulse here.
                if (touched) {
                    any |= vcd_scan_var(i, false);
                } else {
                    std::memcpy(v.shadow.data(), v.mm, v.mm_bytes);
                }
                continue;
            }
            any |= vcd_scan_var(i, true);
        }
    } else {
        // Hot loop over one dense byte array (no fat-struct access): scan a var
        // iff it was written this instant.
        uint8_t* ep = g_vcd.epoch.data();
        for (uint32_t i = 0; i < nvars; ++i) {
            if (ep[i] != 0) {
                ep[i] = 0;
                any |= vcd_scan_var(i, false);
            }
        }
    }
    if (!any) {
        g_vcd.recbuf.resize(mark);
    }
    g_vcd.force_all = false;
    // Hand the batch to the writer once it is large enough; small instants
    // accumulate to amortize the handoff. The remainder flushes at $dumpflush /
    // close.
    if (g_vcd.recbuf.size() >= kVcdHandoffBytes) {
        vcd_handoff();
    }
}

extern "C" void rt_vcd_off(void) {
    if (g_vcd.active) {
        g_vcd.pending_off = true;
    }
}

extern "C" void rt_vcd_on(void) {
    if (g_vcd.started && !vcd_capped()) {
        g_vcd.active    = true;
        rt_vcd_active   = 1;
        g_vcd.force_all = true;
    }
}

extern "C" void rt_vcd_all(void) {
    if (g_vcd.active) {
        g_vcd.force_all = true;
    }
}

extern "C" void rt_vcd_limit(uint64_t bytes) {
    // Legal before or after $dumpvars; the writer picks the new cap up at its next
    // instant boundary. A cap already passed is honoured there, not here, so the
    // cutoff comment is always written by the thread that owns the file.
    vcd_limit.store(bytes, std::memory_order_relaxed);
}

extern "C" void rt_vcd_dumpflush(void) {
    if (g_vcd.file == nullptr) {
        return;
    }
    // Hand off any buffered records and wait until the writer has drained
    // everything, then push the file's own buffer to the OS. Waiting until the
    // writer is idle means no write is in flight, so the fflush cannot race it.
    vcd_handoff();
    clotho::rt::OsLock lk(vcd_mu);
    vcd_cv_idle.wait(lk, [] { return vcd_ready.empty() && vcd_inflight == 0; });
    vcd_flush_out();   // writer is idle -> no concurrent append into iobuf
    std::fflush(g_vcd.file);
}

// Stop the writer thread and reset all threading state. Safe to call when no
// thread was ever spawned (a $dumpvars that never ran).
static void vcd_shutdown_writer() {
    if (vcd_thread.joinable()) {
        {
            clotho::rt::OsLock lk(vcd_mu);
            vcd_stop = true;
            vcd_cv_ready.notify_all();
        }
        vcd_thread.join();
    }
    while (!vcd_ready.empty()) {
        vcd_ready.pop();
    }
    vcd_pool.clear();
    vcd_inflight = 0;
    vcd_stop     = false;
    // The cap outlives the engine struct (both threads read it), so it resets here
    // with the rest of the cross-thread state.
    vcd_limit.store(0, std::memory_order_relaxed);
    vcd_limit_hit.store(false, std::memory_order_relaxed);
}

extern "C" void rt_vcd_close(void) {
    if (g_vcd.file == nullptr) {
        vcd_shutdown_writer();
        g_vcd = VcdEngine{};
        return;
    }
    if (g_vcd.active || g_vcd.pending_off) {
        const uint64_t now = g_vcd.wheel != nullptr ? unwrap(g_vcd.wheel)->now() : 0;
        rt_vcd_flush(now);
    }
    vcd_handoff();            // push the final partial buffer
    vcd_shutdown_writer();    // drain + join the writer before touching the file
    vcd_flush_out();          // writer joined -> flush the last rendered bytes
    std::fflush(g_vcd.file);
    std::fclose(g_vcd.file);
    std::free(g_vcd.iobuf);
    g_vcd = VcdEngine{};      // reset for the next simulation in this process
}
