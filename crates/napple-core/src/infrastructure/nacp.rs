//! NACP (control.nacp) parser: title names, publishers and display version.
use crate::domain::error::{Result, container};

const ENTRY: usize = 0x300;
const LANGS: usize = 16;
const VERSION_OFF: usize = 0x3060;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TitleEntry {
    pub language: usize,
    pub name: String,
    pub publisher: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Nacp {
    pub titles: Vec<TitleEntry>,
    pub display_version: String,
}

impl Nacp {
    /// First non-empty title (American English is language 0, preferred when present).
    pub fn name(&self) -> Option<&str> {
        self.titles.first().map(|t| t.name.as_str())
    }
    pub fn publisher(&self) -> Option<&str> {
        self.titles.first().map(|t| t.publisher.as_str())
    }
}

fn cstr(b: &[u8]) -> String {
    let end = b.iter().position(|&c| c == 0).unwrap_or(b.len());
    String::from_utf8_lossy(&b[..end]).trim().to_string()
}

pub fn parse_nacp(d: &[u8]) -> Result<Nacp> {
    if d.len() < VERSION_OFF + 0x10 {
        return container("nacp too small");
    }
    let titles = (0..LANGS)
        .filter_map(|i| {
            let e = &d[i * ENTRY..(i + 1) * ENTRY];
            let name = cstr(&e[..0x200]);
            (!name.is_empty()).then(|| TitleEntry {
                language: i,
                name,
                publisher: cstr(&e[0x200..]),
            })
        })
        .collect();
    Ok(Nacp {
        titles,
        display_version: cstr(&d[VERSION_OFF..VERSION_OFF + 0x10]),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn blob() -> Vec<u8> {
        let mut d = vec![0u8; 0x4000];
        d[2 * ENTRY..2 * ENTRY + 5].copy_from_slice(b"Hello");
        d[2 * ENTRY + 0x200..2 * ENTRY + 0x203].copy_from_slice(b"Pub");
        d[VERSION_OFF..VERSION_OFF + 5].copy_from_slice(b"1.2.3");
        d
    }

    #[test]
    fn parses_first_non_empty_entry() {
        let n = parse_nacp(&blob()).unwrap();
        assert_eq!(
            (n.name(), n.publisher(), n.titles[0].language),
            (Some("Hello"), Some("Pub"), 2)
        );
        assert_eq!(n.display_version, "1.2.3");
    }

    #[test]
    fn empty_and_truncated() {
        assert!(parse_nacp(&vec![0u8; 0x4000]).unwrap().name().is_none());
        assert!(parse_nacp(&[0u8; 0x100]).is_err());
    }
}
