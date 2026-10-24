//! Share flow: what would be sent, exactly, and (only with explicit consent) how.
//!
//! Default is local only. `plan` is pure; `execute` needs `gh` and is called only from `--yes`.
//! Commit metadata is neutral (fixed identity, UTC-midnight timestamps, isolated git config).
//! The pull request itself is created by the GitHub account `gh` is logged in with: that cannot be
//! hidden from GitHub, so the preview says so and recommends a dedicated account.
use crate::domain::*;
use crate::exec::{self, Spec};
use crate::repo::SignatureRepository;
use crate::validate;
use sha2::{Digest, Sha256};
use std::fs;
use std::path::Path;
use std::time::Duration;

pub const NEUTRAL_NAME: &str = "Napple Signatures";
pub const NEUTRAL_EMAIL: &str = "signatures@napple.invalid";

#[derive(Debug)]
pub struct SharePlan {
    pub upstream: String,
    pub branch: String,
    pub commit_message: String,
    pub pr_title: String,
    pub pr_body: String,
    /// (repo-relative path, exact file content)
    pub files: Vec<(String, String)>,
    pub entries: usize,
    pub excluded: Vec<String>,
}

/// `https://github.com/owner/name[.git]` -> `owner/name`.
pub fn slug(url: &str) -> Result<String, String> {
    let rest = url.strip_prefix("https://github.com/").ok_or("remote must be an https://github.com/ URL")?;
    let s = rest.trim_end_matches('/').trim_end_matches(".git");
    let ok = s.split('/').count() == 2 && s.split('/').all(|p| !p.is_empty() && p.bytes().all(|c| c.is_ascii_alphanumeric() || b"-_.".contains(&c)));
    if ok { Ok(s.to_string()) } else { Err(format!("unexpected repository path {s:?}")) }
}

pub fn build_plan(pending: &dyn SignatureRepository, upstream_url: &str) -> Result<SharePlan, String> {
    let upstream = slug(upstream_url)?;
    let mut files = Vec::new();
    let mut excluded = Vec::new();
    let mut entries = 0;
    for t in pending.titles()? {
        for b in pending.builds(&t)? {
            match pending.load(&t, &b) {
                Ok(Some(f)) if !f.signatures.is_empty() => {
                    let errs = validate::validate(&f);
                    if errs.is_empty() {
                        entries += f.signatures.len();
                        files.push((format!("knownSignatures/{t}/{b}.json"), f.to_json()));
                    } else {
                        excluded.push(format!("{t}/{b}: {}", errs.join("; ")));
                    }
                }
                Ok(_) => {}
                Err(e) => excluded.push(format!("{t}/{b}: {e}")),
            }
        }
    }
    let mut h = Sha256::new();
    for (p, c) in &files {
        h.update(p.as_bytes());
        h.update(c.as_bytes());
    }
    let branch = format!("signatures/{}", hex(&h.finalize()[..4]));
    let nfiles = files.len();
    Ok(SharePlan {
        upstream,
        branch,
        commit_message: format!("Add {entries} signatures ({nfiles} build files)"),
        pr_title: format!("Add {entries} signatures ({nfiles} build files)"),
        pr_body: format!(
            "{entries} masked-pattern signatures in {nfiles} build file(s), generated locally by `napple sigs`.\n\
             Contains only offsets, short masked instruction patterns (<= {MAX_PATTERN_BYTES} bytes), names and hashes; no other game data, strings or dumps.\n\
             Each file passed `napple sigs validate`.\n"
        ),
        files,
        entries,
        excluded,
    })
}

pub fn preview(plan: &SharePlan, dry_run: bool) -> String {
    let mut o = String::new();
    o.push_str(&format!("{}\n", if dry_run { "DRY RUN: nothing will be sent" } else { "SHARE PREVIEW" }));
    o.push_str(&format!("target repository : https://github.com/{} (pull request from your fork)\n", plan.upstream));
    o.push_str(&format!("branch            : {}\n", plan.branch));
    o.push_str(&format!("commit identity   : {NEUTRAL_NAME} <{NEUTRAL_EMAIL}> (UTC midnight timestamps, isolated git config)\n"));
    o.push_str(&format!("commit/PR title   : {}\n", plan.pr_title));
    o.push_str(&format!("signatures        : {} in {} file(s)\n", plan.entries, plan.files.len()));
    o.push_str("note: the pull request is opened by the GitHub account that `gh` is logged in with; GitHub knows that\n");
    o.push_str("      account. Use a dedicated account if you want to stay unlinked. Commit metadata carries no identity.\n");
    for e in &plan.excluded {
        o.push_str(&format!("EXCLUDED (not shared): {e}\n"));
    }
    o.push_str("\nPR description:\n");
    for l in plan.pr_body.lines() {
        o.push_str(&format!("  {l}\n"));
    }
    o.push_str("\nplanned steps:\n");
    for c in steps_preview(plan) {
        o.push_str(&format!("  {c}\n"));
    }
    o.push_str("\nEXACT file contents that would be committed (merged into an existing file if one is already upstream):\n");
    for (p, c) in &plan.files {
        o.push_str(&format!("\n--- {p} ---\n{c}"));
    }
    o
}

