from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import Iterable

from sunpack.core.support.resource_lifecycle import FileIdentity


@dataclass(frozen=True)
class ArchiveConflict:
    owner: str
    origin: str
    paths: tuple[str, ...]


@dataclass
class _Lease:
    owner: str
    origin: str
    identities: tuple[FileIdentity, ...]
    physical: set[tuple[int, int]] = field(init=False, default_factory=set)
    paths: set[str] = field(init=False, default_factory=set)
    unidentified: set[str] = field(init=False, default_factory=set)
    directories: set[str] = field(init=False, default_factory=set)

    def __post_init__(self) -> None:
        for identity in self.identities:
            self.paths.add(identity.path)
            if identity.device is not None and identity.file_id is not None:
                self.physical.add((identity.device, identity.file_id))
            else:
                self.unidentified.add(identity.path)
            if identity.is_directory:
                self.directories.add(identity.path)

    def overlaps(self, other: "_Lease") -> bool:
        return (
            not self.physical.isdisjoint(other.physical)
            or not self.unidentified.isdisjoint(other.paths)
            or not other.unidentified.isdisjoint(self.paths)
            or _contains_paths(self.directories, other.paths)
            or _contains_paths(other.directories, self.paths)
        )


def _contains_paths(directories: set[str], paths: set[str]) -> bool:
    if not directories:
        return False
    # Shared parents need one walk, regardless of the number of selected files.
    visited: set[str] = set()
    for path in paths:
        while path not in visited:
            if path in directories:
                return True
            visited.add(path)
            parent = os.path.dirname(path)
            if parent == path:
                break
            path = parent
    return False


class ActiveArchiveRegistry:
    """RuntimeHost-owned, event-loop-confined archive ownership registry."""

    def __init__(self) -> None:
        self._leases: dict[str, _Lease] = {}

    def reserve(self, owner: str, origin: str, paths: Iterable[str]) -> ArchiveConflict | None:
        normalized_origin = "watch" if str(origin).lower() == "watch" else "foreground"
        identities = tuple(
            FileIdentity.from_path(path)
            for path in dict.fromkeys(os.path.abspath(os.path.normpath(str(path))) for path in paths if path)
        )
        lease = _Lease(owner, normalized_origin, identities)
        conflict = self._find_conflict(lease)
        if conflict is not None:
            return conflict
        self._leases[owner] = lease
        return None

    def release(self, owner: str) -> None:
        self._leases.pop(owner, None)

    def watch_conflict(self, paths: Iterable[str]) -> ArchiveConflict | None:
        candidate = _Lease("", "watch", tuple(FileIdentity.from_path(path) for path in paths if path))
        for lease in self._leases.values():
            if lease.origin != "watch":
                continue
            if candidate.overlaps(lease):
                return ArchiveConflict(lease.owner, lease.origin, tuple(item.path for item in lease.identities))
        return None

    def _find_conflict(self, candidate: _Lease) -> ArchiveConflict | None:
        for current_owner, lease in self._leases.items():
            if current_owner == candidate.owner:
                continue
            if candidate.overlaps(lease):
                return ArchiveConflict(lease.owner, lease.origin, tuple(item.path for item in lease.identities))
        return None
