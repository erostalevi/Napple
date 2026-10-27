//! .app assembly helpers: Info.plist, PkgInfo, icon conversion, tool discovery, ad-hoc signing.
use crate::error::{BuildError, Result};
use std::path::{Path, PathBuf};
use std::process::Command;

pub const LAUNCHER_NAME: &str = "napple-launcher";
pub const RUNTIME_NAME: &str = "napple-runtime";
pub const MIN_MACOS: &str = "26.0";

pub fn xml_escape(s: &str) -> String {
    s.chars()
        .filter(|c| !c.is_control() || matches!(c, '\n' | '\t'))
        .map(|c| match c {
            '&' => "&amp;".to_string(),
            '<' => "&lt;".to_string(),
            '>' => "&gt;".to_string(),
            '"' => "&quot;".to_string(),
            c => c.to_string(),
        })
        .collect()
}

/// Version strings go into CFBundleVersion: keep digits, letters and dots.
pub fn plist_version(v: &str) -> String {
    let s: String = v
        .chars()
        .filter(|c| c.is_ascii_alphanumeric() || *c == '.')
        .collect();
    if s.is_empty() { "1.0".into() } else { s }
}

pub fn info_plist(title_id: u64, name: &str, version: &str, has_icon: bool) -> String {
    let v = plist_version(version);
    let mut kv: Vec<(&str, String)> = vec![
        ("CFBundleDevelopmentRegion", "<string>en</string>".into()),
        (
            "CFBundleExecutable",
            format!("<string>{LAUNCHER_NAME}</string>"),
        ),
        (
            "CFBundleIdentifier",
            format!("<string>com.napple.game.{:016x}</string>", title_id),
        ),
        (
            "CFBundleInfoDictionaryVersion",
            "<string>6.0</string>".into(),
        ),
        (
            "CFBundleName",
            format!("<string>{}</string>", xml_escape(name)),
        ),
        (
            "CFBundleDisplayName",
            format!("<string>{}</string>", xml_escape(name)),
        ),
        ("CFBundlePackageType", "<string>APPL</string>".into()),
        (
            "CFBundleShortVersionString",
            format!("<string>{v}</string>"),
        ),
        ("CFBundleVersion", format!("<string>{v}</string>")),
        (
            "LSMinimumSystemVersion",
            format!("<string>{MIN_MACOS}</string>"),
        ),
        (
            "LSApplicationCategoryType",
            "<string>public.app-category.games</string>".into(),
        ),
        ("LSSupportsGameMode", "<true/>".into()),
        ("NSHighResolutionCapable", "<true/>".into()),
        ("NSSupportsAutomaticGraphicsSwitching", "<true/>".into()),
        ("NSPrincipalClass", "<string>NSApplication</string>".into()),
        // IR camera simulator (frontend/src/simulators.m): the webcam stands in for the Joy-Con IR sensor; the prompt shows only when a game starts an IR processor.
        (
            "NSCameraUsageDescription",
            "<string>Some games use the IR camera of a Joy-Con. Napple can use your webcam to simulate it, and only when a game asks for it.</string>".into(),
        ),
        // Microphone (audin: the game's audio-in source; shown only when a game opens the audio input and no NAPPLE_AUDIN_SOURCE file is set).
        (
            "NSMicrophoneUsageDescription",
            "<string>Some games listen to a microphone. Napple can use yours, and only when a game asks for it.</string>".into(),
        ),
        // microphone source (NAPPLE_AUDIN_SOURCE=mic, audio/src/engine.c): the prompt shows only when a game opens audio input
        (
            "NSMicrophoneUsageDescription",
            "<string>Some games record from a microphone. Napple can use your Mac's microphone for that, and only when a game asks for it.</string>".into(),
        ),
        (
            "NappleTitleId",
            format!("<string>{:016X}</string>", title_id),
        ),
    ];
    if has_icon {
        kv.push(("CFBundleIconFile", "<string>AppIcon</string>".into()));
    }
    let body: String = kv
        .iter()
        .map(|(k, v)| format!("\t<key>{k}</key>\n\t{v}\n"))
        .collect();
    format!(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n{body}</dict>\n</plist>\n"
    )
}

fn run_tool(cmd: &mut Command) -> std::result::Result<(), String> {
    match cmd.output() {
        Ok(o) if o.status.success() => Ok(()),
        Ok(o) => Err(String::from_utf8_lossy(&o.stderr).trim().to_string()),
        Err(e) => Err(e.to_string()),
    }
}

