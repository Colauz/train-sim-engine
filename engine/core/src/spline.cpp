#include "noire/core/spline.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace noire {

namespace {
constexpr int kSamplesPerSegment = 32;  // finesse de la LUT arc-longueur
}  // namespace

void Spline::eval(double u, glm::dvec3& position, glm::dvec3& derivative) const {
    const int segments = static_cast<int>(control_points_.size()) - 1;
    if (segments < 1) {
        position = control_points_.empty() ? glm::dvec3(0.0) : control_points_.front();
        derivative = glm::dvec3(0.0, 0.0, 1.0);
        return;
    }

    const double clamped = glm::clamp(u, 0.0, static_cast<double>(segments));
    int seg = static_cast<int>(std::floor(clamped));
    if (seg >= segments) {
        seg = segments - 1;  // u == segments => dernier segment à t = 1
    }
    const double t = clamped - static_cast<double>(seg);

    const auto pt = [&](int i) -> const glm::dvec3& {
        return control_points_[static_cast<std::size_t>(glm::clamp(i, 0, segments))];
    };
    const glm::dvec3 p0 = pt(seg - 1);
    const glm::dvec3 p1 = pt(seg);
    const glm::dvec3 p2 = pt(seg + 1);
    const glm::dvec3 p3 = pt(seg + 2);

    const double t2 = t * t;
    const double t3 = t2 * t;

    // Catmull-Rom uniforme (tension 0.5).
    const glm::dvec3 a = 2.0 * p1;
    const glm::dvec3 b = -p0 + p2;
    const glm::dvec3 c = 2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3;
    const glm::dvec3 d = -p0 + 3.0 * p1 - 3.0 * p2 + p3;

    position = 0.5 * (a + b * t + c * t2 + d * t3);
    derivative = 0.5 * (b + 2.0 * c * t + 3.0 * d * t2);
}

void Spline::set_control_points(std::vector<glm::dvec3> points) {
    control_points_ = std::move(points);
    lut_s_.clear();
    lut_u_.clear();
    length_ = 0.0;
    if (control_points_.size() < 2) {
        return;
    }

    const int segments = static_cast<int>(control_points_.size()) - 1;
    const int total = segments * kSamplesPerSegment;

    glm::dvec3 prev;
    glm::dvec3 ignored;
    eval(0.0, prev, ignored);
    lut_s_.push_back(0.0);
    lut_u_.push_back(0.0);

    for (int i = 1; i <= total; ++i) {
        const double u = (static_cast<double>(i) / total) * segments;
        glm::dvec3 pos;
        eval(u, pos, ignored);
        length_ += glm::length(pos - prev);
        prev = pos;
        lut_s_.push_back(length_);
        lut_u_.push_back(u);
    }
}

void Spline::sample(double s, glm::dvec3& position, glm::dvec3& tangent) const {
    if (lut_s_.size() < 2) {
        position = control_points_.empty() ? glm::dvec3(0.0) : control_points_.front();
        tangent = glm::dvec3(0.0, 0.0, 1.0);
        return;
    }

    s = glm::clamp(s, 0.0, length_);
    const auto it = std::lower_bound(lut_s_.begin(), lut_s_.end(), s);
    std::size_t hi = static_cast<std::size_t>(it - lut_s_.begin());
    if (hi == 0) {
        hi = 1;
    }
    if (hi >= lut_s_.size()) {
        hi = lut_s_.size() - 1;
    }
    const std::size_t lo = hi - 1;

    const double s0 = lut_s_[lo];
    const double s1 = lut_s_[hi];
    const double f = (s1 > s0) ? (s - s0) / (s1 - s0) : 0.0;
    const double u = lut_u_[lo] + f * (lut_u_[hi] - lut_u_[lo]);

    glm::dvec3 derivative;
    eval(u, position, derivative);
    const double len = glm::length(derivative);
    tangent = (len > 1e-12) ? derivative / len : glm::dvec3(0.0, 0.0, 1.0);
}

glm::dvec3 Spline::position_at(double s) const {
    glm::dvec3 position;
    glm::dvec3 tangent;
    sample(s, position, tangent);
    return position;
}

glm::dvec3 Spline::tangent_at(double s) const {
    glm::dvec3 position;
    glm::dvec3 tangent;
    sample(s, position, tangent);
    return tangent;
}

}  // namespace noire
