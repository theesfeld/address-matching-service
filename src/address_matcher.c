#define _POSIX_C_SOURCE 200809L
#include "address_matcher.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libpq-fe.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define AMS_MAX_TOKENS 64
#define AMS_DEFAULT_STRUCTURED_THRESHOLD 0.65
#define AMS_DEFAULT_FUZZY_THRESHOLD 0.55
#define AMS_DEFAULT_LLM_THRESHOLD 0.70
#define AMS_DEFAULT_MAX_CANDIDATES 5
#define AMS_LLM_PAYLOAD_LIMIT 4096
#define AMS_LLM_MAX_INPUT_CANDIDATES 5

typedef struct {
    const char *needle;
    const char *replacement;
} Replacement;

typedef struct {
    const char *token;
    const char *canonical;
} TokenMapping;

static int ensure_capacity(LocationStore *store, size_t required);
static void copy_field(char *dest, size_t dest_size, const char *src);
static void uppercase_inplace(char *value);
static void trim_whitespace(char *value);
static void clear_address_components(AddressComponents *components);
static void compute_canonical_key(AddressComponents *components);
static void add_breakdown_entry(
    ScoreBreakdown *breakdown,
    const char *key,
    const char *left,
    const char *right,
    double weight);
static ScoreBreakdown score_components(
    const AddressComponents *left,
    const AddressComponents *right,
    int require_zip);
static double similarity_ratio(const char *left, const char *right);
static int levenshtein_distance(const char *left, const char *right);
static int is_zip_code(const char *token);
static void canonicalize_zip(char *postal);
static const char *normalize_direction(const char *token);
static const char *normalize_state(const char *token);
static int is_primary_suffix(const char *token);
static int is_unit_token(const char *token);
static int is_unit_followup(const char *token);
static void expand_address_text(const char *source, char *dest, size_t dest_size);
static void normalize_ordinal_token(char *token);
static int extract_house_number(char *token, char *number_out, size_t number_size);
static void match_result_init(MatchResult *result);
static void add_candidate(
    MatchResult *result,
    const LocationRecord *location,
    double confidence,
    const char *strategy,
    const char *reason,
    const ScoreBreakdown *breakdown,
    size_t max_candidates);
static int compare_candidates(const void *lhs, const void *rhs);
static void strategy_canonical(
    const AddressComponents *query,
    const LocationStore *store,
    const MatcherConfig *config,
    MatchResult *result);
static void strategy_structured(
    const AddressComponents *query,
    const LocationStore *store,
    const MatcherConfig *config,
    MatchResult *result);
static void strategy_fuzzy(
    const AddressComponents *query,
    const LocationStore *store,
    const MatcherConfig *config,
    MatchResult *result);
static void strategy_llm(
    const char *raw_address,
    const LocationStore *store,
    const MatcherConfig *config,
    MatchResult *result);
static const LocationRecord *find_location_by_id(
    const LocationStore *store,
    const char *location_id);

static const char *STATE_CODES[] = {
    "AL", "AK", "AZ", "AR", "CA", "CO", "CT", "DE", "FL", "GA", "HI", "ID", "IL", "IN", "IA", "KS",
    "KY", "LA", "ME", "MD", "MA", "MI", "MN", "MS", "MO", "MT", "NE", "NV", "NH", "NJ", "NM", "NY",
    "NC", "ND", "OH", "OK", "OR", "PA", "RI", "SC", "SD", "TN", "TX", "UT", "VT", "VA", "WA", "WV",
    "WI", "WY", "DC"};

static const Replacement EXPANSIONS[] = {
    {" ST ", " STREET "},      {" ST.", " STREET"},      {" AVE ", " AVENUE "},
    {" AVE.", " AVENUE"},      {" RD ", " ROAD "},       {" RD.", " ROAD"},
    {" BLVD ", " BOULEVARD "}, {" BLVD.", " BOULEVARD"}, {" DR ", " DRIVE "},
    {" DR.", " DRIVE"},        {" LN ", " LANE "},       {" LN.", " LANE"},
    {" CT ", " COURT "},       {" CT.", " COURT"},       {" PKY ", " PARKWAY "},
    {" PKWY ", " PARKWAY "},   {" HWY ", " HIGHWAY "},   {" HWY.", " HIGHWAY"},
    {" PL ", " PLACE "},       {" PL.", " PLACE"},       {" SQ ", " SQUARE "},
    {" SQ.", " SQUARE"},       {" CIR ", " CIRCLE "},    {" CIR.", " CIRCLE"},
    {" TER ", " TERRACE "},    {" TER.", " TERRACE"},    {" APT ", " APARTMENT "},
    {" APT.", " APARTMENT"},   {" STE ", " SUITE "},     {" STE.", " SUITE"},
    {" N ", " NORTH "},        {" S ", " SOUTH "},       {" E ", " EAST "},
    {" W ", " WEST "},         {" NE ", " NORTHEAST "},  {" NW ", " NORTHWEST "},
    {" SE ", " SOUTHEAST "},   {" SW ", " SOUTHWEST "}};

