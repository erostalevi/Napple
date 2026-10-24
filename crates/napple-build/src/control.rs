//! Control NCA: application name, display version and the icon (JPEG in the control RomFS).
use crate::container::Container;
use crate::error::Result;
use napple_core::application::package::Pack;
use napple_core::domain::error::Error as CoreError;
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::io::read_vec;
use napple_core::infrastructure::nacp::{parse_nacp, Nacp};
use napple_core::infrastructure::nca::{self, FsType, Nca};
use napple_core::infrastructure::romfs::RomFs;

pub struct Control {
    pub nacp: Option<Nacp>,
    /// JPEG bytes of the best available icon (American English first).
    pub icon: Option<Vec<u8>>,
}

const MAX_ICON: u64 = 4 << 20;

/// Pure selection rule: `icon_AmericanEnglish.dat`, else the first `icon_*.dat` in path order.
pub fn pick_icon<'a>(names: impl Iterator<Item = &'a str>) -> Option<&'a str> {
    let mut v: Vec<&str> = names.filter(|n| n.starts_with("icon_") && n.ends_with(".dat")).collect();
    v.sort_unstable();
    v.iter().copied().find(|n| *n == "icon_AmericanEnglish.dat").or(v.first().copied())
}

pub fn read_control(cont: &Container, pack: &Pack, keys: &KeySet) -> Result<Option<Control>> {
    let Some(rec) = pack.meta.contents.iter().find(|c| c.ctype == 3) else { return Ok(None) };
    let Some(src) = cont.ncas.get(&rec.nca_name()) else { return Ok(None) };
    let n = match Nca::open(src.clone(), keys, &cont.tks) {
        Ok(n) => n,
        Err(CoreError::Unsupported(_)) => return Ok(None),
        Err(e) => return Err(e.into()),
    };
    for sec in n.sections.iter().filter(|s| s.fs_type == FsType::RomFs) {
        let Ok(s) = n.section(sec.index) else { continue };
        let fs = RomFs::parse(nca::romfs_data(&s, sec)?)?;
        let read = |path: &str, limit: u64| -> Result<Option<Vec<u8>>> {
            let Some(f) = fs.files.iter().find(|f| f.path == path) else { return Ok(None) };
            if f.size > limit {
                return Ok(None);
            }
            Ok(Some(read_vec(&*fs.open(f)?, 0, f.size as usize)?))
        };
        let nacp = match read("control.nacp", 0x10000)? {
            Some(b) => parse_nacp(&b).ok(),
            None => None,
        };
        let icon = match pick_icon(fs.files.iter().map(|f| f.path.as_str())) {
            Some(p) => read(p, MAX_ICON)?.filter(|b| b.starts_with(&[0xFF, 0xD8])),
            None => None,
        };
        return Ok(Some(Control { nacp, icon }));
    }
    Ok(None)
}

/// Prefers American English (language 0) when its title exists.
pub fn app_name(n: &Nacp) -> Option<String> {
    n.titles.iter().find(|t| t.language == 0).or(n.titles.first()).map(|t| t.name.clone())
}

#[cfg(test)]
mod tests {
    use super::*;
    use napple_core::infrastructure::nacp::TitleEntry;

    #[test]
    fn icon_selection() {
        assert_eq!(pick_icon(["control.nacp", "icon_Japanese.dat", "icon_AmericanEnglish.dat"].into_iter()), Some("icon_AmericanEnglish.dat"));
        assert_eq!(pick_icon(["icon_German.dat", "icon_French.dat"].into_iter()), Some("icon_French.dat"));
        assert_eq!(pick_icon(["control.nacp"].into_iter()), None);
    }

    #[test]
    fn prefers_american_english_name() {
        let t = |language, name: &str| TitleEntry { language, name: name.into(), publisher: String::new() };
        let n = Nacp { titles: vec![t(2, "Español"), t(0, "English")], display_version: "1".into() };
        assert_eq!(app_name(&n).as_deref(), Some("English"));
        let n = Nacp { titles: vec![t(2, "Español")], display_version: "1".into() };
        assert_eq!(app_name(&n).as_deref(), Some("Español"));
    }
}
