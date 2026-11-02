//! Copy of `crates/napple-account/src/names.rs` (reserved-name rules of orchestrations/ACCOUNT-CONTRACT.md, "Reserved names"), kept
//! identical apart from this header and `check_text` at the end, so napple-social builds and deploys without the account crate.
//! Change both copies together.
//!
//! Reserved names, for every user-chosen name other people see (account `display_name`, profile `nickname`, and
//! later handles and room names: `check_name` is the one function to call).
//!
//! - "napple" may not appear anywhere in a name, after normalising look-alikes, except the exact name `napple`
//!   (any case) on an account holding the `owner_napple` grant.
//! - A name starting with `nap` + one of `-` `_` ` ` `.` (look-alikes included, e.g. `n4p-`) needs `reserved_prefix`.
//!
//! Normalisation: NFKD, drop combining marks (accents), map confusables (Cyrillic/Greek/Latin look-alikes, dash
//! variants; fullwidth forms fold in NFKD) and leetspeak (0→o, 1/l/|/I→l, 4/@→a, 3→e, 5/$→s, 7→t, 9→g), lower-case.
//! The "contains" check then drops everything that is not a letter or digit. Invisible format characters
//! (zero-width spaces, bidi controls, soft hyphen) are refused outright: they only serve to hide one of these words.

use unicode_normalization::UnicodeNormalization;
use unicode_normalization::char::is_combining_mark;

pub const GRANT_OWNER_NAPPLE: &str = "owner_napple";
pub const GRANT_RESERVED_PREFIX: &str = "reserved_prefix";
pub const GRANT_ADMIN: &str = "admin";
pub const GRANTS: &[&str] = &[GRANT_OWNER_NAPPLE, GRANT_RESERVED_PREFIX, GRANT_ADMIN];

#[derive(Debug, PartialEq, Eq)]
pub enum NameError {
    /// Empty, too long, or control characters.
    Invalid,
    /// Invisible format characters.
    Invisible,
    /// Contains "napple".
    ReservedWord,
    /// Starts with the reserved `nap-` prefix.
    ReservedPrefix,
}

impl NameError {
    pub fn message(&self) -> &'static str {
        match self {
            NameError::Invalid => "names are 1 to 32 characters without control characters",
            NameError::Invisible => "names cannot contain invisible characters",
            NameError::ReservedWord => "names cannot contain \"Napple\"",
            NameError::ReservedPrefix => {
                "names starting with \"nap-\" are reserved for Napple contributors"
            }
        }
    }
}

/// Invisible/format characters refused in names (U+200D ZWJ and variation selectors stay allowed for emoji).
fn is_invisible(c: char) -> bool {
    matches!(c,
        '\u{00AD}' | '\u{034F}' | '\u{061C}' | '\u{115F}' | '\u{1160}' | '\u{17B4}' | '\u{17B5}' | '\u{180E}'
        | '\u{200B}' | '\u{200C}' | '\u{200E}' | '\u{200F}' | '\u{202A}'..='\u{202E}' | '\u{2060}'..='\u{2064}'
        | '\u{2066}'..='\u{206F}' | '\u{3164}' | '\u{FEFF}' | '\u{FFA0}' | '\u{FFF9}'..='\u{FFFB}'
        | '\u{1D173}'..='\u{1D17A}' | '\u{E0000}'..='\u{E007F}')
}

