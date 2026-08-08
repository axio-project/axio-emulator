"""Command-line entry point for the script-based PipeTune controller."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from collections.abc import Sequence

from pipetune.application import (
    ApplicationError,
    BootstrapRequest,
    ResumeRequest,
    bootstrap_session,
    read_session_status,
    resume_session,
)
from pipetune.diagnosis import DiagnosisError, publish_diagnosis
from pipetune.runner import MeasureError, MeasureRequest, measure


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="pipetune")
    commands = parser.add_subparsers(dest="command", required=True)
    measure_parser = commands.add_parser("measure")
    measure_parser.add_argument("--target-config", required=True)
    measure_parser.add_argument("--peer-config", required=True)
    measure_parser.add_argument("--output", required=True)
    measure_parser.add_argument(
        "--axio-configure",
        default="build-tools/axio-configure",
    )
    measure_parser.add_argument("--target-binary")
    measure_parser.add_argument("--peer-binary")
    diagnose_parser = commands.add_parser("diagnose")
    diagnose_parser.add_argument("--session", required=True)
    diagnose_parser.add_argument("--trial")
    diagnose_parser.add_argument("--probe-session")
    diagnose_parser.add_argument("--probe-trial")
    bootstrap_parser = commands.add_parser("bootstrap")
    bootstrap_parser.add_argument("--target-config", required=True)
    bootstrap_parser.add_argument("--peer-config", required=True)
    bootstrap_parser.add_argument("--max-iterations", required=True, type=int)
    bootstrap_parser.add_argument("--output", required=True)
    bootstrap_parser.add_argument(
        "--axio-configure",
        default="build-tools/axio-configure",
    )
    bootstrap_parser.add_argument("--target-binary")
    bootstrap_parser.add_argument("--peer-binary")
    resume_parser = commands.add_parser("resume")
    resume_parser.add_argument("--session", required=True)
    resume_parser.add_argument(
        "--axio-configure",
        default="build-tools/axio-configure",
    )
    status_parser = commands.add_parser("status")
    status_parser.add_argument("--session", required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    if arguments.command == "measure":
        request = MeasureRequest(
            target_config=pathlib.Path(arguments.target_config),
            peer_config=pathlib.Path(arguments.peer_config),
            output=pathlib.Path(arguments.output),
            configure_binary=pathlib.Path(arguments.axio_configure),
            target_binary=arguments.target_binary,
            peer_binary=arguments.peer_binary,
        )
        try:
            result = measure(request)
        except MeasureError as error:
            print(f"pipetune measure: {error}", file=sys.stderr)
            return 2
        document = {
            "manifest": result.manifest.path,
            "session": result.session.path,
            "success": result.success,
        }
    elif arguments.command == "diagnose":
        try:
            publication = publish_diagnosis(
                pathlib.Path(arguments.session),
                trial_id=arguments.trial,
                probe_session_root=(
                    pathlib.Path(arguments.probe_session)
                    if arguments.probe_session
                    else None
                ),
                probe_trial_id=arguments.probe_trial,
            )
        except DiagnosisError as error:
            print(f"pipetune diagnose: {error}", file=sys.stderr)
            return 2
        document = publication.document
    elif arguments.command == "bootstrap":
        try:
            publication = bootstrap_session(
                BootstrapRequest(
                    target_config=pathlib.Path(arguments.target_config),
                    peer_config=pathlib.Path(arguments.peer_config),
                    output=pathlib.Path(arguments.output),
                    configure_binary=pathlib.Path(arguments.axio_configure),
                    max_iterations=arguments.max_iterations,
                    target_binary=arguments.target_binary,
                    peer_binary=arguments.peer_binary,
                )
            )
        except ApplicationError as error:
            print(f"pipetune bootstrap: {error}", file=sys.stderr)
            return 2
        document = publication.document
    elif arguments.command == "resume":
        try:
            publication = resume_session(
                ResumeRequest(
                    session=pathlib.Path(arguments.session),
                    configure_binary=pathlib.Path(arguments.axio_configure),
                )
            )
        except ApplicationError as error:
            print(f"pipetune resume: {error}", file=sys.stderr)
            return 2
        document = publication.document
    elif arguments.command == "status":
        try:
            publication = read_session_status(pathlib.Path(arguments.session))
        except ApplicationError as error:
            print(f"pipetune status: {error}", file=sys.stderr)
            return 2
        document = publication.document
    else:
        raise AssertionError(f"unsupported command {arguments.command}")
    print(
        json.dumps(
            document,
            allow_nan=False,
            ensure_ascii=False,
            indent=2,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
