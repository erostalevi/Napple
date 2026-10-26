//! On-disk formats (`<module>.fixups.tsv`, `.gates.tsv`, `.sites.tsv`, `.patched.bin`); see
//! `docs/architecture/rewrite-format.md` for the specification.
use crate::error::RewriteError;
use crate::types::{Key, RewriteOutput, SiteRow};
use std::fmt::Write as _;
use std::path::Path;

impl RewriteOutput {
    /// Header of `fixups.tsv`: layout of the image and the key-page entry numbers.
    pub fn fixups_tsv(&self) -> String {
        let l = &self.layout;
        let mut s = String::new();
        let _ = writeln!(s, "# PATCH_OFF\t{:#x}", l.patch_off);
        let _ = writeln!(s, "# KEY_PAGE_OFF\t{:#x}", l.key_page_off);
        let _ = writeln!(s, "# KEY_PAGE_SIZE\t{:#x}", crate::image::KEY_PAGE_SIZE);
        let _ = writeln!(s, "# TEXT_END\t{:#x}", l.text_end);
        let _ = writeln!(s, "# RO_OFF\t{:#x}", l.ro_off);
        let _ = writeln!(s, "# DATA_OFF\t{:#x}", l.data_off);
        for k in Key::ALL {
            let _ = writeln!(s, "# KEY_{}_IDX\t{}", k.name(), k.index());
        }
        s
    }

    /// `ret_off<TAB>kind<TAB>arg<TAB>orig_off`, one row per host gate, in emission order.
    pub fn gates_tsv(&self) -> String {
        let mut s = String::new();
        for g in &self.gates {
            let _ = writeln!(
                s,
                "{:#x}\t{}\t{}\t{:#x}",
                g.ret,
                g.kind.label(),
                g.arg,
                g.orig
            );
        }
        s
    }

    /// Rewritten sites: single sites, then windows (`W`), calls (`C`) and jump tables (`J`).
    pub fn sites_tsv(&self) -> String {
        let mut s = String::new();
        for r in &self.sites {
            let _ = match r {
                SiteRow::Single { addr, label } => writeln!(s, "{addr:#x}\t{label}"),
                SiteRow::Window { lo, hi } => writeln!(s, "{lo:#x}\tW\t{hi:#x}"),
                SiteRow::Call { addr, ret } => writeln!(s, "{addr:#x}\tC\t{ret:#x}"),
                SiteRow::Jump { addr, base, count } => {
                    writeln!(s, "{addr:#x}\tJ\t{base:#x}\t{count}")
                }
            };
        }
        s
    }

    pub fn unhandled_tsv(&self) -> String {
        let mut s = String::new();
        for u in &self.unhandled {
            let _ = writeln!(s, "{:#x}\t{:08x}\t{}", u.addr, u.word, u.reason);
        }
        s
    }

    /// Writes the four standard files (plus `unhandled.tsv` / `warnings.txt` when non-empty).
    pub fn write_to(&self, dir: &Path, name: &str) -> Result<(), RewriteError> {
        let io = |e: std::io::Error| RewriteError::Io(e.to_string());
        std::fs::create_dir_all(dir).map_err(io)?;
        let p = |ext: &str| dir.join(format!("{name}.{ext}"));
        std::fs::write(p("patched.bin"), &self.image).map_err(io)?;
        std::fs::write(p("fixups.tsv"), self.fixups_tsv()).map_err(io)?;
        std::fs::write(p("gates.tsv"), self.gates_tsv()).map_err(io)?;
        std::fs::write(p("sites.tsv"), self.sites_tsv()).map_err(io)?;
        for (ext, body) in [
            ("unhandled.tsv", self.unhandled_tsv()),
            ("warnings.txt", self.warnings.join("\n")),
        ] {
            let path = p(ext);
            if body.is_empty() {
                let _ = std::fs::remove_file(path);
            } else {
                std::fs::write(path, body + "\n").map_err(io)?;
            }
        }
        Ok(())
    }
}
