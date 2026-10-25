//! Rewritten module image -> signable arm64 Mach-O dylib (the iOS "no JIT" loader path).
//!
//! macOS builds map `<module>.patched.bin` into anonymous memory and `mprotect(PROT_EXEC)` the text. iOS refuses that for
//! non-JIT apps, so executable pages must come from a code-signed Mach-O that dyld maps. This writer wraps the rewriter's image,
//! byte for byte and at the same relative offsets, in a dylib:
//!
//! ```text
//! vm 0               __TEXT       r-x   Mach-O header page (16 KiB) + image [0, rx)         rx = page_up(text_end)
//!                    __RODATA     r--   image [rx, ro_end)                                  ro_end = data_off rounded down to 16 KiB
//!                    __DATA       rw-   image [ro_end, patch_off)   (data, relocation targets, the key page)
//!                    __TEXT_EXEC  r-x   image [patch_off, size)     (the rewriter's patch area)
//!                    __LINKEDIT   r--   string table (+ the code signature `codesign` appends)
//! ```
//!
//! The runtime dlopen()s the dylib and uses `header + HEADER_PAGE` as the module base, so every offset in the NTB1 tables
//! stays valid and the sealing it does on macOS (`seal()` in runtime/src/module.c) is already encoded in the segment protections.
//! The file is not signed here: `codesign` (Mac) or any iOS signer adds the signature, which is what lets the kernel accept
//! the executable pages. Platform is iOS 26+ (also valid for iPadOS); there are no imports besides libSystem.
use crate::error::{BuildError, Result};
use sha2::{Digest, Sha256};

/// Page size of arm64 Apple platforms and of the guest image layout.
pub const PAGE: u64 = 0x4000;
/// Bytes between the Mach-O header and image offset 0 (a whole page so the image stays 16 KiB aligned).
pub const HEADER_PAGE: u64 = PAGE;
pub const MIN_OS_IOS: u32 = 26;

/// LC_BUILD_VERSION platform of the produced dylib: a device build and a Simulator build differ only in this value
/// (the Simulator refuses a device-platform image and the device refuses the Simulator one).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Platform {
    Ios,
    IosSimulator,
}

impl Platform {
    pub fn id(self) -> u32 {
        match self {
            Platform::Ios => 2,
            Platform::IosSimulator => 7,
        }
    }
}

/// Section offsets of one module image (the NTB1 header fields of docs/runtime-modules.md).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Layout {
    pub file_size: u64,
    pub text_end: u64,
    pub data_off: u64,
    pub key_off: u64,
    pub patch_off: u64,
}

fn up(v: u64) -> u64 {
    (v + PAGE - 1) & !(PAGE - 1)
}

/// Reads the layout from an NTB1 table file (`"NTB1"`, 6 u32, then file_size,text_end,ro_off,data_off,patch_off,key_off as u64).
pub fn layout_from_ntb1(tables: &[u8]) -> Result<Layout> {
    if tables.len() < 24 + 48 || &tables[..4] != b"NTB1" {
        return Err(BuildError::new("macho", "not an NTB1 table file"));
    }
    let u = |i: usize| u64::from_le_bytes(tables[24 + 8 * i..32 + 8 * i].try_into().unwrap());
    Ok(Layout { file_size: u(0), text_end: u(1), data_off: u(3), patch_off: u(4), key_off: u(5) })
}

struct Seg {
    name: &'static str,
    vmaddr: u64,
    vmsize: u64,
    fileoff: u64,
    filesize: u64,
    prot: u32,
}

fn put32(b: &mut Vec<u8>, v: u32) {
    b.extend_from_slice(&v.to_le_bytes());
}
fn put64(b: &mut Vec<u8>, v: u64) {
    b.extend_from_slice(&v.to_le_bytes());
}
fn pad_to(b: &mut Vec<u8>, n: usize) {
    while b.len() < n {
        b.push(0);
    }
}

fn segment_cmd(s: &Seg) -> Vec<u8> {
    let mut c = Vec::with_capacity(72);
    put32(&mut c, 0x19); // LC_SEGMENT_64
    put32(&mut c, 72);
    let mut n = [0u8; 16];
    n[..s.name.len()].copy_from_slice(s.name.as_bytes());
    c.extend_from_slice(&n);
    put64(&mut c, s.vmaddr);
    put64(&mut c, s.vmsize);
    put64(&mut c, s.fileoff);
    put64(&mut c, s.filesize);
    put32(&mut c, s.prot); // maxprot
    put32(&mut c, s.prot); // initprot
    put32(&mut c, 0); // nsects
    put32(&mut c, 0); // flags
    c
}

