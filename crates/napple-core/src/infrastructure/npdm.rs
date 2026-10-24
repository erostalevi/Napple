//! NPDM (main.npdm) parser: META header, ACI0 (granted) and ACID (policy) sections.
//! Extracts the process flags, thread/stack parameters, service access control (SAC) and kernel
//! capabilities (allowed SVCs, handle table size, kernel version, memory/IO mappings, ...).
use crate::domain::error::{container, Result};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ServiceAccess {
    pub name: String,
    /// true = the process hosts (registers) this service, false = it only connects to it.
    pub host: bool,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct KernelCaps {
    pub allowed_svcs: Vec<u8>,
    /// (lowest priority, highest priority, min core, max core)
    pub thread_info: Option<(u8, u8, u8, u8)>,
    pub handle_table_size: Option<u16>,
    pub kernel_version: Option<(u16, u8)>,
    pub program_type: Option<u8>,
    pub enable_debug: bool,
    pub force_debug: bool,
    /// (address, size, read_only, is_normal_memory)
    pub memory_maps: Vec<(u64, u64, bool, bool)>,
    pub io_maps: Vec<u64>,
    pub region_maps: Vec<(u8, bool)>,
    pub interrupts: Vec<u16>,
    pub unknown: Vec<u32>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Npdm {
    pub name: String,
    pub product_code: String,
    pub is_64bit: bool,
    /// 0 = 32-bit, 1 = 64-bit (39-bit legacy), 2 = 32-bit no reloc, 3 = 64-bit.
    pub address_space: u8,
    pub main_thread_priority: u8,
    pub main_thread_core: u8,
    pub system_resource_size: u32,
    pub version: u32,
    pub main_thread_stack_size: u32,
    pub title_id: u64,
    pub acid_flags: u32,
    pub acid_production: bool,
    pub memory_region: u8,
    pub fs_permissions: u64,
    pub services: Vec<ServiceAccess>,
    pub kernel: KernelCaps,
}

fn u32le(b: &[u8], o: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(o..o + 4)?.try_into().ok()?))
}
fn u64le(b: &[u8], o: usize) -> Option<u64> {
    Some(u64::from_le_bytes(b.get(o..o + 8)?.try_into().ok()?))
}
fn cstr(b: &[u8]) -> String {
    let end = b.iter().position(|&c| c == 0).unwrap_or(b.len());
    String::from_utf8_lossy(&b[..end]).into_owned()
}

/// Slice `[off, off+len)` of `d`, bounds-checked.
fn sub<'a>(d: &'a [u8], off: u32, len: u32) -> Result<&'a [u8]> {
    let end = (off as usize).checked_add(len as usize);
    match end.and_then(|e| d.get(off as usize..e)) {
        Some(s) => Ok(s),
        None => container("npdm section out of range"),
    }
}

pub fn parse_sac(b: &[u8]) -> Result<Vec<ServiceAccess>> {
    let (mut out, mut i) = (Vec::new(), 0usize);
    while i < b.len() {
        let c = b[i];
        let len = (c & 7) as usize + 1;
        let Some(n) = b.get(i + 1..i + 1 + len) else { return container("truncated SAC entry") };
        if !n.iter().all(|&x| x.is_ascii_graphic()) {
            return container("non-printable SAC service name");
        }
        out.push(ServiceAccess { name: String::from_utf8_lossy(n).into_owned(), host: c & 0x80 != 0 });
        i += 1 + len;
    }
    Ok(out)
}

