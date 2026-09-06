#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cstdio>
#include <limits>
#include <numeric>
#include <thread>
#include <vector>
#include <cstring>

struct Timer {
    LARGE_INTEGER start;
    LARGE_INTEGER freq;

    Timer() {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }

    double elapsed_ms() const {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart - start.QuadPart) *
               1000.0 / static_cast<double>(freq.QuadPart);
    }

    double restart_ms() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsed =
            static_cast<double>(now.QuadPart - start.QuadPart) *
            1000.0 / static_cast<double>(freq.QuadPart);
        start = now;
        return elapsed;
    }
};

struct MappedFile {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    const uint8_t* data = nullptr;
    size_t size = 0;

    bool open(const wchar_t* path) {
        file = CreateFileW(
            path,
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL |
                FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr
        );

        if (file == INVALID_HANDLE_VALUE)
            return false;

        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(file, &sz) || sz.QuadPart < 0) {
            close();
            return false;
        }

        size = static_cast<size_t>(sz.QuadPart);

        mapping = CreateFileMappingW(
            file,
            nullptr,
            PAGE_READONLY,
            0,
            0,
            nullptr
        );

        if (!mapping) {
            close();
            return false;
        }

        data = static_cast<const uint8_t*>(
            MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)
        );

        if (!data) {
            close();
            return false;
        }

        return true;
    }

    void close() {
        if (data) {
            UnmapViewOfFile(data);
            data = nullptr;
        }

        if (mapping) {
            CloseHandle(mapping);
            mapping = nullptr;
        }

        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
        }

        size = 0;
    }

    ~MappedFile() {
        close();
    }
};

struct FileWriter {
    HANDLE file = INVALID_HANDLE_VALUE;

    bool open(const wchar_t* path) {
        file = CreateFileW(
            path,
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        return file != INVALID_HANDLE_VALUE;
    }

    void close() {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
        }
    }

    ~FileWriter() {
        close();
    }

    bool write_bytes(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);

        while (len) {
            DWORD chunk = static_cast<DWORD>(
                std::min<size_t>(len, 1u << 20)
            );

            DWORD written = 0;
            if (!WriteFile(file, p, chunk, &written, nullptr))
                return false;

            if (written != chunk)
                return false;

            p += written;
            len -= written;
        }

        return true;
    }

    bool write_u16(uint16_t v) {
        uint8_t b[2] = {
            static_cast<uint8_t>(v >> 8),
            static_cast<uint8_t>(v)
        };
        return write_bytes(b, 2);
    }

    bool write_u32(uint32_t v) {
        uint8_t b[4] = {
            static_cast<uint8_t>(v >> 24),
            static_cast<uint8_t>(v >> 16),
            static_cast<uint8_t>(v >> 8),
            static_cast<uint8_t>(v)
        };
        return write_bytes(b, 4);
    }
};

struct MemWriter {
    std::vector<uint8_t> data;

    void reserve(size_t n) {
        data.reserve(n);
    }

    bool write_bytes(const void* src, size_t len) {
        if (len == 0)
            return true;

        const uint8_t* p =
            static_cast<const uint8_t*>(src);

        try {
            data.insert(
                data.end(),
                p,
                p + len
            );
        } catch (...) {
            return false;
        }

        return true;
    }

    bool write_u8(uint8_t v) {
        try {
            data.push_back(v);
        } catch (...) {
            return false;
        }

        return true;
    }

    size_t size() const {
        return data.size();
    }

    const uint8_t* ptr() const {
        return data.empty()
            ? nullptr
            : data.data();
    }
};

static bool read_u16(const uint8_t*& p, const uint8_t* end, uint16_t& out) {
    if (static_cast<size_t>(end - p) < 2)
        return false;

    out = static_cast<uint16_t>(
        (static_cast<uint16_t>(p[0]) << 8) | p[1]
    );

    p += 2;
    return true;
}

static bool read_u32(const uint8_t*& p, const uint8_t* end, uint32_t& out) {
    if (static_cast<size_t>(end - p) < 4)
        return false;

    out =
        (static_cast<uint32_t>(p[0]) << 24) |
        (static_cast<uint32_t>(p[1]) << 16) |
        (static_cast<uint32_t>(p[2]) << 8) |
        static_cast<uint32_t>(p[3]);

    p += 4;
    return true;
}

static bool read_vlq(
    const uint8_t*& p,
    const uint8_t* end,
    uint32_t& out
) {
    out = 0;

    for (int i = 0; i < 4; ++i) {
        if (p >= end)
            return false;

        uint8_t b = *p++;
        out = (out << 7) | (b & 0x7F);

        if (!(b & 0x80))
            return true;
    }

    return false;
}

template <typename Writer>
static bool write_vlq(Writer& out, uint32_t v) {
    if (v > 0x0FFFFFFF)
        return false;

    uint8_t buf[4];
    int n = 1;

    buf[3] = static_cast<uint8_t>(v & 0x7F);
    v >>= 7;

    while (v) {
        buf[3 - n] = static_cast<uint8_t>((v & 0x7F) | 0x80);
        ++n;
        v >>= 7;
    }

    return out.write_bytes(buf + 4 - n, n);
}

enum class EventKind : uint8_t {
    Channel,
    Meta,
    SysEx,
    System
};

struct ParsedEvent {
    uint64_t tick = 0;
    uint32_t order = 0;

    uint8_t status = 0;
    uint8_t meta_type = 0;

    const uint8_t* data = nullptr;
    uint32_t data_len = 0;

    EventKind kind = EventKind::System;
};

template <typename Fn>
static bool scan_events(
    const uint8_t* begin,
    const uint8_t* end,
    Fn&& fn
) {
    const uint8_t* p = begin;

    uint64_t tick = 0;
    uint32_t order = 0;
    uint8_t running = 0;

    while (p < end) {
        uint32_t delta = 0;

        if (!read_vlq(p, end, delta))
            return false;

        tick += delta;

        if (p >= end)
            return false;

        uint8_t first = *p;
        uint8_t status;

        if (first < 0x80) {
            if (running < 0x80 || running >= 0xF0)
                return false;

            status = running;
        } else {
            status = *p++;
            running = (status < 0xF0) ? status : 0;
        }

        ParsedEvent ev;
        ev.tick = tick;
        ev.order = order++;
        ev.status = status;

        if (status == 0xFF) {
            if (p >= end)
                return false;

            ev.kind = EventKind::Meta;
            ev.meta_type = *p++;

            uint32_t len = 0;
            if (!read_vlq(p, end, len))
                return false;

            if (static_cast<size_t>(end - p) < len)
                return false;

            ev.data = p;
            ev.data_len = len;
            p += len;

            if (!fn(ev))
                return false;

            continue;
        }

        if (status == 0xF0 || status == 0xF7) {
            uint32_t len = 0;

            if (!read_vlq(p, end, len))
                return false;

            if (static_cast<size_t>(end - p) < len)
                return false;

            ev.kind = EventKind::SysEx;
            ev.data = p;
            ev.data_len = len;
            p += len;

            if (!fn(ev))
                return false;

            continue;
        }

        if (status >= 0xF0) {
            ev.kind = EventKind::System;

            switch (status) {
                case 0xF1:
                    ev.data_len = 1;
                    break;
                case 0xF2:
                    ev.data_len = 2;
                    break;
                case 0xF3:
                    ev.data_len = 1;
                    break;
                case 0xF6:
                    ev.data_len = 0;
                    break;
                case 0xF8:
                case 0xF9:
                case 0xFA:
                case 0xFB:
                case 0xFC:
                case 0xFD:
                case 0xFE:
                    ev.data_len = 0;
                    break;
                default:
                    return false;
            }

            if (static_cast<size_t>(end - p) < ev.data_len)
                return false;

            ev.data = p;
            p += ev.data_len;

            if (!fn(ev))
                return false;

            continue;
        }

        ev.kind = EventKind::Channel;

        uint8_t high = status & 0xF0;
        ev.data_len = (high == 0xC0 || high == 0xD0) ? 1 : 2;

        if (static_cast<size_t>(end - p) < ev.data_len)
            return false;

        ev.data = p;
        p += ev.data_len;

        if (!fn(ev))
            return false;
    }

    return true;
}

static bool is_global_meta(uint8_t type) {
    switch (type) {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x06:
        case 0x07:
        case 0x21:
        case 0x51:
        case 0x54:
        case 0x58:
        case 0x59:
        case 0x7F:
            return true;

        default:
            return false;
    }
}

struct Note {
    uint64_t start_tick = 0;
    uint64_t end_tick = 0;

    uint64_t start_us = 0;
    uint64_t end_us = 0;

    uint8_t velocity = 0;
    uint8_t pitch = 0;
    uint8_t channel = 0;

    int32_t prev_same = -1;
    int poly_at_on = 0;

    bool keep = false;
};

struct GlobalEvent {
    uint64_t tick = 0;
    uint32_t track = 0;
    uint32_t order = 0;

    uint8_t type = 0;
    const uint8_t* data = nullptr;
    uint32_t len = 0;
};

struct Segment {
    uint64_t start_us = 0;
    uint64_t end_us = 0;

    size_t begin = 0;
    size_t end = 0;

    double avg_velocity = 0.0;
    double first_avg = 0.0;
    double last_avg = 0.0;
    double trend = 0.0;

    double density = 0.0;
    double track_energy_share = 0.0;

    // Realtime-audibility model (see Config::audibility_ratio):
    // own_level       mean level of this track's held notes across the
    //                 segment window, in velocity units
    // ambient_level   mean held-voice level of everything sounding in
    //                 the window, including this track (intensity of
    //                 the moment)
    // audibility_ratio  own_level / (mean level of the *other* sounding
    //                 voices) - how the segment competes with its context
    double own_level = 0.0;
    double ambient_level = 0.0;
    double audibility_ratio = 0.0;

    int max_polyphony = 0;
    int max_velocity = 0;

    bool bimodal = false;

    enum class Mode {
        Drop,
        KeepAll,
        Cluster
    } mode = Mode::Drop;
};

struct TrackInfo {
    const uint8_t* data = nullptr;
    const uint8_t* end = nullptr;

    uint32_t index = 0;
    uint32_t size = 0;

    uint64_t max_tick = 0;

    size_t note_begin = 0;
    size_t note_end = 0;

    std::vector<uint32_t> order;
    std::vector<Segment> segments;

    bool has_notes = false;
    size_t kept_notes = 0;

    double raw_average_velocity = 0.0;
    uint8_t raw_max_velocity = 0;
};

struct Config {
    double low_velocity = 30.0;
    double high_velocity = 70.0;
    int peak_velocity = 100;

    // Segmentation targets a fixed wall-clock duration per segment,
    // with every boundary snapped to a dyadic length: a power-of-two
    // division of a measure (half-bar, quarter-bar, ...) or a
    // power-of-two multiple of a measure (2, 4, 8, ... bars),
    // whichever lands closest to the target. Segment duration is
    // therefore tempo-independent (double/quadruple/octuple-time art
    // sections no longer skew it), while boundaries always land on
    // musically meaningful positions. The 2.0 s default equals one
    // measure at the 120 BPM base tempo common to most songs.
    double segment_seconds = 2.0;

    // Snap grid = measure / 2^snap_divisions (2 = quarter-bar, which
    // is a beat in 4/4; 0 = bar lines only).
    int snap_divisions = 2;

    double mask_share = 0.01;

    // Used as a tiebreaker signal for ambiguous (Cluster) segments:
    // a segment whose velocity ramps up sharply within itself is
    // treated as a signal of internal art/melody mixing, the same
    // way a bimodal velocity histogram or an isolated peak note is.
    double trend_threshold = 12.0;

    uint64_t global_bin_ms = 50;

    double bimodal_fraction = 0.10;

    bool verbose = false;
    bool move_global_events = true;

    // Issue a PrefetchVirtualMemory warm-up over the mapped file before
    // Phase 1b. Phase 1b is the first phase that touches every byte of
    // every track; on a cold cache (file not read in days/weeks) that
    // read is demand-paged one fault at a time, interleaved with parse
    // work and easily starved by Defender/cloud placeholders. One
    // sequential prefetch turns the cold read into a single fast burst.
    bool prefetch = true;

    // Worker threads for the per-track scanning phases. 0 = use all
    // hardware threads (capped at 64); 1 = fully sequential. Output
    // is identical for any thread count.
    int threads = 0;

    // Single user-facing tau in milliseconds for the exponential
    // decay model (the only energy model). If <= 0, the internal
    // default of 500 ms is used.
    double tau_ms = 0.0;

    // Concurrent-voice ceiling applied to the global energy curve
    // during Phase 2a, approximating a real synth's voice stealing /
    // polyphony limiting. The additive envelope model has no such
    // ceiling on its own: it sums a fixed decayed weight per active
    // note with no bound on how many notes can contribute at once,
    // so a dense crash's thousands of held/decaying notes can inflate
    // the global average well past what any real instrument would
    // ever actually be sounding - which was the cause of legitimate
    // moderate-velocity content right after a crash getting cut off.
    // Set to 0 or negative to disable damping entirely (revert to
    // the unbounded additive model).
    int global_polyphony_cap = 0;

