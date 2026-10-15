//! Output-name projection shared by source manifests and verification.
//! Keep the Windows mapping and numbered collisions aligned with the worker.
use std::borrow::Cow;
use std::collections::{HashMap, HashSet};

/// The worker rejects rooted/drive paths and traversal, but accepts ./ and
/// repeated separators. Test the raw name before normalizing its components.
pub(crate) fn unsafe_archive_path(raw: &str) -> bool {
    raw.starts_with(['/', '\\'])
        || (raw.as_bytes().get(1) == Some(&b':') && raw.as_bytes()[0].is_ascii_alphabetic())
        || raw.split(['/', '\\']).any(|part| part == "..")
}

#[derive(Default)]
pub(crate) struct OutputPathAllocator {
    used: HashSet<String>,
    next_numbers: HashMap<String, usize>,
}

impl OutputPathAllocator {
    pub(crate) fn allocate(&mut self, requested: String) -> String {
        let key = crate::path_keys::case_key(&requested);
        if self.used.insert(key.clone()) {
            return requested;
        }
        let next = self.next_numbers.entry(key).or_insert(1);
        let slash = requested.rfind('/').map_or(0, |position| position + 1);
        let dot = requested[slash..]
            .rfind('.')
            .filter(|&position| position > 0);
        let split = dot.map_or(requested.len(), |position| slash + position);
        loop {
            let candidate = format!("{}({}){}", &requested[..split], *next, &requested[split..]);
            *next += 1;
            if self.used.insert(crate::path_keys::case_key(&candidate)) {
                return candidate;
            }
        }
    }
}

pub(crate) fn clean_relative_archive_path(value: &str) -> Cow<'_, str> {
    if !value.contains('\\')
        && value
            .split('/')
            .all(|part| !part.is_empty() && part != "." && part != "..")
    {
        return Cow::Borrowed(value);
    }
    Cow::Owned(
        value
            .replace('\\', "/")
            .trim_matches('/')
            .split('/')
            .filter(|part| !part.is_empty() && *part != "." && *part != "..")
            .collect::<Vec<_>>()
            .join("/"),
    )
}

fn is_windows_invalid_name_char(value: char) -> bool {
    (value as u32) < 0x20 || matches!(value, '<' | '>' | ':' | '"' | '|' | '?' | '*')
}

/// Mirrors `is_windows_reserved_device_name` in the 7z bridge.
fn is_windows_reserved_device_name(part: &str) -> bool {
    let bytes = part.as_bytes();
    let mut stem = part.find('.').unwrap_or(bytes.len());
    while stem > 0 && bytes[stem - 1] == b' ' {
        stem -= 1;
    }
    if stem != 3 && stem != 4 {
        return false;
    }
    let prefix = [
        bytes[0].to_ascii_lowercase(),
        bytes[1].to_ascii_lowercase(),
        bytes[2].to_ascii_lowercase(),
    ];
    if stem == 3 {
        return matches!(&prefix, b"con" | b"prn" | b"aux" | b"nul");
    }
    matches!(bytes[3], b'1'..=b'9') && matches!(&prefix, b"com" | b"lpt")
}

fn windows_output_part_needs_mapping(part: &str) -> bool {
    part.chars().any(is_windows_invalid_name_char)
        || part.ends_with(['.', ' '])
        || is_windows_reserved_device_name(part)
}

/// Mirrors `make_windows_safe_path_part` in the 7z bridge.
fn windows_output_part(part: &str) -> String {
    let mut mapped: String = part
        .chars()
        .map(|value| {
            if is_windows_invalid_name_char(value) {
                '_'
            } else {
                value
            }
        })
        .collect();
    let kept = mapped.trim_end_matches(['.', ' ']).len();
    let trailing = mapped.len() - kept;
    mapped.truncate(kept);
    mapped.extend(std::iter::repeat('_').take(trailing));
    if is_windows_reserved_device_name(&mapped) {
        mapped.insert(0, '_');
    }
    mapped
}

/// Project a cleaned archive path onto the name the worker writes on disk.
/// The mapping is idempotent, so already-projected output paths pass through.
pub(crate) fn windows_output_relative_path(cleaned: Cow<'_, str>) -> Cow<'_, str> {
    if !cleaned.split('/').any(windows_output_part_needs_mapping) {
        return cleaned;
    }
    Cow::Owned(
        cleaned
            .split('/')
            .map(windows_output_part)
            .collect::<Vec<_>>()
            .join("/"),
    )
}