pub fn parse_kac(b: &[u8]) -> KernelCaps {
    let mut k = KernelCaps::default();
    let words: Vec<u32> = b.chunks_exact(4).map(|c| u32::from_le_bytes(c.try_into().unwrap())).collect();
    let mut i = 0;
    while i < words.len() {
        let d = words[i];
        i += 1;
        if d == 0xFFFF_FFFF {
            continue;
        }
        let ones = d.trailing_ones();
        match ones {
            3 => k.thread_info = Some((((d >> 4) & 0x3F) as u8, ((d >> 10) & 0x3F) as u8, ((d >> 16) & 0xFF) as u8, ((d >> 24) & 0xFF) as u8)),
            4 => {
                let (mask, idx) = ((d >> 5) & 0xFF_FFFF, d >> 29);
                for bit in 0..24 {
                    if mask & (1 << bit) != 0 {
                        k.allowed_svcs.push((idx * 24 + bit) as u8);
                    }
                }
            }
            6 => {
                // Memory map: pair of descriptors (begin address + permission, size + type).
                let Some(&d2) = words.get(i) else { k.unknown.push(d); break };
                i += 1;
                let addr = ((d >> 7) & 0x00FF_FFFF) as u64 * 0x1000;
                let size = ((d2 >> 7) & 0x000F_FFFF) as u64 * 0x1000;
                k.memory_maps.push((addr, size, d >> 31 != 0, (d2 >> 31) != 0));
            }
            7 => k.io_maps.push(((d >> 8) & 0x00FF_FFFF) as u64 * 0x1000),
            // Descriptor type = number of trailing ones; payload starts at bit (ones + 1).
            10 => {
                for s in [11u32, 18, 25] {
                    let r = ((d >> s) & 0x3F) as u8;
                    let ro = (d >> (s + 6)) & 1 != 0;
                    if r != 0 || ro {
                        k.region_maps.push((r, ro));
                    }
                }
            }
            11 => {
                for s in [12u32, 22] {
                    let irq = ((d >> s) & 0x3FF) as u16;
                    if irq != 0x3FF {
                        k.interrupts.push(irq);
                    }
                }
            }
            13 => k.program_type = Some(((d >> 14) & 7) as u8),
            14 => k.kernel_version = Some((((d >> 19) & 0x1FFF) as u16, ((d >> 15) & 0xF) as u8)),
            15 => k.handle_table_size = Some(((d >> 16) & 0x3FF) as u16),
            16 => {
                k.enable_debug = (d >> 17) & 1 != 0;
                k.force_debug = (d >> 18) & 1 != 0;
            }
            _ => k.unknown.push(d),
        }
    }
    k.allowed_svcs.sort_unstable();
    k
}