    // ------------------------------------------------------------------
    // Realtime-audibility model. For every segment the filter now also
    // asks "how loud is this segment's own content compared to what
    // else is sounding at that moment, after the piano-decay tail and
    // the synth's voice limit have been taken into account?" That
    // ratio, not the absolute velocity band alone, is what decides
    // keep/drop for content the absolute bands leave ambiguous, and it
    // is what makes the removal scale vary with the intensity of the
    // moment: the same velocity is buried during a crash and clearly
    // audible in a quiet passage.
    //
    // audibility_ratio: a segment whose own mean held-voice level is at
    // least this fraction of the level of the *other* simultaneously
    // sounding voices is considered audible and kept (mid band), or
    // rescued from the low band when the local context carries real
    // melodic content (see ambient_gate). The ratio compares per-voice
    // mean levels, so it is polyphony-invariant: in dense files a spam
    // track's per-voice level matches the mass sounding around it
    // (ratio ~ 1), while real melodic content sits clearly above it.
    // The default 1.25 (~ +2 dB) therefore separates spam from melody
    // in dense files; 0.30 (~ -10 dB) suits sparse files better.
    // Windows where no *other* voices are sounding are anchored to
    // the file's typical sounding level instead of being granted a
    // free ratio (see the solo-window logic in build_track_segments),
    // so a self-dominated art track only survives when its own level
    // is a real fraction of what the file actually sounds like.
    // Set to 0 to keep everything that is not caught by the absolute
    // bands. The informative range is roughly 0.3-10: per-voice mean
    // levels are bounded by 127 and compressed by the decay model, so
    // beyond ~8-10 the statistic saturates (and the hardcoded
    // solo-window fallback ratio is 8.0).
    // ------------------------------------------------------------------
    double audibility_ratio = 1.25;

    // ambient_gate: the low-band rescue additionally requires the local
    // ambient intensity (mean held-voice level, including the segment's
    // own track) to be at least this fraction of the file's typical
    // sounding level, so quiet layers are only rescued where the
    // section around them genuinely carries melodic content - art
    // passages dominated by their own quiet notes do not pass the gate
    // and stay dropped. The default 1.0 rescues only where the local
    // ambience is at least as loud as the file's typical sounding
    // level (p80): values below 1 pass at spam-level ambience (quiet
    // art gets rescued everywhere), while values above ~2 approach the
    // dead zone - per-voice mean levels are bounded by 127, so no
    // window can pass a gate much above 127 / ambient_typical.
    // A narrow silence notch is exempt from the gate (see the decision
    // logic): once a crash tail has decayed to near-silence there is
    // nothing left to mask anything, and quiet content after a crash
    // is exactly what must not be removed.
    double ambient_gate = 1.0;
};

struct TempoSegment {
    uint64_t tick = 0;
    long double cumulative_us = 0;
    uint32_t us_per_qn = 500000;
};

struct TempoMap {
    bool smpte = false;
    long double us_per_tick = 0;
    uint16_t division = 480;

    std::vector<TempoSegment> segments;

    uint64_t tick_to_us(uint64_t tick) const {
        if (smpte) {
            long double value =
                static_cast<long double>(tick) * us_per_tick;

            if (value >= static_cast<long double>(
                std::numeric_limits<uint64_t>::max()
            ))
                return std::numeric_limits<uint64_t>::max();

            return static_cast<uint64_t>(value + 0.5L);
        }

        if (segments.empty())
            return static_cast<uint64_t>(
                static_cast<long double>(tick) * 500000.0L / division
            );

        auto it = std::upper_bound(
            segments.begin(),
            segments.end(),
            tick,
            [](uint64_t t, const TempoSegment& s) {
                return t < s.tick;
            }
        );

        if (it == segments.begin())
            it = segments.begin();
        else
            --it;

        const TempoSegment& s = *it;

        long double result =
            s.cumulative_us +
            static_cast<long double>(tick - s.tick) *
            static_cast<long double>(s.us_per_qn) /
            static_cast<long double>(division);

        if (result >= static_cast<long double>(
            std::numeric_limits<uint64_t>::max()
        ))
            return std::numeric_limits<uint64_t>::max();

        return static_cast<uint64_t>(result + 0.5L);
    }
};

// Monotonic tick->microsecond converter. MIDI ticks never decrease
// within a track, so instead of re-running a binary search over the
// tempo segments for every note (TempoMap::tick_to_us), a cursor
// advances the segment index forward only. Produces the same values
// as TempoMap::tick_to_us for non-decreasing tick sequences; the
// caller must create one cursor per track (ticks restart at 0).
struct TempoCursor {
    const TempoMap* map = nullptr;
    size_t idx = 0;

    explicit TempoCursor(const TempoMap& m) : map(&m) {}

    uint64_t to_us(uint64_t tick) {
        if (map->smpte || map->segments.size() < 2)
            return map->tick_to_us(tick);

        while (
            idx + 1 < map->segments.size() &&
            map->segments[idx + 1].tick <= tick
        ) {
            ++idx;
        }

        const TempoSegment& s = map->segments[idx];

        long double result =
            s.cumulative_us +
            static_cast<long double>(tick - s.tick) *
            static_cast<long double>(s.us_per_qn) /
            static_cast<long double>(map->division);

        if (result >= static_cast<long double>(
            std::numeric_limits<uint64_t>::max()
        ))
            return std::numeric_limits<uint64_t>::max();

        return static_cast<uint64_t>(result + 0.5L);
    }
};

// NOTE: `globals` must already be sorted by (tick, track, order) —
// wmain sorts it once immediately before calling this function. It is
// taken by const reference now that the redundant re-copy/re-sort
// inside this function is gone.
static TempoMap build_tempo_map(
    uint16_t division,
    const std::vector<GlobalEvent>& globals
) {
    TempoMap map;
    map.division = division;

    if (static_cast<int16_t>(division) < 0) {
        map.smpte = true;

        int8_t high = static_cast<int8_t>(division >> 8);
        int fps = -static_cast<int>(high);
        int ticks_per_frame = division & 0xFF;

        if (fps <= 0)
            fps = 30;

        if (ticks_per_frame <= 0)
            ticks_per_frame = 1;

        map.us_per_tick =
            1000000.0L /
            (static_cast<long double>(fps) *
             static_cast<long double>(ticks_per_frame));

        return map;
    }

    map.smpte = false;

    size_t tempo_event_count = 0;

    for (const auto& g : globals) {
        if (g.type == 0x51)
            ++tempo_event_count;
    }

    std::printf(
        "[Phase 1b] Found %zu Set Tempo (0x51) events.\n",
        tempo_event_count
    );
    fflush(stdout);

    // (globals arrives pre-sorted from wmain; the old copy + re-sort
    // here was redundant.)

    uint64_t current_tick = 0;
    uint32_t current_tempo = 500000;
    long double cumulative = 0;

    map.segments.push_back({
        0,
        0,
        current_tempo
    });

    for (const auto& ev : globals) {
        if (ev.type != 0x51 || ev.len != 3)
            continue;

        uint32_t tempo =
            (static_cast<uint32_t>(ev.data[0]) << 16) |
            (static_cast<uint32_t>(ev.data[1]) << 8) |
            static_cast<uint32_t>(ev.data[2]);

        if (tempo == 0)
            continue;

        if (ev.tick > current_tick) {
            cumulative +=
                static_cast<long double>(ev.tick - current_tick) *
                static_cast<long double>(current_tempo) /
                static_cast<long double>(division);

            current_tick = ev.tick;
        }

        if (!map.segments.empty() &&
            map.segments.back().tick == ev.tick) {
            map.segments.back().cumulative_us = cumulative;
            map.segments.back().us_per_qn = tempo;
        } else {
            map.segments.push_back({
                ev.tick,
                cumulative,
                tempo
            });
        }

        current_tempo = tempo;
    }

    return map;
}

// Measure/bar-line map, built from 0x58 (time signature) meta events.
// This gives segmentation a musically meaningful grid to snap to,
// instead of a fixed-duration window with a small rest-snap
// tolerance. Dense art/spam passages in Black MIDI content are
// authored on bar boundaries, so cutting on measures (rather than
// on detected rests within a narrow time window) keeps segment
// edges from bleeding stray notes from adjacent passages.
struct TimeSigSegment {
    uint64_t tick = 0;
    uint64_t measure_index = 0;
    uint64_t ticks_per_measure = 1920;
};

struct MeasureMap {
    std::vector<TimeSigSegment> segments;

    void build(
        uint16_t division,
        const std::vector<GlobalEvent>& sorted_globals
    ) {
        segments.clear();

        uint64_t default_tpm =
            static_cast<uint64_t>(division) * 4;

        if (default_tpm == 0)
            default_tpm = 1920;

        segments.push_back({0, 0, default_tpm});

        uint64_t current_tick = 0;
        uint64_t current_measures = 0;
        uint64_t current_tpm = default_tpm;

        for (const auto& ev : sorted_globals) {
            if (ev.type != 0x58 || ev.len < 2)
                continue;

            uint8_t numerator = ev.data[0];
            uint8_t denom_pow = ev.data[1];

            if (numerator == 0)
                continue;

            uint64_t denom = 1ull << denom_pow;

            if (denom == 0)
                denom = 4;

            uint64_t tpm =
                (static_cast<uint64_t>(division) * 4 *
                 static_cast<uint64_t>(numerator)) / denom;

            if (tpm == 0)
                tpm = default_tpm;

            if (ev.tick > current_tick && current_tpm > 0) {
                current_measures +=
                    (ev.tick - current_tick) / current_tpm;
            }

            current_tick = ev.tick;

            if (!segments.empty() &&
                segments.back().tick == ev.tick) {
                segments.back().ticks_per_measure = tpm;
                segments.back().measure_index = current_measures;
            } else {
                segments.push_back({
                    ev.tick,
                    current_measures,
                    tpm
                });
            }

            current_tpm = tpm;
        }
    }

    // Returns the time-signature segment governing `tick` (the last
    // segment whose tick is <= tick; segments[0].tick is always 0).
    const TimeSigSegment* segment_for_tick(uint64_t tick) const {
        if (segments.empty())
            return nullptr;

        size_t lo = 0;
        size_t hi = segments.size();

        while (lo + 1 < hi) {
            size_t mid = lo + (hi - lo) / 2;

            if (segments[mid].tick <= tick)
                lo = mid;
            else
                hi = mid;
        }

        return &segments[lo];
    }
};

// Number of worker threads to spawn for the per-track scan phases.
static unsigned worker_count(int requested);

// Run fn(worker_id) on worker_count(requested) threads and join.
// fn must be callable with a single unsigned argument. Worker 0 runs
// on the calling thread, so nothing is spawned for --threads 1.
template <typename Fn>
static void run_workers(int requested, Fn&& fn);

// Energy fixed-point scale (integer arithmetic for speed/predictability)
static constexpr int64_t ENERGY_SCALE = 1000LL;

struct GlobalStats {
    std::vector<int64_t> diff;      // fixed-point scaled by ENERGY_SCALE
    std::vector<int64_t> energy;    // fixed-point scaled by ENERGY_SCALE
    std::vector<long double> prefix; // prefix integral in scaled units * microseconds

    uint64_t bin_us = 50000;

    // ------------------------------------------------------------------
    // Voice-model curves (envelope-independent, exponential piano decay).
    //
    // These model what is *actually sounding* at each instant, which is
    // the quantity a realtime synth's voice limiting acts on:
    //
    //   held_count[b]   number of notes whose [start,end) span covers bin b
    //                   (a held note occupies a synth voice until note-off)
    //
    //   held_energy[b]  ENERGY_SCALE-scaled sum of the current decayed
    //                   levels of exactly those held notes. Each note
    //                   injects velocity at note-on, decays by tau while
    //                   held, and subtracts its remaining level at
    //                   note-off (the damper stops the string).
    //
    //   ambient = held_energy / held_count / ENERGY_SCALE
    //                   the mean per-voice level of what is sounding at
    //                   bin b, in plain velocity units (0..127). This is
    //                   the "intensity of the moment" that new notes
    //                   compete against.
    //
    // Both curves are built from one paired note scan with two diff
    // arrays and one O(1)-per-bin recurrence each: O(notes + bins) time
    // and O(bins) memory, with no per-note structures kept alive. This
    // replaces the previous top-K simulation, which held every note of
    // the file in two ordered sets and quadrupled runtime on large
    // Black MIDIs.
    // ------------------------------------------------------------------
    std::vector<int64_t> held_count;
    std::vector<int64_t> held_energy;
    std::vector<long double> held_energy_prefix; // integral, scaled units * us
    std::vector<long double> held_count_prefix;  // integral, notes * us

    long double voice_tau_us = 0;

    // Robust "typical sounding level" of the file as a whole: the p80
    // percentile of the per-bin ambient level over bins where anything
    // is held. A high percentile is used because art passages dominate
    // *duration* in most Black MIDIs, so a mean would collapse to the
    // art's own level and the local-intensity comparisons below would
    // lose their anchor.
    double ambient_typical = 0.0;

    //Forward declaration of build_from_scan function because circular dependency lol
    void build_from_scan(
        const uint8_t* file_begin,
        const uint8_t* file_end,
        uint16_t num_tracks,
        const TempoMap& tempo,
        const Config& cfg
    );
    
    long double integral(uint64_t t) const {
        if (energy.empty() || t == 0)
            return 0.0L;

        size_t bin = static_cast<size_t>(t / bin_us);

        if (bin >= energy.size())
            return prefix.back();

        uint64_t bin_start =
            static_cast<uint64_t>(bin) * bin_us;

        // prefix stores scaled-energy * bin_us integrated up to bin start
        long double res = prefix[bin] +
            static_cast<long double>(energy[bin]) *
            static_cast<long double>(t - bin_start);

        // return scaled units (we keep scaling here; callers must divide by ENERGY_SCALE if needed)
        return res;
    }

