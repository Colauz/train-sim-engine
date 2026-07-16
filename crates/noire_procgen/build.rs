// build.rs — régénère l'en-tête C de la frontière FFI à chaque build du crate.
//
// L'en-tête atterrit dans generated/ (versionné-ignoré) à un chemin STABLE que le CMake
// connaît. Comme le C++ qui l'inclut dépend de la cible Cargo (add_dependencies côté
// CMake), ce build.rs tourne toujours AVANT que track_mesh_rust.cpp ne soit compilé.

fn main() {
    let crate_dir = std::env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR");
    let out_dir = std::path::Path::new(&crate_dir).join("generated");
    std::fs::create_dir_all(&out_dir).expect("création de generated/");
    let out_header = out_dir.join("noire_procgen.h");

    // cbindgen lit cbindgen.toml dans le dossier du crate.
    match cbindgen::generate(&crate_dir) {
        Ok(bindings) => {
            bindings.write_to_file(&out_header);
        }
        Err(e) => {
            // On n'échoue PAS le build sur une erreur cbindgen si un en-tête existe déjà
            // (ex. régénération partielle) : on prévient, c'est tout.
            println!("cargo:warning=cbindgen a échoué : {e}");
        }
    }

    println!("cargo:rerun-if-changed=src/lib.rs");
    println!("cargo:rerun-if-changed=cbindgen.toml");
    println!("cargo:rerun-if-changed=build.rs");
}
