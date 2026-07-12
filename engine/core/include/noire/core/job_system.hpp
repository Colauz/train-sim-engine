#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace noire {

// Pool de threads minimal : exécute des tâches (std::function<void()>) sur des
// workers. `submit()` ne bloque JAMAIS l'appelant (il empile puis réveille un
// worker). Sert à la génération asynchrone du décor (chunks de voie).
class JobSystem {
public:
    JobSystem() = default;
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    void start(unsigned worker_count);
    void stop();  // draine les jobs en attente puis joint les workers

    void submit(std::function<void()> job);

    // Nombre de jobs soumis mais pas encore terminés (diagnostic).
    [[nodiscard]] std::size_t in_flight() const { return in_flight_.load(); }

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> jobs_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> in_flight_{0};
};

}  // namespace noire