    double average(uint64_t a, uint64_t b) const {
        if (b <= a || energy.empty())
            return 0.0;

        long double amount = integral(b) - integral(a); // scaled-energy * microseconds
        long double duration = static_cast<long double>(b - a); // microseconds

        // Convert to "scaled energy per microsecond" then to normal units by dividing ENERGY_SCALE
        long double avg_scaled_per_us = amount / duration;
        long double avg = static_cast<long double>(avg_scaled_per_us) / static_cast<long double>(ENERGY_SCALE);

        return static_cast<double>(avg);
    }

    // Integral of the summed held-voice level up to time t
    // (scaled units * microseconds).
    long double held_level_integral(uint64_t t) const {
        if (held_energy.empty() || t == 0)
            return 0.0L;

        size_t bin = static_cast<size_t>(t / bin_us);

        if (bin >= held_energy.size())
            return held_energy_prefix.back();

        uint64_t bin_start = static_cast<uint64_t>(bin) * bin_us;

        return held_energy_prefix[bin] +
            static_cast<long double>(held_energy[bin]) *
            static_cast<long double>(t - bin_start);
    }

    // Integral of the held-voice count up to time t (notes * microseconds).
    long double held_count_integral(uint64_t t) const {
        if (held_count.empty() || t == 0)
            return 0.0L;

        size_t bin = static_cast<size_t>(t / bin_us);

        if (bin >= held_count.size())
            return held_count_prefix.back();

        uint64_t bin_start = static_cast<uint64_t>(bin) * bin_us;

        return held_count_prefix[bin] +
            static_cast<long double>(held_count[bin]) *
            static_cast<long double>(t - bin_start);
    }

    // Mean summed level of the currently-held voices over [a, b),
    // in plain velocity units. This is the "numerator" of the ambient
    // level: (summed level) / (held count) gives the per-voice mean.
    double held_level_mean(uint64_t a, uint64_t b) const {
        if (b <= a || held_energy.empty())
            return 0.0;

        long double amount =
            held_level_integral(b) - held_level_integral(a);

        return static_cast<double>(
            amount /
            static_cast<long double>(b - a) /
            static_cast<long double>(ENERGY_SCALE)
        );
    }

    // Mean number of simultaneously-held voices over [a, b).
    double held_count_mean(uint64_t a, uint64_t b) const {
        if (b <= a || held_count.empty())
            return 0.0;

        long double amount =
            held_count_integral(b) - held_count_integral(a);

        return static_cast<double>(
            amount / static_cast<long double>(b - a)
        );
    }

};

// ------------------------------------------------------------------
// Exponential voice-model builders.
//
// All curves are computed from diff arrays accumulated during the
// paired note scan, rolled forward with one O(1)-per-bin recurrence
// each: O(notes) scan time, O(bins) memory, and no per-note structure
// kept alive after the scan. The previous top-K simulation reproduced
// similar curves but held one node per note in two ordered sets and
// recomputed exp/log on every promotion, which is what exploded memory
// and quadrupled runtime on extreme Black MIDIs.
// ------------------------------------------------------------------

static constexpr long double EXP_DECAY_FLOOR = 0.01L; // ~1% of onset level

// Number of bins a voice keeps "contributing" for damping purposes:
// the time for its level to decay to EXP_DECAY_FLOOR of its onset.
static size_t significant_voice_bins(uint64_t bin_us, long double tau_us) {
    if (tau_us <= 0.0L || bin_us == 0)
        return 1;

    long double bins =
        -std::log(EXP_DECAY_FLOOR) * tau_us /
        static_cast<long double>(bin_us);

    size_t result = static_cast<size_t>(bins + 0.5L);

    return result < 1 ? 1 : result;
}

// Build energy[]/prefix[] for the exponential model: the sum of every
// voice's exponentially-decayed velocity, damped where the number of
// still-significant voices exceeds the synth's polyphony cap. The
// damping factor cap/contributing_voices is the equal-level
// approximation of top-K voice stealing: when more voices are sounding
// than the cap allows, only cap voices' worth of the summed level can
// actually be heard, so a crash's thousands of decayed voices cannot
// inflate the global average past what a real instrument would output.
static void build_energy_exponential_capped(
    GlobalStats& gs,
    const std::vector<int64_t>& contrib_diff,
    int polyphony_cap,
    long double tau_us,
    size_t& damped_bins
) {
    const size_t bins = gs.diff.size();

    gs.energy.assign(bins, 0);
    gs.prefix.assign(bins + 1, 0.0L);

    if (bins == 0)
        return;

    const long double bin_us_ld =
        static_cast<long double>(gs.bin_us);

    const long double decay =
        tau_us > 0.0L
            ? std::exp(-bin_us_ld / tau_us)
            : 0.0L;

    long double acc = 0.0L;
    int64_t contrib = 0;

    gs.prefix[0] = 0.0L;

    for (size_t i = 0; i < bins; ++i) {
        acc = acc * decay +
              static_cast<long double>(gs.diff[i]);

        if (i < contrib_diff.size())
            contrib += contrib_diff[i];

        long double e = acc;

        if (polyphony_cap > 0 && contrib > polyphony_cap) {
            e *= static_cast<long double>(polyphony_cap) /
                 static_cast<long double>(contrib);

            ++damped_bins;
        }

        if (e > static_cast<long double>(
            std::numeric_limits<int64_t>::max()))
            gs.energy[i] = std::numeric_limits<int64_t>::max();
        else if (e < 0.0L)
            gs.energy[i] = 0;
        else
            gs.energy[i] = static_cast<int64_t>(e);

        gs.prefix[i + 1] = gs.prefix[i] +
            static_cast<long double>(gs.energy[i]) * bin_us_ld;
    }
}

// Roll the held-voice diff arrays into held_count[]/held_energy[] plus
// their prefix integrals, then derive the file-wide ambient anchor
// (p80 of the per-bin mean held level over bins where anything is
// held). held_energy[b] is the summed decayed level of exactly the
// notes whose [start,end) span covers bin b: each note injects its
// velocity at note-on, decays by tau while held, and subtracts its
// remaining level at note-off.
static void build_voice_model_curves(
    GlobalStats& gs,
    const std::vector<int64_t>& held_count_diff,
    const std::vector<int64_t>& held_energy_diff,
    long double tau_us
) {
    const size_t bins = held_count_diff.size();

    gs.held_count.assign(bins, 0);
    gs.held_energy.assign(bins, 0);
    gs.held_count_prefix.assign(bins + 1, 0.0L);
    gs.held_energy_prefix.assign(bins + 1, 0.0L);
    gs.voice_tau_us = tau_us;

    if (bins == 0)
        return;

    const long double bin_us_ld =
        static_cast<long double>(gs.bin_us);

    const long double decay =
        tau_us > 0.0L
            ? std::exp(-bin_us_ld / tau_us)
            : 0.0L;

    int64_t count_running = 0;
    long double level_running = 0.0L;

    std::vector<double> ambient_samples;
    ambient_samples.reserve(bins);

    for (size_t i = 0; i < bins; ++i) {
        count_running += held_count_diff[i];

        if (count_running < 0)
            count_running = 0;

        gs.held_count[i] = count_running;

        level_running = level_running * decay +
            static_cast<long double>(held_energy_diff[i]);

        if (level_running < 0.0L)
            level_running = 0.0L;

        if (level_running > static_cast<long double>(
            std::numeric_limits<int64_t>::max()))
            gs.held_energy[i] = std::numeric_limits<int64_t>::max();
        else
            gs.held_energy[i] = static_cast<int64_t>(level_running);

        gs.held_count_prefix[i + 1] =
            gs.held_count_prefix[i] +
            static_cast<long double>(gs.held_count[i]) * bin_us_ld;

        gs.held_energy_prefix[i + 1] =
            gs.held_energy_prefix[i] +
            static_cast<long double>(gs.held_energy[i]) * bin_us_ld;

        if (gs.held_count[i] > 0) {
            ambient_samples.push_back(
                static_cast<double>(gs.held_energy[i]) /
                static_cast<double>(gs.held_count[i]) /
                static_cast<double>(ENERGY_SCALE)
            );
        }
    }

    if (!ambient_samples.empty()) {
        size_t idx = static_cast<size_t>(
            static_cast<double>(ambient_samples.size() - 1) * 0.80
        );

        std::nth_element(
            ambient_samples.begin(),
            ambient_samples.begin() + idx,
            ambient_samples.end()
        );

        gs.ambient_typical = ambient_samples[idx];
    }
}

static long double select_tau_us(const Config& cfg) {
    // User-provided tau if given, otherwise a single internal default.
    double tau_ms = cfg.tau_ms > 0.0 ? cfg.tau_ms : 500.0;

    return static_cast<long double>(tau_ms * 1000.0L); // ms -> us
}

