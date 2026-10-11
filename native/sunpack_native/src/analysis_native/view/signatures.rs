struct SignaturePrepassData {
    hits: Vec<(&'static str, u64)>,
    formats: Vec<&'static str>,
    scanned_head: usize,
    scanned_tail: usize,
}

fn signature_prepass_native(
    reader: &ManagedReader,
    head_bytes: usize,
    tail_bytes: usize,
) -> io::Result<SignaturePrepassData> {
    let size = reader.len();
    let head_len = head_bytes.min(size as usize);
    let tail_len = tail_bytes.min(size as usize);
    let tail_start = size.saturating_sub(tail_len as u64);
    let mut hits = Vec::new();
    if tail_start <= head_len as u64 {
        let data = reader.read_cached_at(0, size as usize)?;
        collect_signature_hits(&mut hits, 0, data.as_slice());
    } else {
        let ranges = reader.read_many(&[(0, head_len), (tail_start, tail_len)])?;
        collect_signature_hits(&mut hits, 0, &ranges[0]);
        collect_signature_hits(&mut hits, tail_start, &ranges[1]);
    }
    hits.sort_unstable_by_key(|(_, offset)| *offset);
    hits.dedup();
    let mut formats = Vec::new();
    for (name, _) in &hits {
        let format = format_for_hit(name);
        if !formats.contains(&format) {
            formats.push(format);
        }
    }
    formats.sort_unstable();
    Ok(SignaturePrepassData {
        hits,
        formats,
        scanned_head: head_len,
        scanned_tail: tail_len,
    })
}

fn signature_prepass_to_python(py: Python<'_>, data: SignaturePrepassData) -> PyResult<Py<PyDict>> {
    let dict = PyDict::new(py);
    let hits = PyList::empty(py);
    for (name, offset) in data.hits {
        let hit = PyDict::new(py);
        hit.set_item("name", name)?;
        hit.set_item("offset", offset)?;
        hits.append(hit)?;
    }
    dict.set_item("hits", hits)?;
    dict.set_item("formats", data.formats)?;
    dict.set_item("head_bytes", data.scanned_head)?;
    dict.set_item("tail_bytes", data.scanned_tail)?;
    Ok(dict.unbind())
}

fn collect_signature_hits(target: &mut Vec<(&'static str, u64)>, base: u64, data: &[u8]) {
    if data.is_empty() {
        return;
    }
    for signatures in [
        ANALYSIS_SIGNATURES_P,
        ANALYSIS_SIGNATURES_R,
        ANALYSIS_SIGNATURES_7,
        ANALYSIS_SIGNATURES_GZIP,
        ANALYSIS_SIGNATURES_BZIP2,
        ANALYSIS_SIGNATURES_XZ,
        ANALYSIS_SIGNATURES_ZSTD,
        ANALYSIS_SIGNATURES_LZ4,
        ANALYSIS_SIGNATURES_TAR,
    ] {
        for &(name, signature) in signatures {
            let finder = memchr::memmem::Finder::new(signature);
            let mut start = 0;
            while let Some(index) = finder.find(&data[start..]) {
                let index = start + index;
                target.push((name, base + index as u64));
                // Preserve overlapping signatures. The caller orders and
                // deduplicates both windows once after collection.
                start = index + 1;
            }
        }
    }
}

fn format_for_hit(name: &str) -> &'static str {
    if name.starts_with("zip_") {
        "zip"
    } else if name.starts_with("rar") {
        "rar"
    } else if name == "7z" {
        "7z"
    } else if name == "tar_ustar" {
        "tar"
    } else if name == "gzip" {
        "gzip"
    } else if name == "bzip2" {
        "bzip2"
    } else if name == "xz" {
        "xz"
    } else if name == "zstd" {
        "zstd"
    } else if name == "lz4" {
        "lz4"
    } else {
        ""
    }
}

#[cfg(test)]
mod signature_search_tests {
    use super::*;

    // Public window tests cover only some signatures. Check every name/offset
    // against a byte-by-byte oracle, including short and truncated edges.
    #[test]
    fn signature_byte_search_keeps_all_names_offsets_and_short_edges() {
        let patterns = [
            ANALYSIS_SIGNATURES_P, ANALYSIS_SIGNATURES_R, ANALYSIS_SIGNATURES_7,
            ANALYSIS_SIGNATURES_GZIP, ANALYSIS_SIGNATURES_BZIP2, ANALYSIS_SIGNATURES_XZ,
            ANALYSIS_SIGNATURES_ZSTD, ANALYSIS_SIGNATURES_LZ4, ANALYSIS_SIGNATURES_TAR,
        ].into_iter().flatten().copied().collect::<Vec<_>>();
        let mut data = (0..65536usize).map(|i| (i * 73 + 19) as u8).collect::<Vec<_>>();
        for (index, (_, signature)) in patterns.iter().enumerate() {
            let start = index * 13;
            data[start..start + signature.len()].copy_from_slice(signature);
        }
        data.extend_from_slice(b"ustarustarPK\x03\x04PK\x05\x06");
        for len in (0..=160).chain([data.len() - 1, data.len()]) {
            let sample = &data[..len];
            let mut expected = Vec::new();
            for index in 0..sample.len() {
                for &(name, signature) in &patterns {
                    if sample.get(index..index + signature.len()) == Some(signature) {
                        expected.push((name, 37 + index as u64));
                    }
                }
            }
            let mut actual = Vec::new();
            collect_signature_hits(&mut actual, 37, sample);
            actual.sort_unstable_by_key(|&(name, offset)| (offset, name));
            expected.sort_unstable_by_key(|&(name, offset)| (offset, name));
            assert_eq!(actual, expected);
        }
    }
}
