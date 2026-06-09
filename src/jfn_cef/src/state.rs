//! Process-lifetime state for the browser process. The configuration
//! setters (log_severity, platform_switches, etc.) write here between
//! `Start()` and `Initialize()`; the App handlers read here.

use parking_lot::Mutex;

pub struct PendingSwitch {
    pub name: String,
    pub value: Option<String>,
}

impl PendingSwitch {
    pub fn flag(name: &str) -> Self {
        Self {
            name: name.into(),
            value: None,
        }
    }

    pub fn with_value(name: &str, value: &str) -> Self {
        Self {
            name: name.into(),
            value: Some(value.into()),
        }
    }
}

#[derive(Default)]
pub struct Config {
    pub log_severity: i32,
    pub remote_debugging_port: i32,
    pub pending_switches: Vec<PendingSwitch>,
    pub on_context_initialized: Option<extern "C" fn()>,
}

static CONFIG: Mutex<Config> = Mutex::new(Config {
    log_severity: 0,
    remote_debugging_port: 0,
    pending_switches: Vec::new(),
    on_context_initialized: None,
});

pub fn with_config<R>(f: impl FnOnce(&mut Config) -> R) -> R {
    let mut g = CONFIG.lock();
    f(&mut g)
}

pub fn snapshot_switches() -> Vec<PendingSwitch> {
    with_config(|c| {
        c.pending_switches
            .iter()
            .map(|s| PendingSwitch {
                name: s.name.clone(),
                value: s.value.clone(),
            })
            .collect()
    })
}
