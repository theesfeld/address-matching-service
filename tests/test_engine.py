from address_matching.components import LocationRecord, ServiceRecord
from address_matching.engine import AddressMatcher
from address_matching.parser import parse_address


def build_location(location_id: str, street: str, city: str, state: str, postal_code: str):
    return LocationRecord(
        location_id=location_id,
        street=street,
        city=city,
        state=state,
        postal_code=postal_code,
        components=parse_address(f"{street}, {city}, {state} {postal_code}"),
    )


def test_canonical_match_prefers_exact_number():
    matcher = AddressMatcher()

    locations = [
        build_location("loc-1", "601 NE 1st Ave", "Miami", "FL", "33132"),
        build_location("loc-2", "621 NE 1st Ave", "Miami", "FL", "33132"),
    ]

    record = ServiceRecord(record_id="row-1", raw_address="601 NE 1 AVE, Miami, FL 33132")
    result = matcher.match_record(record, locations)

    assert result.best_candidate is not None
    assert result.best_candidate.location.location_id == "loc-1"
    assert result.best_candidate.confidence >= 0.95
    assert result.best_candidate.strategy == "canonical"


def test_structured_match_handles_zip4():
    matcher = AddressMatcher()
    locations = [
        build_location("loc-10", "123 Main Street", "Chicago", "IL", "60601"),
        build_location("loc-11", "200 Main Street", "Chicago", "IL", "60602"),
    ]

    record = ServiceRecord(record_id="row-2", raw_address="123 Main St, Chicago, IL 60601-1234")
    result = matcher.match_record(record, locations)

    assert result.best_candidate is not None
    assert result.best_candidate.location.location_id == "loc-10"
    assert result.best_candidate.confidence >= 0.8


def test_no_match_returns_empty():
    matcher = AddressMatcher()
    locations = [
        build_location("loc-20", "500 Elm Street", "Dallas", "TX", "75201"),
    ]

    record = ServiceRecord(record_id="row-3", raw_address="99 Unknown Road, Austin, TX 73301")
    result = matcher.match_record(record, locations)

    assert result.best_candidate is None
    assert result.candidates == []
