#include "noire/physics/air_brake.hpp"

#include <algorithm>
#include <cmath>

namespace noire::physics {

namespace {
double clamp01(double x) { return std::clamp(x, 0.0, 1.0); }

std::size_t delay_samples(double seconds, double dt) {
    // Au moins un échantillon : une ligne à retard vide déborderait à la lecture.
    return static_cast<std::size_t>(std::max(1L, std::lround(seconds / dt)));
}
}  // namespace

AirBrake::AirBrake(AirBrakeConfig config) : config_(config) {
    // TEMPS MORT DE SERVICE = L / (2c), et NON L / c.
    //
    // Un Wagon forfaitaire EST la rame entière. L/c = 0,8 s est le temps que met l'onde
    // pour atteindre le DERNIER véhicule — mais le premier freine presque tout de suite.
    // L'effort qu'on modélise est la SOMME sur toute la rame, dont le retard moyen est
    // celui du milieu : L/(2c). Prendre L/c ferait « ne rien freiner pendant 0,8 s, puis
    // tout d'un coup », ce qui est faux pour un effort agrégé.
    const double service_delay = config_.train_length / (2.0 * config_.propagation_speed);
    tap_service_ = delay_samples(service_delay, config_.fixed_dt);
    tap_emergency_ = delay_samples(config_.emergency_delay, config_.fixed_dt);

    // La ligne doit couvrir la plus longue des deux prises.
    const std::size_t span = std::max(tap_service_, tap_emergency_) + 1;
    delay_line_.assign(span, 0.0);  // 0 = aucune dépression demandée (repos)

    pipe_pressure_ = config_.nominal_pressure;
}

void AirBrake::set_handle(double demand, bool emergency) {
    demand_ = clamp01(demand);
    emergency_ = emergency;
}

void AirBrake::update(double dt) {
    // Le pas doit être celui pour lequel la ligne à retard a été dimensionnée. On ne
    // redimensionne pas à chaud (ça décalerait le contenu déjà en vol) : on prévient une
    // fois, et le temps mort reste correct tant que l'Engine tient son pas fixe.
    if (!dt_warned_ && std::abs(dt - config_.fixed_dt) > config_.fixed_dt * 0.01) {
        dt_warned_ = true;  // (log volontairement omis : la physique ne dépend pas du log)
    }

    // --- 1. Consigne du robinet -> DÉPRESSION cible dans la ligne à retard ---
    // En urgence, la cible est la CG entière (chute jusqu'à 0 bar).
    const double target_drop =
        emergency_ ? config_.nominal_pressure : demand_ * config_.full_service_drop;

    // On pousse la cible à l'entrée de la ligne, et on lit à la prise voulue : c'est le
    // temps mort de propagation, ni plus ni moins.
    delay_line_[head_] = target_drop;
    const std::size_t span = delay_line_.size();
    const std::size_t tap = emergency_ ? tap_emergency_ : tap_service_;
    const std::size_t read = (head_ + span - tap) % span;
    const double delayed_drop = delay_line_[read];
    head_ = (head_ + 1) % span;

    // --- 2. Pression CG : glisse vers (nominal - dépression) à DÉBIT LIMITÉ ---
    // C'est la vidange/recharge physique du tuyau : la pression ne saute jamais, même si
    // la consigne, elle, saute. Vidange et recharge à des débits différents (asymétrie).
    const double target_pressure = config_.nominal_pressure - delayed_drop;
    const double rate =
        emergency_ ? config_.emergency_rate
                   : (target_pressure < pipe_pressure_ ? config_.apply_rate
                                                        : config_.release_rate);
    const double max_step = rate * dt;
    const double delta = target_pressure - pipe_pressure_;
    pipe_pressure_ += std::clamp(delta, -max_step, max_step);
    pipe_pressure_ = std::clamp(pipe_pressure_, 0.0, config_.nominal_pressure);

    // --- 3. Cylindres : effort cible = dépression rapportée au service maximal ---
    const double drop = config_.nominal_pressure - pipe_pressure_;
    double target_force = drop / config_.full_service_drop;
    // Plafond = emergency_ratio : c'est la saturation mécanique du cylindre, ce qui
    // permet à l'urgence (dépression > service maximal) de mordre plus fort que le service
    // sans jamais partir à l'infini.
    target_force = std::clamp(target_force, 0.0, config_.emergency_ratio);

    // --- 4. Retard du premier ordre : le cylindre ne se remplit pas instantanément ---
    // alpha = 1 - exp(-dt/tau) : la forme EXACTE d'un premier ordre échantillonné, stable
    // quel que soit le pas (un simple dt/tau divergerait si tau < dt).
    const double alpha = 1.0 - std::exp(-dt / config_.cylinder_tau);
    cylinder_ += (target_force - cylinder_) * alpha;
}

}  // namespace noire::physics
