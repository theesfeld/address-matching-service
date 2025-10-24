from address_matching.parser import parse_address


def test_parse_address_extracts_unit_and_city():
    components = parse_address("123 Main St Unit 5, Chicago, IL 60601")
    assert components.street_number == "123"
    assert components.unit == "UNIT 5"
    assert components.city == "CHICAGO"
    assert components.postal_code == "60601"


def test_parse_address_handles_hyphenated_numbers():
    components = parse_address("74-21 46th Ave, Queens, NY 11377")
    assert components.street_number == "74-21"
    assert components.street_name == "46"
    assert components.street_suffix == "AVENUE"


def test_parse_address_normalizes_directional():
    components = parse_address("100 North Main Street, Miami, FL 33132")
    assert components.street_direction == "N"
    assert components.street_name == "MAIN"
