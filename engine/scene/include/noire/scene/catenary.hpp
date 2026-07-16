#pragma once

#include <vector>

#include "noire/core/math.hpp"
#include "noire/core/track_source.hpp"
#include "noire/render/vertex.hpp"
#include "noire/scene/track_mesh.hpp"

namespace noire::scene {

// Caténaire de LGV (M12), engendrée le long de la spline de voie.
//
// CONVENTION VERTICALE : identique au M9 — la courbe rendue par TrackSource::sample EST le
// PLAN DE ROULEMENT. Toutes les hauteurs ci-dessous se comptent donc depuis lui, ce qui
// est exactement la convention des normes ferroviaires (« hauteur du fil de contact au-
// dessus du plan de roulement »). Aucune conversion, aucun décalage à retenir.
struct CatenaryProfile {
    // --- Géométrie longitudinale ---
    double span = 50.0;         // portée entre deux poteaux (LGV)
    double pole_offset = 3.0;   // distance du poteau à l'axe de la voie

    // --- Fil de contact ---
    double contact_height = 5.20;  // au-dessus du plan de roulement
    // DÉSAXEMENT (zigzag). Le fil ne suit pas l'axe : il louvoie de ±20 cm d'un poteau à
    // l'autre. Sans lui, l'archet du pantographe serait usé TOUJOURS au même point et s'y
    // creuserait un sillon ; le zigzag étale l'usure sur toute sa largeur. Le fil est
    // TENDU, donc entre deux poteaux il est droit : le désaxement s'interpole linéairement.
    double stagger = 0.20;

    // --- Câble porteur ---
    // « Hauteur système » : écart porteur/contact AU POTEAU. ~1,40 m sur LGV.
    double system_height = 1.40;
    // Flèche du porteur à mi-portée. Il pend sous son propre poids ; le fil de contact,
    // lui, reste RIGOUREUSEMENT horizontal (c'est tout l'objet des pendules). Doit rester
    // < system_height, sinon le porteur viendrait toucher le fil de contact.
    double messenger_sag = 0.70;
    double dropper_spacing = 5.0;  // entraxe des pendules
    double wire_step = 2.5;        // pas de tessellation du porteur (la parabole)

    // --- Rayons (mètres) ---
    // Le fil de contact fait 150 mm² => ~15 mm de diamètre. Ces rayons ne servent PAS à
    // engendrer des tubes : ils sont poussés dans le sommet et le vertex shader s'en sert
    // pour calculer la COUVERTURE en pixels (cf. wire.vert).
    float contact_radius = 0.0075f;
    float messenger_radius = 0.0055f;
    float dropper_radius = 0.0025f;

    // --- Poteau (profilé en H + console) ---
    float pole_base_y = -0.85f;    // pied, sous le plan de roulement (= la plateforme)
    float pole_top_y = 7.00f;      // sommet du mât
    float pole_flange_half = 0.15f;  // demi-largeur des semelles du H
    float pole_web_half = 0.16f;     // demi-écart entre semelles (l'âme les relie)
    float console_y = 6.70f;         // hauteur de la potence

    // --- Zone de GARE (M19) : sous la verrière, on ne plante PAS de poteaux (ils
    // percuteraient les quais et le toit). Le porteur y est suspendu à la structure par de
    // petites attaches, et abaissé à la hauteur de la verrière. Le fil de contact (et son
    // zigzag) ne change pas. `canopy_end <= canopy_start` => aucune gare (défaut). ---
    double canopy_start = 0.0;
    double canopy_end = 0.0;
    double canopy_attach_height = 5.35;  // hauteur d'attache du porteur sous la verrière
};

// Sortie : les fils d'une part (un maillage en RUBANS, pipeline câble), les poteaux
// d'autre part (des INSTANCES : ils sont tous identiques, seuls leur position et leur
// lacet changent).
struct CatenaryData {
    RailMeshData wires;                       // fils de contact + porteurs + pendules
    std::vector<render::InstanceData> poles;  // un jeu par poteau (hors gare)
    // Attaches de gare (M19) : dans l'emprise de la verrière, elles REMPLACENT les poteaux.
    std::vector<render::InstanceData> insulators;
};

// Engendre la caténaire sur la plage de chainage [x_start, x_end]. Sommets exprimés
// RELATIVEMENT à `origin` (origine flottante). Fonction PURE (aucun état, aucune API GPU)
// => appelable depuis un worker du JobSystem, comme generate_track_mesh.
//
// Les poteaux sont calés sur une grille ABSOLUE de chainage (multiples de `span`) et non
// sur x_start : deux fenêtres qui se recouvrent engendrent ainsi EXACTEMENT les mêmes
// poteaux, et le semis ne saute pas quand la fenêtre glisse.
[[nodiscard]] CatenaryData generate_catenary(const TrackSource& track, double x_start,
                                             double x_end, const WorldPosition& origin,
                                             const CatenaryProfile& profile = {});

// Le maillage d'UN poteau, en repère local : +X = le long de la voie, +Y = vers le haut,
// +Z = vers l'axe de la voie. Engendré une seule fois, puis instancié.
[[nodiscard]] RailMeshData generate_pole_mesh(const CatenaryProfile& profile = {});

// Le maillage d'UNE attache de gare (M19), en repère local, origine au point d'attache du
// porteur : une tige montant vers la verrière + une bague grippant le câble. Instancié.
[[nodiscard]] RailMeshData generate_insulator_mesh(const CatenaryProfile& profile = {});

}  // namespace noire::scene
