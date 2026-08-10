"""Atomic, fingerprinted artifact-evaluation run manifest."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import pathlib
from typing import Iterable

from pipetune.artifacts import sha256_file, write_json_atomic


class ManifestError(RuntimeError):
    pass


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
                "created_at_utc": _utc_now(),
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


def matrix_fingerprint(document: object) -> str:
    payload = json.dumps(
        document, allow_nan=False, separators=(",", ":"), sort_keys=True
    ).encode()
    return hashlib.sha256(payload).hexdigest()