fn steps_preview(plan: &SharePlan) -> Vec<String> {
    let u = &plan.upstream;
    vec![
        "gh auth status".into(),
        "gh api user --jq .login".into(),
        format!("gh repo fork {u} --clone=false"),
        format!("git clone --depth 1 https://github.com/{u} <temp dir inside work/>"),
        format!("git checkout -b {}", plan.branch),
        "(write the files above; git add knownSignatures)".into(),
        format!("git commit -m \"{}\"   [author/committer: {NEUTRAL_NAME}]", plan.commit_message),
        "git push <your fork> <branch>".into(),
        format!("gh pr create --repo {u} --head <login>:{} --title ... --body ...", plan.branch),
    ]
}

fn day_start_epoch() -> u64 {
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    now - now % 86400
}

pub struct Tools {
    pub gh: std::path::PathBuf,
    pub git: std::path::PathBuf,
}

impl Tools {
    pub fn detect() -> Result<Self, String> {
        Ok(Self {
            gh: exec::which("gh").ok_or("`gh` is not installed (brew install gh) and authenticated (gh auth login)")?,
            git: exec::which("git").ok_or("git is not installed")?,
        })
    }
}

/// Performs the share. Only called after explicit `--yes`. Returns the PR URL.
pub fn execute(plan: &SharePlan, tools: &Tools, work_dir: &Path) -> Result<String, String> {
    if plan.files.is_empty() {
        return Err("nothing to share".into());
    }
    let real_home = std::env::var("HOME").unwrap_or_default();
    let gh_cfg = std::env::var("GH_CONFIG_DIR").unwrap_or_else(|_| format!("{real_home}/.config/gh"));
    let path = std::env::var("PATH").unwrap_or_default();
    let gh = |args: &[&str]| -> Result<String, String> {
        let mut s = Spec::new(&tools.gh, args);
        s.env = vec![("PATH".into(), path.clone()), ("HOME".into(), real_home.clone()), ("GH_PROMPT_DISABLED".into(), "1".into())];
        s.timeout = Duration::from_secs(120);
        let o = exec::run(&s)?;
        if o.ok() { Ok(o.stdout_str()) } else { Err(format!("gh {} failed: {}", args.first().unwrap_or(&""), o.stderr_str().trim())) }
    };
    gh(&["auth", "status"]).map_err(|_| "gh is not authenticated; run `gh auth login` yourself, then retry".to_string())?;
    let login = gh(&["api", "user", "--jq", ".login"])?.trim().to_string();
    if login.is_empty() || !login.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-') {
        return Err("unexpected login from gh".into());
    }
    gh(&["repo", "fork", &plan.upstream, "--clone=false"])?;

    let tmp = work_dir.join("sigshare").join(&plan.branch.replace('/', "-"));
    let _ = fs::remove_dir_all(&tmp);
    fs::create_dir_all(&tmp).map_err(|e| e.to_string())?;
    let sandbox_home = tmp.join("home");
    fs::create_dir_all(&sandbox_home).map_err(|e| e.to_string())?;
    let repo = tmp.join("repo");
    let stamp = format!("{} +0000", day_start_epoch());
    let git_env = vec![
        ("PATH".to_string(), path.clone()),
        ("HOME".to_string(), sandbox_home.to_string_lossy().into()),
        ("GH_CONFIG_DIR".to_string(), gh_cfg),
        ("GIT_CONFIG_GLOBAL".to_string(), "/dev/null".to_string()),
        ("GIT_CONFIG_NOSYSTEM".to_string(), "1".to_string()),
        ("GIT_TERMINAL_PROMPT".to_string(), "0".to_string()),
        ("GIT_AUTHOR_NAME".to_string(), NEUTRAL_NAME.to_string()),
        ("GIT_AUTHOR_EMAIL".to_string(), NEUTRAL_EMAIL.to_string()),
        ("GIT_COMMITTER_NAME".to_string(), NEUTRAL_NAME.to_string()),
        ("GIT_COMMITTER_EMAIL".to_string(), NEUTRAL_EMAIL.to_string()),
        ("GIT_AUTHOR_DATE".to_string(), stamp.clone()),
        ("GIT_COMMITTER_DATE".to_string(), stamp),
    ];
    let git = |args: &[&str], cwd: &Path| -> Result<String, String> {
        let mut s = Spec::new(&tools.git, args);
        s.cwd = Some(cwd.to_path_buf());
        s.env = git_env.clone();
        s.timeout = Duration::from_secs(300);
        let o = exec::run(&s)?;
        if o.ok() { Ok(o.stdout_str()) } else { Err(format!("git {} failed: {}", args.first().unwrap_or(&""), o.stderr_str().trim())) }
    };
    let url = format!("https://github.com/{}", plan.upstream);
    git(&["clone", "--depth", "1", "--quiet", &url, repo.to_str().ok_or("bad path")?], &tmp)?;
    let base = git(&["rev-parse", "--abbrev-ref", "HEAD"], &repo)?.trim().to_string();
    git(&["checkout", "-q", "-b", &plan.branch], &repo)?;
    for (rel, content) in &plan.files {
        let dest = repo.join(rel);
        // merge with an existing upstream file; upstream entries win
        let merged = match fs::read_to_string(&dest) {
            Ok(old) => {
                let mut o = validate::validate_json(&old).map_err(|e| format!("upstream {rel} invalid: {}", e.join("; ")))?;
                let n = validate::validate_json(content).map_err(|e| e.join("; "))?;
                for (k, v) in n.signatures {
                    o.signatures.entry(k).or_insert(v);
                }
                o.to_json()
            }
            Err(_) => content.clone(),
        };
        if !validate::validate_json(&merged).is_ok() {
            return Err(format!("merged {rel} failed validation"));
        }
        fs::create_dir_all(dest.parent().unwrap()).map_err(|e| e.to_string())?;
        fs::write(&dest, merged).map_err(|e| e.to_string())?;
    }
    git(&["add", "knownSignatures"], &repo)?;
    git(&["commit", "-q", "--no-gpg-sign", "-m", &plan.commit_message], &repo)?;
    let fork = format!("https://github.com/{login}/{}", plan.upstream.split('/').nth(1).unwrap());
    git(&["remote", "add", "fork", &fork], &repo)?;
    git(&["-c", "credential.helper=", "-c", "credential.helper=!gh auth git-credential", "push", "--quiet", "fork", &plan.branch], &repo)?;
    let head = format!("{login}:{}", plan.branch);
    let out = gh(&["pr", "create", "--repo", &plan.upstream, "--head", &head, "--base", &base, "--title", &plan.pr_title, "--body", &plan.pr_body])?;
    let _ = fs::remove_dir_all(&tmp);
    Ok(out.trim().to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::repo::DirRepository;
    use crate::validate::fixtures::*;

    #[test]
    fn slug_rules() {
        assert_eq!(slug("https://github.com/erostalevi/Napple").unwrap(), "erostalevi/Napple");
        assert_eq!(slug("https://github.com/a/b.git/").unwrap(), "a/b");
        for bad in ["http://github.com/a/b", "https://evil.example/a/b", "https://github.com/a/b/c", "https://github.com/a/b c", "git@github.com:a/b"] {
            assert!(slug(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn plan_and_dry_run_preview_show_exact_content() {
        let d = tempfile::tempdir().unwrap();
        let pending = DirRepository::new(d.path(), "pending", true);
        let f = sample_file();
        pending.save(&f).unwrap();
        let plan = build_plan(&pending, "https://github.com/erostalevi/Napple").unwrap();
        assert_eq!(plan.entries, 1);
        assert_eq!(plan.files.len(), 1);
        assert_eq!(plan.files[0].0, format!("knownSignatures/{}/{}.json", f.title_id, f.build_id));
        let out = preview(&plan, true);
        assert!(out.starts_with("DRY RUN: nothing will be sent"));
        assert!(out.contains(&plan.files[0].1), "exact bytes must be shown");
        assert!(out.contains(NEUTRAL_EMAIL) && out.contains("--head <login>"));
        // no local identity or paths in what is sent
        let home = std::env::var("HOME").unwrap_or_default();
        assert!(!plan.files[0].1.contains(&home) && !plan.pr_body.contains(&home));
        // deterministic branch name derived from content only
        assert_eq!(plan.branch, build_plan(&pending, "https://github.com/erostalevi/Napple").unwrap().branch);
        assert!(plan.branch.starts_with("signatures/"));
    }

    #[test]
    fn invalid_pending_is_excluded_not_shared() {
        let d = tempfile::tempdir().unwrap();
        let pending = DirRepository::new(d.path(), "pending", true);
        let f = sample_file();
        let dir = d.path().join(&f.title_id);
        fs::create_dir_all(&dir).unwrap();
        let mut bad = f.clone();
        bad.signatures.values_mut().next().unwrap().name = Some("deadbeef".repeat(10));
        fs::write(dir.join(format!("{}.json", f.build_id)), bad.to_json()).unwrap();
        let plan = build_plan(&pending, "https://github.com/erostalevi/Napple").unwrap();
        assert!(plan.files.is_empty());
        assert_eq!(plan.excluded.len(), 1);
        assert!(preview(&plan, false).contains("EXCLUDED"));
    }

    #[test]
    fn execute_refuses_empty_plan_without_touching_anything() {
        let d = tempfile::tempdir().unwrap();
        let pending = DirRepository::new(d.path(), "pending", true);
        let plan = build_plan(&pending, "https://github.com/erostalevi/Napple").unwrap();
        let tools = Tools { gh: "/nonexistent/gh".into(), git: "/nonexistent/git".into() };
        assert!(execute(&plan, &tools, d.path()).unwrap_err().contains("nothing"));
    }
}