static const Replacement NUMBERED_STREETS[] = {
    {" 1ST ", " FIRST "},     {" 2ND ", " SECOND "},         {" 3RD ", " THIRD "},
    {" 4TH ", " FOURTH "},    {" 5TH ", " FIFTH "},          {" 6TH ", " SIXTH "},
    {" 7TH ", " SEVENTH "},   {" 8TH ", " EIGHTH "},         {" 9TH ", " NINTH "},
    {" 10TH ", " TENTH "},    {" 11TH ", " ELEVENTH "},      {" 12TH ", " TWELFTH "},
    {" 13TH ", " THIRTEENTH "},{" 14TH ", " FOURTEENTH "},   {" 15TH ", " FIFTEENTH "},
    {" 16TH ", " SIXTEENTH "},{" 17TH ", " SEVENTEENTH "},   {" 18TH ", " EIGHTEENTH "},
    {" 19TH ", " NINETEENTH "},{" 20TH ", " TWENTIETH "},    {" 21ST ", " TWENTY-FIRST "},
    {" 22ND ", " TWENTY-SECOND "},
    {" 23RD ", " TWENTY-THIRD "},
    {" 24TH ", " TWENTY-FOURTH "},
    {" 25TH ", " TWENTY-FIFTH "},
    {" 26TH ", " TWENTY-SIXTH "},
    {" 27TH ", " TWENTY-SEVENTH "},
    {" 28TH ", " TWENTY-EIGHTH "},
    {" 29TH ", " TWENTY-NINTH "},
    {" 30TH ", " THIRTIETH "},
    {" 31ST ", " THIRTY-FIRST "},
    {" 32ND ", " THIRTY-SECOND "},
    {" 33RD ", " THIRTY-THIRD "},
    {" 34TH ", " THIRTY-FOURTH "},
    {" 35TH ", " THIRTY-FIFTH "},
    {" 36TH ", " THIRTY-SIXTH "},
    {" 37TH ", " THIRTY-SEVENTH "},
    {" 38TH ", " THIRTY-EIGHTH "},
    {" 39TH ", " THIRTY-NINTH "},
    {" 40TH ", " FORTIETH "},
    {" 41ST ", " FORTY-FIRST "},
    {" 42ND ", " FORTY-SECOND "},
    {" 43RD ", " FORTY-THIRD "},
    {" 44TH ", " FORTY-FOURTH "},
    {" 45TH ", " FORTY-FIFTH "},
    {" 46TH ", " FORTY-SIXTH "},
    {" 47TH ", " FORTY-SEVENTH "},
    {" 48TH ", " FORTY-EIGHTH "},
    {" 49TH ", " FORTY-NINTH "},
    {" 50TH ", " FIFTIETH "}};

static const TokenMapping DIRECTIONAL_MAP[] = {
    {"N", "N"},         {"NORTH", "N"},   {"S", "S"},          {"SOUTH", "S"},
    {"E", "E"},         {"EAST", "E"},    {"W", "W"},          {"WEST", "W"},
    {"NE", "NE"},       {"NORTHEAST", "NE"}, {"NW", "NW"},     {"NORTHWEST", "NW"},
    {"SE", "SE"},       {"SOUTHEAST", "SE"}, {"SW", "SW"},     {"SOUTHWEST", "SW"}};

static const char *PRIMARY_SUFFIXES[] = {
    "ALLEY",   "ALLY",   "AVENUE", "AVE", "BEND",   "BLVD", "BOULEVARD", "CIRCLE",
    "CIR",     "COURT",  "CT",     "DRIVE", "DR",   "FREEWAY", "FWY",    "HIGHWAY",
    "HWY",     "LANE",   "LN",     "LOOP", "PARKWAY", "PKWY", "PLACE",  "PL",
    "ROAD",    "RD",     "STREET", "ST",  "TERRACE", "TER",   "TRAIL",  "TRL",
    "WAY"};

static const char *UNIT_TOKENS[] = {
    "APT",  "APARTMENT", "UNIT",   "STE",   "SUITE", "#",     "RM",
    "ROOM", "FLOOR",     "FL",     "LEVEL", "BLDG",  "BUILDING"};

static const double WEIGHTS[] = {0.35, 0.25, 0.05, 0.05, 0.15, 0.05, 0.10};

int location_store_init(LocationStore *store) {
    if (store == NULL) {
        return -1;
    }
    store->items = NULL;
    store->count = 0;
    store->capacity = 0;
    return 0;
}

void location_store_free(LocationStore *store) {
    if (store == NULL) {
        return;
    }
    free(store->items);
    store->items = NULL;
    store->count = 0;
    store->capacity = 0;
}

int location_store_load(LocationStore *store, const char *connection_uri) {
    if (store == NULL || connection_uri == NULL || connection_uri[0] == '\0') {
        return -1;
    }

    PGconn *conn = PQconnectdb(connection_uri);
    if (conn == NULL) {
        return -1;
    }

    if (PQstatus(conn) != CONNECTION_OK) {
        PQfinish(conn);
        return -1;
    }

    const char *query =
        "SELECT location_id, street, city, state, postal_code "
        "FROM locations";

    PGresult *result = PQexec(conn, query);
    if (result == NULL) {
        PQfinish(conn);
        return -1;
    }

    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
        PQclear(result);
        PQfinish(conn);
        return -1;
    }

    int rows = PQntuples(result);
    for (int i = 0; i < rows; ++i) {
        const char *location_id = PQgetvalue(result, i, 0);
        const char *street = PQgetvalue(result, i, 1);
        const char *city = PQgetvalue(result, i, 2);
        const char *state = PQgetvalue(result, i, 3);
        const char *postal_code = PQgetvalue(result, i, 4);

        if (location_id == NULL || street == NULL || city == NULL || state == NULL || postal_code == NULL) {
            continue;
        }

        if (ensure_capacity(store, store->count + 1) != 0) {
            PQclear(result);
            PQfinish(conn);
            return -1;
        }

        LocationRecord *record = &store->items[store->count];
        copy_field(record->location_id, sizeof(record->location_id), location_id);
        copy_field(record->street, sizeof(record->street), street);
        copy_field(record->city, sizeof(record->city), city);
        copy_field(record->state, sizeof(record->state), state);
        copy_field(record->postal_code, sizeof(record->postal_code), postal_code);

        uppercase_inplace(record->street);
        uppercase_inplace(record->city);
        uppercase_inplace(record->state);
        uppercase_inplace(record->postal_code);
        canonicalize_zip(record->postal_code);

        char composite[AMS_MAX_LINE_LENGTH];
        snprintf(
            composite,
            sizeof(composite),
            "%s, %s, %s %s",
            record->street,
            record->city,
            record->state,
            record->postal_code);

        parse_address(composite, &record->components);
        ++store->count;
    }

    PQclear(result);
    PQfinish(conn);
    return 0;
}

