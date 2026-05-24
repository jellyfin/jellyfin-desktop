use anyhow::{Context, Result};
use std::collections::HashMap;
use std::path::Path;

/// Replace `@VAR@` substitutions in the file (mirrors CMake `configure_file(@ONLY)`).
pub fn configure_file(src: &Path, dst: &Path, vars: &HashMap<&str, String>) -> Result<()> {
    let content =
        std::fs::read_to_string(src).with_context(|| format!("read {}", src.display()))?;
    let mut out = String::with_capacity(content.len());
    let mut rest = content.as_str();
    while let Some(i) = rest.find('@') {
        out.push_str(&rest[..i]);
        let after = &rest[i + 1..];
        if let Some(j) = after.find('@') {
            let key = &after[..j];
            if vars.contains_key(key) {
                out.push_str(&vars[key]);
                rest = &after[j + 1..];
                continue;
            }
        }
        out.push('@');
        rest = after;
    }
    out.push_str(rest);
    if let Some(parent) = dst.parent() {
        std::fs::create_dir_all(parent)?;
    }
    std::fs::write(dst, out).with_context(|| format!("write {}", dst.display()))?;
    Ok(())
}
