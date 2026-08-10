"""Small, immutable experiment-profile contracts."""

from __future__ import annotations

import dataclasses


class ModelError(ValueError):
    pass


@dataclasses.dataclass(frozen=True)
class RunProfile:
    name: str
    warmup_windows: int
    sample_windows: int
    repeats: int
    tuning_rounds: int


def profile_defaults(name: str, *, experiment: str) -> RunProfile:
    if name == "smoke":
        return RunProfile(name, 2, 3, 1, 2)
    if name != "paper":
        raise ModelError("profile must be smoke or paper")
    if experiment == "figure3":
        repeats = 20
    elif experiment in ("figure6", "figure7", "figure8"):
        repeats = 5
    else:
        repeats = 1
    rounds = 5 if experiment == "figure14" else 20
    return RunProfile(name, 10, 20, repeats, rounds)
