"""Generic execution engine for AE measure and bootstrap cases."""

from __future__ import annotations

import dataclasses
import json
import pathlib
import subprocess
import sys
from collections.abc import Iterable

from artifact_eval.configuration import CaseConfiguration, ConfigMaterializer
from artifact_eval.manifest import RunManifest, matrix_fingerprint
from artifact_eval.runtime import BuildCache, BuildRecord, Preflight, write_build_records
from pipetune.application import (
    BootstrapRequest,
    ResumeRequest,
    bootstrap_session,
    read_session_status,
    resume_session,
)
from pipetune.artifacts import load_session_manifest, load_trial_manifest, write_json_atomic
from pipetune.runner import MeasureRequest, measure


class HarnessError(RuntimeError):
    pass


def report_progress(message: str) -> None:
    print(f"[artifact-eval] {message}", file=sys.stderr, flush=True)


@dataclasses.dataclass(frozen=True)
class ExperimentCase:
    configuration: CaseConfiguration
    mode: str
    repeats: int = 1
    sessions: int = 1
    tuning_rounds: int = 1
    target_role: str = "server"

    def __post_init__(self) -> None:
        if self.mode not in ("measure", "bootstrap"):
            raise HarnessError("case mode must be measure or bootstrap")
        if self.repeats <= 0 or self.sessions <= 0 or self.tuning_rounds <= 0:
            raise HarnessError("case repeat/session/round counts must be positive")
        if self.mode == "measure" and self.sessions != 1:
            raise HarnessError("measure cases use repeats, not sessions")
        if self.mode == "bootstrap" and self.repeats != 1:
            raise HarnessError("bootstrap cases use sessions, not repeats")
        if self.target_role not in ("client", "server"):
            raise HarnessError("target_role must be client or server")

    def as_document(self) -> dict[str, object]:
        return {
            "configuration": dataclasses.asdict(self.configuration),
            "mode": self.mode,
            "repeats": self.repeats,
            "sessions": self.sessions,
            "tuning_rounds": self.tuning_rounds,
            "target_role": self.target_role,
        }

    @classmethod
    def from_document(cls, document: object) -> "ExperimentCase":
        if not isinstance(document, dict):
            raise HarnessError("persisted experiment case must be an object")
        expected = {
            "configuration",
            "mode",
            "repeats",
            "sessions",
            "tuning_rounds",
            "target_role",
        }
        if set(document) != expected:
            raise HarnessError("persisted experiment case fields are invalid")
        configuration = document["configuration"]
        if not isinstance(configuration, dict):
            raise HarnessError("persisted case configuration must be an object")
        configuration_fields = {field.name for field in dataclasses.fields(CaseConfiguration)}
        if set(configuration) != configuration_fields:
            raise HarnessError("persisted case configuration fields are invalid")
        try:
            return cls(
                configuration=CaseConfiguration(**configuration),
                mode=document["mode"],
                repeats=document["repeats"],
                sessions=document["sessions"],
                tuning_rounds=document["tuning_rounds"],
                target_role=document["target_role"],
            )
        except (TypeError, ValueError) as error:
            raise HarnessError(f"persisted experiment case is invalid: {error}") from error


@dataclasses.dataclass(frozen=True)
class HarnessOptions:
    repository: pathlib.Path
    config_dir: pathlib.Path
    output: pathlib.Path
    profile: str
    resume: bool = False
    dry_run: bool = False


