//! The user's prod.keys. Held in memory only, zeroized on drop, redacted in Debug.
use super::error::{Error, Result};
use aes::Aes128;
use aes::cipher::{BlockDecrypt, KeyInit, generic_array::GenericArray};
use std::collections::HashMap;
use zeroize::Zeroize;

const MAX_KEYFILE: u64 = 1 << 20;

#[derive(Default)]
pub struct KeySet(HashMap<String, Vec<u8>>);

impl KeySet {
    pub fn parse(text: &str) -> Result<Self> {
        let mut map = HashMap::new();
        for line in text.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') || line.starts_with(';') {
                continue;
            }
            let Some((k, v)) = line.split_once('=') else {
                continue;
            };
            let (k, v) = (k.trim().to_ascii_lowercase(), v.trim());
            if v.is_empty() || v.len() % 2 != 0 || !v.bytes().all(|b| b.is_ascii_hexdigit()) {
                continue; // tolerate unrelated lines; never echo them
            }
            let bytes = (0..v.len())
                .step_by(2)
                .map(|i| u8::from_str_radix(&v[i..i + 2], 16).unwrap())
                .collect();
            map.insert(k, bytes);
        }
        if map.is_empty() {
            return Err(Error::KeyFile("no keys found".into()));
        }
        let mut ks = Self(map);
        ks.derive_missing();
        Ok(ks)
    }

    /// Fills header_key, titlekek_XX and key_area_key_{application,ocean,system}_XX from the
    /// source keys when the key file does not carry them (newer firmware generations).
    /// Never overwrites a key already present.
    fn derive_missing(&mut self) {
        let dec = |key: &[u8], data: &[u8]| -> Vec<u8> {
            let c = Aes128::new(GenericArray::from_slice(key));
            let mut out = data.to_vec();
            for b in out.as_chunks_mut::<16>().0 {
                c.decrypt_block(GenericArray::from_mut_slice(b));
            }
            out
        };
        let g =
            |m: &HashMap<String, Vec<u8>>, n: &str| m.get(n).filter(|v| v.len() % 16 == 0).cloned();
        let (kek_seed, key_seed) = (
            g(&self.0, "aes_kek_generation_source"),
            g(&self.0, "aes_key_generation_source"),
        );
        let generate_kek = |src: &[u8], master: &[u8]| -> Option<Vec<u8>> {
            let kek = dec(master, kek_seed.as_ref()?);
            Some(dec(&dec(&kek, src), key_seed.as_ref()?))
        };
        let mut add: Vec<(String, Vec<u8>)> = Vec::new();
        if !self.0.contains_key("header_key")
            && let (Some(m0), Some(ks), Some(kk), Some(src)) = (
                g(&self.0, "master_key_00"),
                g(&self.0, "header_kek_source"),
                kek_seed.as_ref(),
                g(&self.0, "header_key_source"),
            )
        {
            let kek = dec(
                &dec(&dec(&m0, kk), &ks),
                key_seed.as_ref().unwrap_or(&vec![0; 16]),
            );
            add.push(("header_key".into(), dec(&kek, &src)));
        }
        for n in 0..=0xFFu8 {
            let Some(master) = g(&self.0, &format!("master_key_{n:02x}")) else {
                continue;
            };
            if let Some(src) = g(&self.0, "titlekek_source") {
                add.push((format!("titlekek_{n:02x}"), dec(&master, &src)));
            }
            for kind in ["application", "ocean", "system"] {
                if let Some(src) = g(&self.0, &format!("key_area_key_{kind}_source"))
                    && let Some(k) = generate_kek(&src, &master)
                {
                    add.push((format!("key_area_key_{kind}_{n:02x}"), k));
                }
            }
        }
        for (k, v) in add {
            if v.len() == 16 || v.len() == 32 {
                self.0.entry(k).or_insert(v);
            }
        }
    }

    pub fn load(path: &std::path::Path) -> Result<Self> {
        if std::fs::metadata(path)?.len() > MAX_KEYFILE {
            return Err(Error::KeyFile("key file too large".into()));
        }
        Self::parse(&String::from_utf8_lossy(&std::fs::read(path)?))
    }

    pub fn get<const N: usize>(&self, name: &str) -> Result<[u8; N]> {
        let v = self
            .0
            .get(&name.to_ascii_lowercase())
            .ok_or_else(|| Error::MissingKey(name.into()))?;
        v.as_slice()
            .try_into()
            .map_err(|_| Error::KeyFile(format!("key {name} has wrong length")))
    }

    pub fn has(&self, name: &str) -> bool {
        self.0.contains_key(&name.to_ascii_lowercase())
    }
}

impl Drop for KeySet {
    fn drop(&mut self) {
        for v in self.0.values_mut() {
            v.zeroize();
        }
    }
}

impl std::fmt::Debug for KeySet {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "KeySet(<{} keys redacted>)", self.0.len())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_and_redacts() {
        let k = KeySet::parse(
            "# c\nheader_key = 00112233445566778899aabbccddeeff\nbad line\nodd=abc\n",
        )
        .unwrap();
        assert!(k.has("HEADER_KEY"));
        assert_eq!(k.get::<16>("header_key").unwrap()[1], 0x11);
        assert!(!format!("{k:?}").contains("0011"));
        assert!(matches!(k.get::<32>("header_key"), Err(Error::KeyFile(_))));
        assert!(matches!(k.get::<16>("nope"), Err(Error::MissingKey(_))));
    }

    #[test]
    fn empty_rejected() {
        assert!(KeySet::parse("garbage\n").is_err());
    }
}