/// JPEG -> AppIcon.icns through the system `sips` and `iconutil`. Ok(false) when the tools are unavailable or fail.
pub fn make_icns(jpeg: &[u8], work: &Path, out: &Path) -> Result<bool> {
    let set = work.join("AppIcon.iconset");
    std::fs::create_dir_all(&set).map_err(|e| BuildError::io("create iconset", e))?;
    let src = work.join("icon.jpg");
    std::fs::write(&src, jpeg).map_err(|e| BuildError::io("write icon", e))?;
    for (name, px) in [
        ("16x16", 16),
        ("16x16@2x", 32),
        ("32x32", 32),
        ("32x32@2x", 64),
        ("128x128", 128),
        ("128x128@2x", 256),
        ("256x256", 256),
        ("256x256@2x", 512),
        ("512x512", 512),
    ] {
        let dst = set.join(format!("icon_{name}.png"));
        let mut c = Command::new("sips");
        c.args([
            "-s",
            "format",
            "png",
            "-z",
            &px.to_string(),
            &px.to_string(),
        ])
        .arg(&src)
        .arg("--out")
        .arg(&dst);
        if run_tool(&mut c).is_err() {
            return Ok(false);
        }
    }
    let mut c = Command::new("iconutil");
    c.args(["-c", "icns"]).arg(&set).arg("-o").arg(out);
    Ok(run_tool(&mut c).is_ok() && out.exists())
}

/// `xattr -cr`: removes Finder info / resource forks that `codesign` rejects as detritus.
pub fn clear_xattrs(app: &Path) {
    let _ = Command::new("xattr").arg("-cr").arg(app).output();
}

pub fn verify(app: &Path) -> Result<()> {
    let mut v = Command::new("codesign");
    v.args(["--verify", "--deep", "--strict"]).arg(app);
    run_tool(&mut v).map_err(|e| BuildError::new("sign", format!("codesign --verify failed: {e}")))
}

/// Entitlements of a generated Mac game app. The camera entitlement goes with `NSCameraUsageDescription` (the IR camera simulator).
/// There is deliberately NO `com.apple.security.app-sandbox`: the system data folders (`~/Library/Application Support/Napple/system`: Mii database,
/// amiibo store, fonts) and the user's amiibo/key files must be reachable by every generated app; a sandboxed app would see only its own container.
pub fn mac_entitlements() -> String {
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n\t<key>com.apple.security.device.audio-input</key>\n\t<true/>\n\t<key>com.apple.security.device.camera</key>\n\t<true/>\n</dict>\n</plist>\n".to_string()
}

pub fn codesign(app: &Path) -> Result<()> {
    let ent =
        std::env::temp_dir().join(format!("napple-entitlements-{}.plist", std::process::id()));
    std::fs::write(&ent, mac_entitlements())
        .map_err(|e| BuildError::io("write entitlements", e))?;
    let mut c = Command::new("codesign");
    c.args(["--force", "--deep", "-s", "-", "--entitlements"])
        .arg(&ent)
        .arg(app);
    let r = run_tool(&mut c);
    let _ = std::fs::remove_file(&ent);
    r.map_err(|e| BuildError::new("sign", format!("codesign failed: {e}")))?;
    verify(app)
}

/// `libnapple-applet.dylib` (the runtime as a library: runs the Mii Maker inside the game app): beside the runtime, in a sibling `build-lib/`
/// (a checkout's runtime/build/ and runtime/build-lib/), or in the checkout's runtime/build-lib. `make -C runtime applet-lib` builds it.
pub fn find_applet_lib(runtime: &Path) -> Option<PathBuf> {
    let name = "libnapple-applet.dylib";
    let mut cands = vec![runtime.with_file_name(name)];
    if let Some(dir) = runtime.parent() {
        cands.push(dir.join("../build-lib").join(name));
    }
    if let Some(root) = repo_root() {
        cands.push(root.join("runtime/build-lib").join(name));
    }
    cands.into_iter().find(|c| c.is_file())
}

