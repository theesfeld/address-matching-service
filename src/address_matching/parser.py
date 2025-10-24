from __future__ import annotations

import re
from typing import Optional

from .components import AddressComponents
from .normalize import (
    DIRECTIONAL_NORMALIZATION,
    HOUSE_NUMBER_PREFIX_PATTERN,
    PRIMARY_STREET_SUFFIXES,
    UNIT_FOLLOWUP_PATTERN,
    UNIT_TOKENS,
    ZIP_CODE_PATTERN,
    canonicalize_zip,
    expand_address_text,
    normalize_direction,
    normalize_state,
)


def _normalize_ordinal_token(token: str) -> str:
    if not token:
        return ""

    base_map = {
        "FIRST": "1",
        "SECOND": "2",
        "THIRD": "3",
        "FOURTH": "4",
        "FIFTH": "5",
        "SIXTH": "6",
        "SEVENTH": "7",
        "EIGHTH": "8",
        "NINTH": "9",
        "TENTH": "10",
        "ELEVENTH": "11",
        "TWELFTH": "12",
        "THIRTEENTH": "13",
        "FOURTEENTH": "14",
        "FIFTEENTH": "15",
        "SIXTEENTH": "16",
        "SEVENTEENTH": "17",
        "EIGHTEENTH": "18",
        "NINETEENTH": "19",
        "TWENTIETH": "20",
        "TWENTY-FIRST": "21",
        "TWENTY-SECOND": "22",
        "TWENTY-THIRD": "23",
        "TWENTY-FOURTH": "24",
        "TWENTY-FIFTH": "25",
        "TWENTY-SIXTH": "26",
        "TWENTY-SEVENTH": "27",
        "TWENTY-EIGHTH": "28",
        "TWENTY-NINTH": "29",
        "THIRTIETH": "30",
        "THIRTY-FIRST": "31",
        "THIRTY-SECOND": "32",
        "THIRTY-THIRD": "33",
        "THIRTY-FOURTH": "34",
        "THIRTY-FIFTH": "35",
        "THIRTY-SIXTH": "36",
        "THIRTY-SEVENTH": "37",
        "THIRTY-EIGHTH": "38",
        "THIRTY-NINTH": "39",
        "FORTIETH": "40",
        "FORTY-FIRST": "41",
        "FORTY-SECOND": "42",
        "FORTY-THIRD": "43",
        "FORTY-FOURTH": "44",
        "FORTY-FIFTH": "45",
        "FORTY-SIXTH": "46",
        "FORTY-SEVENTH": "47",
        "FORTY-EIGHTH": "48",
        "FORTY-NINTH": "49",
        "FIFTIETH": "50",
    }
    ordinal_map = dict(base_map)
    ordinal_map.update({k.replace("-", " "): v for k, v in base_map.items()})

    token_clean = token.replace("-", " ").strip().upper()
    if token_clean in ordinal_map:
        return ordinal_map[token_clean]

    suffix_match = re.fullmatch(r"(\d+)(?:ST|ND|RD|TH)", token_clean)
    if suffix_match:
        return suffix_match.group(1)

    return token_clean


def parse_address(address_text: str) -> AddressComponents:
    if not address_text:
        return AddressComponents()

    expanded = expand_address_text(address_text)
    tokens = [token for token in re.split(r"[\s,]+", expanded) if token]
    if not tokens:
        return AddressComponents()

    number = ""
    directional = ""
    street_tokens = []
    suffix = ""
    unit_tokens = []
    city_tokens = []
    state_token: Optional[str] = None
    postal_code = ""

    # Extract postal code
    for idx in range(len(tokens) - 1, -1, -1):
        token = tokens[idx]
        if ZIP_CODE_PATTERN.fullmatch(token):
            postal_code = canonicalize_zip(token)
            tokens.pop(idx)
            break

    # Extract state
    for idx in range(len(tokens) - 1, -1, -1):
        token = tokens[idx]
        if token in DIRECTIONAL_NORMALIZATION or normalize_state(token) == token:
            candidate = normalize_state(token)
            if candidate in DIRECTIONAL_NORMALIZATION:
                continue
            state_token = candidate
            tokens.pop(idx)
            break

    # Unit detection
    unit_index = None
    for idx, token in enumerate(tokens):
        if token in UNIT_TOKENS or token.startswith("#"):
            unit_index = idx
            break

    if unit_index is not None:
        unit_tokens.append(tokens[unit_index])
        lookahead = unit_index + 1
        while lookahead < len(tokens):
            candidate = tokens[lookahead]
            if candidate in UNIT_TOKENS or candidate.startswith("#") or UNIT_FOLLOWUP_PATTERN.match(candidate):
                unit_tokens.append(candidate)
                lookahead += 1
                continue
            break
        del tokens[unit_index:lookahead]

    # House number
    if tokens:
        number_match = HOUSE_NUMBER_PREFIX_PATTERN.match(tokens[0])
        if number_match:
            number = number_match.group(1).strip()
            remainder = number_match.group(2).strip()
            if remainder:
                tokens[0] = remainder
            else:
                tokens.pop(0)

    # Directional
    if tokens and tokens[0] in DIRECTIONAL_NORMALIZATION:
        directional = normalize_direction(tokens.pop(0))

    suffix_index = None
    for idx, token in enumerate(tokens):
        if token in PRIMARY_STREET_SUFFIXES:
            suffix = token
            suffix_index = idx
            break

    if suffix_index is not None:
        street_tokens = tokens[:suffix_index]
        city_tokens = tokens[suffix_index + 1 :]
    else:
        street_tokens = tokens
        city_tokens = []

    street_tokens = [_normalize_ordinal_token(token) for token in street_tokens]
    street_name = " ".join(street_tokens).strip()
    city = " ".join(city_tokens).strip()

    return AddressComponents(
        street_number=number.upper(),
        street_direction=normalize_direction(directional),
        street_name=street_name.upper(),
        street_suffix=suffix.upper(),
        unit=" ".join(unit_tokens).strip().upper(),
        city=city.upper(),
        state=state_token or "",
        postal_code=postal_code,
    )
