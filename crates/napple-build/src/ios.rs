//! iOS / iPadOS (26+) targets of `napple build`: bundle layout, Info.plist, module dylibs, signing, IPA export, device install.
//!
//! Layout of `<Name>.app` (iOS bundles are flat, there is no `Contents/`):
//! ```text
//! Info.plist  PkgInfo  embedded.mobileprovision  AppIcon60x60@2x.png ...
//! napple-runtime                      CFBundleExecutable: the runtime is the app (iOS forbids exec, so there is no launcher)
//! Frameworks/<buildid16>.<module>.dylib   rewritten modules as signed Mach-O (macho.rs): dyld maps the executable pages
//! game/                               same content as macOS Resources/game, minus the .patched.bin images (they live in the dylibs)
//! ```
//! Nothing here is JIT: all executable pages come from code-signed files. Signing needs an identity and a provisioning profile
//! (see `sign`); without them the build produces an unsigned app/IPA for an external signer.
use crate::error::{BuildError, Result};
use crate::json::{arr, jstr, obj};
use crate::macho;
use std::path::{Path, PathBuf};
use std::process::Command;

pub const MIN_IOS: &str = "26.0";
pub const RUNTIME_NAME: &str = "napple-runtime";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Target {
    Macos,
    /// iPhone and iPad
    Ios,
    Iphone,
    Ipad,
    /// iPhone and iPad Simulator on a Mac (smoke tests only): Simulator platform in the dylibs, ad-hoc signing, no provisioning profile.
    IosSim,
}

impl Target {
    pub fn parse(s: &str) -> Option<Target> {
        Some(match s {
            "macos" => Target::Macos,
            "ios" | "universal" => Target::Ios,
            "iphone" => Target::Iphone,
            "ipad" | "ipados" => Target::Ipad,
            "ios-sim" | "iossim" | "sim" | "simulator" => Target::IosSim,
            _ => return None,
        })
    }
    pub fn is_ios(self) -> bool {
        self != Target::Macos
    }
    pub fn is_sim(self) -> bool {
        self == Target::IosSim
    }
    /// LC_BUILD_VERSION platform of the module dylibs.
    pub fn platform(self) -> macho::Platform {
        if self.is_sim() {
            macho::Platform::IosSimulator
        } else {
            macho::Platform::Ios
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Target::Macos => "macos",
            Target::Ios => "ios",
            Target::Iphone => "iphone",
            Target::Ipad => "ipad",
            Target::IosSim => "ios-sim",
        }
    }
    /// UIDeviceFamily values: 1 iPhone, 2 iPad.
    pub fn families(self) -> &'static [u8] {
        match self {
            Target::Iphone => &[1],
            Target::Ipad => &[2],
            _ => &[1, 2],
        }
    }
}

/// Signing inputs from the command line.
#[derive(Clone, Debug, Default)]
pub struct Signing {
    /// `codesign -s` value: certificate name or SHA-1 (see `napple build --list-signing`).
    pub identity: Option<String>,
    /// `.mobileprovision` to embed (must cover the bundle id and the device).
    pub profile: Option<PathBuf>,
    /// Bundle id override (a free Apple ID needs the explicit id its profile was made for).
    pub bundle_id: Option<String>,
    /// Write `<Name>.ipa` next to the app.
    pub ipa: bool,
    /// Install on this device (`devicectl` identifier, UDID or name) after building.
    pub install: Option<String>,
}

// ---- plists ------------------------------------------------------------------------------------------------------------

fn esc(s: &str) -> String {
    crate::bundle::xml_escape(s)
}

pub fn bundle_id_for(title_id: u64, over: Option<&str>) -> String {
    over.map(str::to_string)
        .unwrap_or_else(|| format!("com.napple.game.{title_id:016x}"))
}

/// Valid reverse-DNS bundle identifier (letters, digits, `-`, `.`; at least one dot; no empty component).
pub fn valid_bundle_id(s: &str) -> bool {
    s.len() <= 155
        && s.contains('.')
        && s.split('.')
            .all(|p| !p.is_empty() && p.chars().all(|c| c.is_ascii_alphanumeric() || c == '-'))
}

