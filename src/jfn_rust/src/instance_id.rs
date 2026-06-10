//! Stable per-config-directory instance id, read from (or created in)
//! `instance.json` and handed to the platform single-instance gate.

use std::sync::OnceLock;

static INSTANCE_ID: OnceLock<String> = OnceLock::new();

pub fn instance_id() -> String {
    INSTANCE_ID.get_or_init(load_or_create_instance_id).clone()
}

fn load_or_create_instance_id() -> String {
    let path = jfn_paths::config_dir().join("instance.json");
    if let Some(id) = read_instance_id(&path) {
        return id;
    }

    let id = new_instance_id();
    let value = serde_json::json!({ "instanceId": &id });
    let Ok(bytes) = serde_json::to_vec_pretty(&value) else {
        return id;
    };

    match jfn_paths::write_atomic_noclobber(&path, &bytes) {
        Ok(true) => id,
        Ok(false) => read_instance_id(&path).unwrap_or(id),
        Err(_) => read_instance_id(&path).unwrap_or(id),
    }
}

fn read_instance_id(path: &std::path::Path) -> Option<String> {
    let text = std::fs::read_to_string(path).ok()?;
    let value: serde_json::Value = serde_json::from_str(&text).ok()?;
    let id = value.get("instanceId")?.as_str()?;
    sanitize_instance_id(id)
}

fn sanitize_instance_id(id: &str) -> Option<String> {
    let clean: String = id
        .chars()
        .filter(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_')
        .take(64)
        .collect();
    if clean.is_empty() { None } else { Some(clean) }
}

fn new_instance_id() -> String {
    uuid::Uuid::new_v4().simple().to_string()
}
