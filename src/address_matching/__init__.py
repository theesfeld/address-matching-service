"""Shared address matching engine."""

from .components import AddressComponents, LocationRecord, MatchCandidate, MatchResult, ServiceRecord
from .engine import AddressMatcher

__all__ = [
    "AddressMatcher",
    "AddressComponents",
    "LocationRecord",
    "MatchCandidate",
    "MatchResult",
    "ServiceRecord",
]