void GlobalStats::build_from_scan(
    const uint8_t* file_begin,
    const uint8_t* file_end,
    uint16_t num_tracks,
    const TempoMap& tempo,
    const Config& cfg
) {
    // Set bin size in microseconds
    bin_us = std::max<uint64_t>(1, cfg.global_bin_ms * 1000);

    std::printf("[Phase 2a] Scanning all tracks for global energy (paired note scan)...\n");
    fflush(stdout);

    // Scan a single track: stacked active notes per channel*128 slot,
    // invoking note_cb(start_us, end_us, velocity) for every completed
    // note. All state is local to this call, so it is safe to run on
    // any worker thread.
    auto scan_one_track =
        [](
            const uint8_t* tdata,
            const uint8_t* tend,
            const TempoMap& t,
            auto&& note_cb
        ) {
        // Active stacks: head per channel*128, -1 = empty
        std::array<int32_t, 16 * 128> head;
        head.fill(-1);

        struct ActiveNode {
            uint64_t start_us;
            uint8_t velocity;
            // While active: previous node in the per-slot stack.
            // Once popped: next node in the free list.
            int32_t prev;
        };
        std::vector<ActiveNode> nodes;
        nodes.reserve(1024);

        // Free list: popped nodes recycle their slots (the `prev`
        // field doubles as the next-free link once a node is dead),
        // so memory tracks peak polyphony instead of the total
        // number of note-ons in the file.
        int32_t free_head = -1;
        int active_notes = 0;

        auto push_active = [&](size_t idx, uint64_t start_us, uint8_t vel) {
            int32_t id;

            if (free_head >= 0) {
                id = free_head;
                free_head = nodes[id].prev;
            } else {
                id = static_cast<int32_t>(nodes.size());
                nodes.push_back(ActiveNode{});
            }

            nodes[id].start_us = start_us;
            nodes[id].velocity = vel;
            nodes[id].prev = head[idx];
            head[idx] = id;
            ++active_notes;
        };

        auto pop_active = [&](size_t idx) -> int32_t {
            int32_t id = head[idx];
            if (id < 0) return -1;
            head[idx] = nodes[id].prev;
            nodes[id].prev = free_head;
            free_head = id;
            --active_notes;
            return id;
        };

        // Ticks restart at 0 for every track, so a fresh tempo cursor.
        TempoCursor tcur(t);

        const uint8_t* tp = tdata;
        uint64_t tick = 0;
        uint8_t running = 0;

        while (tp < tend) {
                uint32_t delta = 0;
                if (!read_vlq(tp, tend, delta))
                    break;

                tick += delta;

                if (tp >= tend)
                    break;

                uint8_t first = *tp;
                uint8_t status;

                if (first < 0x80) {
                    if (running < 0x80 || running >= 0xF0)
                        break;
                    status = running;
                } else {
                    status = *tp++;
                    running = (status < 0xF0) ? status : 0;
                }

                if (status == 0xFF) {
                    if (tp >= tend) break;
                    tp++; // meta type
                    uint32_t len = 0;
                    if (!read_vlq(tp, tend, len)) break;
                    if (tp + len > tend) break;
                    tp += len;
                    continue;
                }

                if (status == 0xF0 || status == 0xF7) {
                    uint32_t len = 0;
                    if (!read_vlq(tp, tend, len)) break;
                    if (tp + len > tend) break;
                    tp += len;
                    continue;
                }

                if (status >= 0xF0) {
                    uint8_t len_map[] = {0,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0};
                    int len = len_map[status & 0x0F];
                    if (tp + len > tend) break;
                    tp += len;
                    continue;
                }

                // Channel message
                uint8_t high = status & 0xF0;
                uint8_t channel = status & 0x0F;
                int data_len = (high == 0xC0 || high == 0xD0) ? 1 : 2;

                if (tp + data_len > tend) break;

                if (high == 0x90) {
                    uint8_t pitch = tp[0];
                    uint8_t vel = tp[1];

                    uint64_t start_us = tcur.to_us(tick);

                    if (vel != 0) {
                        // Note-on: push a new active node (stack)
                        size_t idx = static_cast<size_t>(channel) * 128 + pitch;
                        push_active(idx, start_us, vel);
                    } else {
                        // Running status note-on with vel==0 => note-off: pop the most recent node
                        size_t idx = static_cast<size_t>(channel) * 128 + pitch;
                        int32_t node_id = pop_active(idx);
                        if (node_id >= 0) {
                            uint64_t note_start_us = nodes[node_id].start_us;
                            uint8_t note_vel = nodes[node_id].velocity;
                            uint64_t note_end_us = start_us; // current tick's time

                            note_cb(note_start_us, note_end_us, note_vel);
                        }
                    }
                } else if (high == 0x80) {
                    // Note-off (release velocity in tp[1] is ignored)
                    uint8_t pitch = tp[0];
                    uint64_t end_us = tcur.to_us(tick);

                    size_t idx = static_cast<size_t>(channel) * 128 + pitch;
                    int32_t node_id = pop_active(idx);
                    if (node_id >= 0) {
                        uint64_t note_start_us = nodes[node_id].start_us;
                        uint8_t note_vel = nodes[node_id].velocity;
                        uint64_t note_end_us = end_us;

                        note_cb(note_start_us, note_end_us, note_vel);
                    }
                } else {
                    // Other channel messages: skip data bytes
                }

                tp += data_len;
            }

            // At end of track, finalize any still-active notes (treat end as last tick time)
            uint64_t track_end_us = tcur.to_us(tick);

            if (active_notes > 0) {
                for (size_t ch = 0; ch < 16; ++ch) {
                    for (size_t pch = 0; pch < 128; ++pch) {
                        size_t idx = ch * 128 + pch;
                        int32_t node_id;
                        while ((node_id = head[idx]) >= 0) {
                            // pop manually to avoid modifying head while iterating
                            head[idx] = nodes[node_id].prev;
                            --active_notes;
                            uint64_t note_start_us = nodes[node_id].start_us;
                            uint8_t note_vel = nodes[node_id].velocity;
                            note_cb(note_start_us, track_end_us, note_vel);
                        }
                    }
                }
            }

    };

    // -------------------------
    // Preliminary pass (also the only pass)
    // -------------------------

    // Clear any existing diff (fresh run)
    diff.clear();
    energy.clear();
    prefix.clear();

    // Polyphony ceiling model: real synths cap concurrent voices via
    // voice stealing/limiting, but the additive envelope model above
    // has no such ceiling of its own - it sums a fixed decayed
    // weight per currently-active note with no bound on how many
    // notes can contribute simultaneously. A dense crash's thousands
    // of held/decaying notes can therefore inflate the global
    // average well past what any real instrument would ever actually
    // be sounding, which is what was causing legitimate moderate-
    // velocity content played right after a crash to get cut off.
    //
    // poly_diff/poly track the *actual* concurrent note count -
    // unweighted by velocity or envelope, a note is simply "active"
    // for its literal [start_us, end_us) span - using the same
    // O(1)-per-note diff-array technique already used for the
    // weighted energy curve above. This is built once, during the
    // always-run preliminary pass, and reused to damp whichever
    // energy[] is being finalized (including the second pass in
    // Adaptive mode), since actual note timing doesn't depend on
    // which envelope preset is chosen for weighting.
    std::vector<int64_t> poly_diff;        // held-voice count diff
    std::vector<int64_t> held_energy_diff; // held-voice decayed-level diff (scaled)
    std::vector<int64_t> contrib_diff;     // significant-voice count diff (exponential model)
    long double model_tau_us = select_tau_us(cfg);
    const size_t sig_bins = significant_voice_bins(bin_us, model_tau_us);

    // Pre-scan the track chunk headers so workers can jump straight
    // to a track by index. Same validity rules as a sequential walk:
    // stop at the first non-MTrk chunk or truncated track.
    struct TrackRange {
        const uint8_t* data;
        const uint8_t* end;
    };

    std::vector<TrackRange> track_ranges;
    track_ranges.reserve(num_tracks);

    {
        const uint8_t* p = file_begin;

        for (uint16_t i = 0; i < num_tracks; ++i) {
            if (p + 8 > file_end)
                break;

            if (std::memcmp(p, "MTrk", 4) != 0)
                break;

            p += 4;

            uint32_t track_size =
                (static_cast<uint32_t>(p[0]) << 24) |
                (static_cast<uint32_t>(p[1]) << 16) |
                (static_cast<uint32_t>(p[2]) << 8) |
                static_cast<uint32_t>(p[3]);
            p += 4;

            const uint8_t* track_end = p + track_size;
            if (track_end > file_end)
                break;

            track_ranges.push_back({p, track_end});
            p = track_end;
        }
    }

    // Per-thread accumulators: each worker collects its own diff
    // arrays and they are summed after the workers join. Integer
    // addition is associative, so the merged arrays are identical to
    // the sequential results.
    struct ScanAccum {
        std::vector<int64_t> diff;
        std::vector<int64_t> contrib_diff;
        std::vector<int64_t> poly_diff;
        std::vector<int64_t> held_energy_diff;
    };

    std::vector<ScanAccum> accums(worker_count(cfg.threads));

    std::atomic<size_t> next_range{0};
    std::atomic<uint32_t> scanned{0};
    std::atomic<bool> scan_aborted{false};

    auto scan_worker = [&](unsigned id) {
        ScanAccum& acc = accums[id];

        // Note callback: inject a decay impulse at note-on into the
        // worker's diff array, and accumulate the envelope-independent
        // voice-model curves (held voice count and summed decayed
        // level of held voices).
        auto note_cb = [&](uint64_t ns, uint64_t ne, uint8_t vel) {
            // Impulse at note-on; the decay recurrence in the build
            // step handles the tail, so no per-note structure is
            // retained here (the previous top-K kept one set node per
            // note alive for the whole run).
            {
                size_t a_bin =
                    static_cast<size_t>(ns / bin_us);

                if (acc.diff.size() <= a_bin)
                    acc.diff.resize(a_bin + 1, 0);

                acc.diff[a_bin] +=
                    static_cast<int64_t>(vel) * ENERGY_SCALE;

                size_t c_end = a_bin + sig_bins;

                if (acc.contrib_diff.size() <= c_end)
                    acc.contrib_diff.resize(c_end + 1, 0);

                acc.contrib_diff[a_bin] += 1;
                acc.contrib_diff[c_end] -= 1;
            }

            uint64_t note_end = (ne > ns) ? ne : (ns + 1);

            size_t a_bin =
                static_cast<size_t>(ns / bin_us);

            size_t b_bin =
                static_cast<size_t>(
                    (note_end + bin_us - 1) / bin_us
                );

            if (b_bin <= a_bin)
                b_bin = a_bin + 1;

            if (acc.poly_diff.size() <= b_bin)
                acc.poly_diff.resize(b_bin + 1, 0);

            if (acc.held_energy_diff.size() <= b_bin)
                acc.held_energy_diff.resize(b_bin + 1, 0);

            acc.poly_diff[a_bin] += 1;
            acc.poly_diff[b_bin] -= 1;

            int64_t level =
                static_cast<int64_t>(vel) * ENERGY_SCALE;

            acc.held_energy_diff[a_bin] += level;

            // Level still remaining at note-off: the damper stops the
            // string, so the note stops contributing to the ambient
            // level here even though the exponential model lets its
            // rendered tail (in energy[]) keep decaying.
            long double remaining =
                static_cast<long double>(level) *
                std::exp(
                    -static_cast<long double>(note_end - ns) /
                    model_tau_us
                );

            acc.held_energy_diff[b_bin] -=
                static_cast<int64_t>(remaining);
        };

        try {
            for (;;) {
                size_t i = next_range.fetch_add(1);
                if (i >= track_ranges.size())
                    return;

                scan_one_track(
                    track_ranges[i].data,
                    track_ranges[i].end,
                    tempo,
                    note_cb
                );

                uint32_t done = scanned.fetch_add(1) + 1;

                if (done % 500 == 0 || done == track_ranges.size()) {
                    std::printf(
                        "  Track %u/%zu scanned\n",
                        done,
                        track_ranges.size()
                    );
                    fflush(stdout);
                }
            }
        } catch (...) {
            scan_aborted.store(true, std::memory_order_relaxed);
        }
    };

    run_workers(cfg.threads, scan_worker);

    // Merge the per-thread accumulators into the shared arrays.
    auto merge_into =
        [](std::vector<int64_t>& dst, const std::vector<int64_t>& src) {
        if (src.size() > dst.size())
            dst.resize(src.size(), 0);

        for (size_t i = 0; i < src.size(); ++i)
            dst[i] += src[i];
    };

    for (const auto& acc : accums) {
        merge_into(diff, acc.diff);
        merge_into(contrib_diff, acc.contrib_diff);
        merge_into(poly_diff, acc.poly_diff);
        merge_into(held_energy_diff, acc.held_energy_diff);
    }

    if (scan_aborted.load()) {
        std::printf(
            "[Phase 2a] Warning: a scan worker failed; the profile may "
            "be incomplete.\n"
        );
        fflush(stdout);
    }

    // Max concurrent held notes, for reporting.
    int64_t max_poly_observed = 0;
    {
        int64_t running = 0;

        for (size_t i = 0; i < poly_diff.size(); ++i) {
            running += poly_diff[i];
            max_poly_observed = std::max(max_poly_observed, running);
        }
    }

    // Build the envelope-independent voice-model curves once from the
    // prelim scan: held voice count, summed decayed level of held
    // voices, and the file-wide ambient anchor. These feed the
    // realtime-audibility ratio evaluated per segment in Phase 2b;
    // they do not depend on which envelope preset weights the energy
    // curve, so the second (Adaptive) pass below reuses them as-is.
    build_voice_model_curves(
        *this,
        poly_diff,
        held_energy_diff,
        model_tau_us
    );

    std::printf(
        "[Phase 2a] Voice model: tau=%.1fms, ambient p80=%.1f, "
        "max concurrent notes observed=%lld\n",
        static_cast<double>(model_tau_us) / 1000.0,
        ambient_typical,
        static_cast<long long>(max_poly_observed)
    );
    fflush(stdout);

    size_t damped_bins = 0;

    // Build energy[] and prefix[] from diff[] for prelim profile,
    // applying the polyphony damping factor per bin as it goes: when
    // more voices are contributing than global_polyphony_cap allows,
    // the bin's weighted energy is scaled down by
    // (cap / contributing_count), which is the standard way to
    // approximate "no more than N voices are actually sounding" - it
    // simply rescales the excess rather than trying to decide which
    // specific notes a real voice-stealing implementation would have
    // kept, which would need per-note ranking and is unnecessary
    // here since only a scalar baseline is needed. A voice keeps
    // contributing until its level decays below 1% of its onset.
    build_energy_exponential_capped(
        *this,
        contrib_diff,
        cfg.global_polyphony_cap,
        model_tau_us,
        damped_bins
    );

    std::printf(
        "[Phase 2a] Polyphony cap=%d, max concurrent notes observed=%lld, "
        "damped bins=%zu/%zu\n",
        cfg.global_polyphony_cap,
        static_cast<long long>(max_poly_observed),
        damped_bins,
        diff.size()
    );
    fflush(stdout);

    std::printf("[Phase 2a] Global energy profile complete (%zu bins)\n", diff.size());
    fflush(stdout);
}

static bool segment_is_bimodal(
    const std::array<int, 16>& hist,
    int total,
    double min_fraction
) {
    if (total < 8)
        return false;

    int best_gap = 0;

    for (int split = 0; split < 15; ++split) {
        int left_count = 0;
        int right_count = 0;

        int left_last = -1;
        int right_first = -1;

        for (int i = 0; i <= split; ++i) {
            left_count += hist[i];
            if (hist[i])
                left_last = i;
        }

        for (int i = split + 1; i < 16; ++i) {
            right_count += hist[i];
            if (hist[i] && right_first < 0)
                right_first = i;
        }

        if (left_last < 0 || right_first < 0)
            continue;

        if (left_count <
            static_cast<int>(std::ceil(total * min_fraction)))
            continue;

        if (right_count <
            static_cast<int>(std::ceil(total * min_fraction)))
            continue;

        int gap = right_first - left_last - 1;

        best_gap = std::max(best_gap, gap);
    }

    return best_gap >= 2;
}

static void cluster_segment(
    Segment& seg,
    TrackInfo& track,
    std::vector<Note>& notes
) {
    const size_t count = seg.end - seg.begin;

    if (count == 0)
        return;

    if (count == 1) {
        notes[track.order[seg.begin]].keep = true;
        return;
    }

    struct F {
        double v;
        double d;
        double p;
    };

    std::vector<F> features(count);

    double mv = 0;
    double md = 0;
    double mp = 0;

    for (size_t i = 0; i < count; ++i) {
        const Note& n =
            notes[track.order[seg.begin + i]];

        double duration_ms =
            static_cast<double>(
                n.end_us - n.start_us
            ) / 1000.0;

        features[i] = {
            n.velocity / 127.0,
            std::min(duration_ms, 2000.0) / 2000.0,
            n.pitch / 127.0
        };

        mv += features[i].v;
        md += features[i].d;
        mp += features[i].p;
    }

    mv /= count;
    md /= count;
    mp /= count;

    double sv = 0;
    double sd = 0;
    double sp = 0;

    for (const auto& f : features) {
        sv += (f.v - mv) * (f.v - mv);
        sd += (f.d - md) * (f.d - md);
        sp += (f.p - mp) * (f.p - mp);
    }

    sv = std::sqrt(sv / count);
    sd = std::sqrt(sd / count);
    sp = std::sqrt(sp / count);

    if (sv < 1e-9)
        sv = 1;

    if (sd < 1e-9)
        sd = 1;

    if (sp < 1e-9)
        sp = 1;

    double c[2][3] = {
        { mv, md, mp },
        { mv, md, mp }
    };

    size_t imin = 0;
    size_t imax = 0;

    for (size_t i = 1; i < count; ++i) {
        if (features[i].v < features[imin].v)
            imin = i;

        if (features[i].v > features[imax].v)
            imax = i;
    }

    c[0][0] = (features[imin].v - mv) / sv;
    c[0][1] = (features[imin].d - md) / sd;
    c[0][2] = (features[imin].p - mp) / sp;

    c[1][0] = (features[imax].v - mv) / sv;
    c[1][1] = (features[imax].d - md) / sd;
    c[1][2] = (features[imax].p - mp) / sp;

    std::vector<uint8_t> assignment(count, 0);

    for (int iter = 0; iter < 6; ++iter) {
        double sums[2][3] = {};
        int counts[2] = {};

        for (size_t i = 0; i < count; ++i) {
            double z0 =
                (features[i].v - mv) / sv;
            double z1 =
                (features[i].d - md) / sd;
            double z2 =
                (features[i].p - mp) / sp;

            double d0 =
                (z0 - c[0][0]) * (z0 - c[0][0]) +
                (z1 - c[0][1]) * (z1 - c[0][1]) +
                (z2 - c[0][2]) * (z2 - c[0][2]);

            double d1 =
                (z0 - c[1][0]) * (z0 - c[1][0]) +
                (z1 - c[1][1]) * (z1 - c[1][1]) +
                (z2 - c[1][2]) * (z2 - c[1][2]);

            assignment[i] = (d1 < d0) ? 1 : 0;

            int k = assignment[i];

            sums[k][0] += z0;
            sums[k][1] += z1;
            sums[k][2] += z2;
            counts[k]++;
        }

        for (int k = 0; k < 2; ++k) {
            if (counts[k]) {
                c[k][0] = sums[k][0] / counts[k];
                c[k][1] = sums[k][1] / counts[k];
                c[k][2] = sums[k][2] / counts[k];
            }
        }
    }

    int high_cluster =
        (c[1][0] >= c[0][0]) ? 1 : 0;

    for (size_t i = 0; i < count; ++i) {
        if (assignment[i] == high_cluster)
            notes[track.order[seg.begin + i]].keep = true;
    }
}

