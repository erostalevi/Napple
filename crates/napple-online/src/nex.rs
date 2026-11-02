//! NEX data types as Switch titles send them (little endian; PID = u64; structures carry a version byte and a u32 content length).

use std::fmt;

#[derive(Debug, Clone, PartialEq)]
pub struct DecodeError(pub &'static str);
impl fmt::Display for DecodeError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "decode error: {}", self.0)
    }
}
pub type Result<T> = std::result::Result<T, DecodeError>;

/// A NEX DateTime: year 63..26, month 25..22, day 21..17, hour 16..12, minute 11..6, second 5..0.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct DateTime(pub u64);
impl DateTime {
    pub fn new(y: u64, mo: u64, d: u64, h: u64, mi: u64, s: u64) -> DateTime {
        DateTime(y << 26 | mo << 22 | d << 17 | h << 12 | mi << 6 | s)
    }
    /// From Unix seconds (UTC).
    pub fn from_unix(t: u64) -> DateTime {
        let days = t / 86400;
        let rem = t % 86400;
        // civil-from-days (proleptic Gregorian)
        let z = days as i64 + 719468;
        let era = z.div_euclid(146097);
        let doe = z - era * 146097;
        let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        let mp = (5 * doy + 2) / 153;
        let d = (doy - (153 * mp + 2) / 5 + 1) as u64;
        let m = if mp < 10 { mp + 3 } else { mp - 9 } as u64;
        let y = (yoe + era * 400 + if m <= 2 { 1 } else { 0 }) as u64;
        DateTime::new(y, m, d, rem / 3600, rem / 60 % 60, rem % 60)
    }
    pub fn now() -> DateTime {
        DateTime::from_unix(crate::accounts::now_secs())
    }
    pub fn year(&self) -> u64 {
        self.0 >> 26
    }
}

