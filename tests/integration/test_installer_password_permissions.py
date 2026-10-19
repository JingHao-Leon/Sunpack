from __future__ import annotations

import base64
import json
import os
import platform
from pathlib import Path
import re
import shutil
import subprocess

import pytest


def _ps_string(value) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def _powershell(script: str):
    encoded = base64.b64encode(script.encode("utf-16-le")).decode("ascii")
    result = subprocess.run(
        ["powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded],
        capture_output=True, text=True, timeout=30,
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def _inno_compiler():
    candidates = [shutil.which("ISCC.exe")]
    candidates.extend(str(Path(os.environ.get(key, "")) / suffix) for key, suffix in (
        ("ProgramFiles(x86)", "Inno Setup 6/ISCC.exe"),
        ("ProgramFiles", "Inno Setup 6/ISCC.exe"),
        ("LOCALAPPDATA", "Programs/Inno Setup 6/ISCC.exe"),
    ))
    compiler = next((path for path in candidates if path and Path(path).is_file()), None)
    if compiler is None:
        pytest.skip("Inno Setup compiler is not installed")
    return compiler


def test_installer_repairs_existing_and_new_password_data_permissions(tmp_path, repo_root):
    # No runtime password test can detect leftover installer ACEs or blocked atomic writes.
    compiler = _inno_compiler()

    existing = tmp_path / "existing ' [data]"
    fresh = tmp_path / "fresh"
    nested = existing / "nested"
    nested.mkdir(parents=True)
    fresh.mkdir()
    password_file = existing / "builtin_passwords.json"
    password_file.write_text('{"passwords":[],"clipboard_history":[]}', encoding="utf-8")
    backup = nested / "builtin_passwords.txt.bak"
    backup.write_text("preserved", encoding="utf-8")
    original = password_file.read_bytes()
    _powershell(f"""
$ErrorActionPreference = 'Stop'
$data = {_ps_string(existing)}
$acl = [IO.Directory]::GetAccessControl($data)
$acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
    [Security.Principal.SecurityIdentifier]::new('S-1-5-32-545'), 'Modify', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
[IO.Directory]::SetAccessControl($data, $acl)
$directory = {_ps_string(nested)}
$acl = [IO.Directory]::GetAccessControl($directory)
$acl.SetAccessRuleProtection($true, $true)
[IO.Directory]::SetAccessControl($directory, $acl)
foreach ($path in @({_ps_string(password_file)}, {_ps_string(backup)})) {{
    $acl = [IO.File]::GetAccessControl($path)
    foreach ($sid in @('S-1-1-0', 'S-1-5-11', 'S-1-5-32-545')) {{
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
            [Security.Principal.SecurityIdentifier]::new($sid), 'Modify', 'Allow'))
    }}
    [IO.File]::SetAccessControl($path, $acl)
}}
'true'
""")

    source = (repo_root / "installer/SunPack.iss").read_text(encoding="utf-8-sig")
    routines = []
    for name in ("PowerShellSingleQuotedString", "SecurePasswordDataDirectory"):
        match = re.search(rf"^(?:function|procedure) {name}\b.*?^end;", source, re.M | re.S)
        assert match is not None, name
        routines.append(match.group())
    marker = tmp_path / "completed"
    harness = tmp_path / "permissions.iss"
    harness.write_text(f"""
[Setup]
AppName=SunPack permission regression
AppVersion=1
DefaultDirName={{tmp}}\\SunPackPermissionRegression
PrivilegesRequired=lowest
Uninstallable=no
CreateAppDir=no
OutputDir={tmp_path}
OutputBaseFilename=permissions
[CustomMessages]
DataDirectoryPermissionsFailed=Cannot secure %s
[Code]
{chr(10).join(routines)}
function InitializeSetup: Boolean;
begin
  SecurePasswordDataDirectory({_ps_string(existing)});
  SecurePasswordDataDirectory({_ps_string(fresh)});
  SecurePasswordDataDirectory({_ps_string(existing)});
  SaveStringToFile({_ps_string(marker)}, 'ok', False);
  Result := False;
end;
""", encoding="utf-8-sig")
    result = subprocess.run([compiler, "/Qp", str(harness)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run(
        [str(harness.with_suffix(".exe")), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/SP-",
         "/NORESTART", f"/LOG={tmp_path / 'setup.log'}"],
        capture_output=True, text=True, timeout=30,
    )
    assert marker.exists(), (tmp_path / "setup.log").read_text(encoding="utf-8-sig", errors="replace")
    assert password_file.read_bytes() == original
    assert backup.read_text(encoding="utf-8") == "preserved"

    # Verify effective non-elevated IO as well as exact root and inherited child grants.
    for directory in (existing, fresh):
        temp = directory / ".clipboard.tmp"
        temp.write_text('{"passwords":[],"clipboard_history":["updated"]}', encoding="utf-8")
        temp.replace(directory / "builtin_passwords.json")
        assert json.loads((directory / "builtin_passwords.json").read_text(encoding="utf-8"))["clipboard_history"] == ["updated"]
    security = _powershell(f"""
$ErrorActionPreference = 'Stop'
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$items = foreach ($root in @({_ps_string(existing)}, {_ps_string(fresh)})) {{
    $root
    [IO.Directory]::GetFileSystemEntries($root, '*', [IO.SearchOption]::AllDirectories)
}}
$entries = @($items | ForEach-Object {{
    $acl = if ([IO.Directory]::Exists($_)) {{ [IO.Directory]::GetAccessControl($_) }} else {{ [IO.File]::GetAccessControl($_) }}
    @{{ path = $_; protected = $acl.AreAccessRulesProtected; rules = @(
        $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]) | ForEach-Object {{
            @{{ sid = $_.IdentityReference.Value; rights = [int]$_.FileSystemRights;
               inherited = $_.IsInherited; type = [string]$_.AccessControlType;
               inheritance = [int]$_.InheritanceFlags; propagation = [int]$_.PropagationFlags }}
        }}
    ) }}
}})
@{{ sid = $sid; entries = $entries }} | ConvertTo-Json -Depth 6 -Compress
""")
    roots = {str(existing), str(fresh)}
    expected = {security["sid"]: 0x1301BF, "S-1-5-18": 0x1F01FF, "S-1-5-32-544": 0x1F01FF}
    assert len(security["entries"]) == 6
    for entry in security["entries"]:
        is_root = entry["path"] in roots
        assert entry["protected"] is is_root
        assert {rule["sid"]: rule["rights"] for rule in entry["rules"]} == expected
        assert len(entry["rules"]) == 3
        assert all(rule["type"] == "Allow" and rule["inherited"] is not is_root for rule in entry["rules"])
        if is_root:
            assert all(rule["inheritance"] == 3 and rule["propagation"] == 0 for rule in entry["rules"])


def test_installer_upgrades_retired_config_and_removes_converted_password_text(tmp_path, repo_root):
    # Only an actual Pascal installer harness can catch conversion/cleanup order,
    # an incorrect headless command, or deletion of the source on failed conversion.
    compiler = _inno_compiler()
    arch = "arm64" if platform.machine().lower() in {"arm64", "aarch64"} else "x64"
    manager = repo_root / ".cache" / f"gui-{arch}" / "Release" / "SunPackGUI.exe"
    if not manager.is_file():
        pytest.skip("Build the native GUI with scripts/build_gui.ps1")
    app = tmp_path / "app ' [native]"
    app.mkdir()
    shutil.copy2(manager, app / manager.name)
    data = tmp_path / "data ' [legacy]"
    existing = tmp_path / "already converted"
    invalid = tmp_path / "invalid source"
    invalid_config = tmp_path / "invalid config"
    invalid_json = tmp_path / "invalid existing passwords"
    fresh = tmp_path / "fresh"
    for folder in (data, existing, invalid, invalid_config, invalid_json, fresh):
        folder.mkdir()
    legacy = (
        '# Built-in common password list. You can edit this file; use one password per line.\n'
        '  #中文  \nquote"and\\slash\n'
        '#!SUNPACK-WATCH-CLIPBOARD-BEGIN\n历史密码\n#!SUNPACK-WATCH-CLIPBOARD-END\n'
    )
    (data / "builtin_passwords.txt").write_text(legacy, encoding="utf-8")
    (data / "builtin_passwords.txt.bak").write_text("obsolete backup", encoding="utf-8")
    config = json.loads((repo_root / "sunpack_advanced_config.json").read_text(encoding="utf-8"))
    config["watch"].update(directory_password_file_auto_create=True, password_retry_include_subtree=False)
    config["passwords"].update(directory_passwords_enabled=True, directory_passwords_max_file_bytes=1, directory_passwords_max_password_length=1)
    (data / "sunpack_config.json").write_text(json.dumps(config, ensure_ascii=False), encoding="utf-8")
    old_json = '{"passwords":[{"value":" keep ","scopes":["C:/资料"]}],"clipboard_history":["history"]}'
    (existing / "builtin_passwords.json").write_text(old_json, encoding="utf-8")
    for name in ("builtin_passwords.txt", "builtin_passwords.txt.bak"):
        (existing / name).write_text("obsolete", encoding="utf-8")
    (invalid / "builtin_passwords.txt").write_text("bad\x00password", encoding="utf-8")
    (invalid_config / "sunpack_config.json").write_text("{invalid}", encoding="utf-8")
    (invalid_json / "builtin_passwords.json").write_text("{invalid}", encoding="utf-8")
    (invalid_json / "builtin_passwords.txt").write_text("keep source", encoding="utf-8")

    source = (repo_root / "installer/SunPack.iss").read_text(encoding="utf-8-sig")
    routines = []
    for name in ("SaveUTF8TextLines", "PasswordJsonString", "UpgradeEditableData", "DeleteLegacyPasswordFiles", "EnsurePasswordJson"):
        match = re.search(rf"^(?:function|procedure) {name}\b.*?^end;", source, re.M | re.S)
        assert match is not None, name
        routines.append(match.group())
    constants = []
    for name in ("WatchClipboardBlockBegin", "WatchClipboardBlockEnd"):
        match = re.search(rf"^  {name} = .*?;$", source, re.M)
        assert match is not None, name
        constants.append(match.group())
    marker = tmp_path / "completed"
    harness = tmp_path / "upgrade.iss"
    harness.write_text(f"""
[Setup]
AppName=SunPack data upgrade regression
AppVersion=1
DefaultDirName={{tmp}}\\SunPackUpgradeRegression
PrivilegesRequired=lowest
Uninstallable=no
CreateAppDir=no
OutputDir={tmp_path}
OutputBaseFilename=upgrade
[CustomMessages]
EditableConfigCreateFailed=Cannot create %s
EditableConfigUpgradeFailed=Cannot upgrade %s
[Code]
const
{chr(10).join(constants)}
{chr(10).join(routines)}
function InitializeSetup: Boolean;
var
  Failed: Boolean;
begin
  UpgradeEditableData({_ps_string(app)}, {_ps_string(data)});
  EnsurePasswordJson({_ps_string(data)});
  EnsurePasswordJson({_ps_string(data)});
  UpgradeEditableData({_ps_string(app)}, {_ps_string(existing)});
  EnsurePasswordJson({_ps_string(existing)});
  EnsurePasswordJson({_ps_string(fresh)});
  Failed := False;
  try
    EnsurePasswordJson({_ps_string(invalid)});
  except
    Failed := True;
  end;
  if not Failed then RaiseException('Invalid password source was accepted');
  Failed := False;
  try
    UpgradeEditableData({_ps_string(app)}, {_ps_string(invalid_config)});
  except
    Failed := True;
  end;
  if not Failed then RaiseException('Invalid configuration was accepted');
  Failed := False;
  try
    UpgradeEditableData({_ps_string(app)}, {_ps_string(invalid_json)});
    EnsurePasswordJson({_ps_string(invalid_json)});
  except
    Failed := True;
  end;
  if not Failed then RaiseException('Invalid existing password JSON was accepted');
  SaveStringToFile({_ps_string(marker)}, 'ok', False);
  Result := False;
end;
""", encoding="utf-8-sig")
    result = subprocess.run([compiler, "/Qp", str(harness)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    subprocess.run(
        [str(harness.with_suffix(".exe")), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/SP-", "/NORESTART", f"/LOG={tmp_path / 'setup.log'}"],
        capture_output=True, text=True, timeout=30,
    )
    assert marker.exists(), (tmp_path / "setup.log").read_text(encoding="utf-8-sig", errors="replace")
    upgraded = json.loads((data / "sunpack_config.json").read_text(encoding="utf-8"))
    for name in ("directory_password_file_auto_create", "password_retry_include_subtree"):
        del config["watch"][name]
    for name in ("directory_passwords_enabled", "directory_passwords_max_file_bytes", "directory_passwords_max_password_length"):
        del config["passwords"][name]
    assert upgraded == config  # Every retained advanced value survives ordinary-config upgrade.
    assert json.loads((data / "builtin_passwords.json").read_text(encoding="utf-8")) == {
        "passwords": [{"value": "  #中文  "}, {"value": 'quote"and\\slash'}], "clipboard_history": ["历史密码"],
    }
    assert (existing / "builtin_passwords.json").read_text(encoding="utf-8") == old_json
    for folder in (data, existing, fresh):
        assert not (folder / "builtin_passwords.txt").exists()
        assert not (folder / "builtin_passwords.txt.bak").exists()
    assert (invalid / "builtin_passwords.txt").read_text(encoding="utf-8") == "bad\x00password"
    assert not (invalid / "builtin_passwords.json").exists()
    assert not (invalid / "builtin_passwords.txt.bak").exists()
    assert (invalid_config / "sunpack_config.json").read_text(encoding="utf-8") == "{invalid}"
    assert (invalid_json / "builtin_passwords.txt").read_text(encoding="utf-8") == "keep source"
    assert (invalid_json / "builtin_passwords.json").read_text(encoding="utf-8") == "{invalid}"
