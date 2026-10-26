//! `SignatureRepository` port and its adapters: local directory store, layered view,
//! and a read-only git cache of the public repository.
use crate::domain::*;
use crate::exec::{self, Spec};
use crate::validate;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::time::Duration;

pub trait SignatureRepository {
    fn label(&self) -> String;
    fn load(&self, t: &TitleId, b: &BuildId) -> Result<Option<SignatureFile>, String>;
    fn builds(&self, t: &TitleId) -> Result<Vec<BuildId>, String>;
    fn titles(&self) -> Result<Vec<TitleId>, String>;
    /// Upserts the file's entries (merge by pattern hash). Read-only adapters refuse.
    fn save(&self, _f: &SignatureFile) -> Result<(), String> {
        Err(format!("{} is read-only", self.label()))
    }
    fn remove(&self, _t: &TitleId, _b: &BuildId) -> Result<(), String> {
        Err(format!("{} is read-only", self.label()))
    }

    fn lookup(&self, t: &TitleId, b: &BuildId, hash: &str) -> Result<Option<Signature>, String> {
        Ok(self
            .load(t, b)?
            .and_then(|f| f.signatures.get(hash).cloned()))
    }

    /// Same pattern hash in any build of the title (patterns survive across updates).
    fn find_by_hash(&self, t: &TitleId, hash: &str) -> Result<Vec<(BuildId, Signature)>, String> {
        let mut out = Vec::new();
        for b in self.builds(t)? {
            if let Some(s) = self.lookup(t, &b, hash)? {
                out.push((b, s));
            }
        }
        Ok(out)
    }
}

pub struct DirRepository {
    root: PathBuf,
    label: String,
    writable: bool,
}

impl DirRepository {
    pub fn new(root: impl Into<PathBuf>, label: &str, writable: bool) -> Self {
        Self {
            root: root.into(),
            label: label.into(),
            writable,
        }
    }

    fn file(&self, t: &TitleId, b: &BuildId) -> PathBuf {
        self.root.join(&t.0).join(format!("{}.json", b.0))
    }

    fn read_dir_names(&self, dir: &Path) -> Result<Vec<String>, String> {
        match fs::read_dir(dir) {
            Ok(rd) => {
                let mut v: Vec<String> = rd
                    .filter_map(|e| e.ok())
                    // symlinks are never followed (a hostile cache could point outside)
                    .filter(|e| e.file_type().map(|t| !t.is_symlink()).unwrap_or(false))
                    .filter_map(|e| e.file_name().into_string().ok())
                    .collect();
                v.sort();
                Ok(v)
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Vec::new()),
            Err(e) => Err(format!("{}: {e}", dir.display())),
        }
    }
}

impl SignatureRepository for DirRepository {
    fn label(&self) -> String {
        self.label.clone()
    }

    fn load(&self, t: &TitleId, b: &BuildId) -> Result<Option<SignatureFile>, String> {
        let p = self.file(t, b);
        match fs::symlink_metadata(&p) {
            Ok(m) if m.is_file() => {}
            Ok(_) => return Err(format!("{}: not a regular file", p.display())),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
            Err(e) => return Err(format!("{}: {e}", p.display())),
        }
        let text = fs::read_to_string(&p).map_err(|e| format!("{}: {e}", p.display()))?;
        let f = validate::validate_json(&text)
            .map_err(|e| format!("{}: invalid: {}", p.display(), e.join("; ")))?;
        if f.title_id != t.0 || f.build_id != b.0 {
            return Err(format!(
                "{}: title/build ids do not match the path",
                p.display()
            ));
        }
        Ok(Some(f))
    }

    fn builds(&self, t: &TitleId) -> Result<Vec<BuildId>, String> {
        Ok(self
            .read_dir_names(&self.root.join(&t.0))?
            .iter()
            .filter_map(|n| n.strip_suffix(".json").and_then(|s| BuildId::parse(s).ok()))
            .collect())
    }

    fn titles(&self) -> Result<Vec<TitleId>, String> {
        Ok(self
            .read_dir_names(&self.root)?
            .iter()
            .filter_map(|n| TitleId::parse(n).ok())
            .collect())
    }

