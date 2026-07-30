#include "render/shaper/shape_pool.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <utility>

namespace krait::render {
namespace {

// rules/cpp.md: every thread is named at spawn.
void nameThread(unsigned index) {
    const std::wstring name = L"krait-shaper-" + std::to_wstring(index);
    SetThreadDescription(GetCurrentThread(), name.c_str());
}

unsigned chooseWorkerCount(unsigned requested) {
    if (requested > 0) {
        return requested;
    }
    const unsigned hardware = std::thread::hardware_concurrency();
    return std::clamp(hardware / 2, 1U, 4U);
}

// An idle worker parks here. The poll interval only exists so that no wait in
// the process is unbounded (rules/cpp.md); a lost notify costs a second of
// latency instead of a hang.
constexpr auto kIdlePoll = std::chrono::seconds{1};

}  // namespace

std::size_t ShapeCache::KeyHash::operator()(const Key& key) const {
    std::size_t hash = std::hash<std::u32string>{}(key.text);
    hash = hash * 1000003U ^ key.faceId;
    hash = hash * 1000003U ^ key.shaping;
    hash = hash * 1000003U ^ (key.ligatures ? 1U : 0U);
    hash = hash * 1000003U ^ key.script;
    return hash;
}

std::size_t ShapeCache::costOf(const Key& key, const ShapedRun& run) {
    return (2 * key.text.size() * sizeof(char32_t)) + (run.glyphs.size() * sizeof(ShapedGlyph)) +
           sizeof(Entry);
}

const ShapedRun* ShapeCache::find(const Key& key) {
    const auto it = m_index.find(key);
    if (it == m_index.end()) {
        return nullptr;
    }
    m_lru.splice(m_lru.begin(), m_lru, it->second);
    return &it->second->second;
}

void ShapeCache::insert(const Key& key, const ShapedRun& run) {
    const std::size_t cost = costOf(key, run);
    // No single entry may own more than an eighth of the budget. Derived rather
    // than a second tunable, and it is what stops one pathological run from
    // evicting the entire cache just to not fit anyway.
    if (cost > kMaxCacheBytes / 8) {
        return;
    }

    if (const auto it = m_index.find(key); it != m_index.end()) {
        m_cost -= costOf(it->second->first, it->second->second);
        it->second->second = run;
        m_cost += cost;
        m_lru.splice(m_lru.begin(), m_lru, it->second);
        return;
    }
    m_lru.emplace_front(key, run);
    m_index.emplace(key, m_lru.begin());
    m_cost += cost;

    while (m_cost > kMaxCacheBytes && !m_lru.empty()) {
        const Entry& oldest = m_lru.back();
        m_cost -= costOf(oldest.first, oldest.second);
        m_index.erase(oldest.first);
        m_lru.pop_back();
    }
}

void ShapeCache::clear() {
    m_index.clear();
    m_lru.clear();
    m_cost = 0;
}

ShapePool::ShapePool(unsigned workerCount) {
    const unsigned count = chooseWorkerCount(workerCount);
    m_workers.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        m_workers.emplace_back([this, i](std::stop_token stop) { workerLoop(i, std::move(stop)); });
    }
}

