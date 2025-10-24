# Address Matching Service (C)

This project provides a standalone C service that performs address normalisation and live best-match selection against the `locations` table in the Citywide Postgres cluster. The server binds to an internal-only interface and rejects any caller outside of the `192.168.1.0/24` network.

Key features:
- Canonical matching for deterministic lookups when the incoming address aligns perfectly with indexed records.
- Weighted structured heuristics that consider directional, suffix, city, state, and ZIP agreement.
- Fuzzy comparison using Levenshtein-based similarities to recover near-misses and data-entry errors.
- Optional LLM re-ranking hook that allows downstream scoring using an external command or model.

## Build From Source

```bash
./configure            # accepts --prefix and --bindir overrides
make
```

The compiled binary lands in `bin/address_matching_service`. `./configure` writes `config.mk`, which the `Makefile` consumes; re-run it whenever you want to adjust installation paths.

> Build prerequisite: the host must provide PostgreSQL client headers and linker libraries (e.g. `libpq-dev` on Debian/Ubuntu).
> If `pg_config` is not on your `PATH`, either export `PG_CONFIG=/full/path/to/pg_config` or pass `./configure --with-pg-config=/full/path/to/pg_config` so the build can locate the headers and libraries.

## Installation

```bash
make install          # install to the configured prefix (default /usr/local)
make uninstall        # optional cleanup helper
make distclean        # remove build artefacts and config.mk
```

You can deploy the prebuilt GitHub release by extracting `address_matching_service` and placing it anywhere on your server, or install from source via the steps above for predictable filesystem layout.

After `make install` the following assets are deployed:

- Binary: `${prefix}/bin/address_matching_service`
- Systemd unit: `${unitdir}/address-matching-service.service`
- Environment template: `${sysconfdir}/env.example`

To configure and launch with systemd (defaults shown):

```bash
sudo cp /etc/address-matching-service/env.example /etc/address-matching-service/env
sudo ${EDITOR:-vi} /etc/address-matching-service/env   # adjust AMS_DB_CONNECTION, bind IP, etc.
sudo systemctl daemon-reload
sudo systemctl enable --now address-matching-service
```

The unit restarts automatically on failure and reads its runtime configuration from the environment file.

## Runtime Configuration

Environment variables control server behaviour at runtime:

| Variable | Description | Default |
| --- | --- | --- |
| `AMS_BIND_ADDRESS` | IPv4 address to bind to. Use an internal interface only. | `192.168.1.10` |
| `AMS_BIND_PORT` | TCP port for the service. | `8080` |
| `AMS_DB_CONNECTION` | Postgres connection URI (`libpq` format). | `postgresql://citywide:excelsior!@citywideportal.io:5433/citywide` |
| `AMS_STRUCTURED_THRESHOLD` | Minimum confidence for the structured strategy. | `0.65` |
| `AMS_FUZZY_THRESHOLD` | Minimum confidence for the fuzzy strategy. | `0.55` |
| `AMS_LLM_THRESHOLD` | Minimum confidence required to accept LLM-ranked matches. | `0.70` |
| `AMS_MAX_CANDIDATES` | Maximum number of candidates retained per request (<= 16). | `5` |
| `AMS_LLM_COMMAND` | Optional command used for LLM re-ranking (see below). | _unset_ |

> **Access control**: Regardless of the bind address, remote callers must originate from `192.168.1.*` or the connection is closed with `403 Forbidden`.

Set these values either via shell environment or by editing the systemd environment file (`/etc/address-matching-service/env` by default).

## Database Source

The matcher reads directly from `citywide.locations` using the configured connection string. The query selects the following columns:

- `location_id`
- `street`
- `city`
- `state`
- `postal_code`

Ensure roles granted to the service account (default credentials embedded above) retain read access to this table. Changes committed to the database are picked up on the next service start.

## API

The service implements a minimal HTTP interface.

### `GET /health`

Returns a simple health check payload.

```
HTTP/1.1 200 OK
Content-Type: application/json

{ "status": "healthy" }
```

### `POST /match`

Body: raw address string (UTF-8 text). Example payload:

```
601 NE 1 AVE, Miami, FL 33132
```

Response (success):

```
HTTP/1.1 200 OK
Content-Type: application/json

{
  "best_candidate": {
    "location_id": "loc-1",
    "confidence": 1.000,
    "strategy": "canonical",
    "reason": "canonical_key_match",
    "street": "601 NE 1ST AVE",
    "city": "MIAMI",
    "state": "FL",
    "postal_code": "33132",
    "breakdown": {
      "street_number": { "value": "601|601", "weight": 0.35 },
      "street_name": { "value": "NE 1|NE 1", "weight": 0.25 },
      "postal_code": { "value": "33132|33132", "weight": 0.10 }
    }
  },
  "candidates": [
    { "location_id": "loc-1", "confidence": 1.000, "strategy": "canonical", "reason": "canonical_key_match" },
    { "location_id": "loc-2", "confidence": 0.720, "strategy": "fuzzy", "reason": "approximate_text_similarity" }
  ],
  "diagnostics": {
    "selected_strategy": "canonical",
    "selected_confidence": "1.000"
  },
  "record_components": {
    "street_number": "601",
    "street_direction": "NE",
    "street_name": "1",
    "street_suffix": "AVE",
    "unit": "",
    "city": "MIAMI",
    "state": "FL",
    "postal_code": "33132",
    "canonical_key": "601|NE|1|AVE|MIAMI|FL|33132"
  }
}
```

