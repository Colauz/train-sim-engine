//! Génération procédurale de la géométrie de voie — PoC Rust du moteur Noire (M13.5).
//!
//! Port de `engine/scene/src/track_mesh.cpp`. La RÉPARTITION est le cœur du PoC : le C++
//! garde tout ce qui touche au monde en double précision (échantillonnage de la voie,
//! origine flottante) ; Rust reçoit des échantillons déjà projetés en `f32` et fait tout
//! le travail de maillage (extrusion des profils, normales, tangentes, traverses).
//!
//! La frontière est POD et BATCHÉE : un seul appel par tuile produit les trois
//! sous-maillages. Rust ALLOUE la mémoire de sortie et la LIBÈRE lui-même
//! (`noire_procgen_free`) — le C++ ne fait que copier des vues `const`.
//!
//! SÛRETÉ : tout le maillage est du Rust 100 % sûr (pas un seul `unsafe` dans la logique
//! géométrique — c'est justement là que l'arithmétique d'indices de buffer est risquée en
//! C++). Le `unsafe` est confiné aux deux fonctions FFI, qui ne font que (dé)référencer
//! les pointeurs du contrat C.

use core::ffi::c_void;

// ============================================================================
//  Petite algèbre vectorielle f32 — reproduit les formules de glm à l'identique
//  (v / sqrt(dot) pour normalize, même ordre d'opérations pour le produit croisé).
// ============================================================================
#[derive(Clone, Copy)]
struct Vec3 {
    x: f32,
    y: f32,
    z: f32,
}

impl Vec3 {
    #[inline]
    fn new(x: f32, y: f32, z: f32) -> Self {
        Vec3 { x, y, z }
    }
    #[inline]
    fn add(self, o: Vec3) -> Vec3 {
        Vec3::new(self.x + o.x, self.y + o.y, self.z + o.z)
    }
    #[inline]
    fn sub(self, o: Vec3) -> Vec3 {
        Vec3::new(self.x - o.x, self.y - o.y, self.z - o.z)
    }
    #[inline]
    fn scale(self, s: f32) -> Vec3 {
        Vec3::new(self.x * s, self.y * s, self.z * s)
    }
    #[inline]
    fn dot(self, o: Vec3) -> f32 {
        self.x * o.x + self.y * o.y + self.z * o.z
    }
    #[inline]
    fn cross(self, o: Vec3) -> Vec3 {
        Vec3::new(
            self.y * o.z - self.z * o.y,
            self.z * o.x - self.x * o.z,
            self.x * o.y - self.y * o.x,
        )
    }
    #[inline]
    fn normalize(self) -> Vec3 {
        let len = self.dot(self).sqrt();
        if len > 0.0 {
            self.scale(1.0 / len)
        } else {
            self
        }
    }
}

// ============================================================================
//  Types de la frontière FFI (repr(C)) — cbindgen en dérive noire_procgen.h.
// ============================================================================

/// Sommet PBR — miroir EXACT de `render::MeshVertex` (48 octets, mêmes offsets). Le C++
/// s'en assure par static_assert avant de memcpy le bloc tel quel.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ProcgenVertex {
    pub position: [f32; 3],
    pub normal: [f32; 3],
    pub uv: [f32; 2],
    pub tangent: [f32; 4],
}

/// Un échantillon de voie, déjà projeté en repère local `f32` par le C++. `center` a subi
/// la soustraction d'origine flottante en double ; `forward` est la tangente normalisée
/// en double puis convertie — Rust n'a donc AUCUN calcul en double à refaire.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ProcgenSample {
    pub center: [f32; 3],
    pub forward: [f32; 3],
    /// Coordonnée de texture le long de la voie (sections uniquement ; 0 pour les traverses).
    pub u: f32,
}

/// Cotes du profil de voie, transmises telles quelles par le C++ (miroir de `RailProfile`,
/// champs utiles au maillage seulement).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ProcgenParams {
    pub uv_period: f32,
    pub gauge: f32,
    pub rail_height: f32,
    pub rail_head_half: f32,
    pub rail_web_half: f32,
    pub rail_foot_half: f32,
    pub sleeper_half_length: f32,
    pub sleeper_half_width: f32,
    pub sleeper_thickness: f32,
    pub ballast_crown_y: f32,
    pub ballast_crown_half: f32,
    pub ballast_base_y: f32,
    pub ballast_base_half: f32,
    /// 1 = LOD Full (rail en I + traverses), 0 = Distant (rail en bloc, pas de traverse).
    pub lod_full: u32,
}

/// Vue `const` d'un sous-maillage. Les pointeurs visent la mémoire possédée par le
/// `ProcgenResult` opaque : valides jusqu'à `noire_procgen_free(handle)`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ProcgenSubMesh {
    pub vertices: *const ProcgenVertex,
    pub vertex_count: usize,
    pub indices: *const u32,
    pub index_count: usize,
}

