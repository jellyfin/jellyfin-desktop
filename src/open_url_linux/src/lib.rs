//! Spawn `xdg-open <url>` detached. Caller ensures the URL is non-empty and
//! doesn't start with '-'.

use std::process::{Command, Stdio};
use std::thread;

pub fn open(url: &str) {
    let child = Command::new("xdg-open")
        .arg(url)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn();

    match child {
        Ok(mut child) => {
            // xdg-open exits quickly after daemonizing the real handler; reap it.
            thread::spawn(move || {
                let _ = child.wait();
            });
        }
        Err(e) => {
            tracing::error!("spawn(xdg-open) failed: {}", e);
        }
    }
}