#[derive(Default)]
pub struct Writer {
    pub buf: Vec<u8>,
}
impl Writer {
    pub fn new() -> Writer {
        Writer { buf: Vec::new() }
    }
    pub fn u8(&mut self, v: u8) -> &mut Self {
        self.buf.push(v);
        self
    }
    pub fn u16(&mut self, v: u16) -> &mut Self {
        self.buf.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn u32(&mut self, v: u32) -> &mut Self {
        self.buf.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn u64(&mut self, v: u64) -> &mut Self {
        self.buf.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn bool(&mut self, v: bool) -> &mut Self {
        self.u8(v as u8)
    }
    pub fn pid(&mut self, v: u64) -> &mut Self {
        self.u64(v)
    }
    pub fn bytes(&mut self, v: &[u8]) -> &mut Self {
        self.buf.extend_from_slice(v);
        self
    }
    /// String: u16 length including the NUL, the bytes, NUL.
    pub fn string(&mut self, s: &str) -> &mut Self {
        self.u16(s.len() as u16 + 1);
        self.bytes(s.as_bytes());
        self.u8(0)
    }
    pub fn buffer(&mut self, b: &[u8]) -> &mut Self {
        self.u32(b.len() as u32);
        self.bytes(b)
    }
    pub fn qbuffer(&mut self, b: &[u8]) -> &mut Self {
        self.u16(b.len() as u16);
        self.bytes(b)
    }
    pub fn datetime(&mut self, d: DateTime) -> &mut Self {
        self.u64(d.0)
    }
    pub fn result(&mut self, r: u32) -> &mut Self {
        self.u32(r)
    }
    pub fn list<T>(&mut self, items: &[T], mut f: impl FnMut(&mut Writer, &T)) -> &mut Self {
        self.u32(items.len() as u32);
        for it in items {
            f(self, it);
        }
        self
    }
    /// A structure: version byte, u32 length, then what `f` writes.
    pub fn structure(&mut self, version: u8, f: impl FnOnce(&mut Writer)) -> &mut Self {
        self.u8(version);
        let at = self.buf.len();
        self.u32(0);
        f(self);
        let len = (self.buf.len() - at - 4) as u32;
        self.buf[at..at + 4].copy_from_slice(&len.to_le_bytes());
        self
    }
    /// AnyDataHolder: type name, u32 (length + 4), u32 length, the object.
    pub fn any(&mut self, name: &str, object: &[u8]) -> &mut Self {
        self.string(name);
        self.u32(object.len() as u32 + 4);
        self.u32(object.len() as u32);
        self.bytes(object)
    }
}

pub struct Reader<'a> {
    pub data: &'a [u8],
    pub pos: usize,
}
impl<'a> Reader<'a> {
    pub fn new(data: &'a [u8]) -> Reader<'a> {
        Reader { data, pos: 0 }
    }
    pub fn remaining(&self) -> usize {
        self.data.len() - self.pos
    }
    pub fn take(&mut self, n: usize) -> Result<&'a [u8]> {
        if self.remaining() < n {
            return Err(DecodeError("short read"));
        }
        let s = &self.data[self.pos..self.pos + n];
        self.pos += n;
        Ok(s)
    }
    pub fn rest(&mut self) -> &'a [u8] {
        let s = &self.data[self.pos..];
        self.pos = self.data.len();
        s
    }
    pub fn u8(&mut self) -> Result<u8> {
        Ok(self.take(1)?[0])
    }
    pub fn u16(&mut self) -> Result<u16> {
        let b = self.take(2)?;
        Ok(u16::from_le_bytes([b[0], b[1]]))
    }
    pub fn u32(&mut self) -> Result<u32> {
        let b = self.take(4)?;
        Ok(u32::from_le_bytes(b.try_into().unwrap()))
    }
    pub fn u64(&mut self) -> Result<u64> {
        let b = self.take(8)?;
        Ok(u64::from_le_bytes(b.try_into().unwrap()))
    }
    pub fn bool(&mut self) -> Result<bool> {
        Ok(self.u8()? != 0)
    }
    pub fn pid(&mut self) -> Result<u64> {
        self.u64()
    }
    pub fn string(&mut self) -> Result<String> {
        let n = self.u16()? as usize;
        let b = self.take(n)?;
        let b = b.strip_suffix(&[0]).unwrap_or(b);
        Ok(String::from_utf8_lossy(b).into_owned())
    }
    pub fn buffer(&mut self) -> Result<Vec<u8>> {
        let n = self.u32()? as usize;
        Ok(self.take(n)?.to_vec())
    }
    pub fn qbuffer(&mut self) -> Result<Vec<u8>> {
        let n = self.u16()? as usize;
        Ok(self.take(n)?.to_vec())
    }
    pub fn datetime(&mut self) -> Result<DateTime> {
        Ok(DateTime(self.u64()?))
    }
    pub fn list<T>(&mut self, mut f: impl FnMut(&mut Reader<'a>) -> Result<T>) -> Result<Vec<T>> {
        let n = self.u32()? as usize;
        if n > 100_000 {
            return Err(DecodeError("list too long"));
        }
        let mut v = Vec::with_capacity(n.min(256));
        for _ in 0..n {
            v.push(f(self)?);
        }
        Ok(v)
    }
    /// A structure: returns (version, content reader). The outer reader moves past the whole content.
    pub fn structure(&mut self) -> Result<(u8, Reader<'a>)> {
        let v = self.u8()?;
        let n = self.u32()? as usize;
        Ok((v, Reader::new(self.take(n)?)))
    }
    /// AnyDataHolder: (type name, object bytes).
    pub fn any(&mut self) -> Result<(String, &'a [u8])> {
        let name = self.string()?;
        let _outer = self.u32()?;
        let n = self.u32()? as usize;
        Ok((name, self.take(n)?))
    }
}

/// A StationURL (`prudps:/key=value;...`), keys kept in order.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct StationUrl {
    pub scheme: String,
    pub params: Vec<(String, String)>,
}
impl StationUrl {
    pub fn parse(s: &str) -> StationUrl {
        let (scheme, rest) = s.split_once(":/").unwrap_or((s, ""));
        let params = rest
            .split(';')
            .filter(|p| !p.is_empty())
            .map(|p| {
                let (k, v) = p.split_once('=').unwrap_or((p, ""));
                (k.to_string(), v.to_string())
            })
            .collect();
        StationUrl { scheme: scheme.to_string(), params }
    }
    pub fn get(&self, k: &str) -> Option<&str> {
        self.params.iter().find(|(kk, _)| kk == k).map(|(_, v)| v.as_str())
    }
    pub fn set(&mut self, k: &str, v: impl ToString) {
        match self.params.iter_mut().find(|(kk, _)| kk == k) {
            Some(p) => p.1 = v.to_string(),
            None => self.params.push((k.to_string(), v.to_string())),
        }
    }
}
impl fmt::Display for StationUrl {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{}:/", self.scheme)?;
        for (i, (k, v)) in self.params.iter().enumerate() {
            if i > 0 {
                write!(f, ";")?;
            }
            write!(f, "{k}={v}")?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn primitives_roundtrip() {
        let mut w = Writer::new();
        w.u8(1).u16(2).u32(3).u64(4).bool(true).string("abc").buffer(&[9, 8]).qbuffer(&[7]).datetime(DateTime(5));
        w.list(&[10u32, 20], |w, v| {
            w.u32(*v);
        });
        w.structure(1, |w| {
            w.string("in");
        });
        w.any("Name", &[1, 2, 3]);
        assert_eq!(&w.buf[16..22], &[4, 0, b'a', b'b', b'c', 0]);
        let mut r = Reader::new(&w.buf);
        assert_eq!((r.u8().unwrap(), r.u16().unwrap(), r.u32().unwrap(), r.u64().unwrap(), r.bool().unwrap()), (1, 2, 3, 4, true));
        assert_eq!(r.string().unwrap(), "abc");
        assert_eq!(r.buffer().unwrap(), vec![9, 8]);
        assert_eq!(r.qbuffer().unwrap(), vec![7]);
        assert_eq!(r.datetime().unwrap(), DateTime(5));
        assert_eq!(r.list(|r| r.u32()).unwrap(), vec![10, 20]);
        let (v, mut s) = r.structure().unwrap();
        assert_eq!((v, s.string().unwrap()), (1, "in".to_string()));
        let (n, o) = r.any().unwrap();
        assert_eq!((n.as_str(), o), ("Name", &[1u8, 2, 3][..]));
        assert_eq!(r.remaining(), 0);
        assert!(r.u8().is_err());
    }

    #[test]
    fn datetime_fields() {
        let d = DateTime::from_unix(1_700_000_000); // 2023-11-14 22:13:20 UTC
        assert_eq!(d, DateTime::new(2023, 11, 14, 22, 13, 20));
        assert_eq!(DateTime::from_unix(0), DateTime::new(1970, 1, 1, 0, 0, 0));
        assert_eq!(DateTime::from_unix(951_782_400), DateTime::new(2000, 2, 29, 0, 0, 0));
    }

    #[test]
    fn station_url() {
        let mut u = StationUrl::parse("prudps:/address=0.0.0.1;port=1;CID=1;PID=2;sid=2;stream=10;type=2");
        assert_eq!(u.scheme, "prudps");
        assert_eq!(u.get("sid"), Some("2"));
        u.set("address", "10.0.0.5");
        u.set("RVCID", 7);
        assert_eq!(u.to_string(), "prudps:/address=10.0.0.5;port=1;CID=1;PID=2;sid=2;stream=10;type=2;RVCID=7");
    }
}
