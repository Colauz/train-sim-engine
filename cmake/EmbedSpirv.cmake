# Script -P : transforme un binaire SPIR-V en en-tête C++ (tableau d'octets aligné 4).
# Variables attendues : SPV_FILE, HEADER_FILE, VAR_NAME
#
# On lit le .spv en hexa puis on émet un initialiseur d'octets. L'alignement 4
# garantit un reinterpret_cast<const uint32_t*> valide (exigence de Vulkan).

file(READ "${SPV_FILE}" _hex HEX)
string(LENGTH "${_hex}" _hexlen)
math(EXPR _bytes "${_hexlen} / 2")

# "0a1b..." -> "0x0a,0x1b,..."
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _body "${_hex}")

set(_out "// Généré automatiquement depuis ${SPV_FILE} — NE PAS ÉDITER.\n")
string(APPEND _out "#pragma once\n#include <cstddef>\n\n")
string(APPEND _out "alignas(4) inline constexpr unsigned char ${VAR_NAME}[] = {\n    ${_body}\n};\n")
string(APPEND _out "inline constexpr std::size_t ${VAR_NAME}_size = ${_bytes};\n")

file(WRITE "${HEADER_FILE}" "${_out}")
