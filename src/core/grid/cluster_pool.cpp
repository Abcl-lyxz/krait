#include "core/grid/cluster_pool.h"

namespace krait::core::vt {

char32_t ClusterPool::intern(std::span<const char32_t> cluster) {
    if (cluster.empty()) {
        return 0;
    }
    // The overwhelmingly common case. Storing it literally is why an ASCII
    // screen never touches this class at all.
    if (cluster.size() == 1) {
        return cluster.front();
    }

    const std::u32string_view key(cluster.data(), cluster.size());
    if (const auto it = m_index.find(key); it != m_index.end()) {
        return kClusterTag | static_cast<char32_t>(it->second);
    }
    if (m_clusters.size() >= kMaxClusters) {
        // Full. Degrade to the base codepoint rather than allocate: the text
        // stays readable and memory stays bounded, which is the trade rules/
        // net.md asks for when the input is choosing the workload.
        return cluster.front();
    }

    const auto index = static_cast<std::uint32_t>(m_clusters.size());
    m_clusters.emplace_back(key);
    // Key the index by a view of the STORED string, not of the caller's span,
    // which is a stack buffer that dies at the end of this call.
    m_index.emplace(std::u32string_view(m_clusters.back()), index);
    return kClusterTag | static_cast<char32_t>(index);
}

std::span<const char32_t> ClusterPool::lookup(char32_t ch) const {
    if (!isClusterRef(ch)) {
        return {};
    }
    const std::uint32_t index = clusterRefIndex(ch);
    if (index >= m_clusters.size()) {
        return {};  // a ref that outlived its pool; treat as empty, never index
    }
    const std::u32string& stored = m_clusters[index];
    return {stored.data(), stored.size()};
}

}  // namespace krait::core::vt