ShapePool::~ShapePool() {
    {
        const std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_tasksAvailable.notify_all();
    // m_workers is declared last, so its jthreads join here — before m_cache,
    // m_tasks and m_local, which the workers touch, are destroyed.
}

void ShapePool::workerLoop(unsigned index, std::stop_token stop) {
    nameThread(index);
    // Private to this thread for its whole life: FreeType allows one FT_Face on
    // one thread at a time, and hb-ft inherits that.
    Shaper shaper;

    for (;;) {
        std::function<void(Shaper&)> task;
        {
            std::unique_lock lock(m_mutex);
            m_tasksAvailable.wait_for(lock, kIdlePoll, [this, &stop] {
                return m_stopping || stop.stop_requested() || !m_tasks.empty();
            });
            // Stop is checked BEFORE the queue, so shutdown DROPS the backlog
            // instead of draining it. Draining would make ~ShapePool run every
            // queued hb_shape before its join returns — on the UI thread.
            if (m_stopping || stop.stop_requested()) {
                return;
            }
            if (m_tasks.empty()) {
                continue;  // spurious wake or idle poll
            }
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
        }
        task(shaper);
    }
}

std::optional<std::uint32_t> ShapePool::registerFace(const FaceSpec& spec) {
    const std::lock_guard lock(m_mutex);

    // Idempotent. shapeWithFallback (T24) registers a fallback face for every
    // missing-glyph run it meets, every frame, so without this the spec table —
    // and every worker's FT_Face set — would grow for the life of the session.
    for (std::size_t i = 0; i < m_specs.size(); ++i) {
        if (m_specs[i].path == spec.path && m_specs[i].index == spec.index &&
            m_specs[i].pxHeight == spec.pxHeight) {
            return static_cast<std::uint32_t>(i);
        }
    }

    const auto faceId = static_cast<std::uint32_t>(m_specs.size());
    if (!m_local.loadFace(faceId, spec)) {
        return std::nullopt;
    }
    m_specs.push_back(spec);
    return faceId;
}

std::optional<FaceMetrics> ShapePool::metrics(std::uint32_t faceId) const {
    const std::lock_guard lock(m_mutex);
    return m_local.metrics(faceId);
}

const FaceSpec* ShapePool::specFor(std::uint32_t faceId) const {
    return faceId < m_specs.size() ? &m_specs[faceId] : nullptr;
}

std::size_t ShapePool::cacheSize() const {
    const std::lock_guard lock(m_mutex);
    return m_cache.size();
}

std::uint64_t ShapePool::cacheHits() const {
    const std::lock_guard lock(m_mutex);
    return m_hits;
}

std::uint64_t ShapePool::cacheMisses() const {
    const std::lock_guard lock(m_mutex);
    return m_misses;
}

bool ShapePool::shapeAll(std::span<const Run> runs, std::uint32_t faceId, bool ligatures,
                         std::vector<ShapedRun>& out, std::chrono::milliseconds timeout) {
    out.assign(runs.size(), ShapedRun{});
    if (runs.empty()) {
        return true;
    }

    const auto keyOf = [faceId, ligatures](const Run& run) {
        return ShapeCache::Key{.faceId = faceId,
                               .shaping = run.shaping,
                               .ligatures = ligatures,
                               .text = run.text,
                               .script = run.script,
                               .rightToLeft = run.rightToLeft};
    };

    auto batch = std::make_shared<Batch>();
    // batch index -> every index in `out` wanting that result. Identical runs
    // share one entry: a screen of repeated rows (a `yes` flood, box drawing, a
    // reprinted prompt) would otherwise enqueue N tasks that shape the same text
    // and then overwrite each other's cache entry.
    std::vector<std::vector<std::size_t>> slots;
    std::unordered_map<ShapeCache::Key, std::size_t, ShapeCache::KeyHash> seen;

    {
        std::unique_lock lock(m_mutex);
        // Anything still queued belongs to an earlier batch that timed out. The
        // public API is single-threaded and blocking, so only one batch is ever
        // in flight and this backlog is by definition stale — keeping it would
        // let the deque grow without bound across sustained timeouts.
        m_tasks.clear();

        for (std::size_t i = 0; i < runs.size(); ++i) {
            ShapeCache::Key key = keyOf(runs[i]);
            if (const ShapedRun* hit = m_cache.find(key)) {
                out[i] = *hit;
                ++m_hits;
                continue;
            }
            ++m_misses;
            if (const auto duplicate = seen.find(key); duplicate != seen.end()) {
                slots[duplicate->second].push_back(i);
                continue;
            }
            seen.emplace(std::move(key), batch->runs.size());
            slots.push_back({i});
            batch->runs.push_back(runs[i]);
        }
        if (batch->runs.empty()) {
            return true;
        }
        batch->results.resize(batch->runs.size());
        batch->pending = batch->runs.size();

        for (std::size_t j = 0; j < batch->runs.size(); ++j) {
            m_tasks.emplace_back([this, batch, j, faceId, ligatures](Shaper& shaper) {
                // Workers open their own FT_Face on first use. The spec was
                // already validated by registerFace, so a failure here is an
                // out-of-resources one and degrades to an empty run.
                if (!shaper.hasFace(faceId)) {
                    std::optional<FaceSpec> spec;
                    {
                        const std::lock_guard lock(m_mutex);
                        if (const FaceSpec* found = specFor(faceId)) {
                            spec = *found;
                        }
                    }
                    if (spec.has_value()) {
                        shaper.loadFace(faceId, *spec);
                    }
                }

                const Run& run = batch->runs[j];
                const bool loaded = shaper.hasFace(faceId);
                ShapedRun result = shaper.shape(run, faceId, ligatures);

                const std::lock_guard lock(m_mutex);
                // ONLY cache a result that came from a real face. Caching the
                // empty run a failed load produces would turn every later frame
                // into a cache HIT of zero glyphs — the text would be invisible
                // for the rest of the session, with no invalidation path, and
                // missingGlyphs staying false would stop the T24 fallback from
                // ever firing on it either.
                if (loaded) {
                    m_cache.insert(ShapeCache::Key{.faceId = faceId,
                                                   .shaping = run.shaping,
                                                   .ligatures = ligatures,
                                                   .text = run.text,
                                                   .script = run.script,
                                                   .rightToLeft = run.rightToLeft},
                                   result);
                }
                batch->results[j] = std::move(result);
                if (--batch->pending == 0) {
                    m_batchDone.notify_all();
                }
            });
        }
    }
    m_tasksAvailable.notify_all();

    std::unique_lock lock(m_mutex);
    const bool finished =
        m_batchDone.wait_for(lock, timeout, [&batch] { return batch->pending == 0; });
    // Copied under the lock, which is also where the workers write: that is what
    // makes reading a half-finished batch after a timeout safe rather than torn.
    for (std::size_t j = 0; j < slots.size(); ++j) {
        for (const std::size_t target : slots[j]) {
            out[target] = batch->results[j];
        }
    }
    return finished;
}

}  // namespace krait::render
