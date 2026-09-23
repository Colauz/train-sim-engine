#pragma once

// MICRO-FRAMEWORK DE TEST — 60 lignes, zéro dépendance.
//
// Pourquoi pas GoogleTest ou Catch2 ? Parce que ce qu'on teste ici tient en trois
// primitives (un cas, une comparaison exacte, une comparaison à tolérance) et que la
// règle du projet est constante depuis le M0 : aucune dépendance qui ne paie pas sa
// place. Un framework de 40 000 lignes récupéré par FetchContent pour écrire
// `CHECK_NEAR(v, 0.0, 1e-9)` ne la paierait pas, et il allongerait le premier configure
// de tout le monde.
//
// Ce qu'on y gagne quand même : l'enregistrement AUTOMATIQUE des cas (aucune liste à
// tenir à jour — oublier d'appeler un test est le seul bug qu'un fichier de tests ne
// peut pas signaler), un compte-rendu par cas, et un code de sortie exploitable par
// CTest.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace noire::test {

using Fn = void (*)();

struct Case {
    const char* name;
    Fn fn;
};

// Fonctions plutôt que variables globales : l'ordre d'initialisation statique entre
// unités de compilation n'est pas défini, et le registre DOIT exister avant le premier
// Register construit. Un static local le garantit.
std::vector<Case>& registry();
int& current_failures();

struct Register {
    Register(const char* name, Fn fn) { registry().push_back({name, fn}); }
};

void report_failure(const char* file, int line, const std::string& what);

#define NOIRE_TEST(name)                                              \
    static void name();                                               \
    static const ::noire::test::Register noire_reg_##name{#name, &name}; \
    static void name()

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ::noire::test::report_failure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
        }                                                                        \
    } while (false)

#define CHECK_NEAR(value, expected, tol)                                          \
    do {                                                                          \
        const double v_ = static_cast<double>(value);                             \
        const double e_ = static_cast<double>(expected);                          \
        if (!(std::fabs(v_ - e_) <= static_cast<double>(tol))) {                   \
            char buf_[256];                                                       \
            std::snprintf(buf_, sizeof(buf_),                                     \
                          "CHECK_NEAR(" #value ", " #expected ", " #tol ")"       \
                          " -> %.9g vs %.9g (écart %.3g)",                        \
                          v_, e_, v_ - e_);                                       \
            ::noire::test::report_failure(__FILE__, __LINE__, buf_);              \
        }                                                                         \
    } while (false)

}  // namespace noire::test
