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
        details={"path": path, "operation": operation},
    )
