// Banc A/B du PoC M13.5 : compare le générateur de voie C++ d'origine et le générateur
// Rust, sur la MÊME voie. C'est la validation qui autorise (ou non) le kill switch.
//
// Compte de sommets/indices : doit être IDENTIQUE au sommet près. Données (positions,
// normales, uv, tangentes) : comparées à un epsilon. On ne vise PAS l'égalité bit-à-bit —
// deux compilateurs (gcc/rustc) émettent un code flottant potentiellement différent (ordre
// des FMA, etc.). Un epsilon serré prouve l'équivalence géométrique, ce qui suffit.
//
// Compilé UNIQUEMENT quand NOIRE_USE_RUST=ON (il lie generate_track_mesh_rust).

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "noire/core/track_source.hpp"
#include "noire/scene/track_mesh.hpp"

using namespace noire;

namespace {

// Voie courbe et en pente : sinusoïde en plan + légère montée. Exerce la courbure (donc
// des repères right/up non triviaux), là où une voie droite ne testerait que l'extrusion.
class SineTrack final : public TrackSource {
public:
    void sample(double x, glm::dvec3& position, glm::dvec3& tangent) const override {
        const double amp = 40.0;      // amplitude latérale (m)
        const double k = 0.004;       // nombre d'onde
        const double grade = 0.015;   // pente longitudinale
        const double z = x;
        const double px = amp * std::sin(k * x);
        const double py = grade * x + 2.0 * std::sin(k * 0.5 * x);
        position = glm::dvec3(px, py, z);
        // Tangente = dérivée, normalisée.
        const double dpx = amp * k * std::cos(k * x);
        const double dpy = grade + 2.0 * 0.5 * k * std::cos(k * 0.5 * x);
        tangent = glm::normalize(glm::dvec3(dpx, dpy, 1.0));
    }
    [[nodiscard]] double arc_rate(double) const override { return 1.0; }
};

struct Stats {
    std::size_t verts_cpp = 0, verts_rust = 0;
    std::size_t idx_cpp = 0, idx_rust = 0;
    double max_pos = 0.0, max_nrm = 0.0, max_uv = 0.0, max_tan = 0.0;
    bool index_mismatch = false;
    bool count_mismatch = false;
};

double amax(double a, double b) { return a > b ? a : b; }

void compare_sub(const scene::RailMeshData& a, const scene::RailMeshData& b, Stats& st,
                 const char* /*name*/) {
    st.verts_cpp += a.vertices.size();
    st.verts_rust += b.vertices.size();
    st.idx_cpp += a.indices.size();
    st.idx_rust += b.indices.size();
    if (a.vertices.size() != b.vertices.size() || a.indices.size() != b.indices.size()) {
        st.count_mismatch = true;
        return;  // comparer terme à terme n'a plus de sens
    }
    for (std::size_t i = 0; i < a.vertices.size(); ++i) {
        const auto& va = a.vertices[i];
        const auto& vb = b.vertices[i];
        for (int c = 0; c < 3; ++c) {
            st.max_pos = amax(st.max_pos, std::abs(va.position[c] - vb.position[c]));
            st.max_nrm = amax(st.max_nrm, std::abs(va.normal[c] - vb.normal[c]));
        }
        for (int c = 0; c < 2; ++c)
            st.max_uv = amax(st.max_uv, std::abs(va.uv[c] - vb.uv[c]));
        for (int c = 0; c < 4; ++c)
            st.max_tan = amax(st.max_tan, std::abs(va.tangent[c] - vb.tangent[c]));
    }
    for (std::size_t i = 0; i < a.indices.size(); ++i) {
        if (a.indices[i] != b.indices[i]) {
            st.index_mismatch = true;
            break;
        }
    }
}

int run_case(const char* label, scene::TrackLod lod, double x0, double x1) {
    SineTrack track;
    const WorldPosition origin(0.0, 0.0, x0);  // origine flottante, comme une vraie tuile
    scene::RailProfile profile;  // valeurs par défaut

    const scene::TrackMeshData cpp = scene::generate_track_mesh(track, x0, x1, origin, profile, lod);
    const scene::TrackMeshData rust =
        scene::generate_track_mesh_rust(track, x0, x1, origin, profile, lod);

    Stats st;
    compare_sub(cpp.rails, rust.rails, st, "rails");
    compare_sub(cpp.sleepers, rust.sleepers, st, "traverses");
    compare_sub(cpp.ballast, rust.ballast, st, "ballast");

    std::printf("[%s] sommets C++=%zu Rust=%zu | indices C++=%zu Rust=%zu\n", label,
                st.verts_cpp, st.verts_rust, st.idx_cpp, st.idx_rust);
    std::printf("        écart max : pos=%.2e  normale=%.2e  uv=%.2e  tangente=%.2e\n",
                st.max_pos, st.max_nrm, st.max_uv, st.max_tan);

    // Seuils : les comptes DOIVENT coïncider et les indices être identiques ; les données
    // tolèrent un epsilon flottant (positions en mètres => 1e-3 est déjà 1 mm).
    const double kPosEps = 1e-3, kDirEps = 1e-4;
    bool ok = !st.count_mismatch && !st.index_mismatch && st.verts_cpp == st.verts_rust &&
              st.idx_cpp == st.idx_rust && st.max_pos < kPosEps && st.max_nrm < kDirEps &&
              st.max_uv < kPosEps && st.max_tan < kDirEps;
    if (st.count_mismatch) std::printf("        ÉCHEC : comptes divergents\n");
    if (st.index_mismatch) std::printf("        ÉCHEC : indices divergents\n");
    std::printf("        => %s\n", ok ? "OK" : "ÉCHEC");
    return ok ? 0 : 1;
}

}  // namespace