int parse_address(const char *input, AddressComponents *out) {
    if (out == NULL) {
        return -1;
    }
    clear_address_components(out);
    if (input == NULL || *input == '\0') {
        return -1;
    }

    char expanded[AMS_MAX_LINE_LENGTH];
    expand_address_text(input, expanded, sizeof(expanded));

    char buffer[AMS_MAX_LINE_LENGTH];
    copy_field(buffer, sizeof(buffer), expanded);
    uppercase_inplace(buffer);

    char tokens[AMS_MAX_TOKENS][AMS_MAX_FIELD_LENGTH];
    int active[AMS_MAX_TOKENS] = {0};
    size_t token_count = 0;

    char *token = strtok(buffer, " ,");
    while (token != NULL && token_count < AMS_MAX_TOKENS) {
        trim_whitespace(token);
        if (*token != '\0') {
            copy_field(tokens[token_count], sizeof(tokens[token_count]), token);
            active[token_count] = 1;
            ++token_count;
        }
        token = strtok(NULL, " ,");
    }

    if (token_count == 0) {
        return -1;
    }

    for (ssize_t idx = (ssize_t)token_count - 1; idx >= 0; --idx) {
        if (!active[idx]) {
            continue;
        }
        if (is_zip_code(tokens[idx])) {
            copy_field(out->postal_code, sizeof(out->postal_code), tokens[idx]);
            canonicalize_zip(out->postal_code);
            active[idx] = 0;
            break;
        }
    }

    for (ssize_t idx = (ssize_t)token_count - 1; idx >= 0; --idx) {
        if (!active[idx]) {
            continue;
        }
        const char *state = normalize_state(tokens[idx]);
        if (state != NULL && state[0] != '\0') {
            copy_field(out->state, sizeof(out->state), state);
            active[idx] = 0;
            break;
        }
    }

    size_t unit_index = SIZE_MAX;
    for (size_t idx = 0; idx < token_count; ++idx) {
        if (!active[idx]) {
            continue;
        }
        if (is_unit_token(tokens[idx])) {
            unit_index = idx;
            break;
        }
        if (tokens[idx][0] == '#') {
            unit_index = idx;
            break;
        }
    }

    if (unit_index != SIZE_MAX) {
        size_t write_index = 0;
        for (size_t idx = unit_index; idx < token_count && active[idx]; ++idx) {
            copy_field(out->unit + write_index, sizeof(out->unit) - write_index, tokens[idx]);
            write_index = strlen(out->unit);
            if (write_index + 1 < sizeof(out->unit)) {
                out->unit[write_index++] = ' ';
                out->unit[write_index] = '\0';
            }
            active[idx] = 0;
            if (idx + 1 >= token_count || !active[idx + 1] || !is_unit_followup(tokens[idx + 1])) {
                break;
            }
        }
        trim_whitespace(out->unit);
        uppercase_inplace(out->unit);
    }

    for (size_t idx = 0; idx < token_count; ++idx) {
        if (!active[idx]) {
            continue;
        }
        if (extract_house_number(tokens[idx], out->street_number, sizeof(out->street_number))) {
            if (tokens[idx][0] == '\0') {
                active[idx] = 0;
            } else {
                copy_field(tokens[idx], sizeof(tokens[idx]), tokens[idx]);
            }
            break;
        }
    }

    for (size_t idx = 0; idx < token_count; ++idx) {
        if (!active[idx]) {
            continue;
        }
        const char *direction = normalize_direction(tokens[idx]);
        if (direction != NULL && direction[0] != '\0') {
            copy_field(out->street_direction, sizeof(out->street_direction), direction);
            active[idx] = 0;
            break;
        }
        break;
    }

    ssize_t suffix_index = -1;
    for (size_t idx = 0; idx < token_count; ++idx) {
        if (!active[idx]) {
            continue;
        }
        if (is_primary_suffix(tokens[idx])) {
            suffix_index = (ssize_t)idx;
            copy_field(out->street_suffix, sizeof(out->street_suffix), tokens[idx]);
            active[idx] = 0;
            break;
        }
    }

    char street_buffer[AMS_MAX_FIELD_LENGTH] = {0};
    char city_buffer[AMS_MAX_FIELD_LENGTH] = {0};

    for (size_t idx = 0; idx < token_count; ++idx) {
        if (!active[idx]) {
            continue;
        }

        if (suffix_index >= 0 && (ssize_t)idx > suffix_index) {
            if (city_buffer[0] != '\0') {
                strncat(city_buffer, " ", sizeof(city_buffer) - strlen(city_buffer) - 1);
            }
            strncat(city_buffer, tokens[idx], sizeof(city_buffer) - strlen(city_buffer) - 1);
        } else {
            char normalized[AMS_MAX_FIELD_LENGTH];
            copy_field(normalized, sizeof(normalized), tokens[idx]);
            normalize_ordinal_token(normalized);
            if (street_buffer[0] != '\0') {
                strncat(street_buffer, " ", sizeof(street_buffer) - strlen(street_buffer) - 1);
            }
            strncat(street_buffer, normalized, sizeof(street_buffer) - strlen(street_buffer) - 1);
        }
    }

    copy_field(out->street_name, sizeof(out->street_name), street_buffer);
    uppercase_inplace(out->street_name);
    copy_field(out->city, sizeof(out->city), city_buffer);
    uppercase_inplace(out->city);

    compute_canonical_key(out);
    return 0;
}

