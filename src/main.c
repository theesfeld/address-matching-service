#include "address_matcher.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_BIND_ADDRESS "192.168.1.10"
#define DEFAULT_BIND_PORT 8080
#define DEFAULT_DB_CONNECTION "postgresql://citywide:excelsior!@citywideportal.io:5433/citywide"
#define RECV_BUFFER_SIZE 8192

static volatile sig_atomic_t keep_running = 1;
static const char *MATCHER_HTML_PAGE =
    "<!DOCTYPE html>\n"
    "<html><head><meta charset=\"utf-8\" />\n"
    "<title>Address Matcher Test</title>\n"
    "<style>body{font-family:sans-serif;margin:2rem;}textarea{width:100%;min-height:8rem;}pre{background:#f4f4f4;padding:1rem;"
    "border:1px solid #ccc;white-space:pre-wrap;word-break:break-word;}button{margin-top:0.5rem;padding:0.4rem 0.8rem;}"
    "</style></head>\n"
    "<body><h1>Address Matcher Test</h1>\n"
    "<form id=\"matchForm\"><label for=\"addressInput\">Paste an address (or entire row):</label><br/>\n"
    "<textarea id=\"addressInput\" placeholder=\"601 NE 1 AVE, Miami, FL 33132\"></textarea><br/>\n"
    "<button type=\"submit\">Match Address</button></form>\n"
    "<pre id=\"responseBox\">HTTP status will appear here.</pre>\n"
    "<script>\n"
    "const form=document.getElementById('matchForm');\n"
    "const textarea=document.getElementById('addressInput');\n"
    "const output=document.getElementById('responseBox');\n"
    "form.addEventListener('submit',async(event)=>{\n"
    "  event.preventDefault();\n"
    "  const address=textarea.value;\n"
    "  if(!address.trim()){output.textContent='Enter an address first.';return;}\n"
    "  output.textContent='Submitting...';\n"
    "  try{\n"
    "    const response=await fetch('/match',{method:'POST',headers:{'Content-Type':'text/plain; charset=utf-8'},body:address});\n"
    "    const text=await response.text();\n"
    "    output.textContent='HTTP '+response.status+' '+response.statusText+'\\n\\n'+text;\n"
    "  }catch(error){output.textContent='Request failed: '+error;}\n"
    "});\n"
    "</script></body></html>\n";

static void handle_signal(int signal_number);
static int setup_server_socket(const char *bind_address, int port);
static int is_client_allowed(const struct sockaddr_in *client_addr);
static void handle_client(int client_fd, const LocationStore *store, const MatcherConfig *config);
static int parse_request(
    char *buffer,
    ssize_t length,
    char *method,
    size_t method_size,
    char *path,
    size_t path_size,
    char **body,
    size_t *body_length);
static void respond_with_json(int client_fd, int status_code, const char *status_text, const char *json_body);
static void respond_with_text(int client_fd, int status_code, const char *status_text, const char *body);
static void respond_with_html(int client_fd, const char *html_body);
static void build_match_response(char *buffer, size_t buffer_size, const MatchResult *result);
static void trim_buffer(char *buffer);
static void json_escape(char *dest, size_t dest_size, const char *src);

int main(void) {
    const char *bind_address = getenv("AMS_BIND_ADDRESS");
    if (bind_address == NULL || bind_address[0] == '\0') {
        bind_address = DEFAULT_BIND_ADDRESS;
    }

    const char *port_env = getenv("AMS_BIND_PORT");
    int port = DEFAULT_BIND_PORT;
    if (port_env != NULL && port_env[0] != '\0') {
        int parsed_port = atoi(port_env);
        if (parsed_port > 0 && parsed_port < 65536) {
            port = parsed_port;
        }
    }

    const char *connection_uri = getenv("AMS_DB_CONNECTION");
    if (connection_uri == NULL || connection_uri[0] == '\0') {
        connection_uri = DEFAULT_DB_CONNECTION;
    }

    LocationStore store;
    if (location_store_init(&store) != 0) {
        fprintf(stderr, "Failed to initialise location store\n");
        return EXIT_FAILURE;
    }

    if (location_store_load(&store, connection_uri) != 0) {
        fprintf(stderr, "Unable to load locations from database.\n");
        location_store_free(&store);
        return EXIT_FAILURE;
    }

    MatcherConfig matcher_config;
    matcher_config_init(&matcher_config);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    int server_fd = setup_server_socket(bind_address, port);
    if (server_fd < 0) {
        location_store_free(&store);
        return EXIT_FAILURE;
    }

    printf("Address Matching Service listening on %s:%d (records: %zu)\n", bind_address, port, store.count);
    fflush(stdout);

    while (keep_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        if (!is_client_allowed(&client_addr)) {
            respond_with_text(client_fd, 403, "Forbidden", "Access denied\r\n");
            close(client_fd);
            continue;
        }

        handle_client(client_fd, &store, &matcher_config);
        close(client_fd);
    }

    close(server_fd);
    location_store_free(&store);
    return EXIT_SUCCESS;
}

