"""Human-readable CSV and Markdown publication for AE runs."""

from __future__ import annotations

import csv
import datetime as dt
import json
import pathlib
import statistics
from typing import Any

from artifact_eval.harness import ExperimentCase
from pipetune.artifacts import load_metric_sample, load_session_manifest, load_trial_manifest
from pipetune.diagnosis import summarize_trial
from pipetune.metrics import AXIO_METRICS_SCHEMA, AxioWindow, load_axio_jsonl
from pipetune.runner import AxioConfigTool
from pipetune.session import TuningSessionStore
from pipetune.stage_distribution import (
    DistributionSummary,
    StageDistributionRecord,
    load_stage_distribution_jsonl,
)


class SummaryError(RuntimeError):
    pass


def _object(value: object, location: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise SummaryError(f"{location} must be an object")
    return value


def _number(value: object, location: str) -> float:
    if type(value) not in (int, float):
        raise SummaryError(f"{location} must be numeric")
    return float(value)


def _objective_metric(objective: object, metric: str) -> float:
    document = _object(objective, "objective")
    statistic = _object(document.get(metric), f"objective.{metric}")
    return _number(statistic.get("median"), f"objective.{metric}.median")


def _triplet(document: dict[str, object]) -> str:
    runtime = _object(_object(document.get("knobs"), "knobs").get("runtime"), "runtime")
    c1 = runtime.get("application_core_count")
    c2 = runtime.get("dispatcher_queue_count")
    names = (
        "app_rx_batch_size",
        "app_tx_batch_size",
        "dispatcher_rx_batch_size",
        "dispatcher_tx_batch_size",
        "nic_rx_post_size",
        "nic_tx_post_size",
    )
    c3_values = tuple(runtime.get(name) for name in names)
    c3 = str(c3_values[0]) if len(set(c3_values)) == 1 else "/".join(map(str, c3_values))
    return f"{c1}/{c2}/{c3}"


def _iteration_records(path: pathlib.Path) -> tuple[dict[str, Any], ...]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
        records = tuple(json.loads(line) for line in lines if line)
    except (OSError, json.JSONDecodeError) as error:
        raise SummaryError(f"cannot load {path}: {error}") from error
    if not records or not all(isinstance(record, dict) for record in records):
        raise SummaryError(f"{path} has no iteration records")
    return records


def _baseline_objective(records: tuple[dict[str, Any], ...]) -> dict[str, Any]:
    for record in records:
        baseline = record.get("baseline")
        if isinstance(baseline, dict) and isinstance(baseline.get("objective"), dict):
            return baseline["objective"]
    raise SummaryError("session does not publish a baseline objective")


def _trial_values(session: pathlib.Path, trial_id: str) -> tuple[float, float, float]:
    trial_path = session / "trials" / trial_id / "trial.json"
    manifest = load_trial_manifest(trial_path, artifact_root=trial_path.parent)
    summary = summarize_trial(trial_path)
    endpoints = {item.spec.role: item for item in (summary.target, summary.peer)}
    try:
        throughput = endpoints["server"].throughput.median
        p999 = endpoints["client"].latency["p999_us"].median
    except KeyError as error:
        raise SummaryError(f"trial {trial_id} lacks objective metrics") from error
    started = dt.datetime.fromisoformat(manifest.started_at_utc)
    ended = dt.datetime.fromisoformat(manifest.ended_at_utc)
    return throughput, p999, (ended - started).total_seconds()


def _markdown_table(columns: tuple[str, ...], rows: list[dict[str, object]]) -> list[str]:
    lines = [
        "| " + " | ".join(columns) + " |",
        "| " + " | ".join("---:" if column in {
            "Baseline Mpps", "Best Mpps", "Improvement", "P99.9", "Rounds", "Elapsed s"
        } else "---" for column in columns) + " |",
    ]
    for row in rows:
        lines.append("| " + " | ".join(str(row.get(column, "n/a")) for column in columns) + " |")
    return lines


def write_e2e_summary(
    root: pathlib.Path,
    cases: tuple[ExperimentCase, ...],
    *,
    configure_binary: pathlib.Path,
    title: str = "Axio end-to-end tuning summary",
    introduction: str = (
        "PipeTune decisions are transcribed without additional trend classification."
    ),
) -> None:
    tool = AxioConfigTool(configure_binary)
    top_columns = (
        "Backend",
        "Handler",
        "Session",
        "Baseline Mpps",
        "Best Mpps",
        "Improvement",
        "Baseline C1/C2/C3",
        "Best C1/C2/C3",
        "P99.9",
        "Rounds",
        "Stop reason",
    )
    top_rows: list[dict[str, object]] = []
    detail_sections: list[str] = []
    for case in cases:
        configuration = case.configuration
        baseline_config = tool.dump(
            root / "generated-configs" / configuration.case_id / "target.toml"
        )
        for session_index in range(1, case.sessions + 1):
            session = root / "cases" / configuration.case_id / f"session-{session_index:02d}"
            records = _iteration_records(session / "iterations.jsonl")
            state = TuningSessionStore(session).status()
            convergence = _object(state.details.get("convergence"), "convergence")
            baseline_objective = _baseline_objective(records)
            best_objective = convergence.get("best_objective")
            if not isinstance(best_objective, dict):
                best_objective = baseline_objective
            baseline_mpps = _objective_metric(baseline_objective, "server_throughput")
            best_mpps = _objective_metric(best_objective, "server_throughput")
            best_p999 = _objective_metric(best_objective, "client_p999")
            improvement = (best_mpps / baseline_mpps - 1.0) * 100.0
            best_config = tool.dump(session / "best.toml")
            top_rows.append(
                {
                    "Backend": configuration.backend.upper(),
                    "Handler": (
                        "packet_echo"
                        if configuration.packet_handler == "echo"
                        else configuration.handler
                    ),
                    "Session": session_index,
                    "Baseline Mpps": f"{baseline_mpps:.2f}",
                    "Best Mpps": f"{best_mpps:.2f}",
                    "Improvement": f"{improvement:+.2f}%",
                    "Baseline C1/C2/C3": _triplet(baseline_config),
                    "Best C1/C2/C3": _triplet(best_config),
                    "P99.9": f"{best_p999:.2f} us",
                    "Rounds": convergence.get("completed_rounds"),
                    "Stop reason": convergence.get("stop_reason"),
                }
            )
            detail_columns = (
                "Round",
                "Diagnosis",
                "Baseline trial",
                "Candidate trial",
                "Action",
                "C1/C2/C3",
                "Expected impact",
                "E2E objective",
                "Decision",
                "Throughput Mpps",
                "P99.9 us",
                "Elapsed s",
            )
            detail_rows: list[dict[str, object]] = []
            for record in records:
                diagnosis = record.get("diagnosis")
                result = diagnosis.get("result") if isinstance(diagnosis, dict) else None
                result = result if isinstance(result, dict) else {}
                diagnosis_text = f"{result.get('point', 'unavailable')}/{result.get('direction') or 'n/a'}"
                baseline = record.get("baseline")
                baseline_id = baseline.get("trial_id") if isinstance(baseline, dict) else "n/a"
                outcome = record.get("outcome")
                accepted_id = outcome.get("accepted_trial_id") if isinstance(outcome, dict) else None
                candidates = record.get("candidates")
                if not isinstance(candidates, list) or not candidates:
                    detail_rows.append(
                        {
                            "Round": record.get("round"),
                            "Diagnosis": diagnosis_text,
                            "Baseline trial": baseline_id,
                            "Candidate trial": "none",
                            "Action": "none",
                            "Decision": outcome.get("kind") if isinstance(outcome, dict) else "n/a",
                        }
                    )
                    continue
                for candidate in candidates:
                    if not isinstance(candidate, dict):
                        continue
                    trial_id = candidate.get("trial_id")
                    if not isinstance(trial_id, str):
                        continue
                    throughput, p999, elapsed = _trial_values(session, trial_id)
                    topology = candidate.get("candidate_topology")
                    topology = topology if isinstance(topology, dict) else {}
                    trial_summary = summarize_trial(
                        session / "trials" / trial_id / "trial.json"
                    )
                    runtime = trial_summary.canonical_target["knobs"]["runtime"]
                    c3_values = tuple(
                        runtime[name]
                        for name in (
                            "app_rx_batch_size",
                            "app_tx_batch_size",
                            "dispatcher_rx_batch_size",
                            "dispatcher_tx_batch_size",
                            "nic_rx_post_size",
                            "nic_tx_post_size",
                        )
                    )
                    c3 = c3_values[0] if len(set(c3_values)) == 1 else "/".join(map(str, c3_values))
                    expected = candidate.get("expected_impact")
                    objective = candidate.get("objective")
                    detail_rows.append(
                        {
                            "Round": record.get("round"),
                            "Diagnosis": diagnosis_text,
                            "Baseline trial": baseline_id,
                            "Candidate trial": trial_id,
                            "Action": candidate.get("action"),
                            "C1/C2/C3": f"{topology.get('application_count')}/{topology.get('dispatcher_count')}/{c3}",
                            "Expected impact": "pass" if isinstance(expected, dict) and expected.get("accepted") else "reject",
                            "E2E objective": "pass" if isinstance(objective, dict) and objective.get("accepted") else "reject",
                            "Decision": "accepted" if trial_id == accepted_id else "rollback",
                            "Throughput Mpps": f"{throughput:.2f}",
                            "P99.9 us": f"{p999:.2f}",
                            "Elapsed s": f"{elapsed:.2f}",
                        }
                    )
            detail_sections.extend(
                [
                    "",
                    f"## {configuration.backend.upper()} / {configuration.handler} / session {session_index}",
                    "",
                    *_markdown_table(detail_columns, detail_rows),
                ]
            )

    csv_path = root / "summary.csv"
    with csv_path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=top_columns)
        writer.writeheader()
        writer.writerows(top_rows)
    markdown = [
        f"# {title}",
        "",
        introduction,
        "",
        *_markdown_table(top_columns, top_rows),
        *detail_sections,
        "",
    ]
    (root / "summary.md").write_text("\n".join(markdown), encoding="utf-8")
    print("\n".join(_markdown_table(top_columns, top_rows)))


