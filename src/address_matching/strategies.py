from __future__ import annotations

from abc import ABC, abstractmethod
from typing import Iterable, List, Optional

from .components import AddressComponents, LocationRecord, MatchCandidate, ServiceRecord
from .parser import parse_address
from .scorer import score_components


def _components_for_location(location: LocationRecord) -> AddressComponents:
    if location.components:
        return location.components
    return parse_address(
        ", ".join(
            filter(
                None,
                [location.street, location.city, location.state, location.postal_code],
            )
        )
    )


def _components_for_record(record: ServiceRecord) -> AddressComponents:
    if record.components_hint:
        return record.components_hint
    if record.raw_address:
        return parse_address(record.raw_address)
    text = " ".join(record.attributes.values())
    return parse_address(text)


class MatchingStrategy(ABC):
    name: str

    @abstractmethod
    def generate(
        self, record: ServiceRecord, locations: Iterable[LocationRecord]
    ) -> List[MatchCandidate]:
        raise NotImplementedError


class CanonicalStrategy(MatchingStrategy):
    name = "canonical"

    def generate(
        self, record: ServiceRecord, locations: Iterable[LocationRecord]
    ) -> List[MatchCandidate]:
        record_components = _components_for_record(record)
        canonical_key = record_components.canonical_key()
        if not canonical_key:
            return []

        index = {}
        for location in locations:
            loc_components = _components_for_location(location)
            key = loc_components.canonical_key()
            if key:
                index.setdefault(key, []).append((location, loc_components))

        matches: List[MatchCandidate] = []
        for location, loc_components in index.get(canonical_key, []):
            breakdown = score_components(record_components, loc_components)
            # Canonical matches are expected to be perfect
            confidence = 1.0 if breakdown.score >= 0.9 else breakdown.score
            matches.append(
                MatchCandidate(
                    location=location,
                    confidence=confidence,
                    strategy=self.name,
                    diagnostics={"reason": "canonical_key_match"},
                    comparison=breakdown.comparisons,
                )
            )
        return matches


class StructuredHeuristicStrategy(MatchingStrategy):
    name = "structured"

    def __init__(self, minimum_confidence: float = 0.65) -> None:
        self.minimum_confidence = minimum_confidence

    def generate(
        self, record: ServiceRecord, locations: Iterable[LocationRecord]
    ) -> List[MatchCandidate]:
        record_components = _components_for_record(record)
        candidates: List[MatchCandidate] = []
        for location in locations:
            loc_components = _components_for_location(location)
            breakdown = score_components(record_components, loc_components)
            if breakdown.score >= self.minimum_confidence:
                candidates.append(
                    MatchCandidate(
                        location=location,
                        confidence=breakdown.score,
                        strategy=self.name,
                        diagnostics={"reason": "weighted_component_score"},
                        comparison=breakdown.comparisons,
                    )
                )
        return sorted(candidates, key=lambda c: c.confidence, reverse=True)


DEFAULT_STRATEGIES = (CanonicalStrategy(), StructuredHeuristicStrategy())
