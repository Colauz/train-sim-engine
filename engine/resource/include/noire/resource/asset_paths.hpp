#pragma once

#include <string>
#include <string_view>

namespace noire::resource {

// Résout les chemins d'assets relatifs à une racine « assets/ ».
//
// L'exécutable tourne typiquement depuis build/<preset>/ : la racine du dépôt (et
// donc le dossier assets/) se trouve plus haut. `discover()` remonte l'arborescence
// depuis le répertoire courant jusqu'à trouver le dossier voulu. Si rien n'est
// trouvé, la racine reste vide : `resolve()` renvoie le chemin tel quel et
// `exists()` renvoie toujours false — ce qui déclenchera les fallbacks du M7.
class AssetPaths {
public:
    AssetPaths() = default;
    explicit AssetPaths(std::string root);

    // Cherche `folder_name` en remontant depuis le CWD (jusqu'à `max_levels` niveaux).
    static AssetPaths discover(std::string_view folder_name = "assets", int max_levels = 6);

    // Chemin de `relative` sous la racine. Racine vide => `relative` inchangé.
    [[nodiscard]] std::string resolve(std::string_view relative) const;
    // true si le fichier résolu existe réellement sur le disque.
    [[nodiscard]] bool exists(std::string_view relative) const;

    [[nodiscard]] const std::string& root() const { return root_; }
    [[nodiscard]] bool valid() const { return !root_.empty(); }

private:
    std::string root_;
};

}  // namespace noire::resource
