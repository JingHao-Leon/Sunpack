import tarfile
import lzma
import os
import subprocess
import struct
import sys
import zipfile
from concurrent.futures import ThreadPoolExecutor
from io import BytesIO

from sunpack_native import reader_cache_stats

from sunpack.pipeline.coordinator.scan_session import DiscoveryScanSession
from sunpack.pipeline.coordinator.target_scan import build_native_table_for_targets
from tests.helpers.config_factory import make_config


def test_static_noisy_carrier_extracts_nested_and_cross_chunk_xz_with_one_rayon_worker(tmp_path):
    """Catch lost outputs across scanner/discovery/extraction boundaries.

    Existing fixtures do not combine noisy hits, a false primary stream, forged
    ZIP ownership, extended ZIP64 and a split XZ footer with one Rayon worker.
    Removing this case leaves no end-to-end check for those routing losses;
    one small carrier/child process covers both origins without extra processes.
    """
    nested = BytesIO()
    with zipfile.ZipFile(nested, "w") as writer:
        writer.writestr("payload.txt", b"nested payload")
    outer = BytesIO()
    with zipfile.ZipFile(outer, "w") as writer:
        writer.writestr("inner.opaque", nested.getvalue())
        writer.writestr("keep.txt", b"ordinary output")
    outer_bytes = outer.getvalue()
    data = b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03\x03" + b"\xff" * 16
    data += b"PK\x03\x04" * 1_000_001
    outer_start = len(data)
    data += outer_bytes
    xz = lzma.compress(b"xz payload", format=lzma.FORMAT_XZ)
    chunk = 2 * 1024 * 1024
    # Place Y before the IOCP boundary and Z after it; its CRC/flags are in carry.
    end = ((len(data) + len(xz)) // chunk + 1) * chunk + 1
    carrier = tmp_path / "carrier.opaque"
    data += b"x" * (end - len(data) - len(xz)) + xz + b"trailer"
    # This plausible EOCD reuses the real directory, but falsely extends it
    # across the XZ peer. It must never acquire ownership of that peer.
    eocd = outer_bytes.rfind(b"PK\x05\x06")
    fields = list(struct.unpack("<4s4H2IH", outer_bytes[eocd:eocd + 22]))
    fields[5] = len(data) - outer_start - fields[6]
    data += struct.pack("<4s4H2IH", *fields)
    extended = BytesIO()
    with zipfile.ZipFile(extended, "w") as writer:
        writer.writestr("zip64.txt", b"ZIP64 payload")
    archive = extended.getvalue()
    eocd = archive.rfind(b"PK\x05\x06")
    fields = list(struct.unpack("<4s4H2IH", archive[eocd:eocd + 22]))
    extension = b"\0" * 70000 + b"PK\x06\x06" + b"\0" * 64
    record = struct.pack("<4sQ2H2I4Q", b"PK\x06\x06", 44 + len(extension),
                         45, 45, 0, 0, fields[4], fields[4], fields[5], fields[6])
    locator = struct.pack("<4sIQI", b"PK\x06\x07", 0, eocd, 1)
    fields[3] = fields[4] = 65535
    fields[5] = fields[6] = 4294967295
    data += archive[:eocd] + record + extension + locator + struct.pack("<4s4H2IH", *fields)
    carrier.write_bytes(data + b"trailer")
    completed = subprocess.run(
        [sys.executable, "-c", """
import asyncio, sys
from pathlib import Path
from sunpack.pipeline.coordinator.engine import PipelineEngine
from tests.helpers.config_factory import make_config
async def run():
    for origin in ('foreground', 'watch'):
        root = Path(sys.argv[2]) / origin
        config = make_config({
            'recursive_extract': '3', 'output': {'root': str(root)},
            'user_passwords': [], 'builtin_passwords': [],
            'post_extract': {'archive_cleanup_mode': 'k', 'flatten_single_directory': False},
        })
        async with PipelineEngine(config) as engine:
            response = await engine.run([sys.argv[1]], origin=origin)
        assert not response.summary.failed_tasks, response.summary.failed_tasks
        assert response.summary.success_count == 2, response.summary
        payloads = [p.read_bytes() for p in root.rglob('*') if p.is_file()]
        assert b'nested payload' in payloads and b'xz payload' in payloads and b'ZIP64 payload' in payloads
asyncio.run(run())
""", str(carrier), str(tmp_path / "output")],
        env={**os.environ, "RAYON_NUM_THREADS": "1"},
        capture_output=True, text=True, timeout=30,
    )
    assert completed.returncode == 0, completed.stdout + completed.stderr


def test_native_discovery_does_not_project_negative_embedded_candidates(tmp_path):
    for index in range(32):
        (tmp_path / f"fake_{index:03}.zip").write_bytes(b"not an archive")

    session = DiscoveryScanSession(config=make_config({}))
    table = build_native_table_for_targets([str(tmp_path)], session=session, config=make_config({}))
    assert len(table) == 32

    routes = table.resolve(include_residual_details=False)
    assert routes.relation_events == []
    assert routes.detection_events == []
    batch = table.scan_embedded(routes, include_residual_details=False)
    assert len(batch) == 32
    assert batch.scan_page(table, 0, 32) == []


def test_native_embedded_pages_scan_on_demand(tmp_path):
    path = tmp_path / "fake.zip"
    path.write_bytes(b"not an archive")
    table = build_native_table_for_targets([str(tmp_path)], config=make_config({}))
    batch = table.scan_embedded(table.resolve(), include_residual_details=True)

    path.unlink()
    assert batch.scan_page(table, 0, 1)[0][2] == "embedded_scan_io_error"


def test_native_head_fact_cache_accepts_concurrent_reads(tmp_path):
    path = tmp_path / "payload.bin"
    payload = b"PK\x03\x04" + b"x" * 1024
    path.write_bytes(payload)
    session = DiscoveryScanSession(config=make_config({}))

    def read(magic_size):
        return session.file_head_facts_for_path(str(path), magic_size=magic_size)

    with ThreadPoolExecutor(max_workers=8) as pool:
        rows = list(pool.map(read, [0, 16] * 32))

    assert all(row["size"] == len(payload) for row in rows)
    assert all(row["magic"].startswith(b"PK\x03\x04") for row in rows[1::2])


def test_native_detection_batches_keep_order_during_concurrent_resolves(tmp_path):
    buffer = BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        entry = tarfile.TarInfo("payload.txt")
        entry.size = 1
        archive.addfile(entry, BytesIO(b"x"))
    valid = buffer.getvalue()
    invalid = bytearray(valid)
    invalid[148] ^= 1
    for index in range(1100):
        (tmp_path / f"archive-{index:04}.tar").write_bytes(valid if index % 2 == 0 else invalid)

    table = build_native_table_for_targets([str(tmp_path)], config=make_config({}))
    cache_entries_before = reader_cache_stats()["cache_entries"]
    with ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(lambda _index: table.resolve(include_residual_details=False), range(4)))

    expected = results[0].detection_resolved
    assert len(expected) == 550
    assert expected == sorted(expected)
    assert all(result.detection_resolved == expected for result in results)
    assert reader_cache_stats()["cache_entries"] == cache_entries_before
