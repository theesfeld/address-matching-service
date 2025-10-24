# Address Matching Service

Shared address normalization and matching engine that powers multiple City-Wide Property pipelines. The goals for this package are:

- Provide a deterministic, explainable matching flow that aligns with both the `coding` ingestion pipeline and the financial processing services.
- Offer reusable address parsing, normalization, canonical key generation, and scoring primitives.
- Allow callers to compose multiple matching strategies (canonical lookups, structured heuristics, fuzzy comparison, LLM-assisted ranking) and obtain an arbitrated best match with rich diagnostics.

## Features

- 🇺🇸 US-focused address normalization with ordinal and directional canonicalization.
- Component parsing that preserves unit, city, and ZIP+4 data without losing structure.
- Canonical key generation for cache/index lookups.
- Strategy framework with built-in canonical, structured, and fuzzy passes.
- Deterministic arbitration: every strategy returns a candidate with a confidence/diagnostic payload, and the engine selects the winner using strict validation rules (street number agreement, ZIP tolerances, etc.).
- Optional hooks for LLM-assisted disambiguation.

## Package Layout

```
src/address_matching/
├── __init__.py
├── components.py      # dataclasses + canonical models
├── normalize.py       # shared normalization helpers
├── parser.py          # text → AddressComponents
├── scorer.py          # alignment + confidence scoring
├── strategies.py      # pluggable strategy implementations
└── engine.py          # AddressMatcher orchestrator
tests/
└── test_components.py # baseline coverage & regressions
```

## Getting Started

```bash
python -m venv .venv
source .venv/bin/activate
pip install -e .
pytest
```

Example usage:

```python
from address_matching.engine import AddressMatcher
from address_matching.components import LocationRecord, ServiceRecord

matcher = AddressMatcher()

location = LocationRecord(
    location_id="loc-123",
    street="601 NE 1st Ave",
    city="Miami",
    state="FL",
    postal_code="33132",
)

record = ServiceRecord(
    record_id="row-1",
    raw_address="601 NE 1 AVE, MIAMI, FL 33132",
)

result = matcher.match_record(record, [location])

print(result.best_candidate.location_id)  # loc-123
print(result.best_candidate.confidence)   # 0.99
print(result.best_candidate.strategy)     # canonical
print(result.diagnostics)
```

## Roadmap

- Extract HubSpot-specific search providers into an adapter layer.
- Provide a lightweight HTTP service facade for non-Python consumers.
- Expand test corpus with real-world fixtures from both pipeline repos.

## License

Proprietary – internal City-Wide Property use only.
