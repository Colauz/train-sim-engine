#pragma once

// En-tête maths central du moteur.
//
// Les conventions Vulkan (profondeur 0..1) sont injectées globalement via une
// compile-definition sur la cible glm (voir cmake/Dependencies.cmake) :
//   GLM_FORCE_DEPTH_ZERO_TO_ONE
// Tout le moteur passe par cet en-tête pour garantir des matrices cohérentes.

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace noire {

// Positions monde : DOUBLE (64 bits) — indispensable à grande échelle (centaines de km).
using WorldPosition = glm::dvec3;

}  // namespace noire