fn dylib_cmd(cmd: u32, name: &str, ts: u32, cur: u32, compat: u32) -> Vec<u8> {
    let size = (24 + name.len() + 1 + 7) & !7;
    let mut c = Vec::with_capacity(size);
    put32(&mut c, cmd);
    put32(&mut c, size as u32);
    put32(&mut c, 24);
    put32(&mut c, ts);
    put32(&mut c, cur);
    put32(&mut c, compat);
    c.extend_from_slice(name.as_bytes());
    pad_to(&mut c, size);
    c
}

/// Wraps `image` (the `.patched.bin` bytes) in a dylib. `install_name` is the LC_ID_DYLIB path (e.g. `@rpath/<buildid16>.main.dylib`).
pub fn build_dylib(image: &[u8], l: &Layout, install_name: &str, platform: Platform) -> Result<Vec<u8>> {
    let bad = |m: &str| Err(BuildError::new("macho", format!("{install_name}: {m}")));
    if image.len() as u64 != l.file_size {
        return bad("image size differs from its tables");
    }
    let size = up(l.file_size);
    let rx = up(l.text_end);
    let ro_end = l.data_off & !(PAGE - 1);
    if l.text_end == 0 || rx > ro_end {
        return bad(&format!("text overlaps data (text_end {:#x} rounds to {rx:#x}, data starts in page {ro_end:#x})", l.text_end));
    }
    if l.key_off % PAGE != 0 || l.patch_off % PAGE != 0 || l.key_off + PAGE > l.patch_off || l.patch_off >= size || ro_end >= l.key_off {
        return bad("key page / patch layout is not 16 KiB aligned or ordered");
    }
    if install_name.len() > 200 {
        return bad("install name too long");
    }
    let at = |off: u64| HEADER_PAGE + off;
    let mut segs = vec![Seg { name: "__TEXT", vmaddr: 0, vmsize: at(rx), fileoff: 0, filesize: at(rx), prot: 5 }];
    if ro_end > rx {
        segs.push(Seg { name: "__RODATA", vmaddr: at(rx), vmsize: ro_end - rx, fileoff: at(rx), filesize: ro_end - rx, prot: 1 });
    }
    segs.push(Seg { name: "__DATA", vmaddr: at(ro_end), vmsize: l.patch_off - ro_end, fileoff: at(ro_end), filesize: l.patch_off - ro_end, prot: 3 });
    segs.push(Seg { name: "__TEXT_EXEC", vmaddr: at(l.patch_off), vmsize: size - l.patch_off, fileoff: at(l.patch_off), filesize: size - l.patch_off, prot: 5 });
    let linkedit_off = at(size);
    let linkedit_len = 16u64; // string table: "\0" + padding (no symbols); codesign appends the signature after it
    segs.push(Seg { name: "__LINKEDIT", vmaddr: linkedit_off, vmsize: PAGE, fileoff: linkedit_off, filesize: linkedit_len, prot: 1 });

    let mut cmds: Vec<Vec<u8>> = segs.iter().map(segment_cmd).collect();
    cmds.push(dylib_cmd(0xd, install_name, 1, 1 << 16, 1 << 16)); // LC_ID_DYLIB
    let mut uuid = vec![];
    put32(&mut uuid, 0x1b);
    put32(&mut uuid, 24);
    let digest = Sha256::digest(image);
    uuid.extend_from_slice(&digest[..16]);
    cmds.push(uuid);
    let mut bv = vec![];
    put32(&mut bv, 0x32); // LC_BUILD_VERSION
    put32(&mut bv, 24);
    put32(&mut bv, platform.id()); // PLATFORM_IOS (2) or PLATFORM_IOSSIMULATOR (7)
    put32(&mut bv, MIN_OS_IOS << 16);
    put32(&mut bv, MIN_OS_IOS << 16); // sdk
    put32(&mut bv, 0); // ntools
    cmds.push(bv);
    cmds.push(dylib_cmd(0xc, "/usr/lib/libSystem.B.dylib", 2, 1351 << 16, 1 << 16)); // LC_LOAD_DYLIB
    let mut st = vec![];
    for v in [0x2u32, 24, linkedit_off as u32, 0, linkedit_off as u32, linkedit_len as u32] {
        put32(&mut st, v); // LC_SYMTAB: no symbols, empty string table
    }
    cmds.push(st);
    let mut ds = vec![];
    put32(&mut ds, 0xb); // LC_DYSYMTAB
    put32(&mut ds, 80);
    pad_to(&mut ds, 80);
    cmds.push(ds);
    let mut di = vec![];
    put32(&mut di, 0x8000_0022); // LC_DYLD_INFO_ONLY: nothing to rebase or bind (relocation happens in the runtime)
    put32(&mut di, 48);
    pad_to(&mut di, 48);
    cmds.push(di);

    let sizeofcmds: usize = cmds.iter().map(Vec::len).sum();
    // codesign needs room for LC_CODE_SIGNATURE (16 bytes) after the commands
    if 32 + sizeofcmds + 16 > HEADER_PAGE as usize {
        return bad("load commands do not fit the header page");
    }
    let mut out = Vec::with_capacity((linkedit_off + linkedit_len) as usize);
    put32(&mut out, 0xfeed_facf);
    put32(&mut out, 0x0100_000c); // CPU_TYPE_ARM64
    put32(&mut out, 0); // CPU_SUBTYPE_ARM64_ALL
    put32(&mut out, 6); // MH_DYLIB
    put32(&mut out, cmds.len() as u32);
    put32(&mut out, sizeofcmds as u32);
    put32(&mut out, 0x0010_0085); // NOUNDEFS | DYLDLINK | TWOLEVEL | NO_REEXPORTED_DYLIBS
    put32(&mut out, 0);
    for c in &cmds {
        out.extend_from_slice(c);
    }
    pad_to(&mut out, HEADER_PAGE as usize);
    out.extend_from_slice(image);
    pad_to(&mut out, linkedit_off as usize);
    pad_to(&mut out, (linkedit_off + linkedit_len) as usize);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rd32(b: &[u8], o: usize) -> u32 {
        u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
    }
    fn rd64(b: &[u8], o: usize) -> u64 {
        u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
    }

    /// 1 page text, 1 page rodata, 2 pages data (+ key page), 1 patch page.
    fn sample() -> (Vec<u8>, Layout) {
        let l = Layout { file_size: 5 * PAGE, text_end: PAGE - 0x100, data_off: 2 * PAGE + 0x80, key_off: 4 * PAGE, patch_off: 5 * PAGE };
        // patch area must exist: size must exceed patch_off
        let l = Layout { file_size: 5 * PAGE + 0x40, ..l };
        let img: Vec<u8> = (0..l.file_size).map(|i| (i % 251) as u8).collect();
        (img, l)
    }

    /// (name, vmaddr, vmsize, fileoff, filesize, prot)
    fn segments(b: &[u8]) -> Vec<(String, u64, u64, u64, u64, u32)> {
        let (n, mut o) = (rd32(b, 16), 32usize);
        let mut v = vec![];
        for _ in 0..n {
            let (cmd, sz) = (rd32(b, o), rd32(b, o + 4) as usize);
            if cmd == 0x19 {
                let name = String::from_utf8_lossy(&b[o + 8..o + 24]).trim_end_matches('\0').to_string();
                v.push((name, rd64(b, o + 24), rd64(b, o + 32), rd64(b, o + 40), rd64(b, o + 48), rd32(b, o + 56)));
            }
            o += sz;
        }
        v
    }

    #[test]
    fn segments_mirror_the_runtime_seal() {
        let (img, l) = sample();
        let d = build_dylib(&img, &l, "@rpath/x.dylib", Platform::Ios).unwrap();
        assert_eq!((rd32(&d, 0), rd32(&d, 4), rd32(&d, 12)), (0xfeedfacf, 0x0100000c, 6));
        let s = segments(&d);
        let names: Vec<_> = s.iter().map(|x| x.0.as_str()).collect();
        assert_eq!(names, ["__TEXT", "__RODATA", "__DATA", "__TEXT_EXEC", "__LINKEDIT"]);
        // text = header page + one page; rodata = page 2 (data_off is inside page 2, so ro_end = 2*PAGE) ...
        assert_eq!((s[0].1, s[0].2, s[0].5), (0, HEADER_PAGE + PAGE, 5));
        assert_eq!((s[1].1, s[1].2, s[1].5), (HEADER_PAGE + PAGE, PAGE, 1));
        assert_eq!((s[2].1, s[2].2, s[2].5), (HEADER_PAGE + 2 * PAGE, 3 * PAGE, 3)); // data + key page, writable
        assert_eq!((s[3].1, s[3].2, s[3].5), (HEADER_PAGE + 5 * PAGE, PAGE, 5));
        for x in &s {
            assert_eq!((x.1 % PAGE, x.2 % PAGE, x.3 % PAGE), (0, 0, 0), "{} not 16 KiB aligned", x.0);
            assert_eq!(x.1, x.3, "{}: vm and file offsets are identical", x.0);
        }
        // image bytes sit at HEADER_PAGE + offset, unchanged
        assert_eq!(&d[HEADER_PAGE as usize..HEADER_PAGE as usize + img.len()], &img[..]);
        // linkedit is last in the file (codesign appends its signature there)
        let le = s.last().unwrap();
        assert_eq!(le.3 + le.4, d.len() as u64);
    }

    fn build_version_platform(d: &[u8]) -> u32 {
        let (n, mut o) = (rd32(d, 16), 32);
        for _ in 0..n {
            let (c, s) = (rd32(d, o), rd32(d, o + 4) as usize);
            if c == 0x32 {
                return rd32(d, o + 8);
            }
            o += s;
        }
        panic!("no LC_BUILD_VERSION");
    }

    #[test]
    fn platform_is_a_parameter_and_only_that_differs() {
        let (img, l) = sample();
        let dev = build_dylib(&img, &l, "@rpath/x.dylib", Platform::Ios).unwrap();
        let sim = build_dylib(&img, &l, "@rpath/x.dylib", Platform::IosSimulator).unwrap();
        assert_eq!((build_version_platform(&dev), build_version_platform(&sim)), (2, 7));
        assert_eq!(dev.len(), sim.len());
        let diff: Vec<usize> = (0..dev.len()).filter(|&i| dev[i] != sim[i]).collect();
        assert_eq!(diff.len(), 1, "layout, segments and image must be identical: {diff:?}");
    }

    #[test]
    fn load_commands_leave_room_for_codesign_and_name_ios() {
        let (img, l) = sample();
        let d = build_dylib(&img, &l, "@rpath/0123456789abcdef.main.dylib", Platform::Ios).unwrap();
        let (n, sz) = (rd32(&d, 16) as usize, rd32(&d, 20) as usize);
        assert!(32 + sz + 16 <= HEADER_PAGE as usize);
        let mut o = 32;
        let mut seen = vec![];
        for _ in 0..n {
            let (c, s) = (rd32(&d, o), rd32(&d, o + 4) as usize);
            assert_eq!(s % 8, 0, "cmd {c:#x} not 8-byte multiple");
            if c == 0x32 {
                assert_eq!((rd32(&d, o + 8), rd32(&d, o + 12)), (2, 26 << 16));
            }
            seen.push(c);
            o += s;
        }
        assert_eq!(o, 32 + sz);
        for need in [0xd, 0x1b, 0x32, 0xc, 0x2, 0xb] {
            assert!(seen.contains(&need), "missing load command {need:#x}");
        }
        assert!(String::from_utf8_lossy(&d).contains("0123456789abcdef.main.dylib"));
    }

    #[test]
    fn rejects_inconsistent_layouts() {
        let (img, l) = sample();
        assert!(build_dylib(&img[1..], &l, "x", Platform::Ios).is_err());
        assert!(build_dylib(&img, &Layout { patch_off: 5 * PAGE + 4, ..l }, "x", Platform::Ios).is_err());
        assert!(build_dylib(&img, &Layout { text_end: 3 * PAGE, ..l }, "x", Platform::Ios).is_err());
        assert!(build_dylib(&img, &Layout { key_off: 4 * PAGE + 1, ..l }, "x", Platform::Ios).is_err());
    }

    #[test]
    fn ntb1_layout_parse() {
        let mut t = b"NTB1".to_vec();
        t.resize(24, 0);
        for v in [10u64, 20, 30, 40, 50, 60] {
            t.extend_from_slice(&v.to_le_bytes());
        }
        assert_eq!(layout_from_ntb1(&t).unwrap(), Layout { file_size: 10, text_end: 20, data_off: 40, patch_off: 50, key_off: 60 });
        assert!(layout_from_ntb1(b"NTB2").is_err());
    }

    /// Cross-check with LLVM when it is installed: the file must parse as a Mach-O arm64 dylib with our segments.
    #[test]
    fn llvm_reads_it() {
        let Ok(out) = std::process::Command::new("llvm-objdump").arg("--version").output() else { return };
        if !out.status.success() {
            return;
        }
        let (img, l) = sample();
        let d = build_dylib(&img, &l, "@rpath/x.dylib", Platform::Ios).unwrap();
        let p = std::env::temp_dir().join(format!("napple-macho-test-{}.dylib", std::process::id()));
        std::fs::write(&p, &d).unwrap();
        let r = std::process::Command::new("llvm-objdump").args(["--macho", "--private-headers"]).arg(&p).output().unwrap();
        let _ = std::fs::remove_file(&p);
        let text = String::from_utf8_lossy(&r.stdout).to_string() + &String::from_utf8_lossy(&r.stderr);
        assert!(r.status.success(), "llvm-objdump failed: {text}");
        for k in ["ARM64", "DYLIB", "__TEXT_EXEC", "__RODATA", "LC_ID_DYLIB", "LC_BUILD_VERSION", "platform ios"] {
            assert!(text.contains(k), "llvm-objdump output lacks {k}:\n{text}");
        }
    }
}