/// Résultat d'un appel : trois sous-maillages + le handle opaque à rendre à `..._free`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ProcgenMesh {
    pub rails: ProcgenSubMesh,
    pub sleepers: ProcgenSubMesh,
    pub ballast: ProcgenSubMesh,
    /// Handle opaque (Box<ProcgenResult>). À libérer par noire_procgen_free, jamais côté C++.
    pub handle: *mut c_void,
}

// ============================================================================
//  Buffers possédés côté Rust (jamais exposés directement — seules des vues sortent).
// ============================================================================
#[derive(Default)]
struct SubMeshBuf {
    vertices: Vec<ProcgenVertex>,
    indices: Vec<u32>,
}

impl SubMeshBuf {
    fn view(&self) -> ProcgenSubMesh {
        ProcgenSubMesh {
            vertices: self.vertices.as_ptr(),
            vertex_count: self.vertices.len(),
            indices: self.indices.as_ptr(),
            index_count: self.indices.len(),
        }
    }
}

struct ProcgenResult {
    rails: SubMeshBuf,
    sleepers: SubMeshBuf,
    ballast: SubMeshBuf,
}

// ============================================================================
//  Géométrie interne — 100 % sûr.
// ============================================================================

/// Point du profil transversal, plan (latéral s, vertical t). t = 0 au plan de roulement.
#[derive(Clone, Copy)]
struct P2 {
    s: f32,
    t: f32,
}

/// Repère local d'une section (identique à `Frame` du C++).
#[derive(Clone, Copy)]
struct Frame {
    center: Vec3,
    right: Vec3,
    up: Vec3,
    u: f32,
}

const WORLD_UP: Vec3 = Vec3 {
    x: 0.0,
    y: 1.0,
    z: 0.0,
};

fn frame_from_sample(s: &ProcgenSample) -> Frame {
    let center = Vec3::new(s.center[0], s.center[1], s.center[2]);
    let forward = Vec3::new(s.forward[0], s.forward[1], s.forward[2]);
    let right = forward.cross(WORLD_UP).normalize();
    let up = right.cross(forward).normalize();
    Frame {
        center,
        right,
        up,
        u: s.u,
    }
}

#[inline]
fn world_of(f: &Frame, p: P2, lateral_offset: f32) -> Vec3 {
    f.center
        .add(f.right.scale(p.s + lateral_offset))
        .add(f.up.scale(p.t))
}

/// Extrude un profil le long des frames. Port fidèle de `extrude()` (track_mesh.cpp) :
/// normales issues du WINDING, sommets jamais partagés (arêtes franches).
fn extrude(
    out: &mut SubMeshBuf,
    frames: &[Frame],
    profile: &[P2],
    closed: bool,
    lateral_offset: f32,
    uv_period: f32,
) {
    if frames.len() < 2 || profile.len() < 2 {
        return;
    }
    let edges = if closed { profile.len() } else { profile.len() - 1 };

    // v cumulé le long du profil / période : même échelle physique en travers qu'en long.
    let mut v_coord = vec![0.0f32; profile.len() + 1];
    for j in 0..edges {
        let p0 = profile[j];
        let p1 = profile[(j + 1) % profile.len()];
        let len = ((p1.s - p0.s) * (p1.s - p0.s) + (p1.t - p0.t) * (p1.t - p0.t)).sqrt();
        v_coord[j + 1] = v_coord[j] + len / uv_period;
    }

    for i in 0..frames.len() - 1 {
        let f0 = &frames[i];
        let f1 = &frames[i + 1];
        for j in 0..edges {
            let p0 = profile[j];
            let p1 = profile[(j + 1) % profile.len()];

            let ds = p1.s - p0.s;
            let dt = p1.t - p0.t;
            let len = (ds * ds + dt * dt).sqrt();
            if len < 1e-6 {
                continue; // arête dégénérée
            }
            let normal = f0
                .right
                .scale(dt / len)
                .add(f0.up.scale(-ds / len))
                .normalize();

            let a = world_of(f0, p0, lateral_offset);
            let b = world_of(f1, p0, lateral_offset);
            let c = world_of(f1, p1, lateral_offset);
            let d = world_of(f0, p1, lateral_offset);

            let tangent = b.sub(a).normalize();
            // Handedness glTF : cross(N,T)·(d-a) < 0 => w = -1.
            let w = if normal.cross(tangent).dot(d.sub(a)) < 0.0 {
                -1.0
            } else {
                1.0
            };

            push_quad(
                out,
                [a, b, c, d],
                normal,
                [tangent.x, tangent.y, tangent.z, w],
                [
                    [f0.u, v_coord[j]],
                    [f1.u, v_coord[j]],
                    [f1.u, v_coord[j + 1]],
                    [f0.u, v_coord[j + 1]],
                ],
            );
        }
    }
}

