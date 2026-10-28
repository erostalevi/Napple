//! NCA header-aware mutator: decrypts the NCA header sectors with `header_key`, mutates the structural
//! fields, re-encrypts (so the fields behind the XTS layer are reached). Included with #[path].
use super::mutlib::{HUGE, Rng, mutate};

/// Mutates the DECRYPTED NCA header (so the structural fields behind the XTS layer are reached) and
/// re-encrypts it; the body is mutated in the clear as usual.
pub fn mutate_nca_bytes(seed: &[u8], hk: [u8; 32], r: &mut Rng) -> Vec<u8> {
    use aes::Aes128;
    use aes::cipher::{KeyInit, generic_array::GenericArray};
    use xts_mode::Xts128;
    let xts = Xts128::<Aes128>::new(
        Aes128::new(GenericArray::from_slice(&hk[..16])),
        Aes128::new(GenericArray::from_slice(&hk[16..])),
    );
    let mut v = seed.to_vec();
    if v.len() < 0xC00 {
        return mutate(seed, 0x4000, r);
    }
    let sector_of = |ver3: bool, i: usize| if ver3 || i < 2 { i as u128 } else { 0 };
    let sectors_dec = |v: &mut [u8], ver3: bool, enc: bool| {
        for i in 0..6 {
            let sec = &mut v[i * 0x200..(i + 1) * 0x200];
            let n = sector_of(ver3, i).to_be_bytes();
            if enc {
                xts.encrypt_sector(sec, n);
            } else {
                xts.decrypt_sector(sec, n);
            }
        }
    };
    let ver3 = {
        let mut probe = v[..0x400].to_vec();
        for i in 0..2 {
            xts.decrypt_sector(
                &mut probe[i * 0x200..(i + 1) * 0x200],
                (i as u128).to_be_bytes(),
            );
        }
        &probe[0x200..0x204] == b"NCA3"
    };
    sectors_dec(&mut v, ver3, false);
    for _ in 0..1 + r.below(4) {
        let at = r.below(0xC00);
        match r.below(4) {
            0 => v[at] ^= 1 << r.below(8),
            1 => v[at] = r.next() as u8,
            2 => {
                let h = HUGE[r.below(HUGE.len())].to_le_bytes();
                let at = at & !7;
                v[at..at + 8].copy_from_slice(&h);
            }
            _ => {
                let at = at & !3;
                v[at..at + 4].copy_from_slice(&(HUGE[r.below(HUGE.len())] as u32).to_le_bytes());
            }
        }
    }
    sectors_dec(&mut v, ver3, true);
    if r.below(5) == 0 {
        let cut = r.below(v.len() + 1);
        v.truncate(cut);
    }
    v
}
