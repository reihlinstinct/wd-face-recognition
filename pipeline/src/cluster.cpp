#include "cluster.h"

#include <cmath>

namespace wfp {

double cosine(const std::vector<float>& a, const std::vector<float>& b) {
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += (double)a[i] * b[i];
        na += (double)a[i] * a[i];
        nb += (double)b[i] * b[i];
    }
    if (na == 0 || nb == 0) return 0;
    return dot / std::sqrt(na * nb);
}

}  // namespace wfp
