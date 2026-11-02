//! Napple friend codes: 12 digits (11 random + 1 Luhn check digit), shown as `NP-1234-5678-9012`.
//! Input is forgiving: the `NP` prefix, dashes and spaces are optional; the check digit catches most typos before a lookup.

use rand::Rng;

pub const PREFIX: &str = "NP";

fn luhn_digit(payload: &[u8]) -> u8 {
    // Luhn check digit for a payload of decimal digits (values 0..=9), rightmost payload digit doubled.
    let mut sum = 0u32;
    for (i, d) in payload.iter().rev().enumerate() {
        let mut v = *d as u32;
        if i % 2 == 0 {
            v *= 2;
            if v > 9 {
                v -= 9;
            }
        }
        sum += v;
    }
    ((10 - (sum % 10)) % 10) as u8
}

/// A fresh random code, 12 digits, stored form (no prefix, no dashes).
pub fn generate() -> String {
    let mut rng = rand::thread_rng();
    let mut d: Vec<u8> = (0..11).map(|_| rng.gen_range(0..10u8)).collect();
    // Keep the first digit nonzero so codes never look truncated.
    if d[0] == 0 {
        d[0] = rng.gen_range(1..10u8);
    }
    let c = luhn_digit(&d);
    d.push(c);
    d.iter().map(|v| char::from(b'0' + v)).collect()
}

/// Stored form -> display form `NP-dddd-dddd-dddd`.
pub fn display(stored: &str) -> String {
    if stored.len() != 12 {
        return stored.to_string();
    }
    format!("{PREFIX}-{}-{}-{}", &stored[0..4], &stored[4..8], &stored[8..12])
}

/// Any user input -> stored form, or None when it is not a valid Napple friend code.
pub fn parse(input: &str) -> Option<String> {
    let s = input.trim();
    let s = if s.len() >= 2 && s[..2].eq_ignore_ascii_case(PREFIX) { &s[2..] } else { s };
    let mut digits = Vec::with_capacity(12);
    for ch in s.chars() {
        match ch {
            '0'..='9' => digits.push(ch as u8 - b'0'),
            '-' | ' ' | '\u{2010}' | '\u{2013}' => {}
            _ => return None,
        }
    }
    if digits.len() != 12 {
        return None;
    }
    if luhn_digit(&digits[..11]) != digits[11] {
        return None;
    }
    Some(digits.iter().map(|v| char::from(b'0' + v)).collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn generated_codes_round_trip() {
        for _ in 0..1000 {
            let c = generate();
            assert_eq!(c.len(), 12);
            assert_ne!(&c[..1], "0");
            let shown = display(&c);
            assert!(shown.starts_with("NP-"));
            assert_eq!(shown.len(), 17);
            assert_eq!(parse(&shown).as_deref(), Some(c.as_str()));
            assert_eq!(parse(&c).as_deref(), Some(c.as_str()));
            assert_eq!(parse(&shown.to_lowercase()).as_deref(), Some(c.as_str()));
            assert_eq!(parse(&format!(" {} {} {} ", &c[..4], &c[4..8], &c[8..])).as_deref(), Some(c.as_str()));
        }
    }

    #[test]
    fn typos_are_rejected() {
        let c = generate();
        // Every single-digit substitution breaks the Luhn check.
        for i in 0..12 {
            let mut b = c.clone().into_bytes();
            b[i] = b'0' + ((b[i] - b'0' + 1) % 10);
            assert!(parse(std::str::from_utf8(&b).unwrap()).is_none());
        }
        assert!(parse("").is_none());
        assert!(parse("NP-1234-5678").is_none());
        assert!(parse("SW-1234-5678-9012").is_none());
        assert!(parse("NP-1234-5678-901x").is_none());
        assert!(parse("1234567890123").is_none());
    }

    #[test]
    fn known_vector() {
        // 7992739871 -> Luhn check digit 3 (the classic example); padded to our 11-digit payload.
        assert_eq!(luhn_digit(&[7, 9, 9, 2, 7, 3, 9, 8, 7, 1]), 3);
    }
}