void matcher_config_init(MatcherConfig *config) {
    if (config == NULL) {
        return;
    }
    config->structured_min_confidence = AMS_DEFAULT_STRUCTURED_THRESHOLD;
    config->fuzzy_min_confidence = AMS_DEFAULT_FUZZY_THRESHOLD;
    config->llm_min_confidence = AMS_DEFAULT_LLM_THRESHOLD;
    config->max_candidates = AMS_DEFAULT_MAX_CANDIDATES;
    config->llm_enabled = 0;
    config->llm_command[0] = '\0';

    const char *structured_env = getenv("AMS_STRUCTURED_THRESHOLD");
    if (structured_env && structured_env[0] != '\0') {
        double value = atof(structured_env);
        if (value > 0.0 && value < 1.0) {
            config->structured_min_confidence = value;
        }
    }

    const char *fuzzy_env = getenv("AMS_FUZZY_THRESHOLD");
    if (fuzzy_env && fuzzy_env[0] != '\0') {
        double value = atof(fuzzy_env);
        if (value > 0.0 && value < 1.0) {
            config->fuzzy_min_confidence = value;
        }
    }

    const char *llm_env = getenv("AMS_LLM_THRESHOLD");
    if (llm_env && llm_env[0] != '\0') {
        double value = atof(llm_env);
        if (value > 0.0 && value < 1.0) {
            config->llm_min_confidence = value;
        }
    }

    const char *max_candidates_env = getenv("AMS_MAX_CANDIDATES");
    if (max_candidates_env && max_candidates_env[0] != '\0') {
        int ivalue = atoi(max_candidates_env);
        if (ivalue > 0 && ivalue <= (int)MATCHER_MAX_CANDIDATES) {
            config->max_candidates = (size_t)ivalue;
        }
    }

    const char *llm_command = getenv("AMS_LLM_COMMAND");
    if (llm_command && llm_command[0] != '\0') {
        copy_field(config->llm_command, sizeof(config->llm_command), llm_command);
        config->llm_enabled = 1;
    }
}

void match_record(
    const char *raw_address,
    const LocationStore *store,
    const MatcherConfig *config,
    MatchResult *result) {
    if (result == NULL) {
        return;
    }

    MatcherConfig local_config;
    if (config == NULL) {
        matcher_config_init(&local_config);
        config = &local_config;
    }

    match_result_init(result);
    if (raw_address) {
        copy_field(result->raw_address, sizeof(result->raw_address), raw_address);
    }

    if (parse_address(raw_address, &result->record_components) != 0) {
        snprintf(result->selected_strategy, sizeof(result->selected_strategy), "%s", "none");
        snprintf(result->selected_confidence, sizeof(result->selected_confidence), "%.2f", 0.0);
        return;
    }

    strategy_canonical(&result->record_components, store, config, result);
    strategy_structured(&result->record_components, store, config, result);
    strategy_fuzzy(&result->record_components, store, config, result);
    strategy_llm(raw_address, store, config, result);

    if (result->count > 1) {
        qsort(result->items, result->count, sizeof(MatchCandidate), compare_candidates);
    }

    if (result->count > 0) {
        result->has_best_candidate = 1;
        result->best_index = 0;
        snprintf(
            result->selected_strategy,
            sizeof(result->selected_strategy),
            "%s",
            result->items[0].strategy);
        snprintf(
            result->selected_confidence,
            sizeof(result->selected_confidence),
            "%.3f",
            result->items[0].confidence);
    } else {
        snprintf(result->selected_strategy, sizeof(result->selected_strategy), "%s", "none");
        snprintf(result->selected_confidence, sizeof(result->selected_confidence), "%.2f", 0.0);
    }
}

static int ensure_capacity(LocationStore *store, size_t required) {
    if (store->capacity >= required) {
        return 0;
    }
    size_t new_capacity = (store->capacity == 0) ? 32 : store->capacity * 2;
    while (new_capacity < required) {
        new_capacity *= 2;
    }
    LocationRecord *items = realloc(store->items, new_capacity * sizeof(LocationRecord));
    if (items == NULL) {
        return -1;
    }
    store->items = items;
    store->capacity = new_capacity;
    return 0;
}

static void copy_field(char *dest, size_t dest_size, const char *src) {
    if (dest == NULL || dest_size == 0) {
        return;
    }
    if (src == NULL) {
        dest[0] = '\0';
        return;
    }
    size_t length = strlen(src);
    if (length >= dest_size) {
        length = dest_size - 1;
    }
    memcpy(dest, src, length);
    dest[length] = '\0';
}

static void uppercase_inplace(char *value) {
    if (value == NULL) {
        return;
    }
    for (size_t i = 0; value[i] != '\0'; ++i) {
        value[i] = (char)toupper((unsigned char)value[i]);
    }
}

static void trim_whitespace(char *value) {
    if (value == NULL) {
        return;
    }
    size_t length = strlen(value);
    size_t start = 0;
    while (start < length && isspace((unsigned char)value[start])) {
        ++start;
    }
    size_t end = length;
    while (end > start && isspace((unsigned char)value[end - 1])) {
        --end;
    }
    if (start > 0 || end < length) {
        memmove(value, value + start, end - start);
    }
    value[end - start] = '\0';
}

static void clear_address_components(AddressComponents *components) {
    if (components == NULL) {
        return;
    }
    memset(components, 0, sizeof(*components));
}

static void compute_canonical_key(AddressComponents *components) {
    if (components == NULL) {
        return;
    }
    if (components->street_number[0] == '\0' || components->street_name[0] == '\0') {
        components->canonical_key[0] = '\0';
        return;
    }
    components->canonical_key[0] = '\0';
    const char *parts[] = {
        components->street_number,
        components->street_direction,
        components->street_name,
        components->street_suffix,
        components->city,
        components->state,
        components->postal_code};

    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        size_t current_len = strlen(components->canonical_key);
        if (i > 0 && current_len + 1 < sizeof(components->canonical_key)) {
            components->canonical_key[current_len] = '|';
            components->canonical_key[current_len + 1] = '\0';
            current_len += 1;
        }
        if (parts[i] != NULL && parts[i][0] != '\0') {
            strncat(
                components->canonical_key,
                parts[i],
                sizeof(components->canonical_key) - strlen(components->canonical_key) - 1);
        }
    }
}