// Débit de génération : N tuiles Full de 2 km, C++ puis Rust. Ne vaut QUE si les deux
// côtés sont compilés au même niveau d'optimisation (RelWithDebInfo/Release).
void bench(int iters) {
    SineTrack track;
    const WorldPosition origin(0.0, 0.0, 0.0);
    scene::RailProfile profile;
    volatile std::size_t sink = 0;  // empêche l'élision du résultat

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        const double x = static_cast<double>(i) * 2000.0;
        auto m = scene::generate_track_mesh(track, x, x + 2000.0, origin, profile,
                                            scene::TrackLod::Full);
        sink += m.rails.vertices.size();
    }
    auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        const double x = static_cast<double>(i) * 2000.0;
        auto m = scene::generate_track_mesh_rust(track, x, x + 2000.0, origin, profile,
                                                 scene::TrackLod::Full);
        sink += m.rails.vertices.size();
    }
    auto t2 = std::chrono::steady_clock::now();

    const double cpp_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double rust_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::printf("\n=== Débit (%d tuiles Full de 2 km) ===\n", iters);
    std::printf("  C++  : %.1f ms total, %.3f ms/tuile\n", cpp_ms, cpp_ms / iters);
    std::printf("  Rust : %.1f ms total, %.3f ms/tuile  (%.2fx)\n", rust_ms, rust_ms / iters,
                cpp_ms / rust_ms);
    (void)sink;
}

int main() {
    std::printf("=== Banc A/B M13.5 : generate_track_mesh (C++) vs _rust ===\n");
    int failures = 0;
    failures += run_case("Full   [0, 2000]", scene::TrackLod::Full, 0.0, 2000.0);
    failures += run_case("Distant[0, 2000]", scene::TrackLod::Distant, 0.0, 2000.0);
    failures += run_case("Full   [12345, 13000]", scene::TrackLod::Full, 12345.0, 13000.0);
    std::printf("=== %s ===\n", failures == 0 ? "TOUS OK" : "DES ÉCHECS");
    bench(200);
    return failures == 0 ? 0 : 1;
}
