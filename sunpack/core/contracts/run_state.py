"""Mutable coordinator state for one pipeline execution."""

import threading
from collections.abc import Iterable

from sunpack.core.contracts.failures import FailureInfo
from sunpack.core.contracts.results import ArchiveCleanupResult, RunSummary, TargetRunResult
from sunpack.core.support.filesystem_scan_errors import filesystem_scan_failure
from sunpack.core.support.path_keys import absolute_path_key


class RunState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.scan_failed_tasks: list[str] = []
        self.scan_failures: list[FailureInfo] = []
        self.processed_keys: set[str] = set()
        self.cleanup_results: list[ArchiveCleanupResult] = []
        self.target_results: list[TargetRunResult] = []
        self.policy_skips: list[dict] = []
        self._scan_error_keys: set[tuple[str, str, str]] | None = None

    def record_scan_errors(self, errors: Iterable[tuple[str, str, str]], language: str) -> None:
        for error in errors:
            operation, path, detail = error
            key = (operation, absolute_path_key(path) if path else "", detail)
            with self.lock:
                if self._scan_error_keys is None:
                    self._scan_error_keys = set()
                if key in self._scan_error_keys:
                    continue
                self._scan_error_keys.add(key)
                failure = filesystem_scan_failure(error, language)
                self.scan_failed_tasks.append(failure.message)
                self.scan_failures.append(failure)

    def snapshot(
        self,
        *,
        target_results: list[TargetRunResult],
        scan_failed_tasks: list[str],
        scan_failures: list[FailureInfo],
        policy_skips: list[dict],
    ) -> RunSummary:
        return RunSummary(
            target_results=tuple(target_results),
            scan_failed_tasks=tuple(scan_failed_tasks),
            scan_failures=tuple(scan_failures),
            policy_skips=tuple(policy_skips),
            cleanup_results=tuple(self.cleanup_results),
        )
