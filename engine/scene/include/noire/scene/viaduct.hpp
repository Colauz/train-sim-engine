#pragma once

#include <vector>

#include "noire/core/math.hpp"
#include "noire/core/track_source.hpp"
#include "noire/scene/track_mesh.hpp"

namespace noire {
class Terrain;
}

namespace noire::scene {

// Viaduc urbain (M31) : tablier de béton sous la voie + piles descendant au sol naturel.
//
// CONVENTION VERTICALE : comme track_mesh et catenary — la courbe rendue par
// TrackSource::sample EST le PLAN DE ROULEMENT. Le tablier se construit donc en dessous
// (son dessus affleure le pied du ballast, à -0,80 m), et les piles partent du dessous
// du tablier pour atteindre Terrain::height.
struct ViaductProfile {
    double step = 5.0;            // pas de tessellation du tablier le long de la voie
    float deck_half_width = 4.6f; // demi-largeur du tablier (le ballast fait 3,4 m)
    float deck_top_y = -0.80f;    // dessus du tablier = base du ballast
    float deck_thickness = 1.30f; // poutre-caisson béton
    double pillar_spacing = 35.0; // entraxe des piles, sur grille ABSOLUE de chainage
    float pillar_half_width = 1.6f;  // section du fût (pile-box)
    float pillar_embed = 4.0f;    // la pile s'enfonce sous le terrain (jamais de jour)
};

// Engendre le viaduc sur la plage de chainage [x_start, x_end]. Sommets exprimés
// RELATIVEMENT à `origin` (origine flottante). Fonction PURE (aucun état, aucune API
// GPU) => appelable depuis un worker, comme generate_track_mesh. Le terrain n'est lu
// que pour l'ancrage des piles (Terrain::height est pure et thread-safe).
[[nodiscard]] RailMeshData generate_viaduct(const TrackSource& track, const Terrain& terrain,
                                            double x_start, double x_end,
                                            const WorldPosition& origin,
                                            const ViaductProfile& profile = {});

// M47 — Gare aérienne (le reste de la ligne reste à ciel ouvert). Engendre, centré
// sur le chainage `s_center` : deux quais latéraux (plateformes à +1,10 m au-dessus
// du plan de roulement, soit ~10,5 m au-dessus du sol), une verrière de `length`
// mètres de long UNIQUEMENT au-dessus des quais, et ses porteurs. Mêmes conventions
// que generate_viaduct (plan de roulement = 0, origine flottante, fonction pure).
//
// SYMÉTRIE (M51) : tout ce qui est latéral — quais, façades de quai, colonnes,
// piliers, panneaux — est engendré par une boucle sur `sign ∈ {-1, +1}` appliquée
// aux MÊMES cotes. La gare est donc symétrique par construction, pas par
// coïncidence : il est impossible de décaler un quai sans décaler l'autre.
// AUTOPORTANCE (M51) : ses piliers descendent jusqu'au sol unifié (Y = 0 monde).
struct StationProfile {
    double length = 150.0;      // longueur des quais et de la verrière (m)
    double step = 10.0;         // pas de tessellation le long de la voie (m)
    // M52 — LE GAP. Le nez de quai était calé sur la rive du TABLIER (4,60 m) : il
    // restait 3,12 m entre le flanc de la rame et le quai. On le cale désormais sur
    // le GABARIT DE LA RAME : caisse large de 2,95 m => flanc à 1,475 m, nez de quai
    // à 1,55 m, soit 7,5 cm de jeu — la cote d'un quai réel.
    float platform_inner = 1.55f;
    float platform_outer = 8.0f;  // rive extérieure du quai (6,45 m de large)
    // Plancher de la rame : origine de caisse à +2,20 m (CarBodyConfig::body_height),
    // plancher intérieur à -1,00 m dans le gabarit (IN_FLOOR, tools/gen_metro.py)
    // => +1,20 m au-dessus du plan de roulement. Le quai est mis EXACTEMENT à cette
    // cote : embarquement de plain-pied, aucune marche. (La consigne citait 1,15 m
    // « par exemple » ; 1,20 m est la valeur qui annule réellement la marche.)
    float platform_top = 1.20f;
    // Nez de quai en encorbellement AU-DESSUS de l'épaulement du ballast. À 1,55 m de
    // l'axe on est à l'intérieur du plateau de ballast (RailProfile::ballast_crown_half
    // = 1,90 m) : un quai plein descendant jusqu'au tablier le traverserait. Le profil
    // est donc décroché — plein depuis le tablier au-delà du pied du talus, en dalle
    // au-dessus. Ces deux cotes sont vérifiées contre RailProfile (static_assert).
    float edge_bottom = -0.30f;  // = RailProfile::ballast_crown_y
    float edge_outer = 2.70f;    // > RailProfile::ballast_base_half (2,65 m)
    // M50 — VERRIÈRE BORNÉE. Le bug M49 : la dalle de toiture était extrudée sur
    // toute la fenêtre du viaduc (7 km), d'où un « plafond infini ». Elle est
    // désormais enfermée dans une boîte englobante EXPLICITE, vérifiable :
    //   Z (le long de la voie) : `length` = 150 m — les quais, rien de plus ;
    //   X (travers voie)       : ±`roof_half_width` = 20 m — les deux quais
    //                            (16 m de bâti) + 2 m de débord de rive par côté ;
    //   Y                      : intrados à `roof_y` AU-DESSUS DU PLAN DE ROULEMENT.
    //
    // Sur la hauteur : toutes les cotes du moteur se comptent depuis le plan de
    // roulement, qui ONDULE (base 10 m, ±6 m — ProceduralTrack). Un « Y = 14 m
    // absolu » figé ferait plonger le toit dans le tablier là où la voie descend.
    // La consigne « 14 m minimum pour laisser passer le fil » est donc appliquée
    // RELATIVEMENT, et largement tenue : intrados à +7,20 m => 17,20 m au chainage
    // nominal, soit bien plus que 14 m, et au-dessus du fil de contact (+5,20 m),
    // du porteur (+6,60 m) et du sommet des mâts (+7,00 m). Un toit à 14 m ABSOLU
    // (= +4,00 m) serait passé sous le fil ET dans la rame (toit de caisse à
    // +3,60 m, pantographe à +5,20 m) : c'est cette lecture-là qui est impossible.
    float roof_half_width = 10.0f;  // demi-largeur de la verrière (20 m au total)
    float roof_y = 7.2f;
    float roof_thickness = 0.35f;
    float column_half = 0.18f;  // section des colonnes de verrière (au-dessus du quai)
    // M51 : sous le quai, le même appui devient un PILIER qui descend jusqu'au sol
    // unifié (Y = 0). Plus trapu : il reprend une vingtaine de mètres de hauteur.
    float pillar_half = 0.35f;
    double column_spacing = 15.0;
    // --- M52 : FAÇADES DE QUAI CALQUÉES SUR LE PLAN DE PORTES DE LA RAME -------
    // La façade n'est plus une clôture générique sur une trame arbitraire de 2,5 m
    // (qui ne tombait en face d'une porte de rame que par accident). `door_centers`
    // donne les chainages des portes de la rame RELATIVEMENT au centre de la gare,
    // la rame étant à son point d'arrêt idéal. La façade se construit AUTOUR d'eux :
    // une baie ouvrante à chaque porte, du verre fixe partout ailleurs.
    //
    // C'est l'app qui remplit ce vecteur, en le dérivant de la géométrie de la rame
    // (ConsistConfig + implantation des portes du gabarit) : la gare ne devine rien,
    // elle reçoit le plan du train. Vide => façade entièrement fixe, sans baie.
    std::vector<double> door_centers;
    float door_half = 0.65f;      // = DOOR_HALF du gabarit => 1,30 m de passage
    float jamb_half = 0.08f;      // montant opaque, POSÉ EN DEHORS de la baie
    float psd_height = 1.8f;      // hauteur vitrée au-dessus du quai
    float psd_thickness = 0.03f;  // demi-épaisseur du vitrage
    float psd_offset = 0.0f;      // retrait de la FACE vitrée depuis le nez de quai
    double panel_max = 2.5f;      // longueur max d'un panneau fixe (tessellation)
    double sign_spacing = 25.0;   // entraxe des panneaux suspendus
    // M52 — Repère d'arrêt : chainage RELATIF au centre de la gare. C'est la position
    // de la CABINE quand le centre de la rame coïncide avec le centre de la gare ;
    // l'app la calcule depuis la même géométrie de rame que `door_centers`.
    double stop_marker_offset = 0.0;
};

// Six maillages : le béton (fusionné au viaduc), la verrière translucide (BLEND,
// bornée : 20 m x 150 m, intrados à +7,20 m), le vitrage FIXE des façades de quai
// (BLEND), les DEUX demi-vantaux mobiles (BLEND — les baies sont bi-parting : `a`
// coulisse vers l'arrière, `b` vers l'avant) et la signalétique (émissif).
struct StationMeshes {
    RailMeshData concrete;
    RailMeshData canopy;
    RailMeshData glass;
    RailMeshData psd_doors;    // vantaux qui coulissent vers les chainages DÉCROISSANTS
    RailMeshData psd_doors_b;  // vantaux qui coulissent vers les chainages CROISSANTS
    RailMeshData signs;
};

[[nodiscard]] StationMeshes generate_station(const TrackSource& track, double s_center,
                                             const WorldPosition& origin,
                                             const StationProfile& profile = {});

}  // namespace noire::scene