/// Boîte orientée (traverse) — port de `add_box()`. 6 faces, normales explicites.
#[allow(clippy::too_many_arguments)]
fn add_box(
    out: &mut SubMeshBuf,
    center: Vec3,
    right: Vec3,
    forward: Vec3,
    up: Vec3,
    half: Vec3,
    uv_period: f32,
) {
    let x = right.scale(half.x);
    let y = up.scale(half.y);
    let z = forward.scale(half.z);

    // (normale, axe u, axe v, offset du centre de face, demi-longueur u, demi-longueur v)
    struct Face {
        n: Vec3,
        uu: Vec3,
        vv: Vec3,
        offset: Vec3,
        ul: f32,
        vl: f32,
    }
    let neg = |v: Vec3| v.scale(-1.0);
    let faces = [
        Face { n: up, uu: right, vv: forward, offset: y, ul: half.x, vl: half.z }, // dessus
        Face { n: neg(up), uu: right, vv: neg(forward), offset: neg(y), ul: half.x, vl: half.z }, // dessous
        Face { n: right, uu: forward, vv: up, offset: x, ul: half.z, vl: half.y }, // flanc droit
        Face { n: neg(right), uu: neg(forward), vv: up, offset: neg(x), ul: half.z, vl: half.y }, // flanc gauche
        Face { n: forward, uu: right, vv: up, offset: z, ul: half.x, vl: half.y }, // bout avant
        Face { n: neg(forward), uu: neg(right), vv: up, offset: neg(z), ul: half.x, vl: half.y }, // bout arrière
    ];

    for f in &faces {
        let c = center.add(f.offset);
        let du = f.uu.scale(f.ul);
        let dv = f.vv.scale(f.vl);
        let a = c.sub(du).sub(dv);
        let b = c.add(du).sub(dv);
        let cc = c.add(du).add(dv);
        let d = c.sub(du).add(dv);

        let tangent = f.uu.normalize();
        let w = if f.n.cross(tangent).dot(f.vv) < 0.0 {
            -1.0
        } else {
            1.0
        };
        let us = f.ul * 2.0 / uv_period;
        let vs = f.vl * 2.0 / uv_period;

        push_quad(
            out,
            [a, b, cc, d],
            f.n,
            [tangent.x, tangent.y, tangent.z, w],
            [[0.0, 0.0], [us, 0.0], [us, vs], [0.0, vs]],
        );
    }
}

/// Émet un quad (4 sommets non partagés + 6 indices). Facteur commun d'extrude et add_box.
#[inline]
fn push_quad(
    out: &mut SubMeshBuf,
    pos: [Vec3; 4],
    normal: Vec3,
    tangent: [f32; 4],
    uv: [[f32; 2]; 4],
) {
    let base = out.vertices.len() as u32;
    let n = [normal.x, normal.y, normal.z];
    for k in 0..4 {
        out.vertices.push(ProcgenVertex {
            position: [pos[k].x, pos[k].y, pos[k].z],
            normal: n,
            uv: uv[k],
            tangent,
        });
    }
    out.indices
        .extend_from_slice(&[base, base + 1, base + 2, base, base + 2, base + 3]);
}

// --- Profils transversaux (port des make_*_profile) -------------------------

fn make_rail_profile(p: &ProcgenParams) -> Vec<P2> {
    let top = 0.0;
    let bottom = -p.rail_height;
    let foot_top = bottom + 0.030;
    let head_bottom = top - 0.040;
    let fillet = 0.012;
    vec![
        P2 { s: -p.rail_foot_half, t: bottom },
        P2 { s: p.rail_foot_half, t: bottom },
        P2 { s: p.rail_foot_half, t: foot_top - fillet },
        P2 { s: p.rail_web_half, t: foot_top + fillet },
        P2 { s: p.rail_web_half, t: head_bottom - fillet },
        P2 { s: p.rail_head_half, t: head_bottom + fillet },
        P2 { s: p.rail_head_half, t: top },
        P2 { s: -p.rail_head_half, t: top },
        P2 { s: -p.rail_head_half, t: head_bottom + fillet },
        P2 { s: -p.rail_web_half, t: head_bottom - fillet },
        P2 { s: -p.rail_web_half, t: foot_top + fillet },
        P2 { s: -p.rail_foot_half, t: foot_top - fillet },
    ]
}

fn make_rail_profile_distant(p: &ProcgenParams) -> Vec<P2> {
    vec![
        P2 { s: -p.rail_head_half, t: -p.rail_height },
        P2 { s: p.rail_head_half, t: -p.rail_height },
        P2 { s: p.rail_head_half, t: 0.0 },
        P2 { s: -p.rail_head_half, t: 0.0 },
    ]
}

