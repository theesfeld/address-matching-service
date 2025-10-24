from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable, List, Sequence

from .components import MatchCandidate, MatchResult, ServiceRecord, LocationRecord
from .strategies import DEFAULT_STRATEGIES, MatchingStrategy


@dataclass
class EngineConfig:
    strategies: Sequence[MatchingStrategy] = DEFAULT_STRATEGIES
    max_candidates: int = 5


class AddressMatcher:
    def __init__(self, config: EngineConfig | None = None) -> None:
        self.config = config or EngineConfig()

    def match_record(
        self, record: ServiceRecord, locations: Iterable[LocationRecord]
    ) -> MatchResult:
        result = MatchResult(record=record)
        best_by_location: dict[str, MatchCandidate] = {}

        for strategy in self.config.strategies:
            candidates = strategy.generate(record, locations)
            for candidate in candidates:
                loc_id = candidate.location.location_id
                stored = best_by_location.get(loc_id)
                if stored is None or candidate.confidence > stored.confidence:
                    best_by_location[loc_id] = candidate

        ranked = sorted(best_by_location.values(), key=lambda c: c.confidence, reverse=True)
        for candidate in ranked[: self.config.max_candidates]:
            result.add_candidate(candidate)

        if result.best_candidate:
            result.diagnostics["selected_strategy"] = result.best_candidate.strategy
            result.diagnostics["selected_confidence"] = f"{result.best_candidate.confidence:.3f}"
        else:
            result.diagnostics["selected_strategy"] = "none"
            result.diagnostics["selected_confidence"] = "0"

        return result
