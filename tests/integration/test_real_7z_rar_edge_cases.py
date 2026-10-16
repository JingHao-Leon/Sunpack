import pytest

from sunpack.core.contracts.failures import FailureKind
from tests.helpers.failure_assertions import assert_failure_kind
from tests.helpers.marker_utils import marker_was_extracted
from tests.helpers.pipeline_engine import execute_pipeline
from tests.helpers.real_archives import ArchiveFixtureFactory
from tests.helpers.tool_config import get_optional_rar
from tests.real.plan1_real_archives.plan1_support import plan1_config


@pytest.mark.parametrize("sfx", [False, True])
@pytest.mark.parametrize("encrypted", [False, True])
def test_real_7z_missing_tail_reports_missing_volume(tmp_path, sfx, encrypted):
    case = ArchiveFixtureFactory().create(
        tmp_path, "seven_missing_tail", "7z", split=True,
        sfx=sfx, password="correct" if encrypted else None, payload_size=320 * 1024,
    )
    _remove_current_tail_volume(case)
    _assert_missing_from_real_discovery(case, tmp_path / "out", ["correct"] if encrypted else [])


def test_real_rar_sfx_missing_tail_reports_missing_volume_when_rar_available(tmp_path):
    if get_optional_rar() is None:
        pytest.skip("RAR generator is not configured")
    case = ArchiveFixtureFactory().create(
        tmp_path, "rar_sfx_missing_tail", "rar", split=True, sfx=True, payload_size=320 * 1024,
    )
    _remove_current_tail_volume(case)
    _assert_missing_from_real_discovery(case, tmp_path / "out", [])


def _assert_missing_from_real_discovery(case, output, passwords):
    # Missing-volume evidence and the descriptor must come from native analysis,
    # not a handcrafted task which bypasses the failing production path.
    config = plan1_config(passwords=passwords)
    config["output"] = {"root": str(output)}
    summary = execute_pipeline(config, str(case.archive_dir))
    assert summary.success_count == summary.partial_success_count == 0
    assert len(summary.failed_tasks) == 1
    assert_failure_kind(summary.failures, FailureKind.MISSING_VOLUME)
    assert not marker_was_extracted(output, case.marker_name, case.marker_text)


def _remove_current_tail_volume(case):
    parts = sorted(path for path in case.archive_dir.iterdir() if path.is_file() and path != case.entry_path)
    assert parts, "fixture must contain a removable tail volume"
    parts[-1].unlink()