/// Locates a prebuilt helper (runtime or launcher): explicit path, next to / relative to the installed `napple`
/// executable, or the repository build output when running from a checkout.
pub fn find_tool(explicit: Option<&Path>, name: &str, repo_rel: &str) -> Option<PathBuf> {
    if let Some(p) = explicit {
        return p.is_file().then(|| p.to_path_buf());
    }
    let exe = std::env::current_exe()
        .ok()
        .and_then(|e| e.canonicalize().ok())?;
    let dir = exe.parent()?;
    let mut cands = vec![
        dir.join(name),
        dir.join("../libexec/napple").join(name),
        dir.join("../share/napple").join(name),
    ];
    for a in exe.ancestors().chain(
        std::env::current_dir()
            .ok()
            .as_deref()
            .into_iter()
            .flat_map(|d| d.ancestors()),
    ) {
        if a.join("runtime/Makefile").is_file() && a.join("crates").is_dir() {
            cands.push(a.join(repo_rel));
        }
    }
    cands.into_iter().find(|c| c.is_file())
}

/// Repository root (a checkout containing runtime/Makefile) reachable from the executable or the working directory.
pub fn repo_root() -> Option<PathBuf> {
    let exe = std::env::current_exe()
        .ok()
        .and_then(|e| e.canonicalize().ok())?;
    let cwd = std::env::current_dir().ok();
    exe.ancestors()
        .chain(cwd.as_deref().into_iter().flat_map(|d| d.ancestors()))
        .find(|a| a.join("runtime/Makefile").is_file() && a.join("crates").is_dir())
        .map(|p| p.to_path_buf())
}

pub fn make_in(root: &Path, dir: &str) -> Result<()> {
    let mut c = Command::new("make");
    c.arg("-C").arg(root.join(dir));
    run_tool(&mut c).map_err(|e| BuildError::new("runtime", format!("make -C {dir} failed: {e}")))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn plist_has_required_keys_and_escapes() {
        let p = info_plist(0x0100000000010000, "Tom & <Jerry> \"X\"", "1.2.0", true);
        for k in [
            "com.napple.game.0100000000010000",
            "Tom &amp; &lt;Jerry&gt; &quot;X&quot;",
            "LSMinimumSystemVersion",
            "<string>26.0</string>",
            "NSHighResolutionCapable",
            "CFBundleIconFile",
            "napple-launcher",
            "<string>APPL</string>",
        ] {
            assert!(p.contains(k), "missing {k}");
        }
        assert!(!info_plist(1, "x", "1", false).contains("CFBundleIconFile"));
        assert!(info_plist(1, "x", "1", false).contains("NSCameraUsageDescription"));
        assert!(info_plist(1, "x", "1", false).contains("NSMicrophoneUsageDescription"));
        assert!(info_plist(1, "x", "1", false).contains("NSMicrophoneUsageDescription"));
        assert!(p.starts_with("<?xml") && p.trim_end().ends_with("</plist>"));
    }

    #[test]
    fn entitlements_have_camera_and_no_sandbox() {
        let e = mac_entitlements();
        assert!(e.contains("com.apple.security.device.camera"));
        assert!(e.contains("com.apple.security.device.audio-input"));
        // codesign (AMFI) rejects a repeated key
        assert_eq!(
            e.matches("<key>com.apple.security.device.audio-input</key>")
                .count(),
            1
        );
        assert_eq!(
            e.matches("<key>com.apple.security.device.camera</key>")
                .count(),
            1
        );
        assert!(
            !e.contains("app-sandbox"),
            "system data folders need an unsandboxed app"
        );
        assert!(e.starts_with("<?xml") && e.trim_end().ends_with("</plist>"));
    }

    #[test]
    fn applet_lib_is_found_beside_or_in_a_sibling_build_lib() {
        let d = std::env::temp_dir().join(format!("napple-applet-lib-{}", std::process::id()));
        std::fs::create_dir_all(d.join("build")).unwrap();
        std::fs::create_dir_all(d.join("build-lib")).unwrap();
        let rt = d.join("build/napple-runtime");
        assert!(find_applet_lib(&rt).is_none() || repo_root().is_some());
        std::fs::write(d.join("build-lib/libnapple-applet.dylib"), b"x").unwrap();
        assert!(find_applet_lib(&rt).is_some());
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn versions_are_sanitized() {
        assert_eq!(plist_version("1.3.0 (beta)"), "1.3.0beta");
        assert_eq!(plist_version("   "), "1.0");
        assert_eq!(xml_escape("a\u{0}b"), "ab");
    }

    #[test]
    fn find_tool_explicit_only_when_it_exists() {
        assert!(find_tool(Some(Path::new("/definitely/missing")), "x", "x").is_none());
        assert!(find_tool(Some(Path::new("/bin/ls")), "x", "x").is_some());
    }
}