static void build_track_segments(
    TrackInfo& track,
    std::vector<Note>& notes,
    const GlobalStats& global_stats,
    const TempoMap& tempo,
    const MeasureMap& measure_map,
    const Config& cfg
) {
    track.segments.clear();

    if (track.note_begin == track.note_end)
        return;

    track.order.resize(track.note_end - track.note_begin);

    std::iota(
        track.order.begin(),
        track.order.end(),
        static_cast<uint32_t>(track.note_begin)
    );

    bool sorted = true;

    for (size_t i = 1; i < track.order.size(); ++i) {
        if (notes[track.order[i]].start_us <
            notes[track.order[i - 1]].start_us) {
            sorted = false;
            break;
        }
    }

    if (!sorted) {
        std::stable_sort(
            track.order.begin(),
            track.order.end(),
            [&](uint32_t a, uint32_t b) {
                return notes[a].start_us < notes[b].start_us;
            }
        );
    }

    const uint64_t first_tick =
        notes[track.order.front()].start_tick;

    const uint64_t last_tick =
        std::max(
            notes[track.order.back()].end_tick,
            notes[track.order.back()].start_tick + 1
        );

    // Time-targeted segmentation: each boundary is chosen so the
    // segment's wall-clock duration lands close to
    // cfg.segment_seconds, but snapped to a dyadic length from the
    // previous boundary - a power-of-two division of a measure
    // (half-bar, quarter-bar, ...) or a power-of-two multiple (2, 4,
    // 8, ... bars). Candidates are generated relative to the previous
    // boundary's grid position, so no global bar array is needed.
    // The duration-vs-step relationship is monotonic in tick (the
    // tempo map is strictly increasing), so the search stops as soon
    // as a candidate is at least twice the target: the optimum is
    // then bracketed.
    const long double target_us =
        std::max(1000.0L,
                 static_cast<long double>(cfg.segment_seconds) * 1.0e6L);

    const int snap_divisions =
        std::clamp(cfg.snap_divisions, 0, 20);

    std::vector<uint64_t> boundaries;

    // First boundary: snap the track's first note down to the
    // measure-division grid so the whole chain stays on-grid.
    const TimeSigSegment* first_seg =
        measure_map.segment_for_tick(first_tick);

    uint64_t first_tpm =
        first_seg ? first_seg->ticks_per_measure : 0;
    if (first_tpm == 0)
        first_tpm = 1;

    uint64_t first_sub = first_tpm >> snap_divisions;
    if (first_sub == 0)
        first_sub = 1;

    uint64_t first_seg_tick = first_seg ? first_seg->tick : 0;

    uint64_t prev_tick =
        first_seg_tick +
        ((first_tick - first_seg_tick) / first_sub) * first_sub;

    uint64_t prev_us = tempo.tick_to_us(prev_tick);
    boundaries.push_back(prev_us);

    for (;;) {
        const TimeSigSegment* seg =
            measure_map.segment_for_tick(prev_tick);

        uint64_t tpm = seg ? seg->ticks_per_measure : 0;
        if (tpm == 0)
            tpm = 1;

        uint64_t sub = tpm >> snap_divisions;
        if (sub == 0)
            sub = 1;

        uint64_t seg_tick = seg ? seg->tick : 0;
        uint64_t anchor =
            seg_tick + ((prev_tick - seg_tick) / sub) * sub;

        bool have_best = false;
        uint64_t best_tick = 0;
        long double best_score = 0.0L;

        for (int m = 0; m <= 40; ++m) {
            // Guard against overflow in the dyadic step.
            if (m >= 63 ||
                sub > (std::numeric_limits<uint64_t>::max() >> m))
                break;

            uint64_t cand_tick = anchor + (sub << m);

            if (cand_tick <= prev_tick)
                continue;

            if (cand_tick >= last_tick)
                break;

            uint64_t cand_us = tempo.tick_to_us(cand_tick);

            long double dur =
                static_cast<long double>(cand_us) -
                static_cast<long double>(prev_us);

            long double score =
                dur > target_us ? dur - target_us : target_us - dur;

            if (!have_best || score < best_score) {
                have_best = true;
                best_tick = cand_tick;
                best_score = score;
            }

            // Candidates only grow, and the tempo map is strictly
            // increasing, so once a candidate is at least twice the
            // target the optimum is bracketed.
            if (dur >= 2.0L * target_us)
                break;
        }

        if (!have_best)
            break;

        prev_tick = best_tick;
        prev_us = tempo.tick_to_us(prev_tick);
        boundaries.push_back(prev_us);
    }

    boundaries.push_back(tempo.tick_to_us(last_tick));

    size_t pos = 0;

    for (size_t b = 1; b < boundaries.size(); ++b) {
        uint64_t end_us = boundaries[b];

        size_t begin_pos = pos;

        while (
            pos < track.order.size() &&
            notes[track.order[pos]].start_us < end_us
        ) {
            ++pos;
        }

        if (pos == begin_pos)
            continue;

        Segment seg;
        seg.start_us = boundaries[b - 1];
        seg.end_us = end_us;
        seg.begin = begin_pos;
        seg.end = pos;

        int total = 0;
        double sum = 0;

        std::array<int, 16> hist{};

        for (size_t i = seg.begin; i < seg.end; ++i) {
            const Note& n =
                notes[track.order[i]];

            double v = n.velocity;

            sum += v;

            seg.max_velocity =
                std::max(
                    seg.max_velocity,
                    static_cast<int>(n.velocity)
                );

            seg.max_polyphony =
                std::max(
                    seg.max_polyphony,
                    n.poly_at_on
                );

            int bin =
                std::min(
                    15,
                    static_cast<int>(n.velocity * 16 / 128)
                );

            hist[bin]++;
            ++total;
        }

        seg.avg_velocity = sum / total;

        size_t count = seg.end - seg.begin;

        size_t third =
            std::max<size_t>(1, count / 3);

        double first_sum = 0;
        double last_sum = 0;

        for (size_t i = 0; i < third; ++i) {
            first_sum +=
                notes[track.order[seg.begin + i]].velocity;
        }

        for (size_t i = 0; i < third; ++i) {
            last_sum +=
                notes[
                    track.order[
                        seg.end - third + i
                    ]
                ].velocity;
        }

        seg.first_avg = first_sum / third;
        seg.last_avg = last_sum / third;
        seg.trend = seg.last_avg - seg.first_avg;

        double seconds =
            static_cast<double>(
                seg.end_us - seg.start_us
            ) / 1000000.0;

        seg.density =
            seconds > 0 ? total / seconds : 0;

        seg.bimodal =
            segment_is_bimodal(
                hist,
                total,
                cfg.bimodal_fraction
            );
        
        // include notes that started before the segment but overlap it
        size_t start_idx = seg.begin;
        while (start_idx > 0) {
            const Note& prev = notes[track.order[start_idx - 1]];
            if (prev.end_us > seg.start_us)
                --start_idx;
            else
                break;
        }
        
        // ------------------------------------------------------------------
        // Realtime-audibility model.
        //
        // The share test below asks "what fraction of the rendered mix is
        // this track?", which is the wrong question in two situations:
        //
        //   1. After a crash, the rendered mix is dominated by the
        //      lingering tail. A realtime synth caps its voice count, so
        //      the tail cannot grow without bound and - crucially - it
        //      *decays*: a held crash note at tau-decayed level no longer
        //      masks the way its onset velocity suggests. Content that is
        //      comparable in level to what is actually still sounding is
        //      audible and must not be removed.
        //
        //   2. In quiet passages (e.g. overlapping polyrhythmic melodies),
        //      every track's share of the mix is small simply because
        //      many tracks play together, yet all of them are audible on
        //      the realtime synth because nothing loud competes with
        //      them.
        //
        // Both are fixed by comparing levels rather than mix fractions:
        // the segment's own mean held-voice level against the mean level
        // of the *other* voices sounding in the same window (this
        // track's own contribution is subtracted so a dense track cannot
        // mask itself). The same velocity is buried during a crash and
        // clearly audible in a quiet passage, so this is what makes the
        // removal scale vary with the intensity of the moment.
        // ------------------------------------------------------------------
        const uint64_t voice_bin_us =
            global_stats.bin_us > 0 ? global_stats.bin_us : 50000ULL;

        const size_t wb =
            static_cast<size_t>(seg.start_us / voice_bin_us);

        const size_t webin =
            static_cast<size_t>((seg.end_us + voice_bin_us - 1) / voice_bin_us);

        const size_t win_bins =
            webin > wb ? (webin - wb) : 1;

        // Ambient over the window: summed level and count of held
        // voices, straight from the global voice-model curves.
        double ambient_sum =
            global_stats.held_level_mean(seg.start_us, seg.end_us);

        double ambient_cnt =
            global_stats.held_count_mean(seg.start_us, seg.end_us);

        // This track's own held-voice sums over the same window,
        // computed with the same per-bin exponential model as the
        // global curves so the subtraction below is consistent. Each
        // note contributes velocity * r^(bin - onset_bin) for every bin
        // it is held, summed in closed form as a geometric series.
        const long double model_tau =
            global_stats.voice_tau_us > 0.0L
                ? global_stats.voice_tau_us
                : 1.0L;

        const long double r =
            std::exp(
                -static_cast<long double>(voice_bin_us) / model_tau
            );

        const long double one_minus_r = 1.0L - r;

        // Precomputed powers of r for the per-note geometric series
        // below: tail exponents are bounded by the window length, and
        // head exponents only matter up to the point where the level
        // has decayed below 1e-4 of onset (beyond that the note
        // contributes nothing anyway), so a small table replaces two
        // std::exp calls per note on files with hundreds of millions
        // of notes.
        size_t k_floor = 0;
        {
            long double p = 1.0L;
            while (k_floor < 4096 && p > 1e-4L) {
                p *= r;
                ++k_floor;
            }
        }

        const size_t rpow_size =
            std::max(k_floor, win_bins) + 2;

        std::vector<long double> rpow(rpow_size);
        rpow[0] = 1.0L;
        for (size_t k = 1; k < rpow_size; ++k)
            rpow[k] = rpow[k - 1] * r;

        long double own_level_sum = 0.0L; // velocity * bins
        uint64_t own_count_sum = 0;       // bins

        for (size_t i = start_idx; i < seg.end; ++i) {
            const Note& n = notes[track.order[i]];

            uint64_t note_a = n.start_us;
            uint64_t note_b = n.end_us;
            if (note_b <= note_a) note_b = note_a + 1;

            size_t ob = static_cast<size_t>(note_a / voice_bin_us);
            size_t eb = static_cast<size_t>((note_b + voice_bin_us - 1) / voice_bin_us);
            if (eb <= ob) eb = ob + 1;

            size_t cb0 = std::max(ob, wb);
            size_t cb1 = std::min(eb, webin);

            if (cb1 <= cb0)
                continue;

            size_t span = cb1 - cb0;
            own_count_sum += span;

            long double geom = 0.0L;

            if (one_minus_r < 1e-9L) {
                geom = static_cast<long double>(span);
            } else {
                // Voices that started long before the window have
                // decayed to nothing; past the table floor the head
                // is <= 1e-4 and the note contributes nothing.
                long double head = 1.0L;
                if (cb0 > ob) {
                    size_t hk = cb0 - ob;
                    head = hk < rpow_size ? rpow[hk] : 0.0L;
                }

                if (head > 1e-4L) {
                    long double tail = rpow[span];

                    geom = head *
                        (1.0L - tail) / one_minus_r;
                }
            }

            own_level_sum +=
                static_cast<long double>(n.velocity) * geom;
        }

        long double own_sum_mean =
            static_cast<long double>(own_level_sum) /
            static_cast<long double>(win_bins);

        long double own_cnt_mean =
            static_cast<long double>(own_count_sum) /
            static_cast<long double>(win_bins);

        double own_level =
            own_cnt_mean > 1e-6L
                ? static_cast<double>(
                      own_sum_mean / own_cnt_mean
                  )
                : 0.0;

        // Exponential-weighted segment energy for the share test: the
        // same decay model as the global curve (and the same per-note
        // geometric weights as own_level above), integrated over the
        // segment window and compared against the global average.
        long double duration_us =
            static_cast<long double>(seg.end_us - seg.start_us);

        double own_avg =
            duration_us > 0.0L
                ? static_cast<double>(
                      own_level_sum *
                      static_cast<long double>(voice_bin_us) /
                      duration_us
                  )
                : 0.0;

        double global_avg =
            global_stats.average(seg.start_us, seg.end_us);

        double share = 0.0;
        if (global_avg > 0.0)
            share = own_avg / global_avg;

        seg.track_energy_share = std::clamp(share, 0.0, 1.0);

        double ambient_local =
            ambient_cnt > 1e-6
                ? ambient_sum / ambient_cnt
                : 0.0;

        // Shared context references for the band decision below:
        // kSilenceNotch marks "near-silence" (nothing left to mask
        // anything, e.g. a fully decayed crash tail) and gate_ref
        // marks "carrying real content". The band between them is
        // where art-dominated passages live.
        constexpr double kSilenceNotch = 0.15;
        const double silence_ref =
            kSilenceNotch * global_stats.ambient_typical;

        double others_cnt =
            ambient_cnt - static_cast<double>(own_cnt_mean);

        double ambient_others;

        if (others_cnt <= 0.25) {
            // No *other* voices are sounding (or only vanishing
            // traces of them). Two very different situations land
            // here:
            //
            //   * a genuinely near-silent moment (decayed crash
            //     tail): nothing exists that could mask the segment,
            //     so comparing it against itself is honest and it
            //     passes the ratio;
            //   * a self-dominated window (a dense art track whose
            //     own voices crowd the other voices out of the
            //     denominator): "audible relative to the others" is
            //     meaningless when the others ARE this track, and
            //     granting a free ratio of 1 was letting exactly
            //     these passages through. Anchor them to the file's
            //     typical sounding level instead, so they survive
            //     only when their own level is a real fraction of
            //     what the file actually sounds like.
            ambient_others =
                (global_stats.ambient_typical > 0.0 &&
                 ambient_local > silence_ref)
                    ? std::max(own_level,
                               global_stats.ambient_typical)
                    : own_level;
        } else {
            ambient_others =
                (ambient_sum - static_cast<double>(own_sum_mean)) /
                others_cnt;
        }

        double audibility_ratio =
            ambient_others > 1e-6
                ? own_level / ambient_others
                : (own_level > 1e-6 ? 8.0 : 0.0);

        seg.own_level = own_level;
        seg.ambient_level = ambient_local;
        seg.audibility_ratio = audibility_ratio;
        
        // Dense percussive/"crash-like" passages are not classified
        // as a special case any more: a loud one lands in the high
        // band and is kept, a quiet uniform one lands in the low
        // band and is dropped, and a mid-velocity dense passage that
        // is actually audible will naturally show a high
        // track_energy_share at the moment it dominates the mix.
        // The one genuinely useful crash-specific signal - a sharp
        // rising velocity trend within the segment - is kept below
        // as one of the inputs that pushes an ambiguous segment into
        // the Cluster (per-note) path rather than a flat drop.
        //
        // The absolute velocity bands stay as the *stylistic* anchor
        // for what counts as art (they are the user's tuned cutoffs);
        // the realtime-audibility ratio is layered on top as the
        // *physical* anchor for "can this be heard at this moment",
        // which is what varies with local intensity:
        //
        //   * Mid band (30..70): kept when the segment is a dominant
        //     share of the mix (existing share test) OR when its own
        //     level is comparable to the level of the other voices
        //     sounding at the same time. A 50-velocity descent right
        //     after a held-note crash competes against the crash's
        //     *decayed* held level, not its onset velocity, and a
        //     40-velocity melody among 200 simultaneous tracks is one
        //     voice in a texture where every voice is equally audible.
        //
        //   * Low band (< 30): rescued only when the local ambient
        //     level shows the section carries real melodic content
        //     (ambient_gate) - art passages dominated by their own
        //     quiet notes fail the gate - or when the moment is
        //     effectively silent (decayed crash tail: nothing is left
        //     to mask anything). The rescue threshold is stricter than
        //     the mid-band one so quiet art layered under real content
        //     still gets removed.
        if (
            seg.avg_velocity >=
            cfg.high_velocity
        ) {
            seg.mode = Segment::Mode::KeepAll;
        } else if (
            seg.avg_velocity >=
            cfg.low_velocity
        ) {
            if (seg.track_energy_share >=
                cfg.mask_share) {
                seg.mode = Segment::Mode::KeepAll;
            } else if (
                seg.audibility_ratio >=
                cfg.audibility_ratio
            ) {
                seg.mode = Segment::Mode::KeepAll;
            } else if (
                seg.bimodal ||
                seg.max_velocity >=
                    cfg.peak_velocity ||
                seg.trend >=
                    cfg.trend_threshold
            ) {
                seg.mode = Segment::Mode::Cluster;
            } else {
                seg.mode = Segment::Mode::Drop;
            }
        } else {
            if (
                seg.max_velocity >=
                    cfg.peak_velocity ||
                seg.bimodal ||
                seg.trend >=
                    cfg.trend_threshold
            ) {
                seg.mode = Segment::Mode::Cluster;
            } else {
                // Bandpass gate on the local ambient intensity: the
                // notch between "near-silence" and "carrying real
                // content" is exactly where art-dominated passages
                // live, and there the low band must stay dropped.
                // (kSilenceNotch/silence_ref are computed above,
                // shared with the solo-window anchor.)
                const double gate_ref =
                    cfg.ambient_gate *
                    global_stats.ambient_typical;

                bool context_allows_rescue =
                    global_stats.ambient_typical > 0.0 &&
                    (seg.ambient_level >= gate_ref ||
                     seg.ambient_level <= silence_ref);

                if (context_allows_rescue &&
                    seg.audibility_ratio >=
                        cfg.audibility_ratio * 1.75) {
                    seg.mode = Segment::Mode::KeepAll;
                } else {
                    seg.mode = Segment::Mode::Drop;
                }
            }
        }

        track.segments.push_back(seg);
    }

    for (auto& seg : track.segments) {
        if (seg.mode == Segment::Mode::KeepAll) {
            for (size_t i = seg.begin; i < seg.end; ++i)
                notes[track.order[i]].keep = true;
        } else if (
            seg.mode == Segment::Mode::Cluster
        ) {
            cluster_segment(seg, track, notes);
        }

        if (cfg.verbose) {
            const char* mode = "DROP";

            if (seg.mode == Segment::Mode::KeepAll)
                mode = "KEEP";
            else if (seg.mode == Segment::Mode::Cluster)
                mode = "CLUSTER";

            std::printf(
                "track %u  %.3f-%.3f s  "
                "notes=%zu avg=%.1f max=%d "
                "nps=%.1f poly=%d share=%.3f "
                "own=%.1f amb=%.1f ratio=%.2f "
                "trend=%+.1f  %s\n",
                track.index,
                seg.start_us / 1000000.0,
                seg.end_us / 1000000.0,
                seg.end - seg.begin,
                seg.avg_velocity,
                seg.max_velocity,
                seg.density,
                seg.max_polyphony,
                seg.track_energy_share,
                seg.own_level,
                seg.ambient_level,
                seg.audibility_ratio,
                seg.trend,
                mode
            );
        }
    }

    track.kept_notes = 0;
    track.raw_average_velocity = 0;
    track.raw_max_velocity = 0;

    for (size_t i = track.note_begin;
         i < track.note_end;
         ++i) {
        track.raw_average_velocity +=
            notes[i].velocity;

        track.raw_max_velocity =
            std::max(
                track.raw_max_velocity,
                notes[i].velocity
            );

        if (notes[i].keep)
            ++track.kept_notes;
    }

    size_t total_notes =
        track.note_end - track.note_begin;

    if (total_notes)
        track.raw_average_velocity /=
            total_notes;
}