def _measure_windows(
    session_root: pathlib.Path,
) -> tuple[tuple[AxioWindow, ...], tuple[AxioWindow, ...]]:
    session = load_session_manifest(
        session_root / "session.json", artifact_root=session_root
    )
    if session.status != "complete" or len(session.trials) != 1:
        raise SummaryError(f"{session_root} is not a complete measure session")
    trial_path = session_root / session.trials[0].path
    manifest = load_trial_manifest(trial_path, artifact_root=trial_path.parent)
    by_id = {endpoint.spec.endpoint_id: endpoint for endpoint in manifest.endpoints}
    result: dict[str, tuple[AxioWindow, ...]] = {}
    for endpoint_id, endpoint in by_id.items():
        metric_refs = [
            artifact
            for artifact in endpoint.artifacts
            if artifact.schema == AXIO_METRICS_SCHEMA
        ]
        canonical_refs = [
            artifact
            for artifact in endpoint.artifacts
            if artifact.path == f"configs/canonical/{endpoint_id}.json"
        ]
        if len(metric_refs) != 1 or len(canonical_refs) != 1:
            raise SummaryError(f"{endpoint_id} has ambiguous measure artifacts")
        canonical = json.loads(
            (trial_path.parent / canonical_refs[0].path).read_text(encoding="utf-8")
        )
        warmup = canonical["tuning"]["warmup_windows"]
        sample = canonical["tuning"]["sample_windows"]
        windows = load_axio_jsonl(
            trial_path.parent / metric_refs[0].path,
            schema=AXIO_METRICS_SCHEMA,
        )
        selected = tuple(windows[warmup : warmup + sample])
        if len(selected) != sample:
            raise SummaryError(f"{endpoint_id} has too few sample windows")
        result[endpoint_id] = selected
    return result[manifest.target_endpoint_id], result[
        next(endpoint_id for endpoint_id in result if endpoint_id != manifest.target_endpoint_id)
    ]


