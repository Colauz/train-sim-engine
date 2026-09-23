#include "check.hpp"

namespace noire::test {

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

int& current_failures() {
    static int failures = 0;
    return failures;
}

void report_failure(const char* file, int line, const std::string& what) {
    ++current_failures();
    std::printf("      %s:%d : %s\n", file, line, what.c_str());
}

}  // namespace noire::test

// Un seul binaire, tous les cas. Il rend le nombre de cas en échec — donc 0 quand tout
// passe, ce qui est exactement ce que CTest attend.
int main(int argc, char** argv) {
    // Argument optionnel : ne joue que les cas dont le nom contient ce motif. Pratique
    // pour rejouer UN cas après l'avoir cassé, sans relire 40 lignes de sortie.
    const char* filter = (argc > 1) ? argv[1] : nullptr;

    int run = 0;
    int failed = 0;
    for (const auto& c : noire::test::registry()) {
        if (filter != nullptr && std::string(c.name).find(filter) == std::string::npos) {
            continue;
        }
        ++run;
        const int before = noire::test::current_failures();
        c.fn();
        const bool ok = noire::test::current_failures() == before;
        if (!ok) {
            ++failed;
        }
        std::printf("  [%s] %s\n", ok ? "OK  " : "ÉCHEC", c.name);
    }

    if (run == 0) {
        std::printf("Aucun cas ne correspond au filtre.\n");
        return 1;
    }
    std::printf("\n%d cas joués, %d en échec.\n", run, failed);
    return failed;
}
