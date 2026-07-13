#include "noire/resource/asset_paths.hpp"

#include <filesystem>
#include <utility>

#include "noire/core/log.hpp"

namespace noire::resource {

namespace fs = std::filesystem;

AssetPaths::AssetPaths(std::string root) : root_(std::move(root)) {}

AssetPaths AssetPaths::discover(std::string_view folder_name, int max_levels) {
    std::error_code ec;
    fs::path dir = fs::current_path(ec);
    if (ec) {
        log::warn("AssetPaths : lecture du répertoire courant impossible — assets désactivés");
        return AssetPaths{};
    }

    for (int level = 0; level <= max_levels; ++level) {
        const fs::path candidate = dir / folder_name;
        if (fs::is_directory(candidate, ec)) {
            log::info("AssetPaths : racine des assets = {}", candidate.string());
            return AssetPaths{candidate.string()};
        }
        if (!dir.has_parent_path()) {
            break;
        }
        dir = dir.parent_path();
    }

    log::warn("AssetPaths : dossier '{}' introuvable — fallbacks systématiques",
              std::string(folder_name));
    return AssetPaths{};
}

std::string AssetPaths::resolve(std::string_view relative) const {
    if (root_.empty()) {
        return std::string(relative);
    }
    return (fs::path(root_) / relative).string();
}

bool AssetPaths::exists(std::string_view relative) const {
    if (root_.empty()) {
        return false;
    }
    std::error_code ec;
    return fs::is_regular_file(fs::path(root_) / relative, ec);
}

}  // namespace noire::resource
