from __future__ import annotations

import asyncio
import io
import struct
import zipfile
import zlib
from pathlib import Path

import pytest

from sunpack.pipeline.coordinator.engine import PipelineEngine
from tests.helpers.config_factory import make_config


def _config(output: Path, **extra):
    return make_config({
        "output": {"root": str(output)},
        "recursive_extract": 1,
        "post_extract": {"archive_cleanup_mode": "k", "flatten_single_directory": False},
        **extra,
    })


async def _run(config, path: Path, origin: str):
    async with PipelineEngine(config) as engine:
        return await engine.run([str(path)], origin=origin, stdout=io.StringIO(), stderr=io.StringIO())


@pytest.mark.parametrize("origin", ["foreground", "watch"])
def test_unproven_carrier_header_cannot_hide_a_real_archive(tmp_path, origin):
    # Unique coverage: ordinary carrier tests have no competing, CRC-valid
    # start header. Exercise real discovery through extraction, rather than
    # injecting an impossible prepass. A tiny ZIP covers the competing boundary failures.
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_STORED) as archive:
        archive.writestr("payload.txt", b"real independent archive")
    payload = buffer.getvalue()
    for case, next_header, next_crc in (
        ("bad_crc", b"\x01\x00", 12345),
        ("bad_nid", b"\x7f", zlib.crc32(b"\x7f")),
        ("absent_header", b"", 0),
    ):
        fields = struct.pack("<QQI", len(payload), len(next_header), next_crc)
        fake = b"7z\xbc\xaf\x27\x1c\x00\x04" + struct.pack("<I", zlib.crc32(fields)) + fields
        carrier = tmp_path / f"{case}.opaque"
        carrier.write_bytes(b"prefix" + fake + payload + next_header + b"tail")
        output = tmp_path / f"out-{case}"
        response = asyncio.run(_run(_config(output), carrier, origin))
        assert response.summary.success_count == 1, response.summary.failed_tasks
        assert [path.read_bytes() for path in output.rglob("payload.txt")] == [b"real independent archive"]


