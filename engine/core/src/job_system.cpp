#include "noire/core/job_system.hpp"

#include "noire/core/log.hpp"

namespace noire {

JobSystem::~JobSystem() { stop(); }

void JobSystem::start(unsigned worker_count) {
    if (running_.exchange(true)) {
        return;  // déjà démarré
    }
    if (worker_count == 0) {
        worker_count = 1;
    }
    workers_.reserve(worker_count);
    for (unsigned i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
    log::info("Job system : {} worker(s) démarré(s)", worker_count);
}

void JobSystem::stop() {
    if (!running_.exchange(false)) {
        return;  // déjà arrêté
    }
    cv_.notify_all();
    for (std::thread& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();
}

void JobSystem::submit(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push(std::move(job));
    }
    in_flight_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_one();
}

void JobSystem::worker_loop() {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !running_.load() || !jobs_.empty(); });
            // À l'arrêt, on draine la file restante avant de sortir.
            if (jobs_.empty()) {
                if (!running_.load()) {
                    return;
                }
                continue;
            }
            job = std::move(jobs_.front());
            jobs_.pop();
        }
        job();
        in_flight_.fetch_sub(1, std::memory_order_relaxed);
    }
}

}  // namespace noire