    fn save(&self, f: &SignatureFile) -> Result<(), String> {
        if !self.writable {
            return Err(format!("{} is read-only", self.label));
        }
        let (t, b) = (TitleId::parse(&f.title_id)?, BuildId::parse(&f.build_id)?);
        let mut merged = self
            .load(&t, &b)?
            .unwrap_or_else(|| SignatureFile::new(&t, &b));
        merged.signatures.extend(f.signatures.clone());
        let errs = validate::validate(&merged);
        if !errs.is_empty() {
            return Err(format!(
                "refusing to store invalid signatures: {}",
                errs.join("; ")
            ));
        }
        let p = self.file(&t, &b);
        fs::create_dir_all(p.parent().unwrap()).map_err(|e| e.to_string())?;
        let tmp = p.with_extension("json.tmp");
        {
            use std::os::unix::fs::OpenOptionsExt;
            let mut o = fs::OpenOptions::new()
                .write(true)
                .create(true)
                .truncate(true)
                .mode(0o600)
                .open(&tmp)
                .map_err(|e| e.to_string())?;
            o.write_all(merged.to_json().as_bytes())
                .map_err(|e| e.to_string())?;
            o.sync_all().ok();
        }
        fs::rename(&tmp, &p).map_err(|e| e.to_string())
    }

    fn remove(&self, t: &TitleId, b: &BuildId) -> Result<(), String> {
        if !self.writable {
            return Err(format!("{} is read-only", self.label));
        }
        match fs::remove_file(self.file(t, b)) {
            Ok(()) => Ok(()),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(e) => Err(e.to_string()),
        }
    }
}

/// Merged view over several repositories; earlier layers win on equal pattern hashes. Writes go to `write_to`.
pub struct LayeredRepository {
    layers: Vec<Box<dyn SignatureRepository>>,
    write_to: usize,
}

impl LayeredRepository {
    pub fn new(layers: Vec<Box<dyn SignatureRepository>>, write_to: usize) -> Self {
        Self { layers, write_to }
    }
}

impl SignatureRepository for LayeredRepository {
    fn label(&self) -> String {
        self.layers
            .iter()
            .map(|l| l.label())
            .collect::<Vec<_>>()
            .join("+")
    }

    fn load(&self, t: &TitleId, b: &BuildId) -> Result<Option<SignatureFile>, String> {
        let mut out: Option<SignatureFile> = None;
        for l in &self.layers {
            if let Some(f) = l.load(t, b)? {
                match &mut out {
                    None => out = Some(f),
                    Some(o) => {
                        for (k, v) in f.signatures {
                            o.signatures.entry(k).or_insert(v);
                        }
                    }
                }
            }
        }
        Ok(out)
    }

    fn builds(&self, t: &TitleId) -> Result<Vec<BuildId>, String> {
        let mut v = Vec::new();
        for l in &self.layers {
            v.extend(l.builds(t)?);
        }
        v.sort();
        v.dedup();
        Ok(v)
    }

    fn titles(&self) -> Result<Vec<TitleId>, String> {
        let mut v = Vec::new();
        for l in &self.layers {
            v.extend(l.titles()?);
        }
        v.sort();
        v.dedup();
        Ok(v)
    }

    fn save(&self, f: &SignatureFile) -> Result<(), String> {
        self.layers[self.write_to].save(f)
    }
}

pub const DEFAULT_REMOTE: &str = "https://github.com/erostalevi/Napple";

/// Read-only cache of the public repository. Network is touched ONLY by `sync`, never by reads or tests.
pub struct GitCacheRepository {
    pub url: String,
    pub cache: PathBuf,
    inner: DirRepository,
}

impl GitCacheRepository {
    pub fn new(url: &str, cache: impl Into<PathBuf>) -> Self {
        let cache = cache.into();
        Self {
            url: url.into(),
            inner: DirRepository::new(cache.join("knownSignatures"), "remote-cache", false),
            cache,
        }
    }

