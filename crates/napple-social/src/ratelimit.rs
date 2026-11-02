//! In-memory token buckets keyed by IP, account or profile. Single-process service, so memory is enough; buckets
//! that have refilled are pruned by the sweeper.

use std::collections::HashMap;
use std::sync::Mutex;

use crate::error::ApiError;

pub struct RateLimiter {
    buckets: Mutex<HashMap<String, Bucket>>,
    capacity: f64,
    refill_per_ms: f64,
}

struct Bucket {
    tokens: f64,
    last_ms: i64,
}

impl RateLimiter {
    /// `capacity` tokens, refilled at `per_window` tokens per `window_ms`.
    pub fn new(capacity: u32, per_window: u32, window_ms: i64) -> Self {
        RateLimiter {
            buckets: Mutex::new(HashMap::new()),
            capacity: capacity.max(1) as f64,
            refill_per_ms: per_window.max(1) as f64 / window_ms.max(1) as f64,
        }
    }

    pub fn per_minute(rate: u32) -> Self {
        Self::new((rate / 2).max(1), rate, 60_000)
    }

    /// Takes one token, or returns 429 with a Retry-After.
    pub fn check(&self, key: &str, now_ms: i64) -> Result<(), ApiError> {
        let mut map = self.buckets.lock().unwrap_or_else(|e| e.into_inner());
        let b = map.entry(key.to_string()).or_insert(Bucket { tokens: self.capacity, last_ms: now_ms });
        let elapsed = (now_ms - b.last_ms).max(0) as f64;
        b.tokens = (b.tokens + elapsed * self.refill_per_ms).min(self.capacity);
        b.last_ms = now_ms;
        if b.tokens >= 1.0 {
            b.tokens -= 1.0;
            Ok(())
        } else {
            let wait_ms = ((1.0 - b.tokens) / self.refill_per_ms).ceil() as u64;
            Err(ApiError::too_many(wait_ms.div_ceil(1000).max(1)))
        }
    }

    /// Drops buckets that are full again (nothing to remember).
    pub fn prune(&self, now_ms: i64) {
        let mut map = self.buckets.lock().unwrap_or_else(|e| e.into_inner());
        let (cap, rate) = (self.capacity, self.refill_per_ms);
        map.retain(|_, b| b.tokens + (now_ms - b.last_ms).max(0) as f64 * rate < cap);
    }

    pub fn len(&self) -> usize {
        self.buckets.lock().unwrap_or_else(|e| e.into_inner()).len()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bucket_drains_and_refills() {
        let rl = RateLimiter::new(3, 1, 1000);
        let t = 1_000_000;
        for _ in 0..3 {
            rl.check("a", t).unwrap();
        }
        let e = rl.check("a", t).unwrap_err();
        assert_eq!(e.status.as_u16(), 429);
        assert_eq!(e.retry_after_secs, Some(1));
        rl.check("b", t).unwrap();
        rl.check("a", t + 1000).unwrap();
        assert!(rl.check("a", t + 1000).is_err());
        rl.prune(t + 10_000);
        assert!(rl.is_empty());
    }
}