static bool emit_delta(
    MemWriter& out,
    uint64_t& last_tick,
    uint64_t tick
) {
    if (tick < last_tick)
        return false;

    uint64_t delta64 = tick - last_tick;

    if (delta64 > 0x0FFFFFFF)
        return false;

    if (!write_vlq(
        out,
        static_cast<uint32_t>(delta64)
    ))
        return false;

    last_tick = tick;
    return true;
}

struct TrackEmitter {
    MemWriter& out;
    uint64_t last_tick = 0;

    bool channel(
        uint64_t tick,
        uint8_t status,
        const uint8_t* data,
        uint32_t len
    ) {
        if (!emit_delta(out, last_tick, tick))
            return false;

        if (!out.write_u8(status))
            return false;

        return out.write_bytes(data, len);
    }

    bool meta(
        uint64_t tick,
        uint8_t type,
        const uint8_t* data,
        uint32_t len
    ) {
        if (type == 0x2F)
            len = 0;

        if (!emit_delta(out, last_tick, tick))
            return false;

        if (!out.write_u8(0xFF))
            return false;

        if (!out.write_u8(type))
            return false;

        if (!write_vlq(out, len))
            return false;

        if (len)
            return out.write_bytes(data, len);

        return true;
    }

    bool sysex(
        uint64_t tick,
        uint8_t status,
        const uint8_t* data,
        uint32_t len
    ) {
        if (!emit_delta(out, last_tick, tick))
            return false;

        if (!out.write_u8(status))
            return false;

        if (!write_vlq(out, len))
            return false;

        if (len)
            return out.write_bytes(data, len);

        return true;
    }

    bool system(
        uint64_t tick,
        uint8_t status,
        const uint8_t* data,
        uint32_t len
    ) {
        if (!emit_delta(out, last_tick, tick))
            return false;

        if (!out.write_u8(status))
            return false;

        if (len)
            return out.write_bytes(data, len);

        return true;
    }
};

static bool flush_track(
    FileWriter& out,
    const MemWriter& body
) {
    if (body.size() > 0xFFFFFFFFull)
        return false;

    uint8_t header[8] = {
        'M', 'T', 'r', 'k',
        static_cast<uint8_t>(
            static_cast<uint32_t>(body.size()) >> 24
        ),
        static_cast<uint8_t>(
            static_cast<uint32_t>(body.size()) >> 16
        ),
        static_cast<uint8_t>(
            static_cast<uint32_t>(body.size()) >> 8
        ),
        static_cast<uint8_t>(
            static_cast<uint32_t>(body.size())
        )
    };

    if (!out.write_bytes(header, sizeof(header)))
        return false;

    if (body.size() &&
        !out.write_bytes(body.ptr(), body.size()))
        return false;

    return true;
}

static bool write_track(
    FileWriter& out,
    const TrackInfo& track,
    const std::vector<Note>& notes,
    const std::vector<GlobalEvent>& globals,
    uint32_t format,
    bool move_globals
) {
    MemWriter body;
    body.reserve(
        static_cast<size_t>(track.size) +
        (track.index == 0 ? globals.size() * 8 : 0)
    );

    TrackEmitter emitter{body};

    const bool conductor =
        move_globals &&
        format == 1 &&
        track.index == 0;

    const bool emit_globals_track =
        move_globals &&
        track.index == 0 &&
        (format == 0 || format == 2);

    const bool drop_everything =
        track.has_notes &&
        track.kept_notes == 0 &&
        track.index != 0;

    std::array<int32_t, 2048> active;
    active.fill(-1);

    size_t next_note_id =
        track.note_begin;

    size_t next_global = 0;

    auto emit_pending_globals =
        [&](uint64_t tick) -> bool {
            while (
                next_global < globals.size() &&
                globals[next_global].tick <= tick
            ) {
                const auto& g =
                    globals[next_global];

                if (!emitter.meta(
                    g.tick,
                    g.type,
                    g.data,
                    g.len
                ))
                    return false;

                ++next_global;
            }

            return true;
        };

    auto process_event =
        [&](const ParsedEvent& ev) -> bool {
            bool is_global =
                ev.kind == EventKind::Meta &&
                is_global_meta(ev.meta_type);

            if (ev.kind == EventKind::Meta &&
                ev.meta_type == 0x2F) {
                return true;
            }

            int32_t note_id = -1;
            bool note_event = false;
            bool note_keep = true;

            if (ev.kind == EventKind::Channel) {
                uint8_t high = ev.status & 0xF0;
                uint8_t ch = ev.status & 0x0F;

                bool is_on =
                    high == 0x90 &&
                    ev.data_len == 2 &&
                    ev.data[1] != 0;

                bool is_off =
                    (
                        high == 0x80 &&
                        ev.data_len == 2
                    ) ||
                    (
                        high == 0x90 &&
                        ev.data_len == 2 &&
                        ev.data[1] == 0
                    );

                if (is_on) {
                    if (next_note_id >=
                        track.note_end)
                        return false;

                    note_id =
                        static_cast<int32_t>(
                            next_note_id++
                        );

                    active[
                        ch * 128 +
                        ev.data[0]
                    ] = note_id;

                    note_event = true;
                    note_keep =
                        notes[note_id].keep;
                } else if (is_off) {
                    int slot =
                        ch * 128 +
                        ev.data[0];

                    note_id = active[slot];

                    if (note_id >= 0)
                        active[slot] =
                            notes[note_id].prev_same;

                    note_event = true;

                    if (note_id >= 0)
                        note_keep =
                            notes[note_id].keep;
                }
            }

            if (conductor) {
                if (!emit_pending_globals(ev.tick))
                    return false;

                if (is_global)
                    return true;
            } else if (emit_globals_track) {
                if (!emit_pending_globals(ev.tick))
                    return false;

                if (is_global)
                    return true;
            } else if (
                move_globals &&
                format == 1 &&
                is_global
            ) {
                return true;
            }

            if (drop_everything)
                return true;

            if (note_event && !note_keep)
                return true;

            switch (ev.kind) {
                case EventKind::Channel:
                    return emitter.channel(
                        ev.tick,
                        ev.status,
                        ev.data,
                        ev.data_len
                    );

                case EventKind::Meta:
                    return emitter.meta(
                        ev.tick,
                        ev.meta_type,
                        ev.data,
                        ev.data_len
                    );

                case EventKind::SysEx:
                    return emitter.sysex(
                        ev.tick,
                        ev.status,
                        ev.data,
                        ev.data_len
                    );

                case EventKind::System:
                    return emitter.system(
                        ev.tick,
                        ev.status,
                        ev.data,
                        ev.data_len
                    );
            }

            return false;
        };

    // A fully-dropped track (no kept notes, not the conductor) would
    // skip every event anyway, so skip the whole scan and just emit
    // the end-of-track meta below.
    if (!drop_everything &&
        !scan_events(
            track.data,
            track.end,
            process_event
        ))
        return false;

    if (conductor) {
        if (!emit_pending_globals(
            std::numeric_limits<uint64_t>::max()
        ))
            return false;
    } else if (emit_globals_track) {
        if (!emit_pending_globals(
            std::numeric_limits<uint64_t>::max()
        ))
            return false;
    }

    if (!emitter.meta(
        emitter.last_tick,
        0x2F,
        nullptr,
        0
    ))
        return false;

    return flush_track(out, body);
}

