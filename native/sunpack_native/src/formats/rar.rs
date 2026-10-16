/// Data following one RAR4 header. LONG_BLOCK stores its low word at offset
/// seven; large file headers extend it with HIGH_PACK_SIZE at offset 32.
/// Callers supply the complete declared header, including decrypted headers.
pub(crate) fn rar4_data_size(header: &[u8]) -> Option<u64> {
    let flags = u16::from_le_bytes(header.get(3..5)?.try_into().ok()?);
    if flags & 0x8000 == 0 {
        return Some(0);
    }
    let low = u32::from_le_bytes(header.get(7..11)?.try_into().ok()?) as u64;
    let high = if header[2] == 0x74 && flags & 0x0100 != 0 {
        // Both high size words belong to the fixed large-file header.
        let large = header.get(32..40)?;
        u64::from(u32::from_le_bytes(large[..4].try_into().ok()?)) << 32
    } else {
        0
    };
    Some(high | low)
}
