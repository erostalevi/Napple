//! A loaded mod: manifest, file listing and parsed components.
use crate::cheats::{self, CheatFile};
use crate::config::{self, ModConfig};
use crate::ips::{self, IpsPatch};
use crate::manifest::{self, Manifest, SHA_MANIFEST_PATH};
use crate::pchtxt::{self, Pchtxt};
use crate::source::{self, DirFiles, Listing, ModFiles, ZipFiles};
use crate::util::{ModError, Result, err, norm_build_id, sha256_hex};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

pub const MAX_TEXT: u64 = 16 << 20;
pub const MAX_FILE: u64 = 1 << 30;

pub struct LoadedMod {
    pub manifest: Manifest,
    pub manifest_warnings: Vec<String>,
    pub listing: Listing,
    pub files: Box<dyn ModFiles>,
    pub source: PathBuf,
    pub is_archive: bool,
    /// SHA-256 of the `.napmod` file (None for a directory).
    pub package_sha256: Option<String>,
}

/// What the files in the mod actually are (the trust summary the UI must show).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, serde::Serialize)]
pub struct Capabilities {
    /// Patches (IPS/pchtxt): they change game code, so they need an explicit enable.
    pub code_patches: bool,
    pub cheats: bool,
    pub romfs: bool,
    pub config: bool,
    pub shaders: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PatchKind {
    Pchtxt(Pchtxt),
    Ips { build_id: [u8; 32], patch: IpsPatch },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PatchFile {
    /// Archive path, e.g. `patches/widescreen.pchtxt`.
    pub path: String,
    pub kind: PatchKind,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheatSource {
    pub path: String,
    /// Build id prefix from the file name (Atmosphere uses the first 8 bytes = 16 hex digits).
    pub build_id: [u8; 32],
    pub prefix_len: usize,
    pub file: CheatFile,
}

impl LoadedMod {
    /// Opens a `.napmod` (zip) or a directory. Unsafe entries are recorded in `listing.bad`, not hidden:
    /// use [`LoadedMod::load`] when they must be refused.
    pub fn open(path: &Path) -> Result<Self> {
        let meta = std::fs::metadata(path)?;
        let (files, listing, is_archive, package_sha256): (
            Box<dyn ModFiles>,
            Listing,
            bool,
            Option<String>,
        ) = if meta.is_dir() {
            (
                Box::new(DirFiles(path.to_path_buf())),
                source::list_dir(path)?,
                false,
                None,
            )
        } else {
            let h = source::hash_file(path)?;
            let (z, l) = ZipFiles::open(std::fs::File::open(path)?)?;
            (Box::new(z), l, true, Some(h))
        };
        if !listing.entries.contains_key(manifest::MANIFEST_PATH) {
            return err("no mod.json at the top of the mod");
        }
        let (manifest, manifest_warnings) =
            manifest::parse(&files.read(manifest::MANIFEST_PATH, 1 << 20)?)?;
        Ok(Self {
            manifest,
            manifest_warnings,
            listing,
            files,
            source: path.to_path_buf(),
            is_archive,
            package_sha256,
        })
    }

    /// Like [`open`](Self::open) but refuses unsafe entries and an invalid manifest.
    pub fn load(path: &Path) -> Result<Self> {
        let m = Self::open(path)?;
        if !m.listing.bad.is_empty() {
            return err(format!(
                "unsafe archive entries: {}",
                m.listing.bad.join("; ")
            ));
        }
        manifest::require_valid(&m.manifest)?;
        Ok(m)
    }

    pub fn paths<'a>(&'a self, prefix: &'a str) -> impl Iterator<Item = &'a String> {
        self.listing
            .entries
            .keys()
            .filter(move |p| p.starts_with(prefix))
    }

    pub fn capabilities(&self) -> Capabilities {
        let has = |pre: &str| self.paths(pre).next().is_some();
        Capabilities {
            code_patches: self.paths("patches/").any(|p| is_patch_path(p)),
            cheats: self.paths("cheats/").any(|p| p.ends_with(".txt")),
            romfs: has("files/"),
            config: self.listing.entries.contains_key("config.json"),
            shaders: self.paths("shaders/").any(|p| p.ends_with(".msl")),
        }
    }

    pub fn patch_paths(&self) -> Vec<String> {
        self.paths("patches/")
            .filter(|p| is_patch_path(p))
            .cloned()
            .collect()
    }

    pub fn cheat_paths(&self) -> Vec<String> {
        self.paths("cheats/")
            .filter(|p| p.ends_with(".txt"))
            .cloned()
            .collect()
    }

    pub fn load_patches(&self) -> Result<Vec<PatchFile>> {
        let mut out = Vec::new();
        for p in self.patch_paths() {
            let bytes = self.files.read(&p, MAX_TEXT)?;
            let ctx = |e: ModError| ModError(format!("{p}: {e}"));
            let kind = if p.ends_with(".pchtxt") {
                let t =
                    std::str::from_utf8(&bytes).map_err(|_| ModError(format!("{p}: not UTF-8")))?;
                PatchKind::Pchtxt(pchtxt::parse(t).map_err(ctx)?)
            } else {
                let stem = p.rsplit('/').next().unwrap_or("").trim_end_matches(".ips");
                let build_id = norm_build_id(stem).map_err(|_| {
                    ModError(format!(
                        "{p}: IPS file names are the module build id (<BUILDID>.ips)"
                    ))
                })?;
                PatchKind::Ips {
                    build_id,
                    patch: ips::parse(&bytes).map_err(ctx)?,
                }
            };
            out.push(PatchFile { path: p, kind });
        }
        Ok(out)
    }

    pub fn load_cheats(&self) -> Result<Vec<CheatSource>> {
        let mut out = Vec::new();
        for p in self.cheat_paths() {
            let stem = p
                .rsplit('/')
                .next()
                .unwrap_or("")
                .trim_end_matches(".txt")
                .to_string();
            let build_id = norm_build_id(&stem).map_err(|_| {
                ModError(format!(
                    "{p}: cheat file names are the build id (<BUILDID>.txt)"
                ))
            })?;
            if stem.len() < 16 {
                return err(format!("{p}: cheat file names need at least 16 hex digits"));
            }
            let t = String::from_utf8(self.files.read(&p, MAX_TEXT)?)
                .map_err(|_| ModError(format!("{p}: not UTF-8")))?;
            let file = cheats::parse(&t).map_err(|e| ModError(format!("{p}: {e}")))?;
            out.push(CheatSource {
                path: p,
                build_id,
                prefix_len: stem.len().min(64) / 2,
                file,
            });
        }
        Ok(out)
    }

    pub fn load_config(&self) -> Result<Option<(ModConfig, Vec<String>)>> {
        if !self.listing.entries.contains_key("config.json") {
            return Ok(None);
        }
        let t = String::from_utf8(self.files.read("config.json", 1 << 20)?)
            .map_err(|_| ModError("config.json: not UTF-8".into()))?;
        config::parse(&t)
            .map(Some)
            .map_err(|e| ModError(format!("config.json: {e}")))
    }

    /// `<sha256>  <path>` for every file except the manifest itself, sorted by path.
    pub fn compute_sha_manifest(&self) -> Result<String> {
        let mut s = String::new();
        for p in self.listing.entries.keys() {
            if p == SHA_MANIFEST_PATH {
                continue;
            }
            s.push_str(&format!(
                "{}  {}\n",
                sha256_hex(&self.files.read(p, MAX_FILE)?),
                p
            ));
        }
        Ok(s)
    }

    /// Checks `manifest.sha256` against the files. `Ok(None)` = no manifest present.
    pub fn verify_sha_manifest(&self) -> Result<Option<Vec<String>>> {
        if !self.listing.entries.contains_key(SHA_MANIFEST_PATH) {
            return Ok(None);
        }
        let txt = String::from_utf8(self.files.read(SHA_MANIFEST_PATH, MAX_TEXT)?)
            .map_err(|_| ModError("manifest.sha256: not UTF-8".into()))?;
        let mut listed: BTreeMap<&str, &str> = BTreeMap::new();
        let mut problems = vec![];
        for (n, line) in txt.lines().enumerate() {
            match line.split_once("  ") {
                Some((h, p)) if h.len() == 64 && h.bytes().all(|b| b.is_ascii_hexdigit()) => {
                    if listed.insert(p, h).is_some() {
                        problems.push(format!("manifest.sha256:{}: duplicate path {p}", n + 1));
                    }
                }
                _ => problems.push(format!("manifest.sha256:{}: malformed line", n + 1)),
            }
        }
        for (p, h) in &listed {
            if !self.listing.entries.contains_key(*p) {
                problems.push(format!("{p}: listed in manifest.sha256 but missing"));
                continue;
            }
            let got = sha256_hex(&self.files.read(p, MAX_FILE)?);
            if !got.eq_ignore_ascii_case(h) {
                problems.push(format!("{p}: hash mismatch (manifest {h}, file {got})"));
            }
        }
        for p in self.listing.entries.keys() {
            if p != SHA_MANIFEST_PATH && !listed.contains_key(p.as_str()) {
                problems.push(format!("{p}: not listed in manifest.sha256"));
            }
        }
        Ok(Some(problems))
    }

    /// SHA-256 of the hash manifest text (stable identity of the content, same for a directory and its package).
    pub fn content_sha256(&self) -> Result<String> {
        Ok(sha256_hex(self.compute_sha_manifest()?.as_bytes()))
    }

    pub fn thumbnail_ok(&self) -> bool {
        self.manifest
            .thumbnail
            .as_ref()
            .is_some_and(|t| self.listing.entries.contains_key(t))
    }
}

pub fn is_patch_path(p: &str) -> bool {
    p.starts_with("patches/") && (p.ends_with(".pchtxt") || p.ends_with(".ips"))
}

/// What the user chose for one installed mod. Everything that changes game code or memory defaults to off.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Selection {
    pub enabled: bool,
    pub code_patches: bool,
    pub cheats: bool,
    pub shaders: bool,
    /// Per patch override: key `patches/x.pchtxt` (whole file) or `patches/x.pchtxt#Block name`.
    pub overrides: BTreeMap<String, bool>,
}

/// A mod prepared for the apply hook.
#[derive(Debug, Clone)]
pub struct ResolvedMod {
    pub id: String,
    pub version: String,
    /// Content identity (see [`LoadedMod::content_sha256`]).
    pub content_sha256: String,
    pub selection: Selection,
    pub patches: Vec<PatchFile>,
    pub cheats: Vec<CheatSource>,
}

impl LoadedMod {
    pub fn resolve(&self, selection: Selection) -> Result<ResolvedMod> {
        Ok(ResolvedMod {
            id: self.manifest.id.clone(),
            version: self.manifest.version.clone(),
            content_sha256: self.content_sha256()?,
            patches: self.load_patches()?,
            cheats: self.load_cheats()?,
            selection,
        })
    }
}

impl ResolvedMod {
    /// Enabled cheats for a main-module build id (prefix compare using the file name length).
    pub fn cheats_for(&self, build_id: &[u8; 32]) -> Vec<&cheats::Cheat> {
        if !(self.selection.enabled && self.selection.cheats) {
            return vec![];
        }
        self.cheats
            .iter()
            .filter(|c| c.build_id[..c.prefix_len] == build_id[..c.prefix_len])
            .flat_map(|c| c.file.cheats.iter())
            .collect()
    }
}

pub fn short_hash(h: &str) -> String {
    format!("{}…", &h[..h.len().min(12)])
}