// NOTE: this function intentionally does not touch the shared
// `globals` vector. Global meta events (tempo, time signature,
// etc.) are collected exactly once, in Phase 1b in wmain, into a
// single sorted vector that TempoMap/MeasureMap and write_track's
// conductor-track logic all read from. Previously this function
// re-collected the same globals into that same shared vector on
// every track, which both duplicated every tempo/time-signature
// event and appended them out of global tick order - causing
// write_track's conductor pass (format 1, track 0) to either emit
// duplicate tempo meta events or, when a duplicate landed before
// the running tick, fail emit_delta's monotonicity check and abort
// the write. That was the source of tempo not being carried over
// correctly.
static bool analyze_track_into_vector(
    TrackInfo& track,
    std::vector<Note>& track_notes,
    const TempoMap& tempo
) {
    std::array<int32_t, 2048> active;
    active.fill(-1);

    int active_count = 0;

    // Ticks are monotonic within a track, so a cursor avoids a tempo
    // binary search per note.
    TempoCursor tcur(tempo);

    bool ok = scan_events(
        track.data,
        track.end,
        [&](const ParsedEvent& ev) -> bool {
            track.max_tick =
                std::max(track.max_tick, ev.tick);

            if (ev.kind != EventKind::Channel)
                return true;

            uint8_t high =
                ev.status & 0xF0;

            uint8_t channel =
                ev.status & 0x0F;

            if (
                ev.data_len != 2 &&
                (high == 0x80 ||
                 high == 0x90)
            )
                return false;

            bool note_on =
                high == 0x90 &&
                ev.data[1] != 0;

            bool note_off =
                high == 0x80 ||
                (
                    high == 0x90 &&
                    ev.data[1] == 0
                );

            if (note_on) {
                Note n;
                n.start_tick = ev.tick;
                n.end_tick = ev.tick;
                n.start_us = tcur.to_us(n.start_tick);
                n.velocity = ev.data[1];
                n.pitch = ev.data[0];
                n.channel = channel;
                n.poly_at_on = ++active_count;

                uint32_t index =
                    channel * 128 +
                    ev.data[0];

                n.prev_same = active[index];

                int32_t id =
                    static_cast<int32_t>(
                        track_notes.size()
                    );

                active[index] = id;
                track_notes.push_back(n);

                track.has_notes = true;
            } else if (note_off) {
                uint32_t index =
                    channel * 128 +
                    ev.data[0];

                int32_t id = active[index];

                if (id >= 0) {
                    track_notes[id].end_tick =
                        std::max(
                            ev.tick,
                            track_notes[id].start_tick
                        );

                    track_notes[id].end_us =
                        tcur.to_us(track_notes[id].end_tick);

                    if (track_notes[id].end_us <=
                        track_notes[id].start_us)
                        track_notes[id].end_us =
                            track_notes[id].start_us +
                            1000;

                    active[index] =
                        track_notes[id].prev_same;

                    if (active_count > 0)
                        --active_count;
                }
            }

            return true;
        }
    );

    if (!ok)
        return false;

    track.note_begin = 0;
    track.note_end = track_notes.size();

    for (size_t i = 0; i < active.size(); ++i) {
        int32_t id = active[i];

        while (id >= 0) {
            track_notes[id].end_tick =
                std::max(
                    track.max_tick,
                    track_notes[id].start_tick + 1
                );

            track_notes[id].end_us =
                tcur.to_us(track_notes[id].end_tick);

            if (track_notes[id].end_us <=
                track_notes[id].start_us)
                track_notes[id].end_us =
                    track_notes[id].start_us + 1000;

            id = track_notes[id].prev_same;
        }
    }

    return true;
}

static bool write_header(
    FileWriter& out,
    uint16_t format,
    uint16_t tracks,
    uint16_t division
) {
    if (!out.write_bytes("MThd", 4))
        return false;

    if (!out.write_u32(6))
        return false;

    if (!out.write_u16(format))
        return false;

    if (!out.write_u16(tracks))
        return false;

    if (!out.write_u16(division))
        return false;

    return true;
}

static bool parse_number(
    const wchar_t* s,
    double& out
) {
    wchar_t* end = nullptr;

    double v = wcstod(s, &end);

    if (end == s || *end != L'\0')
        return false;

    out = v;
    return true;
}

static bool parse_uint64(
    const wchar_t* s,
    uint64_t& out
) {
    wchar_t* end = nullptr;

    unsigned long long v =
        wcstoull(s, &end, 10);

    if (end == s || *end != L'\0')
        return false;

    out = static_cast<uint64_t>(v);
    return true;
}

static bool parse_int(
    const wchar_t* s,
    int& out
) {
    wchar_t* end = nullptr;

    long v = wcstol(s, &end, 10);

    if (end == s || *end != L'\0')
        return false;

    out = static_cast<int>(v);
    return true;
}

// Total CPU time consumed by this process (kernel + user), in ms.
// Returns -1 on failure. Used to distinguish parse-bound Phase 1b
// (CPU close to wall time) from IO-bound Phase 1b (CPU far below
// wall time: cold cache, Defender scan, cloud placeholder download).
static double process_cpu_ms() {
    FILETIME created, exited, kernel, user;

    if (!GetProcessTimes(
        GetCurrentProcess(),
        &created,
        &exited,
        &kernel,
        &user
    ))
        return -1.0;

    auto to_100ns =
        [](const FILETIME& ft) -> uint64_t {
            return
                (static_cast<uint64_t>(ft.dwHighDateTime) << 32) |
                static_cast<uint64_t>(ft.dwLowDateTime);
        };

    return static_cast<double>(
        to_100ns(kernel) + to_100ns(user)
    ) / 10000.0;
}

// Number of worker threads to spawn for the per-track scan phases.
static unsigned worker_count(int requested) {
    unsigned n =
        requested > 0
            ? static_cast<unsigned>(requested)
            : std::thread::hardware_concurrency();

    if (n == 0)
        n = 1;

    if (n > 64)
        n = 64;

    return n;
}

// Run fn(worker_id) on worker_count(requested) threads and join.
// fn must be callable with a single unsigned argument. Worker 0 runs
// on the calling thread, so nothing is spawned for --threads 1.
template <typename Fn>
static void run_workers(int requested, Fn&& fn) {
    unsigned n = worker_count(requested);

    if (n <= 1) {
        fn(0);
        return;
    }

    std::vector<std::thread> pool;
    pool.reserve(n - 1);

    for (unsigned i = 1; i < n; ++i)
        pool.emplace_back(fn, i);

    fn(0);

    for (auto& t : pool)
        t.join();
}

static void print_usage() {
    std::printf(
        "Usage:\n"
        "  velfilter_smart.exe input.mid output.mid [options]\n"
        "\n"
        "Velocity:\n"
        "  --low N              low-band cutoff (default 30)\n"
        "  --high N             high-band cutoff (default 70)\n"
        "  --peak N             isolated-note rescue threshold (default 100)\n"
        "\n"
        "Segmentation:\n"
        "  --segment-seconds N  target segment length in wall-clock\n"
        "                       seconds; boundaries snap to dyadic\n"
        "                       divisions/multiples of a measure\n"
        "                       (default 2.0 = one measure at the\n"
        "                       120 BPM base tempo)\n"
        "  --snap-divisions N   snap grid = measure / 2^N (default 2 =\n"
        "                       quarter-bar; 0 = bar lines only)\n"
        "\n"
        "Masking:\n"
        "  --mask-share N       minimum MIDI energy share (default 0.01)\n"
        "  --trend-threshold N  rising-velocity tiebreaker for ambiguous\n"
        "                       segments (default 12)\n"
        "\n"
        "Energy / Envelope (single exponential decay model):\n"
        "  --tau-ms N               tau in ms for the exponential decay\n"
        "                           model (default 0 = 500)\n"
        "  --global-polyphony-cap N approximate max concurrent audible voices;\n"
        "                           caps the global energy curve to model a\n"
        "                           real synth's voice stealing/limiting, so\n"
        "                           a dense crash's held/decaying note count\n"
        "                           can't inflate the global average past what\n"
        "                           any real instrument could actually sound\n"
        "                           at once (default 0, 0 disables damping)\n"
        "\n"
        "Realtime audibility (intensity-relative removal scale):\n"
        "  --audibility-ratio N own mean held-voice level vs the level of the\n"
        "                       other voices sounding at the same moment;\n"
        "                       segments at or above this fraction are heard\n"
        "                       on a realtime (voice-limited) synth and kept\n"
        "                       even where the rendered mix would bury them\n"
        "                       (default 1.25, ~+2dB; 0.30 ~-10dB suits\n"
        "                       sparse files)\n"
        "  --ambient-gate N     low-band rescue gate: the local ambient level\n"
        "                       must reach this fraction of the file's\n"
        "                       typical sounding level before quiet segments\n"
        "                       are rescued, so art passages dominated by\n"
        "                       their own quiet notes stay dropped (default\n"
        "                       1.0)\n"
        "\n"
        "Other:\n"
        "  --threads N          worker threads for the per-track scan\n"
        "                       phases (default 0 = all hardware\n"
        "                       threads, output is identical)\n"
        "  --global-bin-ms N    masking curve resolution (default 50)\n"
        "  --verbose            print segment decisions\n"
        "  --no-prefetch        skip the PrefetchVirtualMemory warm-up before\n"
        "                       Phase 1b (Phase 1b is the first full-file\n"
        "                       read; the prefetch turns a cold-cache\n"
        "                       demand-page stall into one sequential read)\n"
        "  --no-global-move     do not consolidate globals into track 0\n"
    );
}