def _git_commit(repository: pathlib.Path) -> str:
    completed = subprocess.run(
        ("git", "rev-parse", "HEAD"),
        cwd=repository,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    value = completed.stdout.strip()
    if completed.returncode != 0 or len(value) not in (40, 64):
        raise HarnessError(completed.stderr.strip() or "cannot determine Git commit")
    return value


def ensure_configure_binary(repository: pathlib.Path) -> pathlib.Path:
    binary = repository / "build-tools/axio-configure"
    if binary.is_file():
        return binary
    for command in (
        ("meson", "setup", "build-tools", "-Ddatapath=false"),
        ("ninja", "-C", "build-tools", "axio-configure"),
    ):
        completed = subprocess.run(
            command,
            cwd=repository,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode != 0:
            raise HarnessError(
                completed.stderr.strip() or f"failed to run {' '.join(command)}"
            )
    if not binary.is_file():
        raise HarnessError("axio-configure build did not publish the binary")
    return binary


def _case_files(root: pathlib.Path) -> tuple[pathlib.Path, ...]:
    return tuple(sorted(path for path in root.rglob("*") if path.is_file()))


def _verify_measure(root: pathlib.Path) -> bool:
    session_path = root / "session.json"
    if not session_path.is_file():
        return False
    session = load_session_manifest(session_path, artifact_root=root)
    if session.status != "complete":
        return False
    for reference in session.trials:
        path = root / reference.path
        load_trial_manifest(path, artifact_root=path.parent)
    return True


class ArtifactHarness:
    def __init__(self, options: HarnessOptions) -> None:
        self.options = options
        self.repository = options.repository.resolve()
        self._config_dir = options.config_dir.resolve()
        self.git_commit = _git_commit(self.repository)

    def _reference_pair(
        self, backend: str, target_role: str
    ) -> tuple[pathlib.Path, pathlib.Path]:
        target = self._config_dir / f"{target_role}-{backend}.toml"
        peer_role = "client" if target_role == "server" else "server"
        peer = self._config_dir / f"{peer_role}-{backend}.toml"
        if not target.is_file() or not peer.is_file():
            raise HarnessError(f"reference {backend} configuration pair is missing")
        return target, peer

    def _snapshot_reference_configs(
        self, manifest: RunManifest, cases: Iterable[ExperimentCase]
    ) -> pathlib.Path:
        sources: dict[str, pathlib.Path] = {}
        for case in cases:
            target, peer = self._reference_pair(
                case.configuration.backend, case.target_role
            )
            sources[target.name] = target
            sources[peer.name] = peer
        destination = manifest.root / "inputs/reference-configs"
        copied = []
        for name, source in sorted(sources.items()):
            output = destination / name
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(source.read_bytes())
            copied.append(output)
        manifest.record_inputs(copied)
        return destination

    def execute(
        self, experiment: str, cases: Iterable[ExperimentCase]
    ) -> RunManifest | None:
        case_list = tuple(cases)
        if not case_list:
            raise HarnessError("experiment matrix must not be empty")
        matrix_document = [case.as_document() for case in case_list]
        fingerprint = matrix_fingerprint(matrix_document)
        if self.options.dry_run:
            print(f"Experiment: {experiment}")
            print(f"Profile: {self.options.profile}")
            print(f"Git commit: {self.git_commit}")
            print("Cases:")
            for case in case_list:
                config = case.configuration
                c3 = "reference" if config.preserve_reference_c3 else config.c3
                print(
                    f"  {config.case_id}: {case.mode}, {config.backend}, "
                    f"{config.handler}, C1/C2/C3={config.c1}/{config.c2}/{c3}, "
                    f"repeats={case.repeats}, sessions={case.sessions}, "
                    f"rounds={case.tuning_rounds}"
                )
            return None

        report_progress("prepare configuration tool")
        configure = ensure_configure_binary(self.repository)
        if self.options.resume:
            report_progress(f"resume manifest: {self.options.output}")
            manifest = RunManifest.resume(
                self.options.output,
                experiment=experiment,
                profile=self.options.profile,
                git_commit=self.git_commit,
                matrix_fingerprint=fingerprint,
            )
            snapshotted = manifest.root / "inputs/reference-configs"
            if snapshotted.is_dir():
                self._config_dir = snapshotted
        else:
            report_progress(f"create manifest: {self.options.output}")
            manifest = RunManifest.create(
                self.options.output,
                experiment=experiment,
                profile=self.options.profile,
                git_commit=self.git_commit,
                matrix_fingerprint=fingerprint,
                cases=(case.configuration.case_id for case in case_list),
                matrix=matrix_document,
            )
            self._config_dir = self._snapshot_reference_configs(manifest, case_list)
        generated_root = manifest.root / "generated-configs"
        evidence_root = manifest.root / "evidence"
        materializer = ConfigMaterializer(configure)
        preflight = Preflight(
            configure_binary=configure,
            evidence_root=evidence_root / "preflight",
            expected_git_commit=self.git_commit,
        )
        build_cache = BuildCache(
            configure_binary=configure,
            evidence_root=evidence_root / "builds",
            expected_git_commit=self.git_commit,
        )
        completed = set(manifest.completed_cases())
        preflight_backends: set[str] = set()
        build_records: dict[tuple[str, str], BuildRecord] = {}
        for case_index, case in enumerate(case_list, start=1):
            config = case.configuration
            if config.case_id in completed:
                report_progress(
                    f"case {case_index}/{len(case_list)}: {config.case_id} (complete; skip)"
                )
                continue
            report_progress(f"case {case_index}/{len(case_list)}: {config.case_id}")
            target_base, peer_base = self._reference_pair(
                config.backend, case.target_role
            )
            target_config = generated_root / config.case_id / "target.toml"
            peer_config = generated_root / config.case_id / "peer.toml"
            materializer.materialize(
                case=config,
                target_input=target_base,
                peer_input=peer_base,
                target_output=target_config,
                peer_output=peer_config,
            )
            if config.backend not in preflight_backends:
                report_progress(f"preflight {config.backend}")
                evidence = preflight.check_pair(target_config, peer_config)
                write_json_atomic(
                    evidence_root / f"preflight-{config.backend}.json", evidence
                )
                preflight_backends.add(config.backend)
            report_progress(f"build target: {config.case_id}")
            target_build = build_cache.ensure("target", target_config)
            report_progress(f"build peer: {config.case_id}")
            peer_build = build_cache.ensure("peer", peer_config)
            build_records[("target", target_build.build_fingerprint)] = target_build
            build_records[("peer", peer_build.build_fingerprint)] = peer_build
            write_build_records(manifest.root / "builds.json", tuple(build_records.values()))

            case_root = manifest.root / "cases" / config.case_id
            if case.mode == "measure":
                for repeat in range(1, case.repeats + 1):
                    session_root = case_root / f"repeat-{repeat:02d}"
                    if _verify_measure(session_root):
                        report_progress(
                            f"measure repeat {repeat}/{case.repeats} (complete; skip)"
                        )
                        continue
                    report_progress(f"measure repeat {repeat}/{case.repeats}")
                    measure(
                        MeasureRequest(
                            target_config=target_config,
                            peer_config=peer_config,
                            output=session_root,
                            configure_binary=configure,
                            target_binary=target_build.binary_path,
                            peer_binary=peer_build.binary_path,
                        ),
                        progress=report_progress,
                    )
            else:
                for session_index in range(1, case.sessions + 1):
                    session_root = case_root / f"session-{session_index:02d}"
                    report_progress(
                        f"bootstrap session {session_index}/{case.sessions}"
                    )
                    if session_root.exists():
                        status = read_session_status(session_root).document
                        if status.get("phase") != "complete":
                            resume_session(
                                ResumeRequest(
                                    session=session_root,
                                    configure_binary=configure,
                                ),
                                progress=report_progress,
                            )
                    else:
                        bootstrap_session(
                            BootstrapRequest(
                                target_config=target_config,
                                peer_config=peer_config,
                                output=session_root,
                                configure_binary=configure,
                                max_iterations=case.tuning_rounds,
                                target_binary=target_build.binary_path,
                                peer_binary=peer_build.binary_path,
                            ),
                            progress=report_progress,
                        )
                    status = read_session_status(session_root).document
                    if status.get("phase") != "complete":
                        raise HarnessError(
                            f"{config.case_id}: bootstrap session did not complete"
                        )
            case_document = {
                "schema": "axio.artifact-evaluation-case/v1",
                "case": case.as_document(),
                "target_build": target_build.as_document(),
                "peer_build": peer_build.as_document(),
            }
            write_json_atomic(case_root / "case.json", case_document)
            manifest.complete_case(config.case_id, _case_files(case_root))
        return manifest
