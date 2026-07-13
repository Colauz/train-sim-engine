#pragma once

#include <string>

#include "noire/resource/asset_types.hpp"

namespace noire::resource {

// Charge un modèle glTF/GLB en CPU : extrait, par primitive triangulée, les
// positions/normales/UV et les indices, ainsi que la texture de base (embarquée
// via bufferView, ou externe via URI). AUCUN appel Vulkan. false si échec.
//
// Limitation M7 : les transformations de nœuds ne sont pas encore appliquées
// (les sommets restent en espace mesh-local) — suffisant pour un modèle simple.
bool load_gltf(const std::string& path, ModelData& out);

}  // namespace noire::resource