pub fn parse_npdm(d: &[u8]) -> Result<Npdm> {
    if d.len() < 0x80 || &d[..4] != b"META" {
        return container("not an NPDM");
    }
    let flags = d[0xC];
    let (aci_off, aci_len, acid_off, acid_len) = (u32le(d, 0x70).unwrap(), u32le(d, 0x74).unwrap(), u32le(d, 0x78).unwrap(), u32le(d, 0x7C).unwrap());
    let aci = sub(d, aci_off, aci_len)?;
    let acid = sub(d, acid_off, acid_len)?;
    if aci.len() < 0x40 || &aci[..4] != b"ACI0" {
        return container("bad ACI0");
    }
    if acid.len() < 0x238 || &acid[0x200..0x204] != b"ACID" {
        return container("bad ACID");
    }
    let g = |o: usize| u32le(aci, o).unwrap();
    let services = parse_sac(sub(aci, g(0x28), g(0x2C))?)?;
    let kernel = parse_kac(sub(aci, g(0x30), g(0x34))?);
    let fs = sub(aci, g(0x20), g(0x24))?;
    let acid_flags = u32le(acid, 0x20C).unwrap();
    Ok(Npdm {
        name: cstr(&d[0x20..0x30]),
        product_code: cstr(&d[0x30..0x40]),
        is_64bit: flags & 1 != 0,
        address_space: (flags >> 1) & 7,
        main_thread_priority: d[0xE],
        main_thread_core: d[0xF],
        system_resource_size: u32le(d, 0x14).unwrap(),
        version: u32le(d, 0x18).unwrap(),
        main_thread_stack_size: u32le(d, 0x1C).unwrap(),
        title_id: u64le(aci, 0x10).unwrap(),
        acid_flags,
        acid_production: acid_flags & 1 != 0,
        memory_region: ((acid_flags >> 2) & 0xF) as u8,
        fs_permissions: u64le(fs, 4).unwrap_or(0),
        services,
        kernel,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn put(v: &mut Vec<u8>, at: usize, b: &[u8]) {
        if v.len() < at + b.len() {
            v.resize(at + b.len(), 0);
        }
        v[at..at + b.len()].copy_from_slice(b);
    }

    fn sac() -> Vec<u8> {
        let mut s = vec![];
        for (n, host) in [("fsp-srv", false), ("sm:", false), ("hid", true)] {
            s.push((n.len() as u8 - 1) | if host { 0x80 } else { 0 });
            s.extend_from_slice(n.as_bytes());
        }
        s
    }

    fn kac() -> Vec<u8> {
        let mut w: Vec<u32> = vec![];
        w.push(0x7 | (59 << 4) | (28 << 10) | (0 << 16) | (3 << 24)); // threads
        w.push(0xF | (0b11 << 5) | (1 << 29)); // svc 24, 25
        w.push(0xFFFF_FFFF);
        w.push(0x3F | (0x1234 << 7)); // memory map, start
        w.push(0x3F | (0x10 << 7) | (1 << 31)); // memory map, size
        w.push(0x3FF | (0x2 << 11)); // region map (10 ones)
        w.push(0x3FFF | (3 << 15) | (2 << 19)); // kernel version: 14 ones, minor 3, major 2
        w.push(0x7FFF | (0x200 << 16)); // handle table 512 (15 ones)
        w.push(0xFFFF | (1 << 17)); // debug flags (16 ones): allow debug
        w.iter().flat_map(|x| x.to_le_bytes()).collect()
    }

    pub fn synthetic() -> Vec<u8> {
        let (sac, kac) = (sac(), kac());
        let mut aci = vec![0u8; 0x40];
        put(&mut aci, 0, b"ACI0");
        put(&mut aci, 0x10, &0x0100_0000_0000_1234u64.to_le_bytes());
        let fs_off = 0x40u32;
        put(&mut aci, 0x20, &fs_off.to_le_bytes());
        put(&mut aci, 0x24, &0x20u32.to_le_bytes());
        put(&mut aci, 0x40, &[0u8; 0x20]);
        put(&mut aci, 0x44, &7u64.to_le_bytes());
        let sac_off = aci.len() as u32;
        put(&mut aci, 0x28, &sac_off.to_le_bytes());
        put(&mut aci, 0x2C, &(sac.len() as u32).to_le_bytes());
        let l = aci.len();
        put(&mut aci, l, &sac);
        let kac_off = aci.len() as u32;
        put(&mut aci, 0x30, &kac_off.to_le_bytes());
        put(&mut aci, 0x34, &(kac.len() as u32).to_le_bytes());
        let l = aci.len();
        put(&mut aci, l, &kac);
        let mut acid = vec![0u8; 0x240];
        put(&mut acid, 0x200, b"ACID");
        put(&mut acid, 0x20C, &(1u32 | (1 << 2)).to_le_bytes());
        let mut d = vec![0u8; 0x80];
        put(&mut d, 0, b"META");
        d[0xC] = 0b0000_0111; // 64-bit, address space 3
        d[0xE] = 44;
        d[0xF] = 3;
        put(&mut d, 0x14, &0x1000u32.to_le_bytes());
        put(&mut d, 0x1C, &0x100000u32.to_le_bytes());
        put(&mut d, 0x20, b"Application");
        let aci_off = d.len() as u32;
        put(&mut d, 0x70, &aci_off.to_le_bytes());
        put(&mut d, 0x74, &(aci.len() as u32).to_le_bytes());
        let l = d.len();
        put(&mut d, l, &aci);
        let acid_off = d.len() as u32;
        put(&mut d, 0x78, &acid_off.to_le_bytes());
        put(&mut d, 0x7C, &(acid.len() as u32).to_le_bytes());
        let l = d.len();
        put(&mut d, l, &acid);
        d
    }

    #[test]
    fn parses_synthetic() {
        let n = parse_npdm(&synthetic()).unwrap();
        assert_eq!((n.name.as_str(), n.is_64bit, n.address_space, n.main_thread_priority, n.main_thread_core), ("Application", true, 3, 44, 3));
        assert_eq!((n.main_thread_stack_size, n.system_resource_size, n.title_id), (0x100000, 0x1000, 0x0100_0000_0000_1234));
        assert!(n.acid_production);
        assert_eq!(n.memory_region, 1);
        assert_eq!(n.fs_permissions, 7);
        let s: Vec<(&str, bool)> = n.services.iter().map(|s| (s.name.as_str(), s.host)).collect();
        assert_eq!(s, [("fsp-srv", false), ("sm:", false), ("hid", true)]);
        let k = &n.kernel;
        assert_eq!(k.allowed_svcs, [24, 25]);
        assert_eq!(k.thread_info, Some((59, 28, 0, 3)));
        assert_eq!(k.memory_maps, [(0x1234000, 0x10000, false, true)]);
        assert_eq!(k.region_maps, [(2, false)]);
        assert_eq!(k.kernel_version, Some((2, 3)));
        assert_eq!(k.handle_table_size, Some(0x200));
        assert!(k.enable_debug && !k.force_debug);
    }

    #[test]
    fn rejects_hostile() {
        assert!(parse_npdm(b"nope").is_err());
        let mut d = synthetic();
        d[0x74..0x78].copy_from_slice(&0xFFFF_FFF0u32.to_le_bytes());
        assert!(parse_npdm(&d).is_err());
        assert!(parse_sac(&[0x07, b'a']).is_err()); // truncated
        assert!(parse_sac(&[0x00, 0x01]).is_err()); // non-printable
        let mut d = synthetic();
        d[0] = b'X';
        assert!(parse_npdm(&d).is_err());
    }
}
