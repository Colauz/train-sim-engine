#pragma once

#include <cstdint>
#include <vector>

#include "noire/core/math.hpp"
#include "noire/core/terrain.hpp"
#include "noire/core/track_source.hpp"
#include "noire/render/vertex.hpp"

namespace noire::scene {

// CONVENTION VERTICALE (M9) : la courbe rendue par TrackSource::sample EST le PLAN DE
// ROULEMENT — le dessus du champignon du rail, là où porte la roue. Tout le reste
// (âme, patin, traverses, ballast) est construit VERS LE BAS depuis ce plan.
//
// C'est la convention ferroviaire naturelle, et elle donne un sens physique à
// `body_height` (hauteur de caisse au-dessus du rail). Avant le M9 la courbe était le
// PIED du rail, ce qui enfonçait les roues de 7 cm dans le champignon.
// Niveau de détail d'une tuile de voie (M9 optimisation). C'est le SEUL levier qui rende
// tenable une ligne de 150 km : une tuile Full coûte ~40x une tuile Distant.
enum class TrackLod {
    Full,     // sous et autour du train : rails en I, traverses, ballast, pas fin
    Distant,  // au loin : rails réduits à un bloc, AUCUNE traverse, pas grossier
};

struct RailProfile {
    double gauge = 1.435;      // écartement standard, mesuré entre faces internes
    double sample_step = 1.0;  // pas d'échantillonnage le long de la voie (m), LOD Full
    // Pas du LOD Distant. À 2 km, 10 m de corde représentent un sous-pixel de flèche :
    // la facettisation est invisible, et on divise le nombre de sections par 10.
    double distant_sample_step = 10.0;
    // Période de répétition des UV (m) : 1 unité UV = uv_period mètres, dans les deux
    // sens. Les textures gardent donc une taille PHYSIQUE constante d'un élément à
    // l'autre — un ballast et une traverse ne peuvent pas avoir deux échelles de grain.
    float uv_period = 2.0f;

    // --- Rail (profil en I, inspiré d'un UIC 60 simplifié) ---
    float rail_height = 0.172f;      // du dessous du patin au plan de roulement
    float rail_head_half = 0.036f;   // demi-largeur du champignon
    float rail_web_half = 0.010f;    // demi-largeur de l'âme
    float rail_foot_half = 0.075f;   // demi-largeur du patin

    // --- Traverses ---
    float sleeper_spacing = 0.60f;   // entraxe (norme française courante)
    float sleeper_half_length = 1.30f;  // demi-longueur, en travers de la voie
    float sleeper_half_width = 0.15f;   // demi-largeur, le long de la voie
    float sleeper_thickness = 0.22f;

    // --- Ballast ---
    // Trapèze : plateau puis talus latéraux. La base tombe à -0.80, ce qui correspond
    // EXACTEMENT au plan de sol de l'app (body_position.y - 3.0, soit rail - 0.8) :
    // le ballast s'y raccorde au lieu de flotter.
    float ballast_crown_y = -0.30f;      // hauteur du plateau, sous le plan de roulement
    float ballast_crown_half = 1.90f;    // demi-largeur du plateau
    float ballast_base_y = -0.80f;       // pied du talus
    float ballast_base_half = 2.65f;     // demi-largeur au pied (pente ~1:1.5)

    // --- Accotement / remblai (M9 correction) ---------------------------------
    // La voie suit une spline qui ondule (±6 m), le sol est un plan PLAT : sans raccord,
    // le ballast flotte ou s'enterre au gré des collines. L'accotement est le remblai qui
    // descend du pied du ballast jusqu'au sol — exactement ce que fait une vraie ligne.
    //
    // Depuis le M11, le point extérieur de l'accotement se cale sur le TERRAIN réel
    // (Terrain::height), plus sur une altitude figée. Le terrain étant lui-même aplani
    // dans le corridor ferroviaire, les deux se raccordent PAR CONSTRUCTION : c'est la
    // même fonction des deux côtés, il n'y a rien à synchroniser. Ça libère aussi la voie
    // de la contrainte M9 « toujours en remblai » — elle peut enfin entrer en tranchée,
    // puisque c'est le terrain qui se creuse.
    float shoulder_half = 22.0f;   // largeur de l'accotement, de l'axe vers l'extérieur
};

// Un sous-maillage : sommets PBR + indices, prêts pour create_mesh_indexed.
struct RailMeshData {
    std::vector<render::MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    [[nodiscard]] bool empty() const { return vertices.empty() || indices.empty(); }
};

// La voie complète d'une tuile, séparée PAR MATÉRIAU : un maillage ne peut porter qu'un
// seul descriptor set, et acier / béton / gravier n'ont rien en commun.
struct TrackMeshData {
    RailMeshData rails;     // acier : metallic 1, poli par les roues
    RailMeshData sleepers;  // béton : diélectrique mat
    RailMeshData ballast;   // gravier : diélectrique très rugueux
    RailMeshData shoulder;  // remblai : même matériau que le sol (herbe/terre)
    [[nodiscard]] bool empty() const { return rails.empty(); }
};

// Génère la voie 3D complète sur la plage de chainage [x_start, x_end] : rails en I,
// traverses et lit de ballast. Sommets exprimés RELATIVEMENT à `origin` (float) =>
// compatible origine flottante, chaque tuile ayant la sienne. Fonction PURE (aucun état,
// aucune API GPU) => appelable depuis un worker du JobSystem.
[[nodiscard]] TrackMeshData generate_track_mesh(const TrackSource& track, const Terrain& terrain,
                                                double x_start, double x_end,
                                                const WorldPosition& origin,
                                                const RailProfile& profile = {},
                                                TrackLod lod = TrackLod::Full);

}  // namespace noire::scene