int wmain(int argc, wchar_t** argv) {
    Timer total_timer;

    if (argc < 3) {
        print_usage();
        return 1;
    }

    Config cfg;

    for (int i = 3; i < argc; ++i) {
        const wchar_t* a = argv[i];

        auto need_value = [&](const wchar_t*& value) -> bool {
            if (i + 1 >= argc)
                return false;

            value = argv[++i];
            return true;
        };

        const wchar_t* value = nullptr;

        if (!_wcsicmp(a, L"--low")) {
            if (!need_value(value) ||
                !parse_number(value, cfg.low_velocity))
                return 1;
        } else if (!_wcsicmp(a, L"--high")) {
            if (!need_value(value) ||
                !parse_number(value, cfg.high_velocity))
                return 1;
        } else if (!_wcsicmp(a, L"--peak")) {
            if (!need_value(value) ||
                !parse_int(value, cfg.peak_velocity))
                return 1;
        } else if (!_wcsicmp(a, L"--segment-seconds")) {
            if (!need_value(value) ||
                !parse_number(value, cfg.segment_seconds))
                return 1;
        } else if (!_wcsicmp(a, L"--snap-divisions")) {
            if (!need_value(value) ||
                !parse_int(value, cfg.snap_divisions))
                return 1;
        } else if (!_wcsicmp(a, L"--mask-share")) {
            if (!need_value(value) ||
                !parse_number(value, cfg.mask_share))
                return 1;
        } else if (!_wcsicmp(a, L"--trend-threshold")) {
            if (!need_value(value) ||
                !parse_number(value, cfg.trend_threshold))
                return 1;
        } else if (!_wcsicmp(a, L"--tau-ms")) {
            if (!need_value(value) || !parse_number(value, cfg.tau_ms))
                return 1;
        } else if (!_wcsicmp(a, L"--global-polyphony-cap")) {
            if (!need_value(value) ||
                !parse_int(value, cfg.global_polyphony_cap))
                return 1;
        } else if (!_wcsicmp(a, L"--audibility-ratio")) {
            if (!need_value(value) ||
                !parse_number(value, cfg.audibility_ratio))
                return 1;
        } else if (!_wcsicmp(a, L"--ambient-gate")) {
            if (!need_value(value) ||
                !parse_number(value, cfg.ambient_gate))
                return 1;
        } else if (!_wcsicmp(a, L"--threads")) {
            if (!need_value(value) ||
                !parse_int(value, cfg.threads))
                return 1;
        } else if (!_wcsicmp(a, L"--global-bin-ms")) {
            if (!need_value(value) ||
                !parse_uint64(value, cfg.global_bin_ms))
                return 1;
        } else if (!_wcsicmp(a, L"--verbose")) {
            cfg.verbose = true;
        } else if (!_wcsicmp(a, L"--no-prefetch")) {
            cfg.prefetch = false;
        } else if (!_wcsicmp(a, L"--no-global-move")) {
            cfg.move_global_events = false;
        } else {
            std::printf(
                "Unknown option: %ls\n",
                a
            );
            return 1;
        }
    }

    MappedFile file;

    if (!file.open(argv[1])) {
        std::printf("Failed to open input file.\n");
        return 1;
    }

    if (file.size < 14) {
        std::printf("File is too small to be MIDI.\n");
        return 1;
    }

    const uint8_t* p = file.data;
    const uint8_t* end = file.data + file.size;

    if (std::memcmp(p, "MThd", 4) != 0) {
        std::printf("Not a standard MIDI file.\n");
        return 1;
    }

    p += 4;

    uint32_t header_len = 0;

    if (!read_u32(p, end, header_len) ||
        header_len != 6)
    {
        std::printf("Invalid MIDI header.\n");
        return 1;
    }

    uint16_t format = 0;
    uint16_t track_count = 0;
    uint16_t division = 0;

    if (!read_u16(p, end, format) ||
        !read_u16(p, end, track_count) ||
        !read_u16(p, end, division))
    {
        std::printf("Invalid MIDI header.\n");
        return 1;
    }

    if (format > 2) {
        std::printf("Unsupported MIDI format %u.\n", format);
        return 1;
    }

    if (track_count == 0) {
        std::printf("MIDI contains no tracks.\n");
        return 1;
    }

    std::vector<TrackInfo> tracks;
    tracks.reserve(track_count);

    std::vector<GlobalEvent> globals;

    Timer phase_timer;

    std::printf("[Phase 1a] Scanning track offsets...\n");
    fflush(stdout);

    for (uint32_t i = 0; i < track_count; ++i) {
        if (static_cast<size_t>(end - p) < 8) {
            std::printf("Truncated track header.\n");
            return 1;
        }

        if (std::memcmp(p, "MTrk", 4) != 0) {
            std::printf(
                "Invalid track chunk at track %u.\n",
                i
            );
            return 1;
        }

        p += 4;

        uint32_t track_size = 0;

        if (!read_u32(p, end, track_size)) {
            std::printf("Invalid track length.\n");
            return 1;
        }

        size_t offset =
            static_cast<size_t>(p - file.data);

        if (track_size >
            file.size - offset)
        {
            std::printf(
                "Track %u extends beyond the file.\n",
                i
            );
            return 1;
        }

        TrackInfo track;
        track.index = i;
        track.data = p;
        track.end = p + track_size;
        track.size = track_size;

        tracks.push_back(std::move(track));

        p += track_size;

        if ((i + 1) % 500 == 0 || (i + 1) == track_count) {
            std::printf(
                "  %u/%u offsets scanned\n",
                i + 1,
                track_count
            );
            fflush(stdout);
        }
    }

    std::printf(
        "[Phase 1a] completed in %.1fms\n",
        phase_timer.restart_ms()
    );
    fflush(stdout);

    const double cpu_1b_before = process_cpu_ms();

    std::printf("[Phase 1b] Extracting globals and building tempo map...\n");
    fflush(stdout);

    if (cfg.prefetch && file.size >= 4096) {
        Timer prefetch_timer;
        WIN32_MEMORY_RANGE_ENTRY range = {
            const_cast<uint8_t*>(file.data),
            file.size
        };

        if (PrefetchVirtualMemory(
            GetCurrentProcess(),
            1,
            &range,
            0
        )) {
            std::printf(
                "  Prefetched %.1f MB into cache in %.1fms\n",
                static_cast<double>(file.size) / (1024.0 * 1024.0),
                prefetch_timer.elapsed_ms()
            );
        } else {
            std::printf(
                "  PrefetchVirtualMemory failed (error %lu); "
                "continuing without warm-up.\n",
                static_cast<unsigned long>(GetLastError())
            );
        }
        fflush(stdout);
    }

    {
        // Parallel per-track scan: each worker appends to its own
        // slice; the slices are concatenated in track order below and
        // sorted immediately after, so the result is identical to the
        // sequential scan regardless of completion order.
        std::vector<std::vector<GlobalEvent>> global_parts(track_count);
        std::atomic<uint32_t> next_track{0};
        std::atomic<bool> scan_failed{false};
        std::atomic<uint32_t> failed_track{0};

        auto globals_worker = [&](unsigned) {
            try {
                for (;;) {
                    uint32_t i = next_track.fetch_add(1);
                    if (i >= track_count)
                        return;

                    if (scan_failed.load(std::memory_order_relaxed))
                        return;

                    TrackInfo& track = tracks[i];
                    std::vector<GlobalEvent>& part = global_parts[i];

                    bool ok = scan_events(
                        track.data,
                        track.end,
                        [&](const ParsedEvent& ev) -> bool {
                            track.max_tick =
                                std::max(track.max_tick, ev.tick);

                            if (
                                ev.kind == EventKind::Meta &&
                                is_global_meta(ev.meta_type)
                            ) {
                                part.push_back({
                                    ev.tick,
                                    track.index,
                                    ev.order,
                                    ev.meta_type,
                                    ev.data,
                                    ev.data_len
                                });
                            }

                            return true;
                        }
                    );

                    if (!ok) {
                        failed_track.store(i, std::memory_order_relaxed);
                        scan_failed.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
            } catch (...) {
                scan_failed.store(true, std::memory_order_relaxed);
            }
        };

        run_workers(cfg.threads, globals_worker);

        if (scan_failed.load()) {
            std::printf(
                "Failed to extract globals from track %u.\n",
                tracks[failed_track.load()].index
            );
            return 1;
        }

        for (auto& part : global_parts) {
            globals.insert(
                globals.end(),
                part.begin(),
                part.end()
            );
        }
    }

    std::sort(
        globals.begin(),
        globals.end(),
        [](const GlobalEvent& a, const GlobalEvent& b) {
            if (a.tick != b.tick)
                return a.tick < b.tick;
            if (a.track != b.track)
                return a.track < b.track;
            return a.order < b.order;
        }
    );

    TempoMap tempo =
        build_tempo_map(
            division,
            globals
        );

    MeasureMap measure_map;
    measure_map.build(division, globals);

    const double phase1b_ms = phase_timer.restart_ms();
    const double cpu_1b_after = process_cpu_ms();

    if (cpu_1b_before >= 0.0 && cpu_1b_after >= 0.0) {
        const double cpu_1b_ms = cpu_1b_after - cpu_1b_before;

        std::printf(
            "[Phase 1b] completed in %.1fms "
            "(CPU %.1fms, %.0f%% busy; a low busy%% means the "
            "time went to disk/AV/cloud, not parsing)\n",
            phase1b_ms,
            cpu_1b_ms,
            phase1b_ms > 0.0
                ? 100.0 * cpu_1b_ms / phase1b_ms
                : 0.0
        );
    } else {
        std::printf(
            "[Phase 1b] completed in %.1fms\n",
            phase1b_ms
        );
    }
    fflush(stdout);

    std::printf("[Phase 2a] Building global energy profile (scan-only)...\n");
    fflush(stdout);

    GlobalStats global_stats;
    const uint8_t* first_track_chunk =
        tracks.front().data - 8;

    global_stats.build_from_scan(
        first_track_chunk,
        file.data + file.size,
        track_count,
        tempo,
        cfg
    );

    std::printf(
        "[Phase 2a] completed in %.1fms\n",
        phase_timer.restart_ms()
    );
    fflush(stdout);

    std::printf("[Phase 2b] Processing %zu tracks...\n", tracks.size());
    fflush(stdout);

    FileWriter out;

    if (!out.open(argv[2])) {
        std::printf("Failed to open output file.\n");
        return 1;
    }

    if (!write_header(
        out,
        format,
        track_count,
        division
    )) {
        std::printf("Failed to write MIDI header.\n");
        return 1;
    }

    bool move_globals =
        cfg.move_global_events &&
        format == 1;

    if (format == 2 &&
        cfg.move_global_events) {
        std::printf(
            "Format 2 detected: global metadata will "
            "remain in its original sequence.\n"
        );
    }

    size_t total_notes = 0;
    size_t kept_notes = 0;

    Timer phase2b_timer;
    double cumulative_parse = 0;
    double cumulative_segment = 0;
    double cumulative_write = 0;

    // Stage 2 parallelism: parse + segment are pure per-track work
    // (global_stats is read-only here), so tracks are processed in
    // bounded blocks on the worker pool; the writes stay sequential
    // and in track order. Block memory is bounded by
    // block_size * largest-track-in-block.
    struct TrackJob {
        std::vector<Note> notes;
        bool ok = true;
        double parse_ms = 0.0;
        double seg_ms = 0.0;
    };

    const size_t parallel_block_size =
        static_cast<size_t>(worker_count(cfg.threads)) * 2;

    for (size_t block_begin = 0;
         block_begin < tracks.size();
         block_begin += parallel_block_size) {
        const size_t block_end =
            std::min(tracks.size(), block_begin + parallel_block_size);
        const size_t block_count = block_end - block_begin;

        std::vector<TrackJob> jobs(block_count);
        std::atomic<size_t> next_job{0};
        std::atomic<bool> job_failed{false};

        auto job_worker = [&](unsigned) {
            try {
                for (;;) {
                    size_t i = next_job.fetch_add(1);
                    if (i >= block_count)
                        return;

                    size_t t = block_begin + i;
                    TrackJob& job = jobs[i];

                    Timer op_timer;

                    if (!analyze_track_into_vector(
                        tracks[t],
                        job.notes,
                        tempo
                    )) {
                        job.ok = false;
                        job_failed.store(true, std::memory_order_relaxed);
                        return;
                    }

                    job.parse_ms = op_timer.restart_ms();

                    if (job.notes.empty()) {
                        tracks[t].note_begin = 0;
                        tracks[t].note_end = 0;
                    } else {
                        build_track_segments(
                            tracks[t],
                            job.notes,
                            global_stats,
                            tempo,
                            measure_map,
                            cfg
                        );

                        job.seg_ms = op_timer.restart_ms();
                    }
                }
            } catch (...) {
                job_failed.store(true, std::memory_order_relaxed);
            }
        };

        run_workers(cfg.threads, job_worker);

        if (job_failed.load()) {
            std::printf(
                "Failed to process tracks %zu-%zu.\n",
                block_begin + 1,
                block_end
            );
            return 1;
        }

        for (size_t i = 0; i < block_count; ++i) {
            size_t t = block_begin + i;
            TrackJob& job = jobs[i];

            cumulative_parse += job.parse_ms;
            cumulative_segment += job.seg_ms;
            total_notes += job.notes.size();

            if ((t + 1) % 100 == 0 || t == 0) {
                std::printf(
                    "  Track %zu/%zu: %zu notes (parse %.1fms)\n",
                    t + 1,
                    tracks.size(),
                    job.notes.size(),
                    job.parse_ms
                );
                fflush(stdout);
            }

            Timer write_timer;

            if (!write_track(
                out,
                tracks[t],
                job.notes,
                globals,
                format,
                move_globals
            )) {
                std::printf(
                    "Failed while writing track %zu.\n",
                    t
                );
                return 1;
            }

            double write_ms = write_timer.elapsed_ms();
            cumulative_write += write_ms;

            kept_notes += tracks[t].kept_notes;

            if (cfg.verbose || (t + 1) % 100 == 0 ||
                (t + 1) == tracks.size()) {
                std::printf(
                    "Track %3zu: notes=%zu kept=%zu "
                    "avg=%.1f max=%u segments=%zu "
                    "(parse=%.1fms seg=%.1fms write=%.1fms)\n",
                    t,
                    job.notes.size(),
                    tracks[t].kept_notes,
                    tracks[t].raw_average_velocity,
                    tracks[t].raw_max_velocity,
                    tracks[t].segments.size(),
                    job.parse_ms,
                    job.seg_ms,
                    write_ms
                );
            }

            // job.notes is freed when the block's jobs vector is
            // destroyed; release the per-track segment data now.
            tracks[t].segments.clear();
            tracks[t].segments.shrink_to_fit();
            tracks[t].order.clear();
            tracks[t].order.shrink_to_fit();
        }
    }

    double total_phase2b_ms = phase2b_timer.elapsed_ms();

    std::printf(
        "\nTotal notes: %zu -> %zu kept (%.2f%%)\n",
        total_notes,
        kept_notes,
        total_notes
            ? 100.0 * kept_notes / total_notes
            : 0.0
    );

    std::printf(
        "\n[Phase 2b] Timing breakdown:\n"
        "  Parse:     %.1fms (avg: %.2fms/track)\n"
        "  Segment:   %.1fms (avg: %.2fms/track)\n"
        "  Write:     %.1fms (avg: %.2fms/track)\n"
        "  Total:     %.1fms\n",
        cumulative_parse,
        cumulative_parse / tracks.size(),
        cumulative_segment,
        cumulative_segment / tracks.size(),
        cumulative_write,
        cumulative_write / tracks.size(),
        total_phase2b_ms
    );
    fflush(stdout);

    std::printf("\n[Phase 3] Finalizing...\n");
    fflush(stdout);

    out.close();

    std::printf("[Phase 3] Releasing memory...\n");
    fflush(stdout);

    tracks.clear();
    tracks.shrink_to_fit();
    globals.clear();
    globals.shrink_to_fit();

    std::printf(
        "Wrote %ls\n",
        argv[2]
    );

    std::printf(
        "Total wall time: %.1fms\n",
        total_timer.elapsed_ms()
    );
    fflush(stdout);

    return 0;
}