pub fn info_plist(
    title_id: u64,
    bundle_id: &str,
    name: &str,
    version: &str,
    target: Target,
    icon_files: &[String],
) -> String {
    let v = crate::bundle::plist_version(version);
    let s = |x: &str| format!("<string>{}</string>", esc(x));
    let strs = |xs: &[&str]| {
        format!(
            "<array>{}</array>",
            xs.iter().map(|x| s(x)).collect::<String>()
        )
    };
    let mut kv: Vec<(&str, String)> = vec![
        ("CFBundleDevelopmentRegion", s("en")),
        ("CFBundleExecutable", s(RUNTIME_NAME)),
        ("CFBundleIdentifier", s(bundle_id)),
        ("CFBundleInfoDictionaryVersion", s("6.0")),
        ("CFBundleName", s(name)),
        ("CFBundleDisplayName", s(name)),
        ("CFBundlePackageType", s("APPL")),
        ("CFBundleShortVersionString", s(&v)),
        ("CFBundleVersion", s(&v)),
        ("CFBundleSupportedPlatforms", strs(&[if target.is_sim() { "iPhoneSimulator" } else { "iPhoneOS" }])),
        ("MinimumOSVersion", s(MIN_IOS)),
        ("UIDeviceFamily", format!("<array>{}</array>", target.families().iter().map(|f| format!("<integer>{f}</integer>")).collect::<String>())),
        ("UIRequiredDeviceCapabilities", strs(&["arm64", "metal"])),
        ("UISupportedInterfaceOrientations", strs(&["UIInterfaceOrientationLandscapeLeft", "UIInterfaceOrientationLandscapeRight"])),
        ("UISupportedInterfaceOrientations~ipad", strs(&["UIInterfaceOrientationLandscapeLeft", "UIInterfaceOrientationLandscapeRight"])),
        ("UIRequiresFullScreen", "<true/>".into()),
        ("UIStatusBarHidden", "<true/>".into()),
        ("UIViewControllerBasedStatusBarAppearance", "<false/>".into()),
        ("UIUserInterfaceStyle", s("Dark")),
        ("UILaunchScreen", "<dict/>".into()),
        // Saves and settings live in Documents; the Files app can show and back them up.
        ("UIFileSharingEnabled", "<true/>".into()),
        ("LSSupportsOpeningDocumentsInPlace", "<true/>".into()),
        ("GCSupportsControllerUserInteraction", "<true/>".into()),
        ("GCSupportsGameMode", "<true/>".into()),
        // 120 Hz on ProMotion iPhones (iPads get it by default)
        ("CADisableMinimumFrameDurationOnPhone", "<true/>".into()),
        ("LSApplicationCategoryType", s("public.app-category.games")),
        ("NappleTitleId", s(&format!("{title_id:016X}"))),
        ("NappleTarget", s(target.name())),
        // UIScene life cycle (required by the iOS releases after 26): the delegate class lives in the runtime (runtime/src/app_ios.m).
        (
            "UIApplicationSceneManifest",
            "<dict><key>UIApplicationSupportsMultipleScenes</key><false/><key>UISceneConfigurations</key><dict><key>UIWindowSceneSessionRoleApplication</key><array><dict><key>UISceneConfigurationName</key><string>Default</string><key>UISceneDelegateClassName</key><string>NappleSceneDelegate</string></dict></array></dict></dict>".into(),
        ),
    ];
    if !icon_files.is_empty() {
        let files: String = icon_files.iter().map(|f| s(f)).collect();
        kv.push(("CFBundleIcons", format!("<dict><key>CFBundlePrimaryIcon</key><dict><key>CFBundleIconFiles</key><array>{files}</array></dict></dict>")));
    }
    let body: String = kv
        .iter()
        .map(|(k, v)| format!("\t<key>{k}</key>\n\t{v}\n"))
        .collect();
    format!(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n{body}</dict>\n</plist>\n"
    )
}

/// Icon sizes written from the game's JPEG (name, pixels): the legacy loose-PNG set iOS still reads without an asset catalog.
pub const ICON_SIZES: &[(&str, u32)] = &[
    ("AppIcon60x60@2x.png", 120),
    ("AppIcon60x60@3x.png", 180),
    ("AppIcon76x76@2x~ipad.png", 152),
    ("AppIcon83.5x83.5@2x~ipad.png", 167),
];