def _measure_evidence(
    session_root: pathlib.Path,
) -> tuple[
    tuple[AxioWindow, ...],
    tuple[AxioWindow, ...],
    tuple[StageDistributionRecord, ...],
    dict[str, tuple[float, ...]],
]:
    session = load_session_manifest(
        session_root / "session.json", artifact_root=session_root
    )
    trial_path = session_root / session.trials[0].path
    manifest = load_trial_manifest(trial_path, artifact_root=trial_path.parent)
    target = next(
        endpoint
        for endpoint in manifest.endpoints
        if endpoint.spec.endpoint_id == manifest.target_endpoint_id
    )
    canonical_ref = next(
        artifact
        for artifact in target.artifacts
        if artifact.path == f"configs/canonical/{manifest.target_endpoint_id}.json"
    )
    canonical = json.loads(
        (trial_path.parent / canonical_ref.path).read_text(encoding="utf-8")
    )
    warmup = canonical["tuning"]["warmup_windows"]
    sample = canonical["tuning"]["sample_windows"]
    distribution_refs = [
        artifact
        for artifact in target.artifacts
        if artifact.path.endswith("/stage-distribution.jsonl")
    ]
    distributions: tuple[StageDistributionRecord, ...] = ()
    if distribution_refs:
        if len(distribution_refs) != 1:
            raise SummaryError("target stage-distribution artifact is ambiguous")
        records = load_stage_distribution_jsonl(
            trial_path.parent / distribution_refs[0].path
        )
        distributions = tuple(records[warmup : warmup + sample])
        if len(distributions) != sample:
            raise SummaryError("target has too few stage-distribution windows")
    host_metrics = load_metric_sample(
        trial_path.parent / manifest.host_metrics.path,
        artifact_root=trial_path.parent,
    )
    counters: dict[str, tuple[float, ...]] = {}
    for counter in host_metrics.counters:
        if not counter.available or counter.rate_percent is None:
            continue
        counters[counter.name] = (
            counter.samples_percent
            if counter.samples_percent
            else (counter.rate_percent,)
        )
    target_windows, peer_windows = _measure_windows(session_root)
    return target_windows, peer_windows, distributions, counters


