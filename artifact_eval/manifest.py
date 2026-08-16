"""Atomic, fingerprinted artifact-evaluation run manifest."""

from __future__ import annotations

import dataclasses
import datetime as dt
import hashlib
import json
import pathlib
from typing import Iterable

from pipetune.artifacts import sha256_file, write_json_atomic


class ManifestError(RuntimeError):
    pass


@dataclasses.dataclass(frozen=True)
class RunIdentity:
    experiment: str
    profile: str
    matrix: tuple[dict[str, object], ...]


def _utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


class RunManifest:
    SCHEMA = "axio.artifact-evaluation/v1"

    def __init__(self, root: pathlib.Path, document: dict[str, object]) -> None:
        self.root = root.resolve()
        self.path = self.root / "manifest.json"
        self.document = document

    @classmethod
    def create(
        cls,
        root: pathlib.Path,
        *,
        experiment: str,
        profile: str,
        git_commit: str,
        matrix_fingerprint: str,
        cases: Iterable[str],
        matrix: object = (),
    ) -> "RunManifest":
        if root.exists():
            raise ManifestError(f"output already exists: {root}")
        root.mkdir(parents=True)
        case_ids = tuple(cases)
        if not case_ids or len(case_ids) != len(set(case_ids)):
            raise ManifestError("case IDs must be non-empty and unique")
        manifest = cls(
            root,
            {
                "schema": cls.SCHEMA,
                "experiment": experiment,
                "profile": profile,
                "git_commit": git_commit,
                "matrix_fingerprint": matrix_fingerprint,
                "matrix": matrix,
                "created_at_utc": _utc_now(),
                "publications": {},
                "cases": {
                    case_id: {"status": "pending", "artifacts": []}
                    for case_id in case_ids
                },
            },
        )
        manifest._write()
        return manifest

    @classmethod
    def resume(
        cls,
        root: pathlib.Path,
        *,
        experiment: str,
        profile: str,
        git_commit: str,
        matrix_fingerprint: str,
    ) -> "RunManifest":
        path = root.resolve() / "manifest.json"
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise ManifestError(f"cannot load run manifest: {error}") from error
        if not isinstance(document, dict):
            raise ManifestError("run manifest must contain an object")
        expected = {
            "schema": cls.SCHEMA,
            "experiment": experiment,
            "profile": profile,
            "git_commit": git_commit,
            "matrix_fingerprint": matrix_fingerprint,
        }
        for key, value in expected.items():
            if document.get(key) != value:
                raise ManifestError(f"resume identity mismatch: {key}")
        manifest = cls(root, document)
        manifest._validate_artifacts()
        return manifest

    def _write(self) -> None:
        write_json_atomic(self.path, self.document)

    @classmethod
    def inspect(cls, root: pathlib.Path) -> RunIdentity:
        path = root.resolve() / "manifest.json"
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise ManifestError(f"cannot load run manifest: {error}") from error
        if not isinstance(document, dict) or document.get("schema") != cls.SCHEMA:
            raise ManifestError("run manifest has an unsupported schema")
        experiment = document.get("experiment")
        profile = document.get("profile")
        matrix = document.get("matrix")
        if not isinstance(experiment, str) or not isinstance(profile, str):
            raise ManifestError("run manifest identity is invalid")
        if not isinstance(matrix, list) or not matrix:
            raise ManifestError("run manifest matrix is invalid")
        if not all(isinstance(item, dict) for item in matrix):
            raise ManifestError("run manifest matrix entries must be objects")
        return RunIdentity(experiment, profile, tuple(matrix))

    def record_inputs(self, paths: Iterable[pathlib.Path]) -> None:
        evidence: dict[str, dict[str, object]] = {}
        for path in paths:
            resolved = path.resolve(strict=True)
            try:
                relative = resolved.relative_to(self.root)
            except ValueError as error:
                raise ManifestError("input is outside the run root") from error
            if not resolved.is_file():
                raise ManifestError("manifest input must be a file")
            evidence[relative.as_posix()] = {
                "sha256": sha256_file(resolved),
                "size_bytes": resolved.stat().st_size,
            }
        if not evidence:
            raise ManifestError("a run requires input evidence")
        self.document["inputs"] = evidence
        self._write()

    def _validate_artifacts(self) -> None:
        cases = self.document.get("cases")
        if not isinstance(cases, dict):
            raise ManifestError("manifest cases must be an object")
        for case_id, value in cases.items():
            if not isinstance(case_id, str) or not isinstance(value, dict):
                raise ManifestError("manifest contains an invalid case")
            if value.get("status") != "complete":
                continue
            artifacts = value.get("artifacts")
            if not isinstance(artifacts, list) or not artifacts:
                raise ManifestError(f"completed case {case_id} has no artifacts")
            for item in artifacts:
                if not isinstance(item, dict):
                    raise ManifestError(f"case {case_id} has invalid artifact evidence")
                path = self.root / str(item.get("path", ""))
                try:
                    relative = path.resolve(strict=True).relative_to(self.root)
                except (OSError, ValueError) as error:
                    raise ManifestError(f"case {case_id} artifact is missing") from error
                if not path.is_file() or relative.as_posix() != item.get("path"):
                    raise ManifestError(f"case {case_id} artifact is invalid")
                if sha256_file(path) != item.get("sha256"):
                    raise ManifestError(f"case {case_id} artifact hash changed")
        publications = self.document.get("publications", {})
        if not isinstance(publications, dict):
            raise ManifestError("manifest publications must be an object")
        for name, item in publications.items():
            if not isinstance(name, str) or not isinstance(item, dict):
                raise ManifestError("manifest contains invalid publication evidence")
            path = self.root / name
            if not path.is_file() or sha256_file(path) != item.get("sha256"):
                raise ManifestError(f"publication changed: {name}")
        inputs = self.document.get("inputs", {})
        if not isinstance(inputs, dict):
            raise ManifestError("manifest inputs must be an object")
        for name, item in inputs.items():
            if not isinstance(name, str) or not isinstance(item, dict):
                raise ManifestError("manifest contains invalid input evidence")
            path = self.root / name
            try:
                relative = path.resolve(strict=True).relative_to(self.root)
            except (OSError, ValueError) as error:
                raise ManifestError(f"input is missing: {name}") from error
            if (
                not path.is_file()
                or relative.as_posix() != name
                or sha256_file(path) != item.get("sha256")
            ):
                raise ManifestError(f"input changed: {name}")

    def complete_case(self, case_id: str, artifacts: Iterable[pathlib.Path]) -> None:
        cases = self.document["cases"]
        if case_id not in cases:
            raise ManifestError(f"unknown case {case_id}")
        evidence = []
        for path in artifacts:
            resolved = path.resolve(strict=True)
            try:
                relative = resolved.relative_to(self.root)
            except ValueError as error:
                raise ManifestError("case artifact is outside the run root") from error
            evidence.append(
                {
                    "path": relative.as_posix(),
                    "sha256": sha256_file(resolved),
                    "size_bytes": resolved.stat().st_size,
                }
            )
        if not evidence:
            raise ManifestError("a completed case requires artifacts")
        cases[case_id] = {"status": "complete", "artifacts": evidence}
        self._write()

    def completed_cases(self) -> tuple[str, ...]:
        cases = self.document["cases"]
        return tuple(
            case_id
            for case_id, value in cases.items()
            if value.get("status") == "complete"
        )

    def publish_files(self, paths: Iterable[pathlib.Path]) -> None:
        publications = self.document.setdefault("publications", {})
        for path in paths:
            if not path.is_file():
                continue
            resolved = path.resolve()
            try:
                relative = resolved.relative_to(self.root)
            except ValueError as error:
                raise ManifestError("publication is outside the run root") from error
            publications[relative.as_posix()] = {
                "sha256": sha256_file(resolved),
                "size_bytes": resolved.stat().st_size,
            }
        self._write()


def matrix_fingerprint(document: object) -> str:
    payload = json.dumps(
        document, allow_nan=False, separators=(",", ":"), sort_keys=True
    ).encode()
    return hashlib.sha256(payload).hexdigest()