static void add_breakdown_entry(
    ScoreBreakdown *breakdown,
    const char *key,
    const char *left,
    const char *right,
    double weight) {
    if (breakdown->comparison_count >= MATCHER_MAX_BREAKDOWN_ENTRIES) {
        return;
    }
    ScoreComparison *entry = &breakdown->comparisons[breakdown->comparison_count++];
    copy_field(entry->key, sizeof(entry->key), key);
    char left_safe[AMS_MAX_FIELD_LENGTH];
    char right_safe[AMS_MAX_FIELD_LENGTH];
    copy_field(left_safe, sizeof(left_safe), left ? left : "");
    copy_field(right_safe, sizeof(right_safe), right ? right : "");
    char combined[AMS_MAX_FIELD_LENGTH];
    size_t remaining = sizeof(combined) - 1;
    size_t length = 0;

    if (left_safe[0] != '\0' && remaining > 0) {
        size_t to_copy = strlen(left_safe);
        if (to_copy > remaining) {
            to_copy = remaining;
        }
        memcpy(combined + length, left_safe, to_copy);
        length += to_copy;
        remaining -= to_copy;
    }

    if (remaining > 0) {
        combined[length++] = '|';
        --remaining;
    }

    if (right_safe[0] != '\0' && remaining > 0) {
        size_t to_copy = strlen(right_safe);
        if (to_copy > remaining) {
            to_copy = remaining;
        }
        memcpy(combined + length, right_safe, to_copy);
        length += to_copy;
        remaining -= to_copy;
    }

    combined[length] = '\0';
    copy_field(entry->value, sizeof(entry->value), combined);
    entry->weight = weight;
}

static ScoreBreakdown score_components(
    const AddressComponents *left,
    const AddressComponents *right,
    int require_zip) {
    ScoreBreakdown breakdown;
    memset(&breakdown, 0, sizeof(breakdown));

    if (left == NULL || right == NULL) {
        return breakdown;
    }

    double score = 0.0;

    double number_match = 0.0;
    if (left->street_number[0] != '\0' && right->street_number[0] != '\0') {
        number_match = (strcmp(left->street_number, right->street_number) == 0) ? 1.0 : 0.0;
    }
    score += WEIGHTS[0] * number_match;
    add_breakdown_entry(
        &breakdown,
        "street_number",
        left->street_number,
        right->street_number,
        WEIGHTS[0]);

    double name_similarity = similarity_ratio(left->street_name, right->street_name);
    score += WEIGHTS[1] * name_similarity;
    add_breakdown_entry(
        &breakdown,
        "street_name",
        left->street_name,
        right->street_name,
        WEIGHTS[1]);

    const char *left_dir = normalize_direction(left->street_direction);
    const char *right_dir = normalize_direction(right->street_direction);
    double dir_match = 0.0;
    if (left_dir[0] != '\0' && right_dir[0] != '\0') {
        dir_match = (strcmp(left_dir, right_dir) == 0) ? 1.0 : 0.0;
    }
    score += WEIGHTS[2] * dir_match;
    add_breakdown_entry(
        &breakdown,
        "directional",
        left_dir,
        right_dir,
        WEIGHTS[2]);

    double suffix_match = 0.0;
    if (left->street_suffix[0] != '\0' && right->street_suffix[0] != '\0') {
        suffix_match = (strcmp(left->street_suffix, right->street_suffix) == 0) ? 1.0 : 0.0;
    }
    score += WEIGHTS[3] * suffix_match;
    add_breakdown_entry(
        &breakdown,
        "suffix",
        left->street_suffix,
        right->street_suffix,
        WEIGHTS[3]);

    double city_similarity = similarity_ratio(left->city, right->city);
    score += WEIGHTS[4] * city_similarity;
    add_breakdown_entry(
        &breakdown,
        "city",
        left->city,
        right->city,
        WEIGHTS[4]);

    double state_match = 0.0;
    if (left->state[0] != '\0' && right->state[0] != '\0') {
        state_match = (strcmp(left->state, right->state) == 0) ? 1.0 : 0.0;
    }
    score += WEIGHTS[5] * state_match;
    add_breakdown_entry(
        &breakdown,
        "state",
        left->state,
        right->state,
        WEIGHTS[5]);

    char left_zip[AMS_MAX_POSTAL_LENGTH];
    char right_zip[AMS_MAX_POSTAL_LENGTH];
    copy_field(left_zip, sizeof(left_zip), left->postal_code);
    copy_field(right_zip, sizeof(right_zip), right->postal_code);
    canonicalize_zip(left_zip);
    canonicalize_zip(right_zip);

    double zip_match = 0.0;
    if (left_zip[0] != '\0' && right_zip[0] != '\0') {
        zip_match = (strcmp(left_zip, right_zip) == 0) ? 1.0 : 0.0;
    }
    if (require_zip && left_zip[0] != '\0' && right_zip[0] == '\0') {
        zip_match = 0.0;
    }
    score += WEIGHTS[6] * zip_match;
    add_breakdown_entry(
        &breakdown,
        "postal_code",
        left_zip,
        right_zip,
        WEIGHTS[6]);

    breakdown.score = score;
    return breakdown;
}

static double similarity_ratio(const char *left, const char *right) {
    if (left == NULL || right == NULL || left[0] == '\0' || right[0] == '\0') {
        return 0.0;
    }
    if (strcmp(left, right) == 0) {
        return 1.0;
    }
    int distance = levenshtein_distance(left, right);
    size_t max_len = strlen(left) > strlen(right) ? strlen(left) : strlen(right);
    if (max_len == 0) {
        return 0.0;
    }
    double ratio = 1.0 - ((double)distance / (double)max_len);
    if (ratio < 0.0) {
        ratio = 0.0;
    } else if (ratio > 1.0) {
        ratio = 1.0;
    }
    return ratio;
}