def _distribution(values: list[float]) -> tuple[float, float, float, float]:
    if not values:
        raise SummaryError("throughput distribution is empty")
    median = statistics.median(values)
    mad = statistics.median(abs(value - median) for value in values)
    return median, mad, min(values), max(values)


def _window_throughput(window: AxioWindow, location: str) -> float:
    throughput = window.e2e_throughput_mpps
    if throughput is None or throughput <= 0.0:
        raise SummaryError(f"{location} has no positive end-to-end throughput")
    counters = window.counters
    if counters.app_enqueue_drop_count or counters.dispatcher_enqueue_drop_count:
        raise SummaryError(f"{location} contains enqueue drops")
    if counters.nic_rx_completion_error_count:
        raise SummaryError(f"{location} contains NIC completion errors")
    return throughput


def write_figure3_summary(root: pathlib.Path, cases: tuple[ExperimentCase, ...]) -> None:
    columns = (
        "Axis",
        "Value",
        "C1",
        "C2",
        "C3",
        "Target median Mpps",
        "Target MAD",
        "Target min",
        "Target max",
        "Peer median Mpps",
        "Peer MAD",
        "Peer min",
        "Peer max",
        "Valid samples",
        "Drop errors",
        "Completion errors",
    )
    rows: list[dict[str, object]] = []
    for case in cases:
        config = case.configuration
        target_values: list[float] = []
        peer_values: list[float] = []
        drops = 0
        completion_errors = 0
        for repeat in range(1, case.repeats + 1):
            target_windows, peer_windows = _measure_windows(
                root / "cases" / config.case_id / f"repeat-{repeat:02d}"
            )
            target_values.extend(
                _window_throughput(window, f"{config.case_id} target window")
                for window in target_windows
            )
            peer_values.extend(
                _window_throughput(window, f"{config.case_id} peer window")
                for window in peer_windows
            )
            for window in (*target_windows, *peer_windows):
                drops += (
                    window.counters.app_enqueue_drop_count
                    + window.counters.dispatcher_enqueue_drop_count
                )
                completion_errors += window.counters.nic_rx_completion_error_count
        if drops or completion_errors:
            raise SummaryError(
                f"{config.case_id} contains drops or NIC completion errors"
            )
        target = _distribution(target_values)
        peer = _distribution(peer_values)
        axis = config.case_id[:2].upper()
        value = {"C1": config.c1, "C2": config.c2, "C3": config.c3}[axis]
        rows.append(
            {
                "Axis": axis,
                "Value": value,
                "C1": config.c1,
                "C2": config.c2,
                "C3": config.c3,
                "Target median Mpps": f"{target[0]:.2f}",
                "Target MAD": f"{target[1]:.2f}",
                "Target min": f"{target[2]:.2f}",
                "Target max": f"{target[3]:.2f}",
                "Peer median Mpps": f"{peer[0]:.2f}",
                "Peer MAD": f"{peer[1]:.2f}",
                "Peer min": f"{peer[2]:.2f}",
                "Peer max": f"{peer[3]:.2f}",
                "Valid samples": len(target_values),
                "Drop errors": drops,
                "Completion errors": completion_errors,
            }
        )
    with (root / "summary.csv").open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)
    markdown = [
        "# Figure 3 numeric results",
        "",
        "DPDK, 128-byte L-App echo on the 200 Gbps reference testbed.",
        "",
        *_markdown_table(columns, rows),
        "",
    ]
    (root / "summary.md").write_text("\n".join(markdown), encoding="utf-8")
    print("\n".join(_markdown_table(columns, rows)))