fn make_ballast_profile(p: &ProcgenParams) -> Vec<P2> {
    vec![
        P2 { s: p.ballast_base_half, t: p.ballast_base_y },
        P2 { s: p.ballast_crown_half, t: p.ballast_crown_y },
        P2 { s: -p.ballast_crown_half, t: p.ballast_crown_y },
        P2 { s: -p.ballast_base_half, t: p.ballast_base_y },
    ]
}

/// Cœur : à partir des échantillons pré-calculés et des cotes, produit les 3 sous-maillages.
fn generate(sections: &[ProcgenSample], sleepers: &[ProcgenSample], p: &ProcgenParams) -> ProcgenResult {
    let mut result = ProcgenResult {
        rails: SubMeshBuf::default(),
        sleepers: SubMeshBuf::default(),
        ballast: SubMeshBuf::default(),
    };

    let uv_period = if p.uv_period > 0.01 { p.uv_period } else { 1.0 };
    let full = p.lod_full != 0;

    let frames: Vec<Frame> = sections.iter().map(frame_from_sample).collect();

    // Rails : profil en I extrudé deux fois (± axe de rail).
    let rail_profile = if full {
        make_rail_profile(p)
    } else {
        make_rail_profile_distant(p)
    };
    let rail_axis = p.gauge * 0.5 + p.rail_head_half;
    extrude(&mut result.rails, &frames, &rail_profile, true, rail_axis, uv_period);
    extrude(&mut result.rails, &frames, &rail_profile, true, -rail_axis, uv_period);

    // Ballast : aux deux LOD.
    extrude(
        &mut result.ballast,
        &frames,
        &make_ballast_profile(p),
        false,
        0.0,
        uv_period,
    );

    // Traverses : uniquement en Full, une boîte par échantillon pré-fourni par le C++.
    if full {
        let half = Vec3::new(
            p.sleeper_half_length,
            p.sleeper_thickness * 0.5,
            p.sleeper_half_width,
        );
        for s in sleepers {
            let center = Vec3::new(s.center[0], s.center[1], s.center[2]);
            let forward = Vec3::new(s.forward[0], s.forward[1], s.forward[2]);
            let right = forward.cross(WORLD_UP).normalize();
            let up = right.cross(forward).normalize();
            // Dessus de la traverse au niveau du dessous du rail => centre une demi-épaisseur plus bas.
            let box_center = center.sub(up.scale(p.rail_height + p.sleeper_thickness * 0.5));
            add_box(&mut result.sleepers, box_center, right, forward, up, half, uv_period);
        }
    }

    result
}

// ============================================================================
//  Frontière FFI — les DEUX seules fonctions unsafe, réduites au (dé)référencement.
// ============================================================================

/// Génère les trois sous-maillages d'une tuile de voie.
///
/// `sections`/`sleepers` : tableaux d'échantillons pré-projetés (peuvent être NULL si le
/// compte est 0). `params` doit être non-NULL. Le résultat contient des vues `const` vers
/// la mémoire possédée par Rust ; l'appelant DOIT rendre `handle` à `noire_procgen_free`.
///
/// # Safety
/// Les pointeurs doivent référencer `section_count` / `sleeper_count` éléments valides et
/// un `ProcgenParams` valide.
#[no_mangle]
pub unsafe extern "C" fn noire_procgen_generate_track(
    sections: *const ProcgenSample,
    section_count: usize,
    sleepers: *const ProcgenSample,
    sleeper_count: usize,
    params: *const ProcgenParams,
) -> ProcgenMesh {
    let sections: &[ProcgenSample] = if sections.is_null() || section_count == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(sections, section_count)
    };
    let sleepers: &[ProcgenSample] = if sleepers.is_null() || sleeper_count == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(sleepers, sleeper_count)
    };
    let params: &ProcgenParams = &*params;

    let result = Box::new(generate(sections, sleepers, params));
    // Les Vec vivent sur le tas : déplacer le Box (into_raw) ne bouge pas leurs buffers,
    // donc les vues calculées ensuite restent valides jusqu'au free.
    let handle = Box::into_raw(result);
    let r = &*handle;
    ProcgenMesh {
        rails: r.rails.view(),
        sleepers: r.sleepers.view(),
        ballast: r.ballast.view(),
        handle: handle as *mut c_void,
    }
}

/// Libère le résultat d'un `noire_procgen_generate_track`. NULL est ignoré. Appeler deux
/// fois sur le même handle est une faute (double-free), comme pour `free`.
///
/// # Safety
/// `handle` doit provenir d'un `noire_procgen_generate_track` et n'être libéré qu'une fois.
#[no_mangle]
pub unsafe extern "C" fn noire_procgen_free(handle: *mut c_void) {
    if !handle.is_null() {
        drop(Box::from_raw(handle as *mut ProcgenResult));
    }
}
