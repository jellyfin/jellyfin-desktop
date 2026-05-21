//! Per-item playback position cache that survives SIGKILL.
//!
//! Writes are synchronous, atomic (tmp + rename), and `fsync`'d both on the
//! data file and the containing directory. The file is rewritten in full on
//! every save — fine because we update at ~1 Hz and the file is small.
//!
//! On-disk schema:
//! ```json
//! {
//!   "items": {
//!     "<jellyfin-item-id>": { "pos_us": 12345678, "ts": 1716000000 }
//!   }
//! }
//! ```
//!
//! Entries older than [`MAX_AGE_SECS`] are ignored on load and pruned on save
//! (covers "user came back next week and clicked Play from beginning"). The
//! map is capped at [`MAX_ITEMS`] with oldest-`ts` eviction.

use serde_json::{Map, Value, json};
use std::ffi::{CStr, c_char};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

const MAX_AGE_SECS: u64 = 7 * 24 * 60 * 60; // 7 days
const MAX_ITEMS: usize = 256;

static PATH: Mutex<Option<PathBuf>> = Mutex::new(None);

fn now_unix() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

fn read_items(path: &Path) -> Map<String, Value> {
    let Ok(text) = fs::read_to_string(path) else {
        return Map::new();
    };
    let Ok(v) = serde_json::from_str::<Value>(&text) else {
        return Map::new();
    };
    v.get("items")
        .and_then(Value::as_object)
        .cloned()
        .unwrap_or_default()
}

fn write_atomic(path: &Path, items: &Map<String, Value>) -> std::io::Result<()> {
    let dir = path.parent().unwrap_or(Path::new("."));
    fs::create_dir_all(dir)?;
    let tmp = dir.join(format!(
        ".{}.tmp",
        path.file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_else(|| "playback-position".into())
    ));
    let payload = json!({ "items": Value::Object(items.clone()) });
    let text = serde_json::to_string(&payload).unwrap_or_else(|_| "{}".into());
    {
        let mut f = fs::File::create(&tmp)?;
        f.write_all(text.as_bytes())?;
        f.sync_all()?; // flush data + metadata to disk before rename
    }
    fs::rename(&tmp, path)?;
    // fsync the directory so the rename itself is durable across SIGKILL.
    if let Ok(d) = fs::File::open(dir) {
        let _ = d.sync_all();
    }
    Ok(())
}

fn prune(items: &mut Map<String, Value>) {
    let now = now_unix();
    items.retain(|_, v| {
        v.get("ts")
            .and_then(Value::as_u64)
            .map(|ts| now.saturating_sub(ts) <= MAX_AGE_SECS)
            .unwrap_or(false)
    });
    while items.len() > MAX_ITEMS {
        // Evict the oldest `ts`.
        let Some(oldest) = items
            .iter()
            .min_by_key(|(_, v)| v.get("ts").and_then(Value::as_u64).unwrap_or(0))
            .map(|(k, _)| k.clone())
        else {
            break;
        };
        items.remove(&oldest);
    }
}

/// Set the cache file path. Idempotent; only the first call sticks.
///
/// # Safety
/// `path` must be a valid NUL-terminated C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_playback_position_init(path: *const c_char) {
    if path.is_null() {
        return;
    }
    let s = unsafe { CStr::from_ptr(path) }.to_string_lossy().into_owned();
    let mut p = PATH.lock().unwrap();
    if p.is_none() {
        *p = Some(PathBuf::from(s));
    }
}

fn path_clone() -> Option<PathBuf> {
    PATH.lock().unwrap().clone()
}

fn save_at(path: &Path, item_id: &str, position_us: i64) {
    if item_id.is_empty() || position_us <= 0 {
        return;
    }
    let mut items = read_items(path);
    items.insert(
        item_id.into(),
        json!({ "pos_us": position_us, "ts": now_unix() }),
    );
    prune(&mut items);
    let _ = write_atomic(path, &items);
}

