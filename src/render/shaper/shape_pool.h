#pragma once

#include "render/shaper/shaper.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace krait::render {

// Bounded LRU of shaped runs, keyed as the plan specifies: (font, attrs,
// cluster-text). Attrs means shapingBits() only — see shaped_run.h for why
// colour is excluded.
//
// The bound matters more than the hit rate: the key content is chosen by remote
// output (rules/net.md), so an attacker picks the workload. It is ONE aggregate
// byte budget rather than a count plus a text-length cap, because counting
// either alone gets it wrong in both directions:
//
//   * an entry count does not bound memory. A run's text is bounded by
//     cols x Grid::kMaxClusterLen (240 x 16 = 3840 codepoints), not by the
//     column count, so 2048 entries can hold tens of megabytes.
//   * a per-run length cap in CODEPOINTS silently excludes ordinary text.
//     สวัสดี is 6 codepoints for 4 cells, so a 256-codepoint cap stops caching
//     Thai at ~170 columns; a row of ZWJ family emoji falls off at ~45. Those
//     rows would then re-shape through HarfBuzz every frame, on the blocking
//     prepare path — Krait's flagship script breaking the render.md fps gate.
class ShapeCache {
  public:
    static constexpr std::size_t kMaxCacheBytes = 8U * 1024U * 1024U;

    struct Key {
        std::uint32_t faceId = 0;
        std::uint16_t shaping = 0;
        bool ligatures = false;
        std::u32string text;
        // Script and direction are today derived from `text` alone by splitRow,
        // so these are redundant — and pinned here anyway. Shaper::shape feeds
        // both to HarfBuzz, so if a later splitter ever decides script from
        // context rather than content, their absence would silently serve
        // wrong-glyph cache hits. Four bytes to make that impossible.
        ScriptTag script = 0;
        bool rightToLeft = false;

        friend bool operator==(const Key&, const Key&) = default;
    };

    // nullptr on a miss. The pointer is valid only until the next insert, so
    // callers copy out of it.
    const ShapedRun* find(const Key& key);

    void insert(const Key& key, const ShapedRun& run);

    std::size_t size() const { return m_lru.size(); }

    std::size_t bytes() const { return m_cost; }

    void clear();

    // What one entry charges against the budget. The key is counted twice
    // because it really is stored twice, once in the list and once as the map
    // key — an accounting that lied about that would defeat the point.
    static std::size_t costOf(const Key& key, const ShapedRun& run);

    // Public because shapeAll dedupes a batch with its own map of these keys
    // before enqueuing any work.
    struct KeyHash {
        std::size_t operator()(const Key& key) const;
    };

  private:
    using Entry = std::pair<Key, ShapedRun>;

    std::list<Entry> m_lru;  // front is most recently used
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> m_index;
    std::size_t m_cost = 0;
};

// The shaping worker pool (T23). Owns the cache and N worker threads, each with
// its own private Shaper — so its own FT_Library, FT_Faces and hb_font_t, which
// is what FreeType's threading rules require.
//
// The public API is called from ONE thread (the renderer's prepare step). Cache
// hits are served on that thread without touching HarfBuzz; only misses are
// handed to a worker, which satisfies rules/render.md's "the render thread never
// calls HarfBuzz" while keeping the steady state lock-light.
class ShapePool {
  public:
    // 0 picks a count from the hardware. Shaping is bursty and the cache absorbs
    // the steady state, so this deliberately does not scale to every core —
    // extra workers would add cache-lock contention and win nothing.
    explicit ShapePool(unsigned workerCount = 0);
    ~ShapePool();
    ShapePool(const ShapePool&) = delete;
    ShapePool& operator=(const ShapePool&) = delete;

    // Registers a face and returns its id. The spec is validated here, on the
    // calling thread, so a bad path fails at registration instead of silently
    // becoming blank glyphs inside a worker later. Workers open their own copy
    // lazily on first use.
    std::optional<std::uint32_t> registerFace(const FaceSpec& spec);

    std::optional<FaceMetrics> metrics(std::uint32_t faceId) const;

    // Shapes every run in order; `out` is resized to runs.size().
    //
    // Returns false if the timeout expired, in which case the unfinished slots
    // hold an empty ShapedRun — the frame draws without them and the next frame
    // finds them cached. A hard timeout is required by rules/cpp.md, and the
    // batch is reference-counted precisely so a late worker cannot write into
    // caller memory that has already gone away.
    //
    // The default is sub-frame on purpose. render.md gates the renderer's share
    // of input latency at under 10 ms, and this call blocks the prepare thread;
    // a generous timeout would spend the whole budget waiting on a cold cache
    // when the documented graceful path — draw this frame without those runs,
    // find them cached next frame — is already better.
    bool shapeAll(std::span<const Run> runs, std::uint32_t faceId, bool ligatures,
                  std::vector<ShapedRun>& out,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds{8});

    unsigned workerCount() const { return static_cast<unsigned>(m_workers.size()); }

    std::size_t cacheSize() const;
    std::uint64_t cacheHits() const;
    std::uint64_t cacheMisses() const;

  private:
    // One batch of misses. Held by shared_ptr so that a timed-out shapeAll can
    // return while workers still hold a reference; nothing here points at the
    // caller's memory.
    struct Batch {
        std::vector<Run> runs;
        std::vector<ShapedRun> results;
        std::size_t pending = 0;
    };

    // Takes the jthread's stop_token: without it request_stop() is inert, and a
    // constructor that throws while spawning worker 2 would leave worker 1
    // spinning on its idle poll forever while ~jthread joins it — m_stopping is
    // only ever set by OUR destructor, which never runs on that path.
    void workerLoop(unsigned index, std::stop_token stop);

    const FaceSpec* specFor(std::uint32_t faceId) const;  // caller holds m_mutex

    mutable std::mutex m_mutex;
    std::condition_variable m_tasksAvailable;
    std::condition_variable m_batchDone;
    std::deque<std::function<void(Shaper&)>> m_tasks;
    bool m_stopping = false;

    ShapeCache m_cache;
    std::uint64_t m_hits = 0;
    std::uint64_t m_misses = 0;

    std::vector<FaceSpec> m_specs;  // index == faceId

    // The calling thread's own Shaper: validates registrations and answers
    // metrics() without a round trip through a worker. Never used for batch
    // shaping.
    Shaper m_local;

    std::vector<std::jthread> m_workers;
};

}  // namespace krait::render