    fn git(&self, args: &[&str], cwd: Option<&Path>) -> Result<exec::Output, String> {
        let git = exec::which("git").ok_or("git is not installed")?;
        let mut s = Spec::new(git, args);
        s.cwd = cwd.map(|p| p.to_path_buf());
        s.timeout = Duration::from_secs(300);
        s.env = vec![
            ("PATH".into(), std::env::var("PATH").unwrap_or_default()),
            ("HOME".into(), std::env::var("HOME").unwrap_or_default()),
            ("GIT_TERMINAL_PROMPT".into(), "0".into()),
            ("GIT_CONFIG_NOSYSTEM".into(), "1".into()),
        ];
        let o = exec::run(&s)?;
        if o.ok() {
            Ok(o)
        } else {
            Err(format!(
                "git {} failed: {}",
                args.first().unwrap_or(&""),
                o.stderr_str().trim()
            ))
        }
    }

    /// Shallow clone or fast-forward fetch. Explicit user action (`napple sigs sync`).
    pub fn sync(&self) -> Result<(), String> {
        let cache = self.cache.to_string_lossy().to_string();
        if self.cache.join(".git").exists() {
            self.git(
                &["fetch", "--depth", "1", "--quiet", "origin"],
                Some(&self.cache),
            )?;
            self.git(
                &["reset", "--hard", "--quiet", "FETCH_HEAD"],
                Some(&self.cache),
            )?;
        } else {
            if let Some(p) = self.cache.parent() {
                fs::create_dir_all(p).map_err(|e| e.to_string())?;
            }
            self.git(
                &[
                    "clone",
                    "--depth",
                    "1",
                    "--quiet",
                    "--no-tags",
                    &self.url,
                    &cache,
                ],
                None,
            )?;
        }
        Ok(())
    }
}

impl SignatureRepository for GitCacheRepository {
    fn label(&self) -> String {
        "remote-cache".into()
    }
    fn load(&self, t: &TitleId, b: &BuildId) -> Result<Option<SignatureFile>, String> {
        self.inner.load(t, b)
    }
    fn builds(&self, t: &TitleId) -> Result<Vec<BuildId>, String> {
        self.inner.builds(t)
    }
    fn titles(&self) -> Result<Vec<TitleId>, String> {
        self.inner.titles()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::validate::fixtures::*;

    #[test]
    fn store_round_trip_and_merge() {
        let d = tempfile::tempdir().unwrap();
        let r = DirRepository::new(d.path(), "local", true);
        let f = sample_file();
        let (t, b) = (TitleId(f.title_id.clone()), BuildId(f.build_id.clone()));
        assert!(r.load(&t, &b).unwrap().is_none());
        r.save(&f).unwrap();
        assert_eq!(r.load(&t, &b).unwrap().unwrap(), f);
        // merging a second entry keeps the first
        let mut g = SignatureFile::new(&t, &b);
        let p2 = Pattern::from_a64(
            &[0xD503201Fu32, 0xD65F03C0, 0xA9BF7BFD, 0x910003FD]
                .iter()
                .flat_map(|x| x.to_le_bytes())
                .collect::<Vec<_>>(),
        )
        .unwrap();
        g.signatures.insert(p2.hash(), sample_sig(&p2));
        r.save(&g).unwrap();
        assert_eq!(r.load(&t, &b).unwrap().unwrap().signatures.len(), 2);
        assert_eq!(r.titles().unwrap(), vec![t.clone()]);
        assert_eq!(r.builds(&t).unwrap(), vec![b.clone()]);
        let h = sample_pattern().hash();
        assert!(r.lookup(&t, &b, &h).unwrap().is_some());
        assert_eq!(r.find_by_hash(&t, &h).unwrap().len(), 1);
        r.remove(&t, &b).unwrap();
        assert!(r.load(&t, &b).unwrap().is_none());
    }

    #[test]
    fn read_only_and_corrupt_and_mismatched_files() {
        let d = tempfile::tempdir().unwrap();
        let ro = DirRepository::new(d.path(), "known", false);
        assert!(ro.save(&sample_file()).is_err());
        let f = sample_file();
        let dir = d.path().join(&f.title_id);
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join(format!("{}.json", f.build_id)), "{ not json").unwrap();
        let (t, b) = (TitleId(f.title_id.clone()), BuildId(f.build_id.clone()));
        assert!(ro.load(&t, &b).unwrap_err().contains("invalid"));
        // valid file stored under the wrong build id
        let other = BuildId("cd".repeat(16));
        fs::write(dir.join(format!("{}.json", other.0)), f.to_json()).unwrap();
        assert!(ro.load(&t, &other).unwrap_err().contains("do not match"));
    }

