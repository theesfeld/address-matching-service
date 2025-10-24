from __future__ import annotations

import re
from typing import Dict, Optional

US_STATE_ABBREVIATIONS = {
    "AL",
    "AK",
    "AZ",
    "AR",
    "CA",
    "CO",
    "CT",
    "DE",
    "FL",
    "GA",
    "HI",
    "ID",
    "IL",
    "IN",
    "IA",
    "KS",
    "KY",
    "LA",
    "ME",
    "MD",
    "MA",
    "MI",
    "MN",
    "MS",
    "MO",
    "MT",
    "NE",
    "NV",
    "NH",
    "NJ",
    "NM",
    "NY",
    "NC",
    "ND",
    "OH",
    "OK",
    "OR",
    "PA",
    "RI",
    "SC",
    "SD",
    "TN",
    "TX",
    "UT",
    "VT",
    "VA",
    "WA",
    "WV",
    "WI",
    "WY",
    "DC",
}

PRIMARY_STREET_SUFFIXES = {
    "ALLEY",
    "ALLY",
    "AVENUE",
    "AVE",
    "BEND",
    "BLVD",
    "BOULEVARD",
    "CIRCLE",
    "CIR",
    "COURT",
    "CT",
    "DRIVE",
    "DR",
    "FREEWAY",
    "FWY",
    "HIGHWAY",
    "HWY",
    "LANE",
    "LN",
    "LOOP",
    "PARKWAY",
    "PKWY",
    "PLACE",
    "PL",
    "ROAD",
    "RD",
    "STREET",
    "ST",
    "TERRACE",
    "TER",
    "TRAIL",
    "TRL",
    "WAY",
}

DIRECTIONAL_NORMALIZATION: Dict[str, str] = {
    "N": "N",
    "NORTH": "N",
    "S": "S",
    "SOUTH": "S",
    "E": "E",
    "EAST": "E",
    "W": "W",
    "WEST": "W",
    "NE": "NE",
    "NORTHEAST": "NE",
    "NW": "NW",
    "NORTHWEST": "NW",
    "SE": "SE",
    "SOUTHEAST": "SE",
    "SW": "SW",
    "SOUTHWEST": "SW",
}

UNIT_TOKENS = {
    "APT",
    "APARTMENT",
    "UNIT",
    "STE",
    "SUITE",
    "#",
    "RM",
    "ROOM",
    "FLOOR",
    "FL",
    "LEVEL",
    "BLDG",
    "BUILDING",
    "PH",
    "PENTHOUSE",
}

HOUSE_NUMBER_PREFIX_PATTERN = re.compile(r"^(\d+(?:[-/]\d+)?(?:[A-Z])?)(.*)$")
UNIT_FOLLOWUP_PATTERN = re.compile(r"^(?:#?\d+(?:[-/]\d+)?[A-Z]?|[A-Z]\d+|\d+/\d+|[A-Z])$")
ZIP_CODE_PATTERN = re.compile(r"\b\d{5}(?:-\d{4})?\b")
STATE_PATTERN = re.compile(r"\b(?:" + "|".join(US_STATE_ABBREVIATIONS) + r")\b")


def canonicalize_zip(value: Optional[str]) -> str:
    if not value:
        return ""
    match = re.search(r"\d{5}", str(value))
    if match:
        return match.group(0)
    return str(value).strip().upper()


def normalize_direction(token: Optional[str]) -> str:
    if not token:
        return ""
    return DIRECTIONAL_NORMALIZATION.get(token.strip().upper(), token.strip().upper())


def normalize_state(token: Optional[str]) -> str:
    if not token:
        return ""
    token = token.strip().upper()
    if token in US_STATE_ABBREVIATIONS:
        return token
    return token[:2]