pub fn make_icons(jpeg: &[u8], work: &Path, app: &Path) -> Result<Vec<String>> {
    std::fs::create_dir_all(work).map_err(|e| BuildError::io("create icon dir", e))?;
    let src = work.join("icon.jpg");
    std::fs::write(&src, jpeg).map_err(|e| BuildError::io("write icon", e))?;
    let mut names = vec![];
    for (name, px) in ICON_SIZES {
        let out = Command::new("sips")
            .args([
                "-s",
                "format",
                "png",
                "-z",
                &px.to_string(),
                &px.to_string(),
            ])
            .arg(&src)
            .arg("--out")
            .arg(app.join(name))
            .output();
        if !matches!(out, Ok(ref o) if o.status.success()) {
            for n in &names {
                let _ = std::fs::remove_file(app.join(n));
            }
            return Ok(vec![]);
        }
        // CFBundleIconFiles lists the base name without scale/idiom suffix
        names.push(name.to_string());
    }
    Ok(vec![
        "AppIcon60x60".into(),
        "AppIcon76x76".into(),
        "AppIcon83.5x83.5".into(),
    ])
}

// ---- module dylibs ------------------------------------------------------------------------------------------------------

/// Replaces every `<ver>/modules/<m>.patched.bin` and `<ver>/nro/<id>.patched.bin` under `game` with a dylib in `frameworks`
/// (`<ver>.<m>.dylib`, `<ver>.nro.<id>.dylib`; the runtime derives the same names, see runtime/src/module.c). Returns the dylib count.
pub fn dylibize(game: &Path, frameworks: &Path, platform: macho::Platform) -> Result<usize> {
    std::fs::create_dir_all(frameworks).map_err(|e| BuildError::io("create Frameworks", e))?;
    let mut n = 0;
    for ver in std::fs::read_dir(game)
        .map_err(|e| BuildError::io("read game dir", e))?
        .flatten()
    {
        if !ver.path().join("manifest.json").is_file() {
            continue;
        }
        let vname = ver.file_name().to_string_lossy().to_string();
        for (sub, tag) in [("modules", ""), ("nro", "nro.")] {
            let dir = ver.path().join(sub);
            let Ok(rd) = std::fs::read_dir(&dir) else {
                continue;
            };
            for f in rd.flatten() {
                let p = f.path();
                let Some(stem) = p
                    .file_name()
                    .and_then(|s| s.to_str())
                    .and_then(|s| s.strip_suffix(".patched.bin"))
                    .map(str::to_string)
                else {
                    continue;
                };
                let image =
                    std::fs::read(&p).map_err(|e| BuildError::io("read module image", e))?;
                let tables = std::fs::read(dir.join(format!("{stem}.tables")))
                    .map_err(|e| BuildError::io(&format!("read {stem}.tables"), e))?;
                if tables.starts_with(b"NTB2") {
                    continue; // AArch32 title (interpreter): the image is read as data, no executable pages to wrap
                }
                let layout = macho::layout_from_ntb1(&tables)?;
                let name = format!("{vname}.{tag}{stem}.dylib");
                let dylib =
                    macho::build_dylib(&image, &layout, &format!("@rpath/{name}"), platform)?;
                std::fs::write(frameworks.join(&name), dylib)
                    .map_err(|e| BuildError::io("write module dylib", e))?;
                std::fs::remove_file(&p).map_err(|e| BuildError::io("remove module image", e))?;
                n += 1;
            }
        }
    }
    Ok(n)
}

// ---- provisioning profiles ------------------------------------------------------------------------------------------------

/// Does a profile's `application-identifier` (`TEAMID.com.example.app` or `TEAMID.*` / `TEAMID.com.example.*`) cover this bundle id?
pub fn profile_covers(app_identifier: &str, bundle_id: &str) -> bool {
    let Some((_, pat)) = app_identifier.split_once('.') else {
        return false;
    };
    match pat.strip_suffix('*') {
        Some(prefix) => bundle_id.starts_with(prefix),
        None => pat == bundle_id,
    }
}

