#ifndef ADDRESS_MATCHER_H
#define ADDRESS_MATCHER_H

#include <stddef.h>

#define AMS_MAX_ID_LENGTH 64
#define AMS_MAX_FIELD_LENGTH 128
#define AMS_MAX_STATE_LENGTH 16
#define AMS_MAX_POSTAL_LENGTH 32
#define AMS_MAX_DIRECTION_LENGTH 16
#define AMS_MAX_SUFFIX_LENGTH 32
#define AMS_MAX_CANONICAL_LENGTH 256
#define AMS_MAX_LINE_LENGTH 512
#define AMS_MAX_REASON_LENGTH 128
#define AMS_MAX_STRATEGY_LENGTH 32
#define AMS_MAX_DIAGNOSTIC_LENGTH 64
#define MATCHER_MAX_CANDIDATES 16
#define MATCHER_MAX_BREAKDOWN_ENTRIES 8

typedef struct {
    char street_number[AMS_MAX_FIELD_LENGTH];
    char street_direction[AMS_MAX_DIRECTION_LENGTH];
    char street_name[AMS_MAX_FIELD_LENGTH];
    char street_suffix[AMS_MAX_SUFFIX_LENGTH];
    char unit[AMS_MAX_FIELD_LENGTH];
    char city[AMS_MAX_FIELD_LENGTH];
    char state[AMS_MAX_STATE_LENGTH];
    char postal_code[AMS_MAX_POSTAL_LENGTH];
    char canonical_key[AMS_MAX_CANONICAL_LENGTH];
} AddressComponents;

typedef struct {
    char key[AMS_MAX_FIELD_LENGTH];
    char value[AMS_MAX_FIELD_LENGTH];
    double weight;
} ScoreComparison;

typedef struct {
    double score;
    size_t comparison_count;
    ScoreComparison comparisons[MATCHER_MAX_BREAKDOWN_ENTRIES];
} ScoreBreakdown;

typedef struct {
    char location_id[AMS_MAX_ID_LENGTH];
    char street[AMS_MAX_FIELD_LENGTH];
    char city[AMS_MAX_FIELD_LENGTH];
    char state[AMS_MAX_STATE_LENGTH];
    char postal_code[AMS_MAX_POSTAL_LENGTH];
    AddressComponents components;
} LocationRecord;

typedef struct {
    LocationRecord *items;
    size_t count;
    size_t capacity;
} LocationStore;

typedef struct {
    const LocationRecord *location;
    double confidence;
    char strategy[AMS_MAX_STRATEGY_LENGTH];
    char reason[AMS_MAX_REASON_LENGTH];
    ScoreBreakdown breakdown;
} MatchCandidate;

typedef struct {
    MatchCandidate items[MATCHER_MAX_CANDIDATES];
    size_t count;
    int has_best_candidate;
    size_t best_index;
    char selected_strategy[AMS_MAX_STRATEGY_LENGTH];
    char selected_confidence[AMS_MAX_DIAGNOSTIC_LENGTH];
    AddressComponents record_components;
    char raw_address[AMS_MAX_LINE_LENGTH];
} MatchResult;

typedef struct {
    double structured_min_confidence;
    double fuzzy_min_confidence;
    double llm_min_confidence;
    size_t max_candidates;
    int llm_enabled;
    char llm_command[AMS_MAX_FIELD_LENGTH];
} MatcherConfig;

int location_store_init(LocationStore *store);
void location_store_free(LocationStore *store);
int location_store_load(LocationStore *store, const char *connection_uri);

int parse_address(const char *input, AddressComponents *out);
void matcher_config_init(MatcherConfig *config);
void match_record(
    const char *raw_address,
    const LocationStore *store,
    const MatcherConfig *config,
    MatchResult *result);

#endif /* ADDRESS_MATCHER_H */
