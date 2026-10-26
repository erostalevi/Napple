//! Public data model of the rewriter: key-page conventions, output tables and options.
//!
//! The key-page, slot and gate conventions are compatible with the reference rewriter
//! (`patch_nso.py`, MIT) so a loader written for that format can consume the output.
use crate::error::Unhandled;
use crate::image::Layout;
use std::collections::BTreeMap;

/// Entries of the key page. The loader writes `thread-slot-number * 8` into entry `index()`;
/// rewritten code adds that value to `tpidrro_el0` to reach the per-thread slot.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Key {
    /// Virtual `x18`.
    K18,
    /// Virtual `tpidr_el0` (guest thread pointer).
    Ktls,
    /// Virtual `tpidrro_el0` (Horizon ThreadLocalRegion pointer).
    Ktlr,
    /// Address of the host gate entry point.
    Kgate,
    /// Reserved for host parameters (unused by the generic rewriter, kept so loaders see all names).
    Kres0,
    Kres1,
    Kaspect,
    Kaspectui,
}

impl Key {
    pub const ALL: [Key; 8] = [
        Key::K18,
        Key::Ktls,
        Key::Ktlr,
        Key::Kgate,
        Key::Kres0,
        Key::Kres1,
        Key::Kaspect,
        Key::Kaspectui,
    ];

    /// Entry number inside the key page (the `ldr` immediate is `index * 8` bytes).
    pub fn index(self) -> u32 {
        Self::ALL.iter().position(|k| *k == self).unwrap() as u32
    }

    pub fn name(self) -> &'static str {
        match self {
            Key::K18 => "K18",
            Key::Ktls => "KTLS",
            Key::Ktlr => "KTLR",
            Key::Kgate => "KGATE",
            Key::Kres0 => "KRES0",
            Key::Kres1 => "KRES1",
            Key::Kaspect => "KASPECT",
            Key::Kaspectui => "KASPECTUI",
        }
    }
}

/// Why control enters the host: a Horizon `svc` or an emulated counter register.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GateKind {
    Svc,
    Cntpct,
    Cntvct,
    Cntfrq,
}

impl GateKind {
    pub fn label(self) -> &'static str {
        match self {
            GateKind::Svc => "svc",
            GateKind::Cntpct => "cntpct",
            GateKind::Cntvct => "cntvct",
            GateKind::Cntfrq => "cntfrq",
        }
    }
}

/// One row of `gates.tsv`. `ret` is the module offset of the instruction after the host call
/// (the return address the host sees); the host looks the call up by it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Gate {
    pub ret: u32,
    pub kind: GateKind,
    /// `svc` immediate, or the destination register number for counter reads.
    pub arg: u32,
    /// Address of the original instruction.
    pub orig: u32,
}

/// An `adrp` in the patch area that points at the key page (informational; absolute module offsets).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Fixup {
    pub at: u32,
    pub key: Key,
}

/// One row of `sites.tsv` (besides the per-site rows of kind labels).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SiteRow {
    /// A single rewritten instruction: `addr<TAB>label`.
    Single { addr: u32, label: &'static str },
    /// Exclusive window `[lo, hi)`: `lo<TAB>W<TAB>hi`.
    Window { lo: u32, hi: u32 },
    /// `blr x18` call at `addr`, returning to `ret`: `addr<TAB>C<TAB>ret`.
    Call { addr: u32, ret: u32 },
    /// `br x18` jump table: `addr<TAB>J<TAB>base<TAB>count` (targets `base + 4*i`).
    Jump { addr: u32, base: u32, count: u32 },
}

#[derive(Clone, Debug)]
pub struct RewriteOptions {
    /// Largest number of landing pads one `br x18` table may need (hostile input guard).
    pub max_jump_pads: u32,
    /// Longest exclusive window (instructions between `ldxr` and the status test).
    pub max_window_len: usize,
}

impl Default for RewriteOptions {
    fn default() -> Self {
        Self {
            max_jump_pads: 1 << 17,
            max_window_len: 16,
        }
    }
}

pub type Stats = BTreeMap<&'static str, usize>;

/// Result of rewriting one module.
#[derive(Clone, Debug)]
pub struct RewriteOutput {
    /// `<name>.patched.bin`: module image, key page, zero fill, then the patch area at `layout.patch_off`.
    pub image: Vec<u8>,
    pub layout: Layout,
    pub build_id: String,
    pub fixups: Vec<Fixup>,
    pub gates: Vec<Gate>,
    pub sites: Vec<SiteRow>,
    pub stats: Stats,
    /// Sites left untouched because no safe template applies; a module with any is not runnable.
    pub unhandled: Vec<Unhandled>,
    /// Non-fatal observations (data words that look like code, unusual system registers, ...).
    pub warnings: Vec<String>,
}

impl RewriteOutput {
    pub fn patch(&self) -> &[u8] {
        &self.image[self.layout.patch_off as usize..]
    }
    pub fn is_complete(&self) -> bool {
        self.unhandled.is_empty()
    }
}