/// One already-decomposed, accent-free character -> its skeleton (possibly several chars), or itself.
fn skeleton_char(c: char, out: &mut String) {
    let m = match c {
        // Uppercase I is the classic l look-alike; map it before lower-casing turns it into i.
        'I' | '1' | '|' | 'ǀ' | 'ı' | 'ℓ' | 'Ɩ' | 'ⅼ' | 'Ι' | 'І' | 'Ӏ' | 'ӏ' | 'ן' | 'ا' | '│'
        | '∣' | '❘' => 'l',
        '0' | 'О' | 'о' | 'Ο' | 'ο' | 'σ' | 'Օ' | 'օ' | 'ჿ' | '〇' | 'ᴏ' => 'o',
        '4' | '@' | 'А' | 'а' | 'Α' | 'α' | 'ɑ' | 'ᴀ' => 'a',
        '3' | 'Е' | 'е' | 'Ε' | 'ε' | 'ɛ' | 'ᴇ' => 'e',
        '5' | '$' | 'Ѕ' | 'ѕ' | 'ꜱ' => 's',
        '7' | 'Т' | 'т' | 'Τ' | 'τ' | 'ᴛ' => 't',
        '9' | 'ɡ' | 'ց' => 'g',
        'Р' | 'р' | 'Ρ' | 'ρ' | 'ᴘ' | 'þ' => 'p',
        'Ν' | 'Ո' | 'ո' | 'ᴎ' | 'ɴ' | 'η' | 'п' => 'n',
        'С' | 'с' | 'Ϲ' | 'ϲ' | 'ᴄ' => 'c',
        'Х' | 'х' | 'Χ' | 'χ' => 'x',
        'У' | 'у' | 'Υ' | 'γ' | 'ү' => 'y',
        'К' | 'к' | 'Κ' | 'κ' | 'ᴋ' => 'k',
        'М' | 'м' | 'Μ' | 'ᴍ' => 'm',
        'Н' | 'н' | 'Η' | 'ʜ' => 'h',
        'В' | 'в' | 'Β' | 'β' | 'ʙ' => 'b',
        'і' | 'ι' | 'ɩ' => 'i',
        'ј' | 'ϳ' => 'j',
        'ԁ' => 'd',
        'ѵ' | 'ν' => 'v',
        'ѡ' | 'ω' | 'ɯ' => 'w',
        'Ζ' => 'z',
        // Dash look-alikes all count as '-', so the prefix rule cannot be dodged with an en dash.
        '‐' | '‑' | '‒' | '–' | '—' | '―' | '−' | '⁃' | '﹘' | '﹣' | '－' | '˗' => {
            '-'
        }
        '＿' | '‗' => '_',
        '·' | '•' | '․' | '‧' | '∙' | '。' | '｡' => '.',
        _ => {
            out.extend(c.to_lowercase());
            return;
        }
    };
    out.push(m);
}

/// The look-alike skeleton of a name, punctuation kept (for the prefix rule).
pub fn skeleton(name: &str) -> String {
    let mut out = String::with_capacity(name.len());
    for c in name.nfkd().filter(|c| !is_combining_mark(*c)) {
        skeleton_char(c, &mut out);
    }
    // NFKD of the mapped result folds anything the table produced in compatibility form.
    out.nfkc().collect()
}

/// Letters and digits of the skeleton only (for the "contains" rule).
pub fn compact(name: &str) -> String {
    skeleton(name)
        .chars()
        .filter(|c| c.is_alphanumeric())
        .collect()
}

/// Validates a user-visible name against the reserved-name rules for an account holding `grants`.
/// Returns the trimmed name to store.
pub fn check_name(name: &str, max_chars: usize, grants: &[String]) -> Result<String, NameError> {
    let t = name.trim();
    let n = t.chars().count();
    if n == 0 || n > max_chars || t.chars().any(char::is_control) {
        return Err(NameError::Invalid);
    }
    if t.chars().any(is_invisible) {
        return Err(NameError::Invisible);
    }
    let has = |g: &str| grants.iter().any(|x| x == g);
    if compact(t).contains("napple") {
        let exact = t.eq_ignore_ascii_case("napple");
        if !(exact && has(GRANT_OWNER_NAPPLE)) {
            return Err(NameError::ReservedWord);
        }
    }
    let sk = skeleton(t);
    let sk = sk.trim_start();
    if sk.starts_with("nap")
        && sk[3..].starts_with(['-', '_', ' ', '.'])
        && !has(GRANT_RESERVED_PREFIX)
    {
        return Err(NameError::ReservedPrefix);
    }
    Ok(t.to_string())
}

