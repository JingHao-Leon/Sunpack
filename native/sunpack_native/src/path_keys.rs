//! Windows filesystem case mapping: one UTF-16 unit stays one unit. Unicode
//! text lowercasing can merge distinct names (for example İ and i + U+0307).

#[cfg(windows)]
#[link(name = "ntdll")]
extern "system" {
    fn RtlDowncaseUnicodeChar(source: u16) -> u16;
}

pub(crate) fn case_key(value: &str) -> String {
    if value.is_ascii() {
        return value.to_ascii_lowercase();
    }
    value
        .chars()
        .map(|ch| {
            #[cfg(windows)]
            if ch as u32 <= u16::MAX as u32 {
                // NT's filesystem mapping is independent of the process locale
                // and does not expand characters or fold surrogate pairs.
                return char::from_u32(unsafe { RtlDowncaseUnicodeChar(ch as u16) } as u32)
                    .unwrap_or(ch);
            }
            ch.to_ascii_lowercase()
        })
        .collect()
}

#[pyo3::pyfunction(name = "windows_path_key")]
pub(crate) fn path_key(value: &str) -> String {
    let key = case_key(value);
    if key.contains('/') {
        key.replace('/', "\\")
    } else {
        key
    }
}