static void handle_signal(int signal_number) {
    (void)signal_number;
    keep_running = 0;
}

static int setup_server_socket(const char *bind_address, int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, bind_address, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid bind address: %s\n", bind_address);
        close(server_fd);
        return -1;
    }

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 16) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

static int is_client_allowed(const struct sockaddr_in *client_addr) {
    if (client_addr == NULL) {
        return 0;
    }

    uint32_t client_ip = ntohl(client_addr->sin_addr.s_addr);
    uint32_t lower_bound = (192U << 24) | (168U << 16) | (1U << 8) | 0U;
    uint32_t upper_bound = lower_bound | 0xFFU;

    if (client_ip >= lower_bound && client_ip <= upper_bound) {
        return 1;
    }
    return 0;
}

static void handle_client(int client_fd, const LocationStore *store, const MatcherConfig *config) {
    char buffer[RECV_BUFFER_SIZE + 1];
    ssize_t total = 0;
    ssize_t bytes_read;
    size_t expected_total = 0;
    size_t content_length = 0;
    int header_parsed = 0;

    while ((bytes_read = recv(client_fd, buffer + total, RECV_BUFFER_SIZE - total, 0)) > 0) {
        total += bytes_read;
        buffer[total] = '\0';

        char *header_end = strstr(buffer, "\r\n\r\n");
        if (header_end != NULL) {
            size_t header_length = (size_t)(header_end - buffer + 4);
            if (!header_parsed) {
                char *cursor = buffer;
                char *line_end = strstr(cursor, "\r\n");
                if (line_end == NULL) {
                    respond_with_text(client_fd, 400, "Bad Request", "Malformed request\r\n");
                    return;
                }
                cursor = line_end + 2;
                while (cursor < buffer + header_length) {
                    char *next = strstr(cursor, "\r\n");
                    if (next == NULL) {
                        break;
                    }
                    if (strncasecmp(cursor, "CONTENT-LENGTH:", 15) == 0) {
                        content_length = (size_t)strtoul(cursor + 15, NULL, 10);
                    }
                    cursor = next + 2;
                }
                expected_total = header_length + content_length;
                header_parsed = 1;
            }
            if (header_parsed && total >= (ssize_t)expected_total) {
                break;
            }
        }

        if (total >= RECV_BUFFER_SIZE) {
            respond_with_text(client_fd, 413, "Payload Too Large", "Request too large\r\n");
            return;
        }
    }

    if (bytes_read < 0) {
        perror("recv");
        return;
    }

    char method[8] = {0};
    char path[256] = {0};
    char *body = NULL;
    size_t body_length = 0;

    if (parse_request(buffer, total, method, sizeof(method), path, sizeof(path), &body, &body_length) != 0) {
        respond_with_text(client_fd, 400, "Bad Request", "Unable to parse request\r\n");
        return;
    }

    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        respond_with_html(client_fd, MATCHER_HTML_PAGE);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        respond_with_json(client_fd, 200, "OK", "{ \"status\": \"healthy\" }\r\n");
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/match") == 0) {
        char address_buffer[AMS_MAX_LINE_LENGTH];
        size_t copy_length = body_length < sizeof(address_buffer) - 1 ? body_length : sizeof(address_buffer) - 1;
        memcpy(address_buffer, body, copy_length);
        address_buffer[copy_length] = '\0';
        trim_buffer(address_buffer);

        if (address_buffer[0] == '\0') {
            respond_with_text(client_fd, 400, "Bad Request", "Address body is empty\r\n");
            return;
        }

        MatchResult result;
        match_record(address_buffer, store, config, &result);

        if (result.count == 0) {
            respond_with_json(client_fd, 404, "Not Found", "{ \"message\": \"No match found\" }\r\n");
            return;
        }

        char response_body[2048];
        build_match_response(response_body, sizeof(response_body), &result);
        respond_with_json(client_fd, 200, "OK", response_body);
        return;
    }

    respond_with_text(client_fd, 404, "Not Found", "Endpoint not found\r\n");
}

static int parse_request(
    char *buffer,
    ssize_t length,
    char *method,
    size_t method_size,
    char *path,
    size_t path_size,
    char **body,
    size_t *body_length) {
    (void)method_size;
    (void)path_size;

    if (buffer == NULL || method == NULL || path == NULL || body == NULL || body_length == NULL) {
        return -1;
    }

    if (length <= 0) {
        return -1;
    }

    buffer[length] = '\0';
    char *header_end = strstr(buffer, "\r\n\r\n");
    if (header_end == NULL) {
        return -1;
    }

    char *request_line_end = strstr(buffer, "\r\n");
    if (request_line_end == NULL) {
        return -1;
    }

    *request_line_end = '\0';
    if (sscanf(buffer, "%7s %255s", method, path) != 2) {
        *request_line_end = '\r';
        return -1;
    }
    *request_line_end = '\r';

    size_t header_length = (size_t)(header_end - buffer + 4);
    *body = header_end + 4;
    *body_length = (size_t)length - header_length;
    return 0;
}

