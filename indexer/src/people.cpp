#include "people.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "cluster.h"

namespace wfi {

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (const unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char u[8];
                    std::snprintf(u, sizeof(u), "\\u%04x", c);
                    o += u;
                } else {
                    o += (char)c;  // UTF-8 passes through
                }
        }
    }
    return o;
}

std::string people_json(const std::vector<PersonSummary>& people) {
    std::string o = "[";
    bool first = true;
    for (const PersonSummary& ps : people) {
        if (!first) o += ",";
        first = false;
        o += "{\"id\":" + std::to_string(ps.id) +
             ",\"name\":\"" + json_escape(ps.name) +
             "\",\"faces\":" + std::to_string(ps.face_count) +
             ",\"cover\":\"" + json_escape(ps.cover_crop) + "\"}";
    }
    o += "]";
    return o;
}

std::string person_json(const PersonSummary& who,
                        const std::vector<PersonFace>& faces) {
    std::string o = "{\"id\":" + std::to_string(who.id) +
                    ",\"name\":\"" + json_escape(who.name) + "\",\"faces\":[";
    bool first = true;
    for (const PersonFace& pf : faces) {
        if (!first) o += ",";
        first = false;
        o += "{\"id\":" + std::to_string(pf.id) +
             ",\"bbox\":\"" + json_escape(pf.bbox) +
             "\",\"crop\":\"" + json_escape(pf.crop_path) +
             "\",\"photo\":\"" + json_escape(pf.photo_path) + "\"}";
    }
    o += "]}";
    return o;
}


bool recluster(Db& db, double threshold, ClusterCounts& counts,
               std::string& err) {
    std::vector<ClusterFace> faces;
    if (!db.all_face_embeddings(faces, err)) return false;

    const size_t n = faces.size();
    wfp::UnionFind uf(n);
    // Skip pairs where either embedding is missing/empty: cosine() returns
    // 0 for them, but being explicit keeps the guard independent of the
    // similarity implementation.
    for (size_t i = 0; i < n; ++i) {
        if (faces[i].embedding.empty()) continue;
        for (size_t j = i + 1; j < n; ++j) {
            // Mixed dimensions can only come from a version skew; cosine()
            // assumes equal length, so skip the pair (never unites).
            if (faces[j].embedding.size() != faces[i].embedding.size())
                continue;
            if (wfp::cosine(faces[i].embedding, faces[j].embedding) > threshold)
                uf.unite((int)i, (int)j);
        }
    }

    std::map<int, std::vector<long>> by_root;
    for (size_t i = 0; i < n; ++i) by_root[uf.find((int)i)].push_back(faces[i].id);
    std::vector<std::vector<long>> groups;
    groups.reserve(by_root.size());
    counts = {};
    counts.faces = (long)n;
    for (auto& kv : by_root) {
        if (kv.second.size() == 1) ++counts.singletons;
        groups.push_back(std::move(kv.second));
    }
    // Deterministic output: largest groups first, then by smallest face id.
    std::sort(groups.begin(), groups.end(),
              [](const std::vector<long>& a, const std::vector<long>& b) {
                  if (a.size() != b.size()) return a.size() > b.size();
                  return a.front() < b.front();
              });
    counts.people = (long)groups.size();
    if (!db.replace_clusters(groups, err)) return false;
    return true;
}
}  // namespace wfi
