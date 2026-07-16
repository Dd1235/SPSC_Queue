"""Validation and round selection shared by the paper-analysis scripts.

The benchmark writes one row per process.  A usable statistical round must
therefore contain exactly the same configuration set as warm-up round 0.
Keeping this policy in one place prevents a partially completed final round
from giving some queues more samples (and therefore more weight) than others.
"""

from dataclasses import dataclass
import math
from pathlib import Path

import pandas as pd


CONFIG_COLUMNS = [
    "queue",
    "mode",
    "producers",
    "consumers",
    "oversubscribe",
    "capacity",
    "qos",
    "seconds",
    "rate",
]
REQUIRED_COLUMNS = CONFIG_COLUMNS + ["trial", "ops"]


class DatasetError(ValueError):
    """Raised when a CSV cannot be aggregated without ambiguous weighting."""


@dataclass(frozen=True)
class DatasetSelection:
    source_rows: int
    selected_rows: int
    seconds: float
    configurations: int
    complete_trials: tuple[int, ...]
    dropped_trials: tuple[int, ...]
    excluded_known_rows: int
    excluded_incomplete_latency_rows: int
    incomplete_latency_by_queue: tuple[tuple[str, int], ...]

    def describe(self) -> str:
        trials = ",".join(str(t) for t in self.complete_trials)
        details = (
            f"seconds={self.seconds:g}; {self.configurations} configurations; "
            f"retained measured trials {trials}"
        )
        if self.dropped_trials:
            details += "; dropped incomplete/trailing trials " + ",".join(
                str(t) for t in self.dropped_trials
            )
        if self.excluded_known_rows:
            details += f"; excluded {self.excluded_known_rows} documented unsupported rows"
        if self.excluded_incomplete_latency_rows:
            by_queue = ", ".join(
                f"{queue}={count}" for queue, count in self.incomplete_latency_by_queue
            )
            details += (
                f"; excluded {self.excluded_incomplete_latency_rows} non-exact latency "
                f"samples ({by_queue})"
            )
        return details


def _configuration_set(df: pd.DataFrame) -> set[tuple[object, ...]]:
    return set(df[CONFIG_COLUMNS].itertuples(index=False, name=None))


def _known_unsupported(df: pd.DataFrame) -> pd.Series:
    """Rows from configurations documented as incompatible with the harness.

    1. moody/cap64/x4 (throughput): frequently wedges during drain.
    2. moody at saturated offered loads (latency mode, rate >= 8M msg/s at
       4:4 x1): the poison-pill drain protocol assumes a pill enqueued after
       all data cannot starve older data forever, but moodycamel's documented
       per-producer sub-queue ordering lets a consumer take its pill while
       another sub-queue still holds messages.  Beyond the queue's saturation
       point this strands a handful of messages in some rounds; the benchmark's
       accounting invariant then aborts (exit 3).  Retaining the rounds that
       happened to win the race would make sample counts depend on luck, so
       the configuration is excluded symmetrically.

    New matrices skip these arms before execution where possible.
    """

    wedge = (
        df["queue"].eq("moody")
        & df["mode"].eq("throughput")
        & df["producers"].eq(4)
        & df["consumers"].eq(4)
        & df["oversubscribe"].eq(4)
        & df["capacity"].eq(64)
        & df["qos"].eq("none")
    )
    strand = (
        df["queue"].eq("moody")
        & df["mode"].eq("latency")
        & df["producers"].eq(4)
        & df["consumers"].eq(4)
        & df["oversubscribe"].eq(1)
        & df["capacity"].eq(1024)
        & df["qos"].eq("none")
        & df["rate"].ge(8_000_000)
    )
    return wedge | strand


