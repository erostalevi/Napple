//! Patch-area builder: an append-only word buffer addressed in module coordinates.
use crate::a64::{self, EncodeError};
use crate::types::{Fixup, Key};

/// A reason a site cannot be rewritten (message only; the caller attaches the address).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Reason(pub String);

impl From<EncodeError> for Reason {
    fn from(e: EncodeError) -> Self {
        Reason(e.0)
    }
}
impl From<String> for Reason {
    fn from(s: String) -> Self {
        Reason(s)
    }
}
impl From<&str> for Reason {
    fn from(s: &str) -> Self {
        Reason(s.to_string())
    }
}

pub type R<T> = Result<T, Reason>;

#[derive(Debug)]
pub struct Patch {
    pub base: u32,
    key_page: u32,
    pub words: Vec<u32>,
    pub fixups: Vec<Fixup>,
}

/// Rollback point so a failed template leaves no partial code behind.
#[derive(Clone, Copy)]
pub struct Mark(usize, usize);

impl Patch {
    pub fn new(base: u32, key_page: u32) -> Self {
        Self { base, key_page, words: Vec::new(), fixups: Vec::new() }
    }

    pub fn here(&self) -> u32 {
        self.base + 4 * self.words.len() as u32
    }

    pub fn emit(&mut self, w: u32) {
        self.words.push(w);
    }

    pub fn mark(&self) -> Mark {
        Mark(self.words.len(), self.fixups.len())
    }

    pub fn rollback(&mut self, m: Mark) {
        self.words.truncate(m.0);
        self.fixups.truncate(m.1);
    }

    /// Emits `b target`.
    pub fn emit_b(&mut self, target: u32) -> R<()> {
        let w = a64::b(self.here() as u64, target as u64)?;
        self.emit(w);
        Ok(())
    }

    /// `mrs t, tpidrro_el0; adrp s, keypage; ldr s, [s, #key]; add t, t, s`
    /// leaves `t` = address of the per-thread slot for `key`; `s` is clobbered.
    pub fn slot_addr(&mut self, t: u32, s: u32, key: Key) -> R<()> {
        self.emit(a64::mrs_tpidrro(t));
        let at = self.here();
        let adrp = a64::adrp(s, at as u64, self.key_page as u64)?;
        self.emit(adrp);
        self.fixups.push(Fixup { at, key });
        let ldr = a64::ldr_slot(s, s, key.index())?;
        self.emit(ldr);
        self.emit(a64::add_reg(t, t, s));
        Ok(())
    }

    /// Words emitted so far as a byte vector (little endian).
    pub fn bytes(&self) -> Vec<u8> {
        self.words.iter().flat_map(|w| w.to_le_bytes()).collect()
    }
}