static void respond_with_json(int client_fd, int status_code, const char *status_text, const char *json_body) {
    if (json_body == NULL) {
        json_body = "{}";
    }

    size_t body_length = strlen(json_body);
    char header[256];
    int header_length = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code,
        status_text,
        body_length);

    if (header_length < 0) {
        return;
    }

    send(client_fd, header, (size_t)header_length, 0);
    send(client_fd, json_body, body_length, 0);
}

static void respond_with_text(int client_fd, int status_code, const char *status_text, const char *body) {
    if (body == NULL) {
        body = "";
    }

    size_t body_length = strlen(body);
    char header[256];
    int header_length = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code,
        status_text,
        body_length);

    if (header_length < 0) {
        return;
    }

    send(client_fd, header, (size_t)header_length, 0);
    send(client_fd, body, body_length, 0);
}

static void respond_with_html(int client_fd, const char *html_body) {
    if (html_body == NULL) {
        html_body = "";
    }

    size_t body_length = strlen(html_body);
    char header[256];
    int header_length = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        body_length);

    if (header_length < 0) {
        return;
    }

    send(client_fd, header, (size_t)header_length, 0);
    send(client_fd, html_body, body_length, 0);
}

static void json_escape(char *dest, size_t dest_size, const char *src) {
    if (dest == NULL || dest_size == 0) {
        return;
    }
    size_t offset = 0;
    dest[0] = '\0';
    if (src == NULL) {
        src = "";
    }
    for (size_t i = 0; src[i] != '\0' && offset + 1 < dest_size; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (offset + 2 >= dest_size) {
                break;
            }
            dest[offset++] = '\\';
            dest[offset++] = (char)c;
        } else if (c == '\n') {
            if (offset + 2 >= dest_size) {
                break;
            }
            dest[offset++] = '\\';
            dest[offset++] = 'n';
        } else if (c == '\r') {
            if (offset + 2 >= dest_size) {
                break;
            }
            dest[offset++] = '\\';
            dest[offset++] = 'r';
        } else if (c == '\t') {
            if (offset + 2 >= dest_size) {
                break;
            }
            dest[offset++] = '\\';
            dest[offset++] = 't';
        } else if (c < 0x20) {
            continue;
        } else {
            dest[offset++] = (char)c;
        }
    }
    dest[offset] = '\0';
}