fn run_out(c: &mut Command) -> std::result::Result<String, String> {
    match c.output() {
        Ok(o) if o.status.success() => Ok(String::from_utf8_lossy(&o.stdout).to_string()),
        Ok(o) => Err(String::from_utf8_lossy(&o.stderr).trim().to_string()),
        Err(e) => Err(e.to_string()),
    }
}

/// Decodes a `.mobileprovision` (CMS) to a plist file with `security cms -D`.
fn decode_profile(profile: &Path, work: &Path) -> Result<PathBuf> {
    let out = work.join("profile.plist");
    let xml = run_out(
        Command::new("security")
            .args(["cms", "-D", "-i"])
            .arg(profile),
    )
    .map_err(|e| {
        BuildError::new(
            "sign",
            format!(
                "cannot read the provisioning profile {}: {e}",
                profile.display()
            ),
        )
    })?;
    std::fs::write(&out, xml).map_err(|e| BuildError::io("write decoded profile", e))?;
    Ok(out)
}

fn plutil_raw(plist: &Path, key: &str) -> Option<String> {
    run_out(
        Command::new("plutil")
            .args(["-extract", key, "raw", "-o", "-"])
            .arg(plist),
    )
    .ok()
    .map(|s| s.trim().to_string())
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProfileInfo {
    pub path: PathBuf,
    pub name: String,
    pub team: String,
    pub app_id: String,
    pub expires: String,
    pub devices: u32,
    pub platform: String,
}

fn profile_info(path: &Path, work: &Path) -> Option<ProfileInfo> {
    let plist = decode_profile(path, work).ok()?;
    Some(ProfileInfo {
        path: path.to_path_buf(),
        name: plutil_raw(&plist, "Name")?,
        team: plutil_raw(&plist, "TeamIdentifier.0")?,
        app_id: plutil_raw(&plist, "Entitlements.application-identifier")?,
        expires: plutil_raw(&plist, "ExpirationDate").unwrap_or_default(),
        devices: plutil_raw(&plist, "ProvisionedDevices")
            .and_then(|s| s.parse().ok())
            .unwrap_or(0),
        platform: plutil_raw(&plist, "Platform.0").unwrap_or_default(),
    })
}

/// `napple build --list-signing [--json]`: code-signing identities and the iOS provisioning profiles Xcode keeps on this Mac.
pub fn list_signing(json: bool) -> Result<()> {
    let idents =
        run_out(Command::new("security").args(["find-identity", "-v", "-p", "codesigning"]))
            .unwrap_or_default();
    let identities: Vec<(String, String)> = idents
        .lines()
        .filter_map(|l| {
            let l = l.trim();
            let rest = l.split_once(") ")?.1;
            let (sha, name) = rest.split_once(' ')?;
            (sha.len() == 40).then(|| (sha.to_string(), name.trim().trim_matches('"').to_string()))
        })
        .collect();
    let work = std::env::temp_dir().join(format!("napple-sign-{}", std::process::id()));
    std::fs::create_dir_all(&work).map_err(|e| BuildError::io("temp dir", e))?;
    let mut profiles = vec![];
    for dir in profile_dirs() {
        if let Ok(rd) = std::fs::read_dir(&dir) {
            for f in rd.flatten() {
                if f.path().extension().and_then(|e| e.to_str()) == Some("mobileprovision")
                    && let Some(p) = profile_info(&f.path(), &work)
                    && (p.platform == "iOS" || p.platform.is_empty())
                {
                    profiles.push(p);
                }
            }
        }
    }
    let _ = std::fs::remove_dir_all(&work);
    if json {
        let ids: Vec<String> = identities
            .iter()
            .map(|(sha, name)| obj(&[("sha1", jstr(sha)), ("name", jstr(name))], false))
            .collect();
        let pr: Vec<String> = profiles
            .iter()
            .map(|p| {
                obj(
                    &[
                        ("path", jstr(&p.path.to_string_lossy())),
                        ("name", jstr(&p.name)),
                        ("team", jstr(&p.team)),
                        ("app_id", jstr(&p.app_id)),
                        ("expires", jstr(&p.expires)),
                        ("devices", p.devices.to_string()),
                    ],
                    false,
                )
            })
            .collect();
        println!(
            "{}",
            obj(&[("identities", arr(&ids)), ("profiles", arr(&pr))], false)
        );
    } else {
        for (sha, name) in &identities {
            println!("identity {sha} {name}");
        }
        for p in &profiles {
            println!(
                "profile  {} app-id {} team {} devices {} expires {} ({})",
                p.name,
                p.app_id,
                p.team,
                p.devices,
                p.expires,
                p.path.display()
            );
        }
        if identities.is_empty() {
            println!(
                "no code-signing identities found: open Xcode > Settings > Accounts, add your Apple ID and create an Apple Development certificate"
            );
        }
    }
    Ok(())
}

pub fn profile_dirs() -> Vec<PathBuf> {
    let home = std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_default();
    vec![
        home.join("Library/Developer/Xcode/UserData/Provisioning Profiles"),
        home.join("Library/MobileDevice/Provisioning Profiles"),
    ]
}

// ---- signing --------------------------------------------------------------------------------------------------------------

fn codesign(args: &[&str], path: &Path) -> Result<()> {
    run_out(Command::new("codesign").args(args).arg(path))
        .map(|_| ())
        .map_err(|e| {
            BuildError::new(
                "sign",
                format!(
                    "codesign {}: {e}",
                    path.file_name()
                        .map(|n| n.to_string_lossy())
                        .unwrap_or_default()
                ),
            )
        })
}

/// Embeds the profile, signs every module dylib, then the app with the profile's own entitlements (what Xcode does), then verifies.
/// Returns notes for the build report.
pub fn sign(app: &Path, work: &Path, s: &Signing, bundle_id: &str) -> Result<Vec<String>> {
    let (Some(identity), Some(profile)) = (&s.identity, &s.profile) else {
        return Err(BuildError::new(
            "sign",
            "iOS signing needs --signing-identity and --provisioning-profile (see `napple build --list-signing`), or pass --no-sign to build an unsigned app/IPA for an external signer",
        ));
    };
    let mut notes = vec![];
    let plist = decode_profile(profile, work)?;
    let app_id = plutil_raw(&plist, "Entitlements.application-identifier").ok_or_else(|| {
        BuildError::new(
            "sign",
            "the provisioning profile has no application-identifier",
        )
    })?;
    if !profile_covers(&app_id, bundle_id) {
        return Err(BuildError::new(
            "sign",
            format!(
                "the profile is for {app_id}, not for the bundle id {bundle_id}; pass --bundle-id with the id the profile was made for"
            ),
        ));
    }
    if let Some(exp) = plutil_raw(&plist, "ExpirationDate") {
        notes.push(format!("provisioning profile {:?} expires {exp}; free Apple ID profiles last 7 days, then the app stops launching until it is rebuilt/re-signed", plutil_raw(&plist, "Name").unwrap_or_default()));
    }
    let ent = work.join("entitlements.plist");
    let xml = run_out(
        Command::new("plutil")
            .args(["-extract", "Entitlements", "xml1", "-o", "-"])
            .arg(&plist),
    )
    .map_err(|e| {
        BuildError::new(
            "sign",
            format!("cannot read the profile's entitlements: {e}"),
        )
    })?;
    std::fs::write(&ent, xml).map_err(|e| BuildError::io("write entitlements", e))?;
    if !std::fs::read_to_string(&ent)
        .unwrap_or_default()
        .contains("increased-memory-limit")
    {
        notes.push("the profile does not grant com.apple.developer.kernel.increased-memory-limit (paid account only): the app runs with the default iOS memory limit, large games may be terminated".into());
    }
    std::fs::copy(profile, app.join("embedded.mobileprovision"))
        .map_err(|e| BuildError::io("embed the profile", e))?;
    crate::bundle::clear_xattrs(app);
    if let Ok(rd) = std::fs::read_dir(app.join("Frameworks")) {
        for f in rd.flatten() {
            codesign(
                &[
                    "--force",
                    "--sign",
                    identity,
                    "--timestamp=none",
                    "--generate-entitlement-der",
                ],
                &f.path(),
            )?;
        }
    }
    let ent_s = ent.to_string_lossy().to_string();
    codesign(
        &[
            "--force",
            "--sign",
            identity,
            "--timestamp=none",
            "--generate-entitlement-der",
            "--entitlements",
            &ent_s,
        ],
        app,
    )?;
    codesign(&["--verify", "--deep", "--strict"], app)?;
    Ok(notes)
}

/// Simulator signing: ad-hoc (`-s -`) on every module dylib, then the app. No profile, no entitlements: the Simulator
/// runs the arm64 code as a Mac process and only needs a valid (even ad-hoc) signature.
pub fn sign_sim(app: &Path) -> Result<Vec<String>> {
    crate::bundle::clear_xattrs(app);
    if let Ok(rd) = std::fs::read_dir(app.join("Frameworks")) {
        for f in rd.flatten() {
            codesign(&["--force", "--sign", "-", "--timestamp=none"], &f.path())?;
        }
    }
    codesign(&["--force", "--sign", "-", "--timestamp=none"], app)?;
    codesign(&["--verify", "--deep", "--strict"], app)?;
    Ok(vec!["ios-sim build: ad-hoc signed for the iOS Simulator only; it will not install on a device (use --target ios)".into()])
}

// ---- IPA and install ----------------------------------------------------------------------------------------------------------

/// `ditto -c -k --keepParent <payload_dir>`: payload_dir must be a directory named `Payload` containing the .app (ditto writes zip64, so multi-GB RomFS images work).
pub fn export_ipa(payload_dir: &Path, ipa: &Path) -> Result<()> {
    let _ = std::fs::remove_file(ipa);
    run_out(
        Command::new("ditto")
            .args([
                "-c",
                "-k",
                "--keepParent",
                "--norsrc",
                "--noextattr",
                "--noacl",
            ])
            .arg(payload_dir)
            .arg(ipa),
    )
    .map(|_| ())
    .map_err(|e| BuildError::new("ipa", format!("ditto failed: {e}")))
}

/// Installs (and optionally launches) with Xcode's `devicectl`.
pub fn install(device: &str, app: &Path, bundle_id: &str, launch: bool) -> Result<()> {
    run_out(Command::new("xcrun").args(["devicectl", "device", "install", "app", "--device", device]).arg(app))
        .map_err(|e| BuildError::new("install", format!("devicectl install failed: {e}. Check the device is unlocked, trusted, in Developer Mode (Settings > Privacy & Security) and listed by `xcrun devicectl list devices`; for a free Apple ID also trust the developer under Settings > General > VPN & Device Management")))?;
    if launch {
        run_out(Command::new("xcrun").args(["devicectl", "device", "process", "launch", "--device", device, bundle_id])).map_err(|e| BuildError::new("install", format!("installed, but launching failed: {e} (trust the developer in Settings > General > VPN & Device Management, then open the app)")))?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn targets() {
        assert_eq!(Target::parse("ipad"), Some(Target::Ipad));
        assert_eq!(Target::parse("ios").unwrap().families(), &[1, 2]);
        assert_eq!(Target::parse("iphone").unwrap().families(), &[1]);
        assert!(
            Target::parse("tvos").is_none() && !Target::Macos.is_ios() && Target::Iphone.is_ios()
        );
    }

    #[test]
    fn simulator_target() {
        for n in ["ios-sim", "sim", "simulator"] {
            assert_eq!(Target::parse(n), Some(Target::IosSim));
        }
        let t = Target::IosSim;
        assert!(t.is_ios() && t.is_sim() && !Target::Ios.is_sim());
        assert_eq!(
            (t.platform(), Target::Ios.platform()),
            (macho::Platform::IosSimulator, macho::Platform::Ios)
        );
        assert_eq!(t.families(), &[1, 2]);
        let p = info_plist(1, "a.b", "x", "1", t, &[]);
        assert!(
            p.contains("iPhoneSimulator")
                && !p.contains("<string>iPhoneOS</string>")
                && p.contains("<string>ios-sim</string>")
        );
        assert!(
            info_plist(1, "a.b", "x", "1", Target::Ios, &[]).contains("<string>iPhoneOS</string>")
        );
    }

    #[test]
    fn plist_is_a_flat_ios_26_bundle() {
        let p = info_plist(
            0x0100000000010000,
            "com.me.napple",
            "Tom & Jerry",
            "1.0",
            Target::Ios,
            &["AppIcon60x60".into()],
        );
        for k in [
            "<string>napple-runtime</string>",
            "MinimumOSVersion",
            "<string>26.0</string>",
            "UIDeviceFamily",
            "<integer>2</integer>",
            "UIRequiredDeviceCapabilities",
            "metal",
            "UIApplicationSceneManifest",
            "NappleSceneDelegate",
            "UIFileSharingEnabled",
            "CFBundleIconFiles",
            "AppIcon60x60",
            "Tom &amp; Jerry",
            "com.me.napple",
            "UIInterfaceOrientationLandscapeLeft",
            "CADisableMinimumFrameDurationOnPhone",
        ] {
            assert!(p.contains(k), "missing {k}");
        }
        assert!(!p.contains("LSMinimumSystemVersion") && !p.contains("NSPrincipalClass"));
        assert!(!info_plist(1, "a.b", "x", "1", Target::Iphone, &[]).contains("CFBundleIcons"));
        assert!(
            !info_plist(1, "a.b", "x", "1", Target::Iphone, &[]).contains("<integer>2</integer>")
        );
    }

    #[test]
    fn bundle_ids() {
        assert_eq!(
            bundle_id_for(0x0100000000010000, None),
            "com.napple.game.0100000000010000"
        );
        assert_eq!(bundle_id_for(1, Some("com.me.x")), "com.me.x");
        assert!(
            valid_bundle_id("com.me.napple-1")
                && !valid_bundle_id("nodots")
                && !valid_bundle_id("a..b")
                && !valid_bundle_id("a.b_c")
                && !valid_bundle_id("a.b/c")
        );
    }

    #[test]
    fn profile_matching() {
        assert!(profile_covers("ABCDE12345.com.me.napple", "com.me.napple"));
        assert!(!profile_covers("ABCDE12345.com.me.napple", "com.me.other"));
        assert!(profile_covers("ABCDE12345.*", "anything.at.all"));
        assert!(profile_covers("ABCDE12345.com.me.*", "com.me.game"));
        assert!(!profile_covers("ABCDE12345.com.me.*", "com.you.game"));
        assert!(!profile_covers("garbage", "a.b"));
    }

    #[test]
    fn dylibize_converts_modules_and_nros() {
        let dir = std::env::temp_dir().join(format!("napple-ios-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let (game, fw) = (dir.join("game"), dir.join("Frameworks"));
        let ver = game.join("0123456789abcdef");
        std::fs::create_dir_all(ver.join("modules")).unwrap();
        std::fs::create_dir_all(ver.join("nro")).unwrap();
        std::fs::write(ver.join("manifest.json"), "{}").unwrap();
        let (page, size) = (macho::PAGE as usize, 5 * macho::PAGE as usize + 64);
        let img = vec![7u8; size];
        // NTB1 header order: file_size, text_end, ro_off, data_off, patch_off, key_off
        let mut t = b"NTB1".to_vec();
        t.resize(24, 0);
        for v in [
            size as u64,
            0x3f00,
            0x8000,
            0x8080,
            5 * page as u64,
            4 * page as u64,
        ] {
            t.extend_from_slice(&v.to_le_bytes());
        }
        for (sub, stem) in [("modules", "main"), ("nro", "abcd")] {
            std::fs::write(ver.join(sub).join(format!("{stem}.patched.bin")), &img).unwrap();
            std::fs::write(ver.join(sub).join(format!("{stem}.tables")), &t).unwrap();
        }
        assert_eq!(dylibize(&game, &fw, macho::Platform::Ios).unwrap(), 2);
        assert!(
            fw.join("0123456789abcdef.main.dylib").is_file()
                && fw.join("0123456789abcdef.nro.abcd.dylib").is_file()
        );
        assert!(
            !ver.join("modules/main.patched.bin").exists(),
            "image must live in the dylib only"
        );
        assert!(ver.join("modules/main.tables").exists());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