/// napple-social: free text other players see that is not a name (an invite message). Only the "napple" word and invisible
/// characters are refused here; the `nap-` prefix rule is for names and labels (`check_name`). Length and control
/// characters are checked by the caller.
pub fn check_text(text: &str) -> Result<(), NameError> {
    if text.chars().any(is_invisible) {
        return Err(NameError::Invisible);
    }
    if compact(text).contains("napple") {
        return Err(NameError::ReservedWord);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn none() -> Vec<String> {
        vec![]
    }
    fn g(x: &[&str]) -> Vec<String> {
        x.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn napple_anywhere_is_refused() {
        for n in [
            "Napple",
            "napple",
            "NAPPLE",
            "xNAPPLEx",
            "n4pple",
            "nappIe",
            "napp1e",
            "na pple",
            "n.a.p.p.l.e",
            "n-a_p p.l e",
            "N@PPLE",
            "nаpple",       // Cyrillic а
            "napplе",       // Cyrillic е
            "ｎａｐｐｌｅ", // fullwidth
            "nápplé",       // accents
            "ⓝⓐⓟⓟⓛⓔ",       // circled (NFKD)
            "𝐧𝐚𝐩𝐩𝐥𝐞",       // math bold
            "NΑPPLE",       // Greek Alpha
            "my napple fan club",
            "TheNapple",
            "n4ppl3",
            "NAPP|E",
            "nap\u{0301}ple",
        ] {
            assert_eq!(
                check_name(n, 32, &none()),
                Err(NameError::ReservedWord),
                "{n:?}"
            );
        }
    }

    #[test]
    fn exact_napple_only_for_the_owner() {
        let owner = g(&[GRANT_OWNER_NAPPLE]);
        assert_eq!(check_name("napple", 32, &owner).as_deref(), Ok("napple"));
        assert_eq!(check_name(" Napple ", 32, &owner).as_deref(), Ok("Napple"));
        // Not exact: still refused for the owner.
        for n in ["napple2", "n4pple", "Napple Team", "nаpple"] {
            assert_eq!(
                check_name(n, 32, &owner),
                Err(NameError::ReservedWord),
                "{n:?}"
            );
        }
        assert_eq!(
            check_name("napple", 32, &g(&[GRANT_RESERVED_PREFIX])),
            Err(NameError::ReservedWord)
        );
    }

    #[test]
    fn nap_prefix_needs_the_grant() {
        for n in [
            "nap-neto",
            "NAP_neto",
            "nap neto",
            "nap.neto",
            "n4p-neto",
            "nаp-x",
            "nap–neto",
            "nap—x",
            "ｎａｐ－x",
            "  nap-x",
            "Nap·x",
        ] {
            assert_eq!(
                check_name(n, 32, &none()),
                Err(NameError::ReservedPrefix),
                "{n:?}"
            );
            assert!(
                check_name(n, 32, &g(&[GRANT_RESERVED_PREFIX])).is_ok(),
                "{n:?} with grant"
            );
        }
    }

    #[test]
    fn ordinary_names_pass() {
        for n in [
            "Neto",
            "Player 1",
            "Naples",
            "napkin",
            "nap",
            "Nappa",
            "apple",
            "Pineapple",
            "nape",
            "Mario & Luigi",
            "ゆうき",
            "Zoë",
            "🍎 fan",
            "👨\u{200D}👩\u{200D}👧",
        ] {
            assert!(check_name(n, 32, &none()).is_ok(), "{n:?}");
        }
        // "snapple" contains "napple": refused (a brand look-alike either way).
        assert_eq!(
            check_name("Snapple", 32, &none()),
            Err(NameError::ReservedWord)
        );
    }

    #[test]
    fn invisible_and_invalid() {
        for n in [
            "nap\u{200B}ple",
            "x\u{202E}elppan",
            "\u{FEFF}hi",
            "na\u{00AD}pple",
        ] {
            assert_eq!(
                check_name(n, 32, &none()),
                Err(NameError::Invisible),
                "{n:?}"
            );
        }
        assert_eq!(check_name("", 32, &none()), Err(NameError::Invalid));
        assert_eq!(check_name("   ", 32, &none()), Err(NameError::Invalid));
        assert_eq!(
            check_name("a\u{0007}b", 32, &none()),
            Err(NameError::Invalid)
        );
        assert_eq!(
            check_name(&"x".repeat(33), 32, &none()),
            Err(NameError::Invalid)
        );
    }

    #[test]
    fn skeletons() {
        assert_eq!(compact("N-4_P p.1 3"), "napple");
        assert_eq!(skeleton("N4P-x"), "nap-x");
        assert_eq!(compact("ｎａｐｐｌｅ"), "napple");
    }
}