static int levenshtein_distance(const char *left, const char *right) {
    size_t len_left = strlen(left);
    size_t len_right = strlen(right);
    if (len_left == 0) {
        return (int)len_right;
    }
    if (len_right == 0) {
        return (int)len_left;
    }

    int *prev = (int *)malloc((len_right + 1) * sizeof(int));
    int *curr = (int *)malloc((len_right + 1) * sizeof(int));
    if (prev == NULL || curr == NULL) {
        free(prev);
        free(curr);
        return (int)(len_left > len_right ? len_left : len_right);
    }

    for (size_t j = 0; j <= len_right; ++j) {
        prev[j] = (int)j;
    }

    for (size_t i = 1; i <= len_left; ++i) {
        curr[0] = (int)i;
        for (size_t j = 1; j <= len_right; ++j) {
            int cost = (left[i - 1] == right[j - 1]) ? 0 : 1;
            int deletion = prev[j] + 1;
            int insertion = curr[j - 1] + 1;
            int substitution = prev[j - 1] + cost;
            int minimum = deletion < insertion ? deletion : insertion;
            if (substitution < minimum) {
                minimum = substitution;
            }
            curr[j] = minimum;
        }
        int *tmp = prev;
        prev = curr;
        curr = tmp;
    }

    int distance = prev[len_right];
    free(prev);
    free(curr);
    return distance;
}

static int is_zip_code(const char *token) {
    if (token == NULL || token[0] == '\0') {
        return 0;
    }
    size_t length = strlen(token);
    size_t digits = 0;
    for (size_t i = 0; i < length; ++i) {
        char c = token[i];
        if (isdigit((unsigned char)c)) {
            ++digits;
            continue;
        }
        if (c == '-' && i >= 5) {
            continue;
        }
        return 0;
    }
    return digits >= 5;
}

static void canonicalize_zip(char *postal) {
    if (postal == NULL) {
        return;
    }
    char filtered[AMS_MAX_POSTAL_LENGTH] = {0};
    size_t write_index = 0;
    for (size_t i = 0; postal[i] != '\0' && write_index < sizeof(filtered) - 1; ++i) {
        char c = postal[i];
        if (isdigit((unsigned char)c)) {
            filtered[write_index++] = c;
        } else if (c == '-' && write_index >= 5) {
            filtered[write_index++] = c;
        }
    }
    filtered[write_index] = '\0';
    copy_field(postal, AMS_MAX_POSTAL_LENGTH, filtered);
}

static const char *normalize_direction(const char *token) {
    if (token == NULL) {
        return "";
    }
    for (size_t i = 0; i < sizeof(DIRECTIONAL_MAP) / sizeof(DIRECTIONAL_MAP[0]); ++i) {
        if (strcmp(token, DIRECTIONAL_MAP[i].token) == 0) {
            return DIRECTIONAL_MAP[i].canonical;
        }
    }
    return token;
}

static const char *normalize_state(const char *token) {
    if (token == NULL) {
        return "";
    }
    char upper[AMS_MAX_STATE_LENGTH];
    copy_field(upper, sizeof(upper), token);
    uppercase_inplace(upper);
    if (strlen(upper) == 2) {
        for (size_t i = 0; i < sizeof(STATE_CODES) / sizeof(STATE_CODES[0]); ++i) {
            if (strcmp(upper, STATE_CODES[i]) == 0) {
                return STATE_CODES[i];
            }
        }
    }
    static char fallback[AMS_MAX_STATE_LENGTH];
    copy_field(fallback, sizeof(fallback), upper);
    if (strlen(fallback) > 2) {
        fallback[2] = '\0';
    }
    return fallback;
}