static void build_match_response(char *buffer, size_t buffer_size, const MatchResult *result) {
    if (buffer == NULL || buffer_size == 0 || result == NULL) {
        return;
    }

    size_t offset = 0;
    const MatchCandidate *best = NULL;
    if (result->count > 0) {
        best = &result->items[0];
    }

    offset += snprintf(buffer + offset, buffer_size - offset, "{ \"best_candidate\": ");

    if (best != NULL) {
        char location_id[AMS_MAX_FIELD_LENGTH];
        char street[AMS_MAX_FIELD_LENGTH];
        char city[AMS_MAX_FIELD_LENGTH];
        char state[AMS_MAX_FIELD_LENGTH];
        char postal[AMS_MAX_FIELD_LENGTH];
        char strategy[AMS_MAX_FIELD_LENGTH];
        char reason[AMS_MAX_FIELD_LENGTH];

        json_escape(location_id, sizeof(location_id), best->location->location_id);
        json_escape(street, sizeof(street), best->location->street);
        json_escape(city, sizeof(city), best->location->city);
        json_escape(state, sizeof(state), best->location->state);
        json_escape(postal, sizeof(postal), best->location->postal_code);
        json_escape(strategy, sizeof(strategy), best->strategy);
        json_escape(reason, sizeof(reason), best->reason);

        offset += snprintf(
            buffer + offset,
            buffer_size - offset,
            "{ \"location_id\": \"%s\", \"confidence\": %.3f, \"strategy\": \"%s\", \"reason\": \"%s\", \"street\": \"%s\", \"city\": \"%s\", \"state\": \"%s\", \"postal_code\": \"%s\", \"breakdown\": {",
            location_id,
            best->confidence,
            strategy,
            reason,
            street,
            city,
            state,
            postal);

        for (size_t i = 0; i < best->breakdown.comparison_count && offset < buffer_size; ++i) {
            char key[AMS_MAX_FIELD_LENGTH];
            char value[AMS_MAX_FIELD_LENGTH];
            json_escape(key, sizeof(key), best->breakdown.comparisons[i].key);
            json_escape(value, sizeof(value), best->breakdown.comparisons[i].value);
            offset += snprintf(
                buffer + offset,
                buffer_size - offset,
                "%s\"%s\": { \"value\": \"%s\", \"weight\": %.2f }",
                (i > 0) ? ", " : "",
                key,
                value,
                best->breakdown.comparisons[i].weight);
        }
        offset += snprintf(buffer + offset, buffer_size - offset, "} }");
    } else {
        offset += snprintf(buffer + offset, buffer_size - offset, "null");
    }

    offset += snprintf(buffer + offset, buffer_size - offset, ", \"candidates\": [");
    for (size_t i = 0; i < result->count && offset < buffer_size; ++i) {
        const MatchCandidate *candidate = &result->items[i];
        char location_id[AMS_MAX_FIELD_LENGTH];
        char strategy[AMS_MAX_FIELD_LENGTH];
        char reason[AMS_MAX_FIELD_LENGTH];
        json_escape(location_id, sizeof(location_id), candidate->location->location_id);
        json_escape(strategy, sizeof(strategy), candidate->strategy);
        json_escape(reason, sizeof(reason), candidate->reason);

        offset += snprintf(
            buffer + offset,
            buffer_size - offset,
            "%s{ \"location_id\": \"%s\", \"confidence\": %.3f, \"strategy\": \"%s\", \"reason\": \"%s\" }",
            (i > 0) ? ", " : "",
            location_id,
            candidate->confidence,
            strategy,
            reason);
    }
    offset += snprintf(buffer + offset, buffer_size - offset, "], ");

    char selected_strategy[AMS_MAX_FIELD_LENGTH];
    char selected_confidence[AMS_MAX_FIELD_LENGTH];
    json_escape(selected_strategy, sizeof(selected_strategy), result->selected_strategy);
    json_escape(selected_confidence, sizeof(selected_confidence), result->selected_confidence);

    offset += snprintf(
        buffer + offset,
        buffer_size - offset,
        "\"diagnostics\": { \"selected_strategy\": \"%s\", \"selected_confidence\": \"%s\" }, ",
        selected_strategy,
        selected_confidence);

    char street_number[AMS_MAX_FIELD_LENGTH];
    char street_direction[AMS_MAX_FIELD_LENGTH];
    char street_name[AMS_MAX_FIELD_LENGTH];
    char street_suffix[AMS_MAX_FIELD_LENGTH];
    char unit[AMS_MAX_FIELD_LENGTH];
    char city[AMS_MAX_FIELD_LENGTH];
    char state[AMS_MAX_FIELD_LENGTH];
    char postal[AMS_MAX_FIELD_LENGTH];
    char canonical[AMS_MAX_FIELD_LENGTH];

    json_escape(street_number, sizeof(street_number), result->record_components.street_number);
    json_escape(street_direction, sizeof(street_direction), result->record_components.street_direction);
    json_escape(street_name, sizeof(street_name), result->record_components.street_name);
    json_escape(street_suffix, sizeof(street_suffix), result->record_components.street_suffix);
    json_escape(unit, sizeof(unit), result->record_components.unit);
    json_escape(city, sizeof(city), result->record_components.city);
    json_escape(state, sizeof(state), result->record_components.state);
    json_escape(postal, sizeof(postal), result->record_components.postal_code);
    json_escape(canonical, sizeof(canonical), result->record_components.canonical_key);

    offset += snprintf(
        buffer + offset,
        buffer_size - offset,
        "\"record_components\": { \"street_number\": \"%s\", \"street_direction\": \"%s\", \"street_name\": \"%s\", \"street_suffix\": \"%s\", \"unit\": \"%s\", \"city\": \"%s\", \"state\": \"%s\", \"postal_code\": \"%s\", \"canonical_key\": \"%s\" } }",
        street_number,
        street_direction,
        street_name,
        street_suffix,
        unit,
        city,
        state,
        postal,
        canonical);

    if (offset + 2 < buffer_size) {
        buffer[offset++] = '\r';
        buffer[offset++] = '\n';
        buffer[offset] = '\0';
    } else if (offset < buffer_size) {
        buffer[offset] = '\0';
    } else {
        buffer[buffer_size - 1] = '\0';
    }
}

static void trim_buffer(char *buffer) {
    if (buffer == NULL) {
        return;
    }

    size_t start = 0;
    size_t length = strlen(buffer);
    while (start < length && isspace((unsigned char)buffer[start])) {
        ++start;
    }

    size_t end = length;
    while (end > start && isspace((unsigned char)buffer[end - 1])) {
        --end;
    }

    if (start > 0 || end < length) {
        memmove(buffer, buffer + start, end - start);
    }

    buffer[end - start] = '\0';
}