fn load_at(path: &Path, item_id: &str) -> Option<i64> {
    if item_id.is_empty() {
        return None;
    }
    let items = read_items(path);
    let entry = items.get(item_id)?;
    let ts = entry.get("ts").and_then(Value::as_u64)?;
    if now_unix().saturating_sub(ts) > MAX_AGE_SECS {
        return None;
    }
    entry.get("pos_us").and_then(Value::as_i64)
}

fn clear_at(path: &Path, item_id: &str) {
    if item_id.is_empty() {
        return;
    }
    let mut items = read_items(path);
    if items.remove(item_id).is_none() {
        return;
    }
    let _ = write_atomic(path, &items);
}

/// Save the current position for `item_id`. Best-effort: silent on errors,
/// no-op if [`jfn_playback_position_init`] hasn't been called yet.
pub fn save(item_id: &str, position_us: i64) {
    if let Some(path) = path_clone() {
        save_at(&path, item_id, position_us);
    }
}

/// Return the cached position for `item_id` if present and fresh, else None.
pub fn load(item_id: &str) -> Option<i64> {
    path_clone().and_then(|p| load_at(&p, item_id))
}

/// Drop the cached entry for `item_id` (called on clean EOF).
pub fn clear(item_id: &str) {
    if let Some(path) = path_clone() {
        clear_at(&path, item_id);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU32, Ordering};

    static SEQ: AtomicU32 = AtomicU32::new(0);

    fn tmp_path() -> PathBuf {
        let n = SEQ.fetch_add(1, Ordering::Relaxed);
        let dir = std::env::temp_dir().join(format!(
            "jfn-pos-{}-{}",
            std::process::id(),
            n
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir.join("playback-position.json")
    }

    #[test]
    fn save_and_load_roundtrip() {
        let p = tmp_path();
        save_at(&p, "itemA", 12_345_678);
        assert_eq!(load_at(&p, "itemA"), Some(12_345_678));
    }

    #[test]
    fn load_returns_none_for_unknown_item() {
        let p = tmp_path();
        save_at(&p, "itemA", 1_000_000);
        assert_eq!(load_at(&p, "itemB"), None);
    }

    #[test]
    fn clear_removes_entry() {
        let p = tmp_path();
        save_at(&p, "itemA", 5_000_000);
        clear_at(&p, "itemA");
        assert_eq!(load_at(&p, "itemA"), None);
    }

    #[test]
    fn save_ignores_empty_or_nonpositive() {
        let p = tmp_path();
        save_at(&p, "", 1_000_000);
        save_at(&p, "itemA", 0);
        save_at(&p, "itemA", -42);
        assert_eq!(load_at(&p, ""), None);
        assert_eq!(load_at(&p, "itemA"), None);
    }

    #[test]
    fn newer_save_overwrites_older() {
        let p = tmp_path();
        save_at(&p, "itemA", 1_000_000);
        save_at(&p, "itemA", 9_000_000);
        assert_eq!(load_at(&p, "itemA"), Some(9_000_000));
    }

    #[test]
    fn prune_evicts_when_over_cap() {
        let p = tmp_path();
        let mut items = Map::new();
        for i in 0..MAX_ITEMS {
            items.insert(
                format!("filler-{i}"),
                json!({ "pos_us": i as i64 + 1, "ts": now_unix() - 1 }),
            );
        }
        write_atomic(&p, &items).unwrap();
        save_at(&p, "newcomer", 42_000_000);
        let after = read_items(&p);
        assert!(after.contains_key("newcomer"));
        assert!(after.len() <= MAX_ITEMS);
    }

    #[test]
    fn stale_entry_is_ignored_on_load() {
        let p = tmp_path();
        let mut items = Map::new();
        items.insert(
            "old".into(),
            json!({ "pos_us": 1_000_000, "ts": now_unix() - MAX_AGE_SECS - 1 }),
        );
        write_atomic(&p, &items).unwrap();
        assert_eq!(load_at(&p, "old"), None);
    }
}