def load_dataset(
    path: str | Path,
    *,
    seconds: float | None = None,
    max_trial: int | None = None,
) -> tuple[pd.DataFrame, DatasetSelection]:
    """Read, validate, and retain a contiguous prefix of complete rounds.

    Trial 0 defines the intended configuration set and remains in the returned
    frame for calibration plots.  Statistical aggregation drops trial 0.  Each
    retained trial >= 1 must exactly match trial 0 after documented exclusions.
    """

    source = Path(path)
    if not source.is_file():
        raise DatasetError(f"dataset not found: {source}")
    try:
        df = pd.read_csv(source)
    except (OSError, pd.errors.ParserError) as exc:
        raise DatasetError(f"cannot read {source}: {exc}") from exc
    if df.empty:
        raise DatasetError(f"dataset is empty: {source}")

    missing = [column for column in REQUIRED_COLUMNS if column not in df.columns]
    if missing:
        raise DatasetError(f"{source} is missing required columns: {', '.join(missing)}")

    source_rows = len(df)
    for column in ("queue", "mode", "qos"):
        if df[column].isna().any() or df[column].astype(str).str.strip().eq("").any():
            raise DatasetError(f"{source}: column {column!r} must contain non-empty values")

    integer_columns = ("producers", "consumers", "oversubscribe", "capacity", "trial", "ops")
    for column in integer_columns:
        converted = pd.to_numeric(df[column], errors="coerce")
        if (converted.isna().any() or not converted.map(math.isfinite).all() or
                (converted % 1 != 0).any() or
                (converted > 2**63 - 1).any() or (converted < -(2**63)).any()):
            raise DatasetError(f"{source}: column {column!r} must contain integers")
        df[column] = converted.astype("int64")
    for column in ("seconds", "rate"):
        converted = pd.to_numeric(df[column], errors="coerce")
        if converted.isna().any() or not converted.map(math.isfinite).all():
            raise DatasetError(f"{source}: column {column!r} must be finite and numeric")
        df[column] = converted.astype("float64")

    for column in ("producers", "consumers", "oversubscribe", "capacity"):
        if (df[column] <= 0).any():
            raise DatasetError(f"{source}: {column} must be positive")
    for column in ("trial", "ops"):
        if (df[column] < 0).any():
            raise DatasetError(f"{source}: {column} must be non-negative")
    for column in ("seconds", "rate"):
        if (df[column] <= 0).any():
            raise DatasetError(f"{source}: {column} must be positive")

    durations = sorted(float(value) for value in df["seconds"].unique())
    if seconds is None:
        if len(durations) != 1:
            choices = ", ".join(f"{value:g}" for value in durations)
            raise DatasetError(
                f"{source} mixes run durations ({choices}); select one with --seconds"
            )
        seconds = durations[0]
    duration_mask = (df["seconds"] - float(seconds)).abs() <= 1e-9
    if not duration_mask.any():
        choices = ", ".join(f"{value:g}" for value in durations)
        raise DatasetError(f"{source} has no rows at {seconds:g}s (available: {choices})")
    df = df[duration_mask].copy()

    unsupported = _known_unsupported(df)
    excluded_known_rows = int(unsupported.sum())
    df = df[~unsupported].copy()

    duplicate_key = CONFIG_COLUMNS + ["trial"]
    duplicates = df.duplicated(duplicate_key, keep=False)
    if duplicates.any():
        example = df.loc[duplicates, duplicate_key].iloc[0].to_dict()
        raise DatasetError(
            f"{source} has duplicate configuration/trial rows (example: {example}); "
            "regenerate or de-duplicate the CSV before aggregation"
        )

    warmup = df[df["trial"] == 0]
    if warmup.empty:
        raise DatasetError(f"{source} has no warm-up round 0")
    expected = _configuration_set(warmup)
    if not expected:
        raise DatasetError(f"{source} has no configurations after exclusions")

    if max_trial is not None and max_trial < 1:
        raise DatasetError("--max-trial must be at least 1")
    observed_trials = sorted(int(t) for t in df.loc[df["trial"] > 0, "trial"].unique())
    upper = max_trial if max_trial is not None else (max(observed_trials) if observed_trials else 0)

    complete: list[int] = []
    first_incomplete: int | None = None
    for trial in range(1, upper + 1):
        actual = _configuration_set(df[df["trial"] == trial])
        if actual != expected:
            first_incomplete = trial
            break
        complete.append(trial)
    if not complete:
        detail = ""
        if first_incomplete is not None:
            actual_count = len(_configuration_set(df[df["trial"] == first_incomplete]))
            detail = f" (trial {first_incomplete}: {actual_count}/{len(expected)} configurations)"
        raise DatasetError(f"{source} has no complete measured round{detail}")

    if max_trial is not None and first_incomplete is not None:
        actual = _configuration_set(df[df["trial"] == first_incomplete])
        raise DatasetError(
            f"{source}: requested --max-trial {max_trial}, but trial {first_incomplete} "
            f"is incomplete ({len(actual)}/{len(expected)} configurations)"
        )

    retained = {0, *complete}
    all_after_warmup = {int(t) for t in observed_trials}
    dropped = tuple(sorted(all_after_warmup - set(complete)))
    df = df[df["trial"].isin(retained)].copy()

    # Latency percentiles are computed from a bounded per-consumer sample
    # buffer in the historical harness.  If any consumer filled its buffer,
    # `ops` is smaller than the exact number scheduled and the retained values
    # are a biased prefix.  Any mismatch is invalid (a larger count would expose
    # a different harness bug). Exclude those rows rather than presenting them
    # as a complete tail distribution. This quality filter deliberately
    # runs after round-completeness selection: the process itself completed,
    # but its latency sample is unusable.
    actual_producers = df["producers"] * df["oversubscribe"]
    per_producer_rate = df["rate"] / actual_producers
    # Match the benchmark's operation order: seconds * (rate / producers),
    # truncated independently by each producer.
    scheduled = ((df["seconds"] * per_producer_rate).astype("int64")
                 * actual_producers)
    incomplete_latency = df["mode"].eq("latency") & df["ops"].ne(scheduled)
    incomplete_counts = (
        df.loc[incomplete_latency, "queue"].value_counts().sort_index().to_dict()
    )
    excluded_incomplete_latency_rows = int(incomplete_latency.sum())
    df = df[~incomplete_latency].copy()
    selection = DatasetSelection(
        source_rows=source_rows,
        selected_rows=len(df),
        seconds=float(seconds),
        configurations=len(expected),
        complete_trials=tuple(complete),
        dropped_trials=dropped,
        excluded_known_rows=excluded_known_rows,
        excluded_incomplete_latency_rows=excluded_incomplete_latency_rows,
        incomplete_latency_by_queue=tuple(
            (str(queue), int(count)) for queue, count in incomplete_counts.items()
        ),
    )
    return df, selection