static int is_primary_suffix(const char *token) {
    if (token == NULL) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(PRIMARY_SUFFIXES) / sizeof(PRIMARY_SUFFIXES[0]); ++i) {
        if (strcmp(token, PRIMARY_SUFFIXES[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_unit_token(const char *token) {
    if (token == NULL) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(UNIT_TOKENS) / sizeof(UNIT_TOKENS[0]); ++i) {
        if (strcmp(token, UNIT_TOKENS[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_unit_followup(const char *token) {
    if (token == NULL) {
        return 0;
    }
    if (token[0] == '#') {
        return 1;
    }
    if (isdigit((unsigned char)token[0])) {
        return 1;
    }
    if (strlen(token) <= 3) {
        return 1;
    }
    return 0;
}

static void expand_address_text(const char *source, char *dest, size_t dest_size) {
    if (dest == NULL || dest_size == 0) {
        return;
    }
    dest[0] = '\0';
    if (source == NULL) {
        return;
    }

    char buffer[AMS_MAX_LINE_LENGTH];
    snprintf(buffer, sizeof(buffer), " %s ", source);
    uppercase_inplace(buffer);

    for (size_t i = 0; i < sizeof(EXPANSIONS) / sizeof(EXPANSIONS[0]); ++i) {
        const char *needle = EXPANSIONS[i].needle;
        const char *replacement = EXPANSIONS[i].replacement;
        char *pos = strstr(buffer, needle);
        while (pos) {
            size_t needle_len = strlen(needle);
            size_t replacement_len = strlen(replacement);
            size_t tail_len = strlen(pos + needle_len);
            if (replacement_len > needle_len &&
                strlen(buffer) + (replacement_len - needle_len) >= sizeof(buffer) - 1) {
                break;
            }
            memmove(pos + replacement_len, pos + needle_len, tail_len + 1);
            memcpy(pos, replacement, replacement_len);
            pos = strstr(pos + replacement_len, needle);
        }
    }

    for (size_t i = 0; i < sizeof(NUMBERED_STREETS) / sizeof(NUMBERED_STREETS[0]); ++i) {
        const char *needle = NUMBERED_STREETS[i].needle;
        const char *replacement = NUMBERED_STREETS[i].replacement;
        char *pos = strstr(buffer, needle);
        while (pos) {
            size_t needle_len = strlen(needle);
            size_t replacement_len = strlen(replacement);
            size_t tail_len = strlen(pos + needle_len);
            if (replacement_len > needle_len &&
                strlen(buffer) + (replacement_len - needle_len) >= sizeof(buffer) - 1) {
                break;
            }
            memmove(pos + replacement_len, pos + needle_len, tail_len + 1);
            memcpy(pos, replacement, replacement_len);
            pos = strstr(pos + replacement_len, needle);
        }
    }

    copy_field(dest, dest_size, buffer);
}

static void normalize_ordinal_token(char *token) {
    if (token == NULL) {
        return;
    }
    size_t length = strlen(token);
    if (length < 3) {
        return;
    }
    if (length > 2) {
        size_t offset = length - 2;
        if (isdigit((unsigned char)token[0]) && isdigit((unsigned char)token[offset - 1])) {
            if ((token[offset] == 'S' && token[offset + 1] == 'T') ||
                (token[offset] == 'N' && token[offset + 1] == 'D') ||
                (token[offset] == 'R' && token[offset + 1] == 'D') ||
                (token[offset] == 'T' && token[offset + 1] == 'H')) {
                token[offset] = '\0';
                return;
            }
        }
    }
}

static int extract_house_number(char *token, char *number_out, size_t number_size) {
    if (token == NULL || number_out == NULL) {
        return 0;
    }
    size_t length = strlen(token);
    if (length == 0) {
        return 0;
    }

    size_t idx = 0;
    while (idx < length && (isdigit((unsigned char)token[idx]) || token[idx] == '-')) {
        ++idx;
    }
    if (idx == 0) {
        return 0;
    }

    size_t copy_length = idx < number_size ? idx : number_size - 1;
    memcpy(number_out, token, copy_length);
    number_out[copy_length] = '\0';

    while (idx < length && token[idx] == '-') {
        ++idx;
    }

    if (idx < length) {
        memmove(token, token + idx, length - idx + 1);
    } else {
        token[0] = '\0';
    }
    return 1;
}

static void match_result_init(MatchResult *result) {
    memset(result, 0, sizeof(*result));
}

static int compare_candidates(const void *lhs, const void *rhs) {
    const MatchCandidate *left = (const MatchCandidate *)lhs;
    const MatchCandidate *right = (const MatchCandidate *)rhs;
    if (left->confidence < right->confidence) {
        return 1;
    }
    if (left->confidence > right->confidence) {
        return -1;
    }
    return strcmp(left->location->location_id, right->location->location_id);
}

static void add_candidate(
    MatchResult *result,
    const LocationRecord *location,
    double confidence,
    const char *strategy,
    const char *reason,
    const ScoreBreakdown *breakdown,
    size_t max_candidates) {
    if (result == NULL || location == NULL || strategy == NULL) {
        return;
    }

    size_t existing_index = SIZE_MAX;
    for (size_t i = 0; i < result->count; ++i) {
        if (strcmp(result->items[i].location->location_id, location->location_id) == 0) {
            existing_index = i;
            break;
        }
    }

    if (existing_index != SIZE_MAX) {
        if (confidence > result->items[existing_index].confidence) {
            result->items[existing_index].confidence = confidence;
            copy_field(result->items[existing_index].strategy, sizeof(result->items[existing_index].strategy), strategy);
            if (reason) {
                copy_field(result->items[existing_index].reason, sizeof(result->items[existing_index].reason), reason);
            }
            if (breakdown) {
                result->items[existing_index].breakdown = *breakdown;
            }
        }
        return;
    }

    if (result->count < max_candidates && result->count < MATCHER_MAX_CANDIDATES) {
        MatchCandidate *candidate = &result->items[result->count++];
        candidate->location = location;
        candidate->confidence = confidence;
        copy_field(candidate->strategy, sizeof(candidate->strategy), strategy);
        if (reason) {
            copy_field(candidate->reason, sizeof(candidate->reason), reason);
        } else {
            candidate->reason[0] = '\0';
        }
        if (breakdown) {
            candidate->breakdown = *breakdown;
        } else {
            memset(&candidate->breakdown, 0, sizeof(candidate->breakdown));
        }
        return;
    }

    size_t lowest_index = SIZE_MAX;
    double lowest_confidence = 1.1;
    for (size_t i = 0; i < result->count; ++i) {
        if (result->items[i].confidence < lowest_confidence) {
            lowest_confidence = result->items[i].confidence;
            lowest_index = i;
        }
    }
    if (lowest_index != SIZE_MAX && confidence > lowest_confidence) {
        MatchCandidate *candidate = &result->items[lowest_index];
        candidate->location = location;
        candidate->confidence = confidence;
        copy_field(candidate->strategy, sizeof(candidate->strategy), strategy);
        if (reason) {
            copy_field(candidate->reason, sizeof(candidate->reason), reason);
        } else {
            candidate->reason[0] = '\0';
        }
        if (breakdown) {
            candidate->breakdown = *breakdown;
        } else {
            memset(&candidate->breakdown, 0, sizeof(candidate->breakdown));
        }
    }
}

static void strategy_canonical(
    const AddressComponents *query,
    const LocationStore *store,
    const MatcherConfig *config,
    MatchResult *result) {
    if (query == NULL || store == NULL || result == NULL || query->canonical_key[0] == '\0') {
        return;
    }

    for (size_t i = 0; i < store->count; ++i) {
        const LocationRecord *location = &store->items[i];
        if (location->components.canonical_key[0] == '\0') {
            continue;
        }
        if (strcmp(query->canonical_key, location->components.canonical_key) != 0) {
            continue;
        }
        ScoreBreakdown breakdown = score_components(query, &location->components, 1);
        double confidence = breakdown.score >= 0.9 ? 1.0 : breakdown.score;
        add_candidate(
            result,
            location,
            confidence,
            "canonical",
            "canonical_key_match",
            &breakdown,
            config->max_candidates);
    }
}

static void strategy_structured(
    const AddressComponents *query,
    const LocationStore *store,
    const MatcherConfig *config,
    MatchResult *result) {
    if (query == NULL || store == NULL || result == NULL || config == NULL) {
        return;
    }
    for (size_t i = 0; i < store->count; ++i) {
        const LocationRecord *location = &store->items[i];
        ScoreBreakdown breakdown = score_components(query, &location->components, 0);
        if (breakdown.score >= config->structured_min_confidence) {
            add_candidate(
                result,
                location,
                breakdown.score,
                "structured",
                "weighted_component_score",
                &breakdown,
                config->max_candidates);
        }
    }
}

static void strategy_fuzzy(
    const AddressComponents *query,
    const LocationStore *store,
    const MatcherConfig *config,
    MatchResult *result) {
    if (query == NULL || store == NULL || result == NULL || config == NULL) {
        return;
    }

    for (size_t i = 0; i < store->count; ++i) {
        const LocationRecord *location = &store->items[i];
        ScoreBreakdown structured = score_components(query, &location->components, 0);

        double name_similarity = similarity_ratio(query->street_name, location->components.street_name);
        double city_similarity = similarity_ratio(query->city, location->components.city);
        double postal_similarity = 0.0;
        if (query->postal_code[0] != '\0' && location->components.postal_code[0] != '\0') {
            postal_similarity = similarity_ratio(query->postal_code, location->components.postal_code);
        }

        double fuzzy_score = 0.6 * structured.score + 0.25 * name_similarity + 0.15 * city_similarity;
        if (postal_similarity > 0.8) {
            fuzzy_score += 0.05;
        }
        if (fuzzy_score > 1.0) {
            fuzzy_score = 1.0;
        }

        if (fuzzy_score >= config->fuzzy_min_confidence) {
            add_candidate(
                result,
                location,
                fuzzy_score,
                "fuzzy",
                "approximate_text_similarity",
                &structured,
                config->max_candidates);
        }
    }
}

static void strategy_llm(
    const char *raw_address,
    const LocationStore *store,
    const MatcherConfig *config,
    MatchResult *result) {
    if (config == NULL || result == NULL || !config->llm_enabled || config->llm_command[0] == '\0') {
        return;
    }
    if (result->count == 0 || raw_address == NULL) {
        return;
    }

    char payload[AMS_LLM_PAYLOAD_LIMIT];
    size_t offset = 0;
    offset += snprintf(
        payload + offset,
        sizeof(payload) - offset,
        "{ \"address\": \"%s\", \"candidates\": [",
        result->raw_address);
    size_t limit = result->count < AMS_LLM_MAX_INPUT_CANDIDATES ? result->count : AMS_LLM_MAX_INPUT_CANDIDATES;
    for (size_t i = 0; i < limit && offset < sizeof(payload) - 1; ++i) {
        const MatchCandidate *candidate = &result->items[i];
        offset += snprintf(
            payload + offset,
            sizeof(payload) - offset,
            "%s{ \"location_id\": \"%s\", \"confidence\": %.3f, \"strategy\": \"%s\", \"street\": \"%s\", \"city\": \"%s\", \"state\": \"%s\", \"postal_code\": \"%s\" }",
            (i > 0) ? ", " : "",
            candidate->location->location_id,
            candidate->confidence,
            candidate->strategy,
            candidate->location->street,
            candidate->location->city,
            candidate->location->state,
            candidate->location->postal_code);
    }
    if (offset >= sizeof(payload) - 1) {
        payload[sizeof(payload) - 2] = '\0';
    }
    strncat(payload, "] }", sizeof(payload) - strlen(payload) - 1);

    char template_path[] = "/tmp/ams-llm-XXXXXX";
    int fd = mkstemp(template_path);
    if (fd < 0) {
        return;
    }

    ssize_t written = write(fd, payload, strlen(payload));
    (void)written;
    close(fd);

    char command[AMS_MAX_FIELD_LENGTH + sizeof(template_path) + 2];
    snprintf(command, sizeof(command), "%s %s", config->llm_command, template_path);

    FILE *pipe = popen(command, "r");
    if (pipe == NULL) {
        unlink(template_path);
        return;
    }

    char response[256];
    if (fgets(response, sizeof(response), pipe) == NULL) {
        pclose(pipe);
        unlink(template_path);
        return;
    }
    pclose(pipe);
    unlink(template_path);

    trim_whitespace(response);
    if (response[0] == '\0') {
        return;
    }

    char *token = strtok(response, " ");
    char location_id[AMS_MAX_ID_LENGTH] = {0};
    double confidence = 0.0;
    while (token != NULL) {
        if (strncmp(token, "location_id=", 12) == 0) {
            copy_field(location_id, sizeof(location_id), token + 12);
        } else if (strncmp(token, "confidence=", 11) == 0) {
            confidence = atof(token + 11);
        }
        token = strtok(NULL, " ");
    }

    if (location_id[0] == '\0' || confidence <= 0.0) {
        return;
    }

    if (confidence < config->llm_min_confidence) {
        return;
    }

    const LocationRecord *location = find_location_by_id(store, location_id);
    if (location == NULL) {
        return;
    }

    ScoreBreakdown breakdown = score_components(&result->record_components, &location->components, 0);
    add_candidate(
        result,
        location,
        confidence,
        "llm",
        "llm_ranked",
        &breakdown,
        config->max_candidates);
}

static const LocationRecord *find_location_by_id(
    const LocationStore *store,
    const char *location_id) {
    if (store == NULL || location_id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < store->count; ++i) {
        if (strcmp(store->items[i].location_id, location_id) == 0) {
            return &store->items[i];
        }
    }
    return NULL;
}