    #[test]
    fn symlinked_entries_are_ignored() {
        let d = tempfile::tempdir().unwrap();
        let f = sample_file();
        let outside = tempfile::tempdir().unwrap();
        fs::write(outside.path().join("x.json"), f.to_json()).unwrap();
        let dir = d.path().join(&f.title_id);
        fs::create_dir_all(&dir).unwrap();
        std::os::unix::fs::symlink(
            outside.path().join("x.json"),
            dir.join(format!("{}.json", f.build_id)),
        )
        .unwrap();
        let r = DirRepository::new(d.path(), "known", false);
        let (t, b) = (TitleId(f.title_id.clone()), BuildId(f.build_id.clone()));
        assert!(r.load(&t, &b).is_err());
        assert!(r.builds(&t).unwrap().is_empty());
    }

    #[test]
    fn layered_prefers_earlier_layers_and_writes_to_selected() {
        let (a, b) = (tempfile::tempdir().unwrap(), tempfile::tempdir().unwrap());
        let (ra, rb) = (
            DirRepository::new(a.path(), "a", true),
            DirRepository::new(b.path(), "b", true),
        );
        let f = sample_file();
        rb.save(&f).unwrap();
        let l = LayeredRepository::new(vec![Box::new(ra), Box::new(rb)], 0);
        let (t, bi) = (TitleId(f.title_id.clone()), BuildId(f.build_id.clone()));
        assert_eq!(l.load(&t, &bi).unwrap().unwrap(), f);
        let mut g = sample_file();
        g.signatures.values_mut().next().unwrap().name = Some("other".into());
        l.save(&g).unwrap();
        let got = l.load(&t, &bi).unwrap().unwrap();
        assert_eq!(
            got.signatures.values().next().unwrap().name.as_deref(),
            Some("other")
        );
        assert!(a.path().join(&f.title_id).exists());
    }

    #[test]
    fn git_cache_sync_from_local_repo_no_network() {
        if exec::which("git").is_none() {
            return;
        }
        let src = tempfile::tempdir().unwrap();
        let f = sample_file();
        let dir = src.path().join("knownSignatures").join(&f.title_id);
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join(format!("{}.json", f.build_id)), f.to_json()).unwrap();
        let g = |args: &[&str]| {
            let mut s = Spec::new(exec::which("git").unwrap(), args);
            s.cwd = Some(src.path().to_path_buf());
            s.env = vec![
                ("PATH".into(), std::env::var("PATH").unwrap()),
                ("HOME".into(), src.path().to_string_lossy().into()),
            ];
            assert!(exec::run(&s).unwrap().ok(), "git {args:?}");
        };
        g(&["init", "-q"]);
        g(&["add", "."]);
        g(&[
            "-c",
            "user.name=t",
            "-c",
            "user.email=t@invalid",
            "-c",
            "commit.gpgsign=false",
            "commit",
            "-qm",
            "x",
        ]);
        let cache = tempfile::tempdir().unwrap();
        let repo = GitCacheRepository::new(
            &format!("file://{}", src.path().display()),
            cache.path().join("sigcache"),
        );
        let (t, b) = (TitleId(f.title_id.clone()), BuildId(f.build_id.clone()));
        assert!(repo.load(&t, &b).unwrap().is_none()); // before sync nothing, and no network touched
        repo.sync().unwrap();
        assert_eq!(repo.load(&t, &b).unwrap().unwrap(), f);
        repo.sync().unwrap(); // fetch path
        assert!(repo.save(&f).is_err());
    }
}
