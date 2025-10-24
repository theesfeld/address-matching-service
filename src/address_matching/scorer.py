from __future__ import annotations

from dataclasses import dataclass
from difflib import SequenceMatcher
from typing import Dict, Tuple

from .components import AddressComponents
from .normalize import canonicalize_zip, normalize_direction

try:  # pragma: no cover - optional dependency
    from rapidfuzz import fuzz
except ImportError:  # pragma: no cover - fallback path
    fuzz = None


@dataclass(frozen=True)
class MatchBreakdown:
    score: float
    weights: Dict[str, float]
    comparisons: Dict[str, str]


_WEIGHTS = {
    "street_number": 0.35,
    "street_name": 0.25,
    "directional": 0.05,
    "suffix": 0.05,
    "city": 0.15,
    "state": 0.05,
    "postal_code": 0.10,
}


def _similarity(a: str, b: str) -> float:
    if not a or not b:
        return 0.0
    if a == b:
        return 1.0
    if fuzz:
        return fuzz.token_sort_ratio(a, b) / 100.0
    return SequenceMatcher(None, a, b).ratio()


def score_components(
    left: AddressComponents,
    right: AddressComponents,
    require_zip: bool = False,
) -> MatchBreakdown:
    """Compare two component sets and produce a weighted score."""

    weights = dict(_WEIGHTS)
    comparisons: Dict[str, str] = {}

    score = 0.0

    number_match = 1.0 if left.street_number and left.street_number == right.street_number else 0.0
    comparisons["street_number"] = f"{left.street_number}|{right.street_number}"
    score += weights["street_number"] * number_match

    direction_left = normalize_direction(left.street_direction)
    direction_right = normalize_direction(right.street_direction)
    dir_match = 1.0 if direction_left and direction_left == direction_right else 0.0
    comparisons["directional"] = f"{direction_left}|{direction_right}"
    score += weights["directional"] * dir_match

    name_similarity = _similarity(left.street_name, right.street_name)
    comparisons["street_name"] = f"{left.street_name}|{right.street_name}"  # type: ignore[arg-type]
    score += weights["street_name"] * name_similarity

    suffix_match = 1.0 if left.street_suffix and left.street_suffix == right.street_suffix else 0.0
    comparisons["suffix"] = f"{left.street_suffix}|{right.street_suffix}"
    score += weights["suffix"] * suffix_match

    city_similarity = _similarity(left.city, right.city)
    comparisons["city"] = f"{left.city}|{right.city}"
    score += weights["city"] * city_similarity

    state_match = 1.0 if left.state and right.state and left.state == right.state else 0.0
    comparisons["state"] = f"{left.state}|{right.state}"
    score += weights["state"] * state_match

    zip_left = canonicalize_zip(left.postal_code)
    zip_right = canonicalize_zip(right.postal_code)
    zip_match = 1.0 if zip_left and zip_right and zip_left == zip_right else 0.0
    if require_zip and zip_left and not zip_right:
        zip_match = 0.0
    comparisons["postal_code"] = f"{zip_left}|{zip_right}"
    score += weights["postal_code"] * zip_match

    return MatchBreakdown(score=score, weights=weights, comparisons=comparisons)
