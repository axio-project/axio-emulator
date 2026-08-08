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
from pipetune.diagnosis import (
    DiagnosisError,
    DiagnosisPublication,
    publish_diagnosis,
)
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
    diagnose_parser.add_argument(
        "--json",
        action="store_true",
        help="print the full diagnosis JSON written to the session",
    )
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


def _diagnosis_summary(publication: DiagnosisPublication) -> str:
    document = publication.document
    result = document["result"]
    steady = document["steady_state"]
    target = steady["target"]
    peer = steady["peer"]
    leader = target["stage_ranking"][0]
    counters = document["counter_rates"]["baseline"]

    def metric(value: object, suffix: str) -> str:
        if value is None:
            return "unavailable"
        return f"{value['median']:.2f}{suffix}"

    required_probe = result["required_probe"]
    if required_probe is not None:
        next_step = (
            "measure C1 probe "
            f"{required_probe['baseline_value']} -> "
            f"{required_probe['candidate_value']}"
        )
    elif result["point"] == "inconclusive":
        next_step = result["confidence_reasons"][0]
    else:
        next_step = "run bootstrap to validate this hypothesis"

    return "\n".join(
        (
            "PipeTune diagnosis",
            (
                f"  Result: {result['point']} ({result['direction'] or 'n/a'}, "
                f"confidence {result['confidence']})"
            ),
            (
                "  Throughput: target "
                f"{target['throughput']['median']:.2f} Mpps, peer "
                f"{peer['throughput']['median']:.2f} Mpps"
            ),
            (
                f"  Longest stage: {leader['name']} = "
                f"{leader['statistic']['median']:.2f} "
                f"{leader['statistic']['unit']}"
            ),
            (
                "  Counters: LLC load "
                f"{metric(counters['llc_load'], '%')}, LLC store "
                f"{metric(counters['llc_store'], '%')}, I/O read "
                f"{metric(counters['io_read'], '%')}, I/O write "
                f"{metric(counters['io_write'], '%')}"
            ),
            f"  Next: {next_step}",
            f"  Details: {publication.path}",
        )
    )


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
        if not arguments.json:
            print(_diagnosis_summary(publication))
            return 0
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