def _summarize_stage(values: list[DistributionSummary]) -> dict[str, float | int]:
    if not values or any(
        item.sample_count == 0
        or item.p1_us is None
        or item.p50_us is None
        or item.p99_us is None
        for item in values
    ):
        raise SummaryError("required stage distribution is unavailable")
    available = values
    sample_count = sum(item.sample_count for item in available)
    return {
        "p1": statistics.median(float(item.p1_us) for item in available),
        "p50": statistics.median(float(item.p50_us) for item in available),
        "p99": statistics.median(float(item.p99_us) for item in available),
        "mean": sum(float(item.mean_us) * item.sample_count for item in available)
        / sample_count,
        "min": min(float(item.min_us) for item in available),
        "max": max(float(item.max_us) for item in available),
        "samples": sample_count,
        "windows": len(available),
    }


def write_stage_figure_summary(
    root: pathlib.Path,
    cases: tuple[ExperimentCase, ...],
    *,
    figure: str,
) -> None:
    if figure not in ("figure6", "figure7", "figure8"):
        raise SummaryError("unsupported stage-distribution figure")
    columns = (
        "Handler",
        "Target role",
        "C1",
        "C2",
        "C3",
        "Stage",
        "P1 us",
        "P50 us",
        "P99 us",
        "Mean us",
        "Min us",
        "Max us",
        "Samples",
        "Windows",
        "Throughput Mpps",
        "LLC load %",
        "LLC store %",
        "ItoM/write %",
    )
    rows: list[dict[str, object]] = []
    for case in cases:
        config = case.configuration
        distributions: list[DistributionSummary] = []
        throughput_values: list[float] = []
        counter_values: dict[str, list[float]] = {
            "llc_load": [],
            "llc_store": [],
            "io_write": [],
        }
        use_allocation = config.handler == "l_app" and figure in ("figure6", "figure8")
        stage_name = (
            "app_tx_allocation_stall"
            if use_allocation
            else "app_rx_handler_completion"
        )
        for repeat in range(1, case.repeats + 1):
            target_windows, _peer, records, counters = _measure_evidence(
                root / "cases" / config.case_id / f"repeat-{repeat:02d}"
            )
            throughput_values.extend(
                _window_throughput(window, f"{config.case_id} target window")
                for window in target_windows
            )
            for window in _peer:
                _window_throughput(window, f"{config.case_id} peer window")
            distributions.extend(
                getattr(record, stage_name) for record in records
            )
            for name in counter_values:
                counter_values[name].extend(counters.get(name, ()))
        stage = _summarize_stage(distributions)
        required_counters: tuple[str, ...]
        if figure == "figure6" and config.handler == "m_app":
            required_counters = ("llc_store",)
        elif figure == "figure7" and config.handler == "l_app":
            required_counters = ("llc_store", "io_write")
        elif config.handler == "t_app":
            required_counters = ("llc_load", "io_write")
        else:
            required_counters = ()
        missing = [name for name in required_counters if not counter_values[name]]
        if missing:
            raise SummaryError(f"{config.case_id} is missing counters {missing}")
        throughput = _distribution(throughput_values)[0]
        counter_median = {
            name: (
                f"{statistics.median(values):.2f}" if values else "n/a"
            )
            for name, values in counter_values.items()
        }
        rows.append(
            {
                "Handler": config.handler,
                "Target role": case.target_role,
                "C1": config.c1,
                "C2": config.c2,
                "C3": config.c3,
                "Stage": stage_name,
                "P1 us": f"{stage['p1']:.2f}",
                "P50 us": f"{stage['p50']:.2f}",
                "P99 us": f"{stage['p99']:.2f}",
                "Mean us": f"{stage['mean']:.2f}",
                "Min us": f"{stage['min']:.2f}",
                "Max us": f"{stage['max']:.2f}",
                "Samples": stage["samples"],
                "Windows": stage["windows"],
                "Throughput Mpps": f"{throughput:.2f}",
                "LLC load %": counter_median["llc_load"],
                "LLC store %": counter_median["llc_store"],
                "ItoM/write %": counter_median["io_write"],
            }
        )
    with (root / "summary.csv").open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)
    title = figure.replace("figure", "Figure ")
    markdown = [
        f"# {title} numeric results",
        "",
        "P1/P50/P99 are medians of the exact per-window percentiles. Mean is sample-count weighted; min and max span all valid windows.",
        "",
        *_markdown_table(columns, rows),
        "",
    ]
    (root / "summary.md").write_text("\n".join(markdown), encoding="utf-8")
    print("\n".join(_markdown_table(columns, rows)))