def expand_address_text(address_text: str) -> str:
    if not address_text:
        return ""

    address = str(address_text).upper()
    address = f" {address} "

    replacements = {
        " ST ": " STREET ",
        " ST.": " STREET",
        " AVE ": " AVENUE ",
        " AVE.": " AVENUE",
        " RD ": " ROAD ",
        " RD.": " ROAD",
        " BLVD ": " BOULEVARD ",
        " BLVD.": " BOULEVARD",
        " DR ": " DRIVE ",
        " DR.": " DRIVE",
        " LN ": " LANE ",
        " LN.": " LANE",
        " CT ": " COURT ",
        " CT.": " COURT",
        " PKY ": " PARKWAY ",
        " PKWY ": " PARKWAY ",
        " HWY ": " HIGHWAY ",
        " HWY.": " HIGHWAY",
        " PL ": " PLACE ",
        " PL.": " PLACE",
        " SQ ": " SQUARE ",
        " SQ.": " SQUARE",
        " CIR ": " CIRCLE ",
        " CIR.": " CIRCLE",
        " TER ": " TERRACE ",
        " TER.": " TERRACE",
        " APT ": " APARTMENT ",
        " APT.": " APARTMENT",
        " STE ": " SUITE ",
        " STE.": " SUITE",
        " N ": " NORTH ",
        " S ": " SOUTH ",
        " E ": " EAST ",
        " W ": " WEST ",
        " NE ": " NORTHEAST ",
        " NW ": " NORTHWEST ",
        " SE ": " SOUTHEAST ",
        " SW ": " SOUTHWEST ",
    }

    for needle, replacement in replacements.items():
        address = address.replace(needle, f" {replacement.strip()} ")

    numbered_streets = {
        "1ST": "FIRST",
        "2ND": "SECOND",
        "3RD": "THIRD",
        "4TH": "FOURTH",
        "5TH": "FIFTH",
        "6TH": "SIXTH",
        "7TH": "SEVENTH",
        "8TH": "EIGHTH",
        "9TH": "NINTH",
        "10TH": "TENTH",
        "11TH": "ELEVENTH",
        "12TH": "TWELFTH",
        "13TH": "THIRTEENTH",
        "14TH": "FOURTEENTH",
        "15TH": "FIFTEENTH",
        "16TH": "SIXTEENTH",
        "17TH": "SEVENTEENTH",
        "18TH": "EIGHTEENTH",
        "19TH": "NINETEENTH",
        "20TH": "TWENTIETH",
        "21ST": "TWENTY-FIRST",
        "22ND": "TWENTY-SECOND",
        "23RD": "TWENTY-THIRD",
        "24TH": "TWENTY-FOURTH",
        "25TH": "TWENTY-FIFTH",
        "26TH": "TWENTY-SIXTH",
        "27TH": "TWENTY-SEVENTH",
        "28TH": "TWENTY-EIGHTH",
        "29TH": "TWENTY-NINTH",
        "30TH": "THIRTIETH",
        "31ST": "THIRTY-FIRST",
        "32ND": "THIRTY-SECOND",
        "33RD": "THIRTY-THIRD",
        "34TH": "THIRTY-FOURTH",
        "35TH": "THIRTY-FIFTH",
        "36TH": "THIRTY-SIXTH",
        "37TH": "THIRTY-SEVENTH",
        "38TH": "THIRTY-EIGHTH",
        "39TH": "THIRTY-NINTH",
        "40TH": "FORTIETH",
        "41ST": "FORTY-FIRST",
        "42ND": "FORTY-SECOND",
        "43RD": "FORTY-THIRD",
        "44TH": "FORTY-FOURTH",
        "45TH": "FORTY-FIFTH",
        "46TH": "FORTY-SIXTH",
        "47TH": "FORTY-SEVENTH",
        "48TH": "FORTY-EIGHTH",
        "49TH": "FORTY-NINTH",
        "50TH": "FIFTIETH",
    }

    for num, word in numbered_streets.items():
        address = address.replace(f" {num} ", f" {word} ")
        address = address.replace(f" {num},", f" {word},")
        address = address.replace(f" {num}.", f" {word}.")

    return " ".join(address.split())
