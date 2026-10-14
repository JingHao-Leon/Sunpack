import io
import os
import subprocess
import zipfile
from contextlib import contextmanager
from types import SimpleNamespace

import pytest
from sunpack_native import NativeDirectoryScanOptions, scan_directory_snapshot

from sunpack.core.config.schema import normalize_config
from sunpack.core.contracts.failures import FailureKind
from sunpack.core.contracts.run_state import RunState
from sunpack.core.i18n import I18nContext
from sunpack.core.i18n.context import validate_catalog
from sunpack.pipeline.coordinator.output_scan_policy import NestedOutputScanPolicy
from sunpack.pipeline.coordinator.task_scan import ArchiveTaskScanner
from sunpack.runtime.cli.cli_context import CliContext
from sunpack.runtime.cli.cli_reporter import CliReporter
from sunpack.runtime.cli.commands import scan
from sunpack.runtime.watch.scheduler import WatchScheduler


@contextmanager
def deny_directory_listing(path):
    principal = os.environ["USERNAME"]
    subprocess.run(["icacls", str(path), "/deny", principal + ":(RD)"], capture_output=True, check=True)
    try:
        yield
    finally:
        subprocess.run(["icacls", str(path), "/remove:d", principal], capture_output=True, check=True)


@pytest.mark.parametrize("json_mode", [False, True])
@pytest.mark.parametrize("language", ["en", "zh"])
def test_unreadable_subtree_is_reported_without_losing_readable_archives(tmp_path, monkeypatch, json_mode, language):
    # Covers real native I/O failures through discovery, CLI and Watch reporting;
    # existing tests inject provider failures and cannot catch swallowed OS errors.
    root = tmp_path / "inputs"
    blocked = root / "blocked"
    blocked.mkdir(parents=True)
    for archive in (root / "good.blob", blocked / "hidden.blob"):
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("payload.txt", "payload")
    config = normalize_config({"verification": {}, "cli": {"language": language}, "filesystem": {
        "scan_filters": [{"name": "size_range", "enabled": False}],
        "directory_scan_mode": "*",
    }})
    monkeypatch.setattr(scan, "load_request_config", lambda _cwd: config)
    stderr = io.StringIO()
    context = CliContext(language=language, cwd=str(tmp_path), reporter=CliReporter(
        json_mode=json_mode, stdout=io.StringIO(), stderr=stderr,
    ))
    notifications = []
    log_events = []
    scheduler = object.__new__(WatchScheduler)
    scheduler.i18n = I18nContext(language)
    scheduler.log = SimpleNamespace(write=lambda event, **fields: log_events.append((event, fields)))
    scheduler._notify = lambda *args: notifications.append(args)
    scheduler.enqueue = lambda *_args, **_kwargs: pytest.fail("scan errors must not enqueue retries")

    with deny_directory_listing(blocked):
        with pytest.raises(PermissionError):
            list(os.scandir(blocked))
        native = NativeDirectoryScanOptions([], [], [], [], [], [], [])
        filtered, raw = native.scan(str(root), None)
        assert filtered.scan_errors() == raw.scan_errors()
        assert len(raw.scan_errors()) == 1 and str(blocked) in raw.scan_errors()[0]
        single = scan_directory_snapshot(str(blocked), None, [], [], [], [], [], [], [])
        assert not single and len(single.scan_errors()) == 1
        state = RunState()
        tasks = ArchiveTaskScanner(config, state).scan_targets([str(root)])
        assert len(tasks) == 1 and tasks[0].main_path == str(root / "good.blob")
        assert state.scan_failures[0].kind == FailureKind.FILESYSTEM_ERROR
        code, result = scan.handle(SimpleNamespace(paths=[str(root)], deep_detect=False, json=json_mode), context)
        assert code != 0 and result.summary["task_count"] == 1
        assert len(result.errors) == 1 and str(blocked) in result.errors[0]
        expected = "无法读取目录：" if language == "zh" else "Cannot read directory: "
        assert result.errors[0].startswith(expected)
        assert "read_dir:" not in result.errors[0]
        validate_catalog()
        assert bool(stderr.getvalue()) is not json_mode
        nested = NestedOutputScanPolicy(config).prepare_scan([str(blocked)])
        assert not nested.roots and len(nested.errors) == 1
        candidates = scheduler._scan_candidates([str(root)], recursive=True)
        assert len(candidates) == 1 and candidates[0].path == str(root / "good.blob")
        assert not notifications
        assert log_events[-1][0] == "filesystem_scan_failed"
        assert str(blocked) in log_events[-1][1]["errors"][0]