If no candidate crosses the confidence threshold the service returns `404 Not Found`.

Example invocation:

```bash
curl -s -X POST \
  --data '601 NE 1 AVE, Miami, FL 33132' \
  http://192.168.1.10:8080/match
```

### Quick Web UI

A lightweight HTML form is available at `GET /`. It is intended for trusted LAN users only and lets you paste an address (or an entire spreadsheet row) and view the JSON response directly in the browser. The interface submits to `/match` using the same API described below.

## Matching Strategies

- **Canonical** (strategy id `canonical`): matches on the exact canonical key derived from street number, directional, name, suffix, city, state, and ZIP. Confidence is forced to 1.0 for high-scoring canonical wins.
- **Structured** (strategy id `structured`): applies weighted comparisons over each component. Adjust `AMS_STRUCTURED_THRESHOLD` to raise or lower admissibility.
- **Fuzzy** (strategy id `fuzzy`): blends the structured score with Levenshtein similarities on street and city tokens, recovering typos or directional swaps. Controlled by `AMS_FUZZY_THRESHOLD`.
- **LLM** (strategy id `llm`): optional re-ranking layer that outsources scoring of the top candidates to an external command.

## LLM Integration

Set `AMS_LLM_COMMAND` to a shell command that accepts a JSON payload file path and prints a single line in the format `location_id=<id> confidence=<score>`. The server creates a temporary file containing the record address and the current candidate list, then executes the command as:

```
${AMS_LLM_COMMAND} /tmp/ams-llm-XXXXXX
```

Example helper script:

```bash
#!/usr/bin/env bash
payload_path="$1"
# Inspect the payload here or forward to an actual LLM.
location=$(jq -r '.candidates[0].location_id' "$payload_path")
echo "location_id=${location} confidence=0.82"
```

Only responses with confidence ≥ `AMS_LLM_THRESHOLD` are retained. If the command is unset or exits without a valid response, the LLM strategy is skipped gracefully.

## Running

```bash
AMS_DB_CONNECTION="postgresql://citywide:excelsior!@citywideportal.io:5433/citywide" \
AMS_BIND_ADDRESS=192.168.1.20 \
AMS_BIND_PORT=8080 \
./bin/address_matching_service
```

When running under systemd, place equivalent exports in `/etc/address-matching-service/env` (or the configured `SYSCONFDIR`). After editing the file run `sudo systemctl restart address-matching-service` to pick up changes. Ensure the host actually owns the `AMS_BIND_ADDRESS` interface, or the bind step will fail.

## Automation & Releases

A GitHub Actions workflow (`.github/workflows/release.yml`) builds the project on every push to `master`, tars the `address_matching_service` binary, and publishes a GitHub Release tagged `release-YYYYMMDDHHMMSS`. To deploy:

1. Push your changes to the `master` branch.
2. Wait for the “C Release” workflow to succeed.
3. Download the latest release asset:
   ```bash
   gh release download --repo <owner>/<repo> --pattern 'address-matching-service-linux.tar.gz'
   tar -xzf address-matching-service-linux.tar.gz
   ```
4. Copy the extracted binary to your server and run it with the desired environment variables.

## Development Notes

- The implementation relies on the system `libpq` client library to talk to Postgres.
- The matching heuristic is deterministic but lightweight. Adjust `src/address_matcher.c` to refine scoring, suffix tables, or parsing rules as new datasets are introduced.

## Technical Integration Guide

This section is aimed at other services that need to call the matcher programmatically.

### Connectivity

- Protocol: HTTP/1.1
- Method: `POST`
- Endpoint: `http://<internal-ip>:<port>/match`
- Network scope: callers must originate from `192.168.1.0/24`. Ensure routing/NAT respects this requirement.
- Content-Type: `text/plain` (the server reads raw body bytes; no JSON envelope is required)
- Body: UTF-8 text containing the address string. Multi-line payloads are supported—the engine concatenates and normalises all tokens.

### Example Request

```bash
curl -s \
  -H 'Content-Type: text/plain; charset=utf-8' \
  --data-binary $'601 NE 1 AVE, Miami, FL 33132' \
  http://192.168.1.10:8080/match
```

### Successful Response

- Status: `200 OK`
- Payload: JSON document describing:
  - `best_candidate`: highest-ranked location with confidence, strategy, and component-by-component breakdown.
  - `candidates`: shortlist (up to `AMS_MAX_CANDIDATES`) with strategy provenance and scores.
  - `diagnostics`: selected strategy/confidence summary.
  - `record_components`: parsed/normalised view of the submitted address.

### Error Responses

| Status | Meaning | Body |
| --- | --- | --- |
| `400 Bad Request` | Address body is empty or could not be parsed. | Plain-text explanation. |
| `403 Forbidden` | Caller IP outside `192.168.1.*`. | Plain-text explanation. |
| `404 Not Found` | No candidate met the configured confidence thresholds. | `{ "message": "No match found" }` |
| `413 Payload Too Large` | Request exceeded `8192` bytes. | Plain-text explanation. |

### Integration Tips

- Preserve the exact address text if possible; additional metadata (customer name, etc.) can be appended—the parser ignores non-address tokens while still benefiting from fuzzy scoring.
- Cache canonical keys (`record_components.canonical_key`) for downstream joins or idempotency.
- Use the per-field breakdown weights to drive UI explanations or auditing (for example, warn when ZIP mismatched but fuzzy match succeeded).
- When enabling LLM re-ranking, deploy the helper command on the same host as the service to avoid shelling out across the network.
