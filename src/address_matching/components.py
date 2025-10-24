from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional


@dataclass(frozen=True)
class AddressComponents:
    """Normalized components for an address record."""

    street_number: str = ""
    street_direction: str = ""
    street_name: str = ""
    street_suffix: str = ""
    unit: str = ""
    city: str = ""
    state: str = ""
    postal_code: str = ""

    def canonical_key(self) -> Optional[str]:
        """Return a canonical key suitable for deterministic lookups."""
        if not self.street_number or not self.street_name:
            return None
        parts = [
            self.street_number,
            self.street_direction,
            self.street_name,
            self.street_suffix,
            self.city,
            self.state,
            self.postal_code,
        ]
        return "|".join(parts)

    def as_dict(self) -> Dict[str, str]:
        return {
            "street_number": self.street_number,
            "street_direction": self.street_direction,
            "street_name": self.street_name,
            "street_suffix": self.street_suffix,
            "unit": self.unit,
            "city": self.city,
            "state": self.state,
            "postal_code": self.postal_code,
        }


@dataclass(frozen=True)
class LocationRecord:
    """A reference location we can match against."""

    location_id: str
    street: str
    city: str
    state: str
    postal_code: str = ""
    components: AddressComponents | None = None
    metadata: Dict[str, str] = field(default_factory=dict)


@dataclass(frozen=True)
class ServiceRecord:
    """An incoming record that requires address matching."""

    record_id: str
    raw_address: str = ""
    components_hint: AddressComponents | None = None
    attributes: Dict[str, str] = field(default_factory=dict)


@dataclass
class MatchCandidate:
    """Candidate match supplied by a matching strategy."""

    location: LocationRecord
    confidence: float
    strategy: str
    diagnostics: Dict[str, str] = field(default_factory=dict)
    comparison: Dict[str, str] = field(default_factory=dict)


@dataclass
class MatchResult:
    """Aggregated result after combining multiple strategies."""

    record: ServiceRecord
    candidates: List[MatchCandidate] = field(default_factory=list)
    best_candidate: Optional[MatchCandidate] = None
    diagnostics: Dict[str, str] = field(default_factory=dict)

    def add_candidate(self, candidate: MatchCandidate) -> None:
        self.candidates.append(candidate)
        if self.best_candidate is None or candidate.confidence > self.best_candidate.confidence:
            self.best_candidate = candidate
