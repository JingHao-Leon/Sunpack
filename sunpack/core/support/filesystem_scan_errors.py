from collections.abc import Iterable

from sunpack.core.contracts.failures import FailureInfo, FailureKind
from sunpack.core.i18n import I18nContext


def filesystem_scan_failure(error: tuple[str, str, str], language: str) -> FailureInfo:
    operation, path, detail = error
    key = f"filesystem_scan.{operation}"
    params = {"path": path, "error": detail}
    return FailureInfo(
        kind=FailureKind.FILESYSTEM_ERROR,
        stage="filesystem_scan",
        message=I18nContext(language).t(key, **params),
        message_key=key,
        message_params=params,
        details={
            "path": path,
            "operation": operation,
            "count": int(detail) if operation in {"omitted", "output_scan"} else 1,
        },
    )


def is_filesystem_scan_failure(failure: FailureInfo) -> bool:
    return failure.kind == FailureKind.FILESYSTEM_ERROR and failure.stage == "filesystem_scan"


def filesystem_scan_error_count(failures: Iterable[FailureInfo]) -> int:
    return sum(failure.details.get("count", 1) for failure in failures if is_filesystem_scan_failure(failure))
