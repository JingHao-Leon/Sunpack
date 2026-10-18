from __future__ import annotations

import copy
import json
import subprocess
import sys
from pathlib import Path

import pytest
import sunpack.core.passwords.internal.builtin as builtin
from sunpack.core.passwords.internal.store import PasswordStore, EMPTY_PASSWORD_SNAPSHOT


def seed(path, values=(), scopes=()):
    path.write_text(json.dumps({"passwords": [
        *({"value": v} for v in values),
        *({"value": v, "scopes": dirs} for v, dirs in scopes),
    ], "clipboard_history": []}, ensure_ascii=False), encoding="utf-8")


def test_snapshot_literal_values_scopes_and_semantic_reuse(tmp_path):
    # Unique coverage: literal JSON values, Windows ancestry, immutable sharing.
    path = tmp_path / "passwords.json"
    special = ' #a|b \t"c" \n'
    seed(path, [special, "global"], [("secret", ["D:/Work", "//server/share/Files"]), ("near", ["D:/Work/Child"])])
    first = builtin.read_builtin_password_file(path)
    store = PasswordStore.from_sources(password_snapshot=first, cli_passwords=["cli", "global"], clipboard_passwords=["clip"])
    store.remember_success("secret")
    assert store.candidates("d:/work/child/deep") == ["secret", "near", "cli", "global", "clip", special]
    assert "secret" not in store.candidates("D:/Workspace")
    assert "secret" in store.candidates("//SERVER/share/files/child")
    assert "secret" not in store.candidates("//server/share/FilesOther")
    assert copy.deepcopy(first) is first
    with pytest.raises(TypeError):
        first.scoped_index["new"] = ()
    path.write_text(json.dumps(json.loads(path.read_text(encoding="utf-8")), indent=4), encoding="utf-8")
    assert builtin.read_builtin_password_file(path, first) is first
    seed(path, [special, "changed"], [("secret", ["D:/Work", "//server/share/Files"]), ("near", ["D:/Work/Child"])])
    second = builtin.read_builtin_password_file(path, first)
    assert second.signature != first.signature
    assert all(second.scoped_index[key] is values for key, values in first.scoped_index.items())
    assert store.snapshot is first
    assert store.candidates("E:/Private") == ["cli", "global", "clip", special]
    store = PasswordStore.from_sources(recent_passwords=store.recent_passwords, password_snapshot=EMPTY_PASSWORD_SNAPSHOT)
    assert "secret" not in store.candidates("D:/Work")
    root = builtin._parse_document('{"passwords":[{"value":"root","scopes":["//server/share"]}]}')[0]
    assert PasswordStore.from_sources(password_snapshot=root).candidates("//server/share/child") == ["root"]


def test_file_events_keep_last_valid_snapshot_clear_deletion_and_never_restore_defaults(tmp_path, monkeypatch):
    # Unique coverage: invalid updates, real deletion, explicit empty configuration.
    path = tmp_path / "builtin_passwords.json"
    seed(path, ["secret"])
    monkeypatch.setattr(builtin, "builtin_password_path", lambda: path)
    source = builtin.PasswordFileSource()
    good = source.snapshot
    invalid = [
        '{"passwords":[{"value":"x","scopes":[]}]}',
        '{"passwords":[{"value":"x","scopes":["relative"]}]}',
        '{"passwords":[{"value":"x","scopes":["C:relative"]}]}',
        '{"passwords":[{"value":"x","scopes":[42]}]}',
        '{"passwords":[{"value":"x\\u0000"}]}',
        '{"passwords":[],"unexpected":1}',
        '{"passwords":[],"passwords":[]}',
        '{"passwords":[],}',
        '{"passwords":[{"value":"x"},]}',
        '{"passwords":['
    ]
    for text in invalid:
        path.write_text(text, encoding="utf-8")
        assert source.reload() is good
    path.unlink()
    assert source.reload() is EMPTY_PASSWORD_SNAPSHOT
    assert not path.exists()
    seed(path)
    assert source.reload().global_passwords == ()
    assert builtin.PasswordFileSource().snapshot.global_passwords == ()
    assert good.global_passwords == ("secret",)


def test_clipboard_merge_preserves_manual_records_and_detects_edit_conflicts(tmp_path, monkeypatch):
    # Unique coverage: concurrent writers, bounded history, editor conflict and no-op writes.
    path = tmp_path / "builtin_passwords.json"
    seed(path, [" manual "] , [("scoped", ["D:/Work"])])
    monkeypatch.setattr(builtin, "builtin_password_path", lambda: path)
    original_records = json.loads(path.read_text(encoding="utf-8"))["passwords"]
    child = """
import sys
from pathlib import Path
from sunpack.core.passwords.internal import builtin
builtin.builtin_password_path = lambda: Path(sys.argv[1])
print('ready', flush=True)
sys.stdin.read(1)
sys.exit(0 if builtin.merge_watch_clipboard_passwords([sys.argv[2]], max_entries=4) else 1)
"""
    processes = [subprocess.Popen(
        [sys.executable, "-c", child, str(path), f"clip-{i}"],
        cwd=Path(__file__).resolve().parents[2], stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    ) for i in range(4)]
    try:
        for process in processes:
            assert process.stdout.readline().strip() == "ready"
        for process in processes:
            process.stdin.write("g")
            process.stdin.flush()
        for process in processes:
            _, error = process.communicate(timeout=10)
            assert process.returncode == 0, error
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
                process.communicate()
    doc = json.loads(path.read_text(encoding="utf-8"))
    assert doc["passwords"] == original_records
    assert set(doc["clipboard_history"]) == {f"clip-{i}" for i in range(4)}
    assert builtin.merge_watch_clipboard_passwords(["last", "last"], max_entries=2)
    before = path.stat().st_mtime_ns
    assert not builtin.merge_watch_clipboard_passwords(["last"], max_entries=2)
    assert path.stat().st_mtime_ns == before
    write = builtin.write_task_text
    def race(temp, content, **kwargs):
        write(temp, content, **kwargs)
        seed(path, ["editor-wins"])
    monkeypatch.setattr(builtin, "write_task_text", race)
    assert not builtin.merge_watch_clipboard_passwords(["racing"])
    assert json.loads(path.read_text(encoding="utf-8"))["passwords"] == [{"value": "editor-wins"}]
    assert not list(tmp_path.glob(".*.tmp"))
