// Cosine similarity + union-find, shared by the demo's threshold clustering
// and the indexer's people clustering (issue #40 PR 3).
#pragma once

#include <cstddef>
#include <vector>

namespace wfp {

// Cosine similarity in [-1, 1]; 0 when either vector is all-zero.
double cosine(const std::vector<float>& a, const std::vector<float>& b);

struct UnionFind {
    std::vector<int> parent;
    explicit UnionFind(size_t n) : parent(n) {
        for (size_t i = 0; i < n; ++i) parent[i] = (int)i;
    }
    int find(int x) { return parent[x] == x ? x : (parent[x] = find(parent[x])); }
    void unite(int a, int b) { parent[find(a)] = find(b); }
};

}  // namespace wfp
