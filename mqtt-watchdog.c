
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>

#include "include/util_linux.h"

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define CONFIG_FILE_DEFAULT "mqtt-watchdog.cfg"

#define MQTT_CLIENT_DEFAULT "mqtt-watchdog"
#define MQTT_SERVER_DEFAULT "mqtt://localhost"

#define EMAIL_SMTP_DEFAULT "smtp://localhost:25"
#define EMAIL_USERNAME_DEFAULT ""
#define EMAIL_PASSWORD_DEFAULT ""
#define EMAIL_USE_SSL_DEFAULT false
#define EMAIL_NAME_DEFAULT "MQTT Watchdog"
#define EMAIL_FROM_DEFAULT "no-reply@localhost"
#define EMAIL_TO_DEFAULT ""
#define EMAIL_SUBJECT_DEFAULT "MQTT Watchdog Alert"

#define NTFY_SERVER_DEFAULT ""
#define NTFY_TOPIC_DEFAULT ""
#define NTFY_TOKEN_DEFAULT ""

#define TOPIC_TIMEOUT_LEVEL1_DEFAULT 60
#define TOPIC_TIMEOUT_LEVEL2_DEFAULT 300
#define TOPIC_PRIORITY_DEFAULT "high"
#define TOPIC_RECOVER_DEFAULT false

#define ALERT_PRIORITY_DEFAULT "default"
#define ALERT_RECOVER_DEFAULT true

#define SERVICE_NAME_DEFAULT ""

#define REPORT_PERIOD_DEFAULT 300

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define CONFIG_MAX_ENTRIES 256

#include "include/config_linux.h"

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define MQTT_CONNECT_TIMEOUT 60
#define MQTT_SUBSCRIBE_QOS 0

#include "include/mqtt_linux.h"

MqttConfig mqttConfig;

bool mqtt_config(void) {
    mqttConfig.server = config_get_string("mqtt-server", MQTT_SERVER_DEFAULT);
    mqttConfig.client = config_get_string("mqtt-client", MQTT_CLIENT_DEFAULT);
    mqttConfig.debug = config_get_bool("debug", false);
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include "include/email_linux.h"

typedef struct {
    const char *smtp;
    const char *username;
    const char *password;
    bool use_ssl;
    const char *name;
    const char *from;
    const char *to;
    const char *subject;
} EmailConfig;

EmailConfig emailConfig;

bool action_email_notification(const char *topic, const char *content) {
    if (strlen(emailConfig.to) == 0 || strlen(emailConfig.smtp) == 0)
        return true;
    char subject[256];
    snprintf(subject, sizeof(subject), "%s: %s", emailConfig.subject, topic);
    if (!email_send(emailConfig.smtp, emailConfig.username, emailConfig.password, emailConfig.use_ssl, emailConfig.name, emailConfig.from, emailConfig.to, subject, content))
        return false;
    printf("email: send notification issued, to '%s'\n", emailConfig.to);
    return true;
}
bool action_email_config(void) {
    emailConfig.smtp = config_get_string("email-smtp", EMAIL_SMTP_DEFAULT);
    emailConfig.username = config_get_string("email-username", EMAIL_USERNAME_DEFAULT);
    emailConfig.password = config_get_string("email-password", EMAIL_PASSWORD_DEFAULT);
    emailConfig.use_ssl = config_get_bool("email-use-ssl", EMAIL_USE_SSL_DEFAULT);
    emailConfig.name = config_get_string("email-name", EMAIL_FROM_DEFAULT);
    emailConfig.from = config_get_string("email-from", EMAIL_NAME_DEFAULT);
    emailConfig.to = config_get_string("email-to", EMAIL_TO_DEFAULT);
    emailConfig.subject = config_get_string("email-subject", EMAIL_SUBJECT_DEFAULT);
    return true;
}
bool action_email_begin(void) { return true; }
void action_email_end(void) {}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

// ntfy (https://ntfy.sh, or a self-hosted server): a push notification per alert, with a priority (min, low, default, high,
// urgent -- urgent breaks through do-not-disturb on phones) and tags (emoji shortcodes). Not configured: skipped.

typedef struct {
    const char *server;
    const char *topic;
    const char *token;
} NtfyConfig;

NtfyConfig ntfyConfig;

int ntfy_priority(const char *priority) {
    static const char *const names[] = {"min", "low", "default", "high", "urgent"};
    for (int i = 0; i < 5; i++)
        if (strcasecmp(priority, names[i]) == 0)
            return i + 1;
    const int number = atoi(priority);
    return (number >= 1 && number <= 5) ? number : 3;
}

size_t __ntfy_response_discard(char *buffer __attribute__((unused)), size_t size, size_t nitems, void *userdata __attribute__((unused))) { return size * nitems; }

bool action_ntfy_notification(const char *priority, const char *tags, const char *title, const char *content) {
    if (strlen(ntfyConfig.server) == 0 || strlen(ntfyConfig.topic) == 0)
        return true;
    // published as JSON to the server root, so that titles and messages need no header encoding
    json_object *message = json_object_new_object();
    json_object_object_add(message, "topic", json_object_new_string(ntfyConfig.topic));
    json_object_object_add(message, "title", json_object_new_string(title));
    json_object_object_add(message, "message", json_object_new_string(strlen(content) > 0 ? content : title));
    json_object_object_add(message, "priority", json_object_new_int(ntfy_priority(priority)));
    if (tags != NULL && *tags != '\0') {
        json_object *list = json_object_new_array();
        char buffer[CONFIG_MAX_STRING];
        snprintf(buffer, sizeof(buffer), "%s", tags);
        for (char *save = NULL, *tag = strtok_r(buffer, ",", &save); tag != NULL; tag = strtok_r(NULL, ",", &save))
            json_object_array_add(list, json_object_new_string(tag));
        json_object_object_add(message, "tags", list);
    }
    bool result = false;
    CURL *curl = curl_easy_init();
    if (curl) {
        struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
        char authorization[CONFIG_MAX_STRING + 32];
        if (strlen(ntfyConfig.token) > 0) {
            snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", ntfyConfig.token);
            headers = curl_slist_append(headers, authorization);
        }
        curl_easy_setopt(curl, CURLOPT_URL, ntfyConfig.server);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_object_to_json_string_ext(message, JSON_C_TO_STRING_PLAIN));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, __ntfy_response_discard);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
        const CURLcode res = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (res != CURLE_OK)
            fprintf(stderr, "ntfy: send failed: %s\n", curl_easy_strerror(res));
        else if (status != 200)
            fprintf(stderr, "ntfy: send failed: HTTP %ld\n", status);
        else {
            printf("ntfy: send notification issued, to '%s' (priority=%s)\n", ntfyConfig.topic, priority);
            result = true;
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    } else
        fprintf(stderr, "ntfy: failed to initialize curl\n");
    json_object_put(message);
    return result;
}
bool action_ntfy_config(void) {
    ntfyConfig.server = config_get_string("ntfy-server", NTFY_SERVER_DEFAULT);
    ntfyConfig.topic = config_get_string("ntfy-topic", NTFY_TOPIC_DEFAULT);
    ntfyConfig.token = config_get_string("ntfy-token", NTFY_TOKEN_DEFAULT);
    if (strlen(ntfyConfig.server) > 0 && strlen(ntfyConfig.topic) > 0)
        printf("ntfy: configured (server='%s', topic='%s', token=%s)\n", ntfyConfig.server, ntfyConfig.topic, strlen(ntfyConfig.token) > 0 ? "yes" : "no");
    return true;
}
bool action_ntfy_begin(void) { return true; }
void action_ntfy_end(void) {}

// every notification goes to each channel that is configured
bool notify(const char *priority, const char *tags, const char *title, const char *content) {
    const bool email = action_email_notification(title, content);
    const bool ntfy = action_ntfy_notification(priority, tags, title, content);
    return email && ntfy;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include "include/systemd_linux.h"

bool action_systemd_service_restart(const char *service_name) {
    if (strlen(service_name) == 0)
        return true;
    if (!systemd_service_restart(service_name))
        return false;
    printf("systemd: service restarted, for '%s'\n", service_name);
    return true;
}
bool action_systemd_config(void) { return true; }
bool action_systemd_begin(void) { return true; }
void action_systemd_end(void) {}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define MAX_TOPICS 32
typedef struct {
    const char *topic;             // MQTT topic to monitor
    const char *service_name;      // Systemd service name (can be NULL)
    time_t warning_seconds;        // Level 1 (notification) threshold in seconds
    time_t restart_seconds;        // Level 2 (restart) threshold in seconds
    const char *priority;          // ntfy priority of its alerts
    bool recover;                  // Notify when messages resume after an alert
    time_t last_message;           // Timestamp of last received message
    time_t silent_for;             // How long it was silent, when messages resumed after an alert
    bool warned;                   // Flag to track if warning has been sent
    bool restarted;                // Flag to track if service has been restarted
                                   //
    unsigned long level1_timeouts; // Level 1 timeout count
    unsigned long level2_timeouts; // Level 2 timeout count
    time_t level1_timelast;        // Level 1 timestamp last
    time_t level2_timelast;        // Level 2 timestamp last
} TopicMonitor;
TopicMonitor topic_monitors[MAX_TOPICS];
int topic_monitor_count = 0;
bool topic_debug = false;
unsigned long topic_level1_timeouts = 0, topic_level2_timeouts = 0;

void topic_receive_message(const char *topic) {
    for (int i = 0; i < topic_monitor_count; i++) {
        TopicMonitor *monitor = &topic_monitors[i];
        if (strcmp(monitor->topic, topic) == 0) {
            const time_t now = time(NULL);
            if (monitor->warned)
                monitor->silent_for = now - monitor->last_message;
            monitor->last_message = now;
            if (topic_debug)
                printf("topic: message received for '%s'\n", topic);
            break;
        }
    }
}
bool topic_process(void) {
    char subject[256];
    const time_t now = time(NULL);
    for (int i = 0; i < topic_monitor_count; i++) {
        TopicMonitor *monitor = &topic_monitors[i];
        const time_t seconds_since_last = now - monitor->last_message;
        // Level 2
        if (seconds_since_last >= monitor->restart_seconds && !monitor->restarted) {
            printf("topic: restart threshold exceeded for '%s' (%ld seconds)\n", monitor->topic, seconds_since_last);
            snprintf(subject, sizeof(subject), "Alert '%s' level-2 timeout (%ld seconds) [notify+restart]", monitor->topic, seconds_since_last);
            notify(monitor->priority, "rotating_light", subject, "");
            action_systemd_service_restart(monitor->service_name);
            monitor->warned = true;
            monitor->restarted = true;
            monitor->level2_timeouts++;
            monitor->level2_timelast = now;
            topic_level2_timeouts++;
        }
        // Level 1
        else if (seconds_since_last >= monitor->warning_seconds && !monitor->warned) {
            printf("topic: warning threshold exceeded for '%s' (%ld seconds)\n", monitor->topic, seconds_since_last);
            snprintf(subject, sizeof(subject), "Alert '%s' level-1 timeout (%ld seconds) [notify]", monitor->topic, seconds_since_last);
            notify(monitor->priority, "warning", subject, "");
            monitor->warned = true;
            monitor->level1_timeouts++;
            monitor->level1_timelast = now;
            topic_level1_timeouts++;
        }
        if (seconds_since_last < monitor->warning_seconds) {
            if (monitor->warned && monitor->recover) {
                printf("topic: messages resumed for '%s' (silent %ld seconds)\n", monitor->topic, monitor->silent_for);
                snprintf(subject, sizeof(subject), "Recovery '%s' (silent %ld seconds)", monitor->topic, monitor->silent_for);
                notify("default", "white_check_mark", subject, "");
            }
            monitor->warned = monitor->restarted = false;
        }
    }
    return true;
}
bool topic_stats_to_string(char *buffer, size_t size) {
    size_t offset = (size_t)snprintf(buffer, size, "L1=%lu, L2=%lu: ", topic_level1_timeouts, topic_level2_timeouts);
    for (int i = 0; i < topic_monitor_count; i++) {
        TopicMonitor *monitor = &topic_monitors[i];
        offset += (size_t)snprintf(buffer + offset, size - offset, "%s%s", (i == 0 ? "" : ", "), monitor->topic);
        if (monitor->level1_timeouts > 0 || monitor->level2_timeouts > 0) {
            offset += (size_t)snprintf(buffer + offset, size - offset, " (");
            if (monitor->level1_timeouts > 0) {
                offset += (size_t)snprintf(buffer + offset, size - offset, "L1=%lu/", monitor->level1_timeouts);
                offset += (size_t)strftime(buffer + offset, size - offset, "%Y-%m-%dT%H:%M:%S", localtime(&monitor->level1_timelast));
            }
            if (monitor->level2_timeouts > 0) {
                if (monitor->level1_timeouts > 0)
                    offset += (size_t)snprintf(buffer + offset, size - offset, ", ");
                offset += (size_t)snprintf(buffer + offset, size - offset, "L2=%lu/", monitor->level2_timeouts);
                offset += (size_t)strftime(buffer + offset, size - offset, "%Y-%m-%dT%H:%M:%S", localtime(&monitor->level2_timelast));
            }
            offset += (size_t)snprintf(buffer + offset, size - offset, ")");
        }
        if (offset >= size - 1) {
            buffer[size - 1] = '\0';
            return false;
        }
    }
    return true;
}
bool topic_config(void) {
    char buffer[64];
    topic_monitor_count = 0;
    topic_debug = config_get_bool("debug", false);
    const time_t now = time(NULL);
    for (size_t i = 0; i < MAX_TOPICS; i++) {
        snprintf(buffer, sizeof(buffer), "topic.%d.name", (int)i);
        const char *topic = config_get_string(buffer, NULL);
        if (topic == NULL)
            continue;
        TopicMonitor *monitor = &topic_monitors[topic_monitor_count];
        monitor->topic = topic;
        snprintf(buffer, sizeof(buffer), "topic.%d.service", (int)i);
        monitor->service_name = config_get_string(buffer, SERVICE_NAME_DEFAULT);
        snprintf(buffer, sizeof(buffer), "topic.%d.warning", (int)i);
        monitor->warning_seconds = (time_t)config_get_integer(buffer, TOPIC_TIMEOUT_LEVEL1_DEFAULT);
        snprintf(buffer, sizeof(buffer), "topic.%d.restart", (int)i);
        monitor->restart_seconds = (time_t)config_get_integer(buffer, TOPIC_TIMEOUT_LEVEL2_DEFAULT);
        snprintf(buffer, sizeof(buffer), "topic.%d.priority", (int)i);
        monitor->priority = config_get_string(buffer, TOPIC_PRIORITY_DEFAULT);
        snprintf(buffer, sizeof(buffer), "topic.%d.recover", (int)i);
        monitor->recover = config_get_bool(buffer, TOPIC_RECOVER_DEFAULT);
        monitor->last_message = now;
        monitor->silent_for = 0;
        monitor->warned = false;
        monitor->restarted = false;
        monitor->level1_timeouts = 0;
        monitor->level2_timeouts = 0;
        monitor->level1_timelast = 0;
        monitor->level2_timelast = 0;
        printf("topic: monitoring '%s' (warning=%lds, restart=%lds, service=%s, priority=%s, recover=%s)\n", monitor->topic, monitor->warning_seconds,
               monitor->restart_seconds, monitor->service_name ? monitor->service_name : "n/a", monitor->priority, monitor->recover ? "true" : "false");
        if (++topic_monitor_count >= MAX_TOPICS) {
            printf("watchdog: maximum number of topics (%d) reached\n", MAX_TOPICS);
            return false;
        }
    }
    return true;
}
bool topic_begin(void) {
    for (int i = 0; i < topic_monitor_count; i++) {
        TopicMonitor *monitor = &topic_monitors[i];
        if (!mqtt_subscribe(monitor->topic)) {
            fprintf(stderr, "topic: failed to subscribe to '%s'\n", monitor->topic);
            return false;
        }
    }
    return true;
}
void topic_end(void) {
    for (int i = 0; i < topic_monitor_count; i++) {
        TopicMonitor *monitor = &topic_monitors[i];
        mqtt_unsubscribe(monitor->topic);
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

// Alerts watch what JSON messages say rather than whether they arrive. An entity is alerting while a field has a given value;
// notifications go out once that has held for `hold`, as a reminder every `repeat` while it lasts, and when it clears. The entity is
// the part of the topic matched by wildcards (zigbee2mqtt/+ -> the device name), or, for a message holding an array of entries
// (devices/scan), each entry named by its `key` field. With `flags`, an entry is only watched when that field holds a
// notifiable(hold/repeat) flag -- which also gives its thresholds -- and not 'ignore', as in devices-known.conf.
//
// Messages arrive on the mqtt thread and only update entity state; the main loop sends the notifications (which can take seconds).

#define MAX_ALERTS 32
#define MAX_ALERT_ENTITIES 256
#define ALERT_NAME_MAX 96
#define ALERT_VALUE_MAX 64
#define ALERT_DETAILS_MAX 512

typedef struct {
    const char *name;     // label in notifications, e.g. "Smoke alarm" (default: the field)
    const char *topic;    // subscription, + and # wildcards allowed
    const char *array;    // optional: field holding an array of entries, each its own entity
    const char *key;      // with array: entry field naming the entity
    const char *field;    // field tested
    const char *equals;   // value of the field that means alerting (as text: true, offline, ...)
    const char *flags;    // optional: field holding flags, see above
    const char *since;    // optional: field with the local time the condition began (YYYY-MM-DD HH:MM:SS)
    const char *details;  // optional: comma-separated fields copied into notifications
    const char *priority; // ntfy priority of alerts and reminders
    const char *tags;     // ntfy tags
    time_t hold;          // seconds the condition must hold before alerting
    time_t repeat;        // seconds between reminders while it holds (0: none)
    bool recover;         // notify when it clears
} AlertRule;

typedef struct {
    int rule;                        // index into alert_rules, -1: free slot
    char name[ALERT_NAME_MAX];       // the entity
    char value[ALERT_VALUE_MAX];     // value of the field that set it off
    char cleared[ALERT_VALUE_MAX];   // value of the field that cleared it
    char details[ALERT_DETAILS_MAX]; // last details
    time_t hold, repeat;             // the rule's, or the entry's notifiable() flag
    bool active;                     // the condition holds
    bool pending;                    // became active since the last pass (a blip shorter than a pass still alerts, with hold 0)
    time_t since, until;             // when it began, and ended
    bool notified;                   // alerted on, this time round
    time_t notified_last;            // when last notified
    unsigned int notified_count;     // alert + reminders, this time round
} AlertEntity;

AlertRule alert_rules[MAX_ALERTS];
int alert_rule_count = 0;
AlertEntity alert_entities[MAX_ALERT_ENTITIES];
pthread_mutex_t alert_mutex = PTHREAD_MUTEX_INITIALIZER;
bool alert_debug = false;
unsigned long alert_notified_total = 0;

// "600", "45s", "30m", "3h", "1d"; negative or malformed: default_value
time_t parse_duration(const char *string, const time_t default_value) {
    if (string == NULL || *string == '\0')
        return default_value;
    char *end;
    const long value = strtol(string, &end, 10);
    if (end == string || value < 0 || (*end != '\0' && end[1] != '\0'))
        return default_value;
    switch (*end) {
    case '\0':
    case 's':
        return (time_t)value;
    case 'm':
        return (time_t)value * 60;
    case 'h':
        return (time_t)value * 60 * 60;
    case 'd':
        return (time_t)value * 24 * 60 * 60;
    default:
        return default_value;
    }
}

// notifiable(30m/3h) or notifiable(30m,3h), anywhere in flags
bool parse_notifiable(const char *flags, time_t *hold, time_t *repeat) {
    const char *start = strstr(flags, "notifiable(");
    if (start == NULL)
        return false;
    char first[16], second[16];
    if (sscanf(start, "notifiable(%15[0-9smhd]%*[/,]%15[0-9smhd])", first, second) != 2)
        return false;
    const time_t first_seconds = parse_duration(first, -1), second_seconds = parse_duration(second, -1);
    if (first_seconds < 0 || second_seconds < 0)
        return false;
    *hold = first_seconds;
    *repeat = second_seconds;
    return true;
}

// "YYYY-MM-DD HH:MM:SS", local time; 0 if malformed
time_t parse_localtime(const char *string) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (string == NULL || sscanf(string, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    const time_t result = mktime(&tm);
    return result < 0 ? 0 : result;
}

void format_duration(char *buffer, const size_t size, const time_t seconds) {
    if (seconds < 60)
        snprintf(buffer, size, "%lds", (long)seconds);
    else if (seconds < 60 * 60)
        snprintf(buffer, size, "%ldm", (long)(seconds / 60));
    else if (seconds < 24 * 60 * 60)
        snprintf(buffer, size, "%ld.%ldh", (long)(seconds / 3600), (long)((seconds % 3600) / 360));
    else
        snprintf(buffer, size, "%ld.%ldd", (long)(seconds / 86400), (long)((seconds % 86400) / 8640));
}

// a field as text ("true", "offline", "42"; "null" for null); NULL when absent
const char *json_field_string(json_object *object, const char *field) {
    json_object *value;
    if (field == NULL || !json_object_object_get_ex(object, field, &value))
        return NULL;
    const char *string = value != NULL ? json_object_get_string(value) : NULL;
    return string != NULL ? string : "null";
}

// the levels of topic matched by + and # in pattern, joined by '/'; the whole topic when there are none
void alert_entity_name(const char *pattern, const char *topic, char *name, const size_t size) {
    size_t length = 0;
    name[0] = '\0';
    while (*pattern != '\0' && *topic != '\0' && length < size) {
        const char *pattern_end = strchr(pattern, '/'), *topic_end = strchr(topic, '/');
        const size_t pattern_length = pattern_end ? (size_t)(pattern_end - pattern) : strlen(pattern);
        const bool rest = pattern_length == 1 && *pattern == '#';
        const size_t topic_length = (topic_end && !rest) ? (size_t)(topic_end - topic) : strlen(topic);
        if (pattern_length == 1 && (*pattern == '+' || rest))
            length += (size_t)snprintf(name + length, size - length, "%s%.*s", length > 0 ? "/" : "", (int)topic_length, topic);
        if (rest || pattern_end == NULL || topic_end == NULL)
            break;
        pattern = pattern_end + 1;
        topic = topic_end + 1;
    }
    if (length >= size)
        name[size - 1] = '\0';
    if (name[0] == '\0')
        snprintf(name, size, "%s", topic);
}

void alert_details(const AlertRule *rule, json_object *object, char *buffer, const size_t size) {
    size_t length = 0;
    buffer[0] = '\0';
    if (rule->details == NULL)
        return;
    char fields[CONFIG_MAX_STRING];
    snprintf(fields, sizeof(fields), "%s", rule->details);
    for (char *save = NULL, *field = strtok_r(fields, ",", &save); field != NULL && length < size; field = strtok_r(NULL, ",", &save)) {
        const char *value = json_field_string(object, field);
        if (value != NULL)
            length += (size_t)snprintf(buffer + length, size - length, "%s: %s\n", field, value);
    }
    if (length >= size)
        buffer[size - 1] = '\0';
}

// under alert_mutex; only adds when asked (an entity is only tracked once it has alerted)
AlertEntity *alert_entity_find(const int rule, const char *name, const bool add) {
    AlertEntity *free_slot = NULL;
    for (int i = 0; i < MAX_ALERT_ENTITIES; i++) {
        AlertEntity *entity = &alert_entities[i];
        if (entity->rule == rule && strcmp(entity->name, name) == 0)
            return entity;
        if (entity->rule < 0 && free_slot == NULL)
            free_slot = entity;
    }
    if (!add)
        return NULL;
    if (free_slot == NULL) {
        fprintf(stderr, "alert: too many entities (%d), ignoring '%s'\n", MAX_ALERT_ENTITIES, name);
        return NULL;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->rule = rule;
    snprintf(free_slot->name, sizeof(free_slot->name), "%s", name);
    return free_slot;
}

void alert_update(const int index, const char *name, json_object *object) {
    const AlertRule *rule = &alert_rules[index];
    const char *value = json_field_string(object, rule->field);
    if (value == NULL)
        return; // not reported by this one
    time_t hold = rule->hold, repeat = rule->repeat;
    if (rule->flags != NULL) {
        const char *flags = json_field_string(object, rule->flags);
        if (flags == NULL || strstr(flags, "ignore") != NULL || !parse_notifiable(flags, &hold, &repeat))
            return;
    }
    const bool matched = strcmp(value, rule->equals) == 0;
    const time_t now = time(NULL);
    pthread_mutex_lock(&alert_mutex);
    AlertEntity *entity = alert_entity_find(index, name, matched);
    if (entity != NULL) {
        if (matched)
            snprintf(entity->value, sizeof(entity->value), "%s", value);
        else
            snprintf(entity->cleared, sizeof(entity->cleared), "%s", value);
        alert_details(rule, object, entity->details, sizeof(entity->details));
        entity->hold = hold;
        entity->repeat = repeat;
        if (matched && !entity->active) {
            const time_t since = parse_localtime(json_field_string(object, rule->since));
            entity->active = entity->pending = true;
            entity->since = (since > 0 && since <= now) ? since : now;
            if (alert_debug)
                printf("alert: '%s' %s=%s (since %lds ago)\n", entity->name, rule->field, value, (long)(now - entity->since));
        } else if (!matched && entity->active) {
            entity->active = false;
            entity->until = now;
            if (alert_debug)
                printf("alert: '%s' %s=%s (cleared)\n", entity->name, rule->field, value);
        }
    }
    pthread_mutex_unlock(&alert_mutex);
}

void alert_receive_message(const char *topic, const char *payload, const int length) {
    bool matches[MAX_ALERTS], any = false;
    for (int i = 0; i < alert_rule_count; i++) {
        bool match = false;
        matches[i] = mosquitto_topic_matches_sub(alert_rules[i].topic, topic, &match) == MOSQ_ERR_SUCCESS && match;
        any = any || matches[i];
    }
    if (!any || payload == NULL || length <= 0)
        return;
    char *text = strndup(payload, (size_t)length);
    if (text == NULL)
        return;
    json_object *root = json_tokener_parse(text);
    free(text);
    if (root == NULL) {
        if (alert_debug)
            printf("alert: '%s': payload is not JSON\n", topic);
        return;
    }
    for (int i = 0; i < alert_rule_count; i++) {
        if (!matches[i])
            continue;
        const AlertRule *rule = &alert_rules[i];
        if (rule->array != NULL) {
            json_object *array;
            if (!json_object_object_get_ex(root, rule->array, &array) || !json_object_is_type(array, json_type_array))
                continue;
            const size_t count = json_object_array_length(array);
            for (size_t j = 0; j < count; j++) {
                json_object *entry = json_object_array_get_idx(array, j);
                const char *key = entry != NULL ? json_field_string(entry, rule->key) : NULL;
                if (key != NULL)
                    alert_update(i, key, entry);
            }
        } else if (json_object_is_type(root, json_type_object)) {
            char name[ALERT_NAME_MAX];
            alert_entity_name(rule->topic, topic, name, sizeof(name));
            alert_update(i, name, root);
        }
    }
    json_object_put(root);
}

bool alert_process(void) {
    const time_t now = time(NULL);
    for (int i = 0; i < MAX_ALERT_ENTITIES; i++) {
        char priority[16] = "", tags[64] = "", title[256] = "", content[1024] = "", duration[32];
        pthread_mutex_lock(&alert_mutex);
        AlertEntity *entity = &alert_entities[i];
        if (entity->rule >= 0) {
            const AlertRule *rule = &alert_rules[entity->rule];
            if (!entity->notified && ((entity->active && now - entity->since >= entity->hold) || (entity->pending && entity->hold == 0))) {
                entity->notified = true;
                entity->notified_last = now;
                entity->notified_count = 1;
                format_duration(duration, sizeof(duration), (entity->active ? now : entity->until) - entity->since);
                snprintf(priority, sizeof(priority), "%s", rule->priority);
                snprintf(tags, sizeof(tags), "%s", rule->tags);
                snprintf(title, sizeof(title), "%s '%s'", rule->name, entity->name);
                snprintf(content, sizeof(content), "%s is '%s', for %s\n%s", rule->field, entity->value, duration, entity->details);
            } else if (entity->notified && entity->active && entity->repeat > 0 && now - entity->notified_last >= entity->repeat) {
                entity->notified_last = now;
                entity->notified_count++;
                format_duration(duration, sizeof(duration), now - entity->since);
                snprintf(priority, sizeof(priority), "%s", rule->priority);
                snprintf(tags, sizeof(tags), "%s", rule->tags);
                snprintf(title, sizeof(title), "%s '%s' (reminder #%u)", rule->name, entity->name, entity->notified_count - 1);
                snprintf(content, sizeof(content), "%s is '%s', for %s\n%s", rule->field, entity->value, duration, entity->details);
            } else if (!entity->active && !entity->pending) {
                if (entity->notified && rule->recover) {
                    format_duration(duration, sizeof(duration), entity->until - entity->since);
                    snprintf(priority, sizeof(priority), "%s", "default");
                    snprintf(tags, sizeof(tags), "%s", "white_check_mark");
                    snprintf(title, sizeof(title), "Recovered: %s '%s'", rule->name, entity->name);
                    snprintf(content, sizeof(content), "%s is '%s', after %s\n%s", rule->field, entity->cleared, duration, entity->details);
                }
                entity->rule = -1; // cleared (or cleared before its hold was up): forget it
            }
            entity->pending = false;
        }
        pthread_mutex_unlock(&alert_mutex);
        if (title[0] != '\0') {
            printf("alert: %s\n", title);
            notify(priority, tags, title, content);
            alert_notified_total++;
        }
    }
    return true;
}
bool alert_stats_to_string(char *buffer, const size_t size) {
    int tracking = 0, active = 0;
    pthread_mutex_lock(&alert_mutex);
    for (int i = 0; i < MAX_ALERT_ENTITIES; i++)
        if (alert_entities[i].rule >= 0) {
            tracking++;
            if (alert_entities[i].active)
                active++;
        }
    pthread_mutex_unlock(&alert_mutex);
    return snprintf(buffer, size, "alerts=%d, tracking=%d, active=%d, notified=%lu", alert_rule_count, tracking, active, alert_notified_total) < (int)size;
}
const char *alert_config_string(const int index, const char *name, const char *default_value) {
    char key[64];
    snprintf(key, sizeof(key), "alert.%d.%s", index, name);
    return config_get_string(key, default_value);
}
bool alert_config(void) {
    alert_rule_count = 0;
    alert_debug = config_get_bool("debug", false);
    for (int i = 0; i < MAX_ALERT_ENTITIES; i++)
        alert_entities[i].rule = -1;
    for (int i = 0; i < MAX_ALERTS; i++) {
        const char *topic = alert_config_string(i, "topic", NULL);
        if (topic == NULL)
            continue;
        AlertRule *rule = &alert_rules[alert_rule_count];
        rule->topic = topic;
        rule->field = alert_config_string(i, "field", NULL);
        rule->equals = alert_config_string(i, "equals", NULL);
        rule->name = alert_config_string(i, "name", rule->field);
        rule->array = alert_config_string(i, "array", NULL);
        rule->key = alert_config_string(i, "key", NULL);
        rule->flags = alert_config_string(i, "flags", NULL);
        rule->since = alert_config_string(i, "since", NULL);
        rule->details = alert_config_string(i, "details", NULL);
        rule->priority = alert_config_string(i, "priority", ALERT_PRIORITY_DEFAULT);
        rule->tags = alert_config_string(i, "tags", "");
        rule->hold = parse_duration(alert_config_string(i, "hold", NULL), 0);
        rule->repeat = parse_duration(alert_config_string(i, "repeat", NULL), 0);
        const char *recover = alert_config_string(i, "recover", NULL);
        rule->recover = recover == NULL ? ALERT_RECOVER_DEFAULT : (strcasecmp(recover, "true") == 0 || strcmp(recover, "1") == 0);
        if (rule->field == NULL || rule->equals == NULL || (rule->array != NULL && rule->key == NULL)) {
            fprintf(stderr, "alert: alert.%d needs field and equals (and key, with array)\n", i);
            return false;
        }
        printf("alert: watching '%s'%s%s%s%s: %s == '%s' (name='%s', hold=%lds, repeat=%lds, recover=%s, priority=%s%s%s)\n", rule->topic,
               rule->array ? ", entries of " : "", rule->array ? rule->array : "", rule->array ? " by " : "", rule->array ? rule->key : "", rule->field, rule->equals,
               rule->name, rule->hold, rule->repeat, rule->recover ? "true" : "false", rule->priority, rule->flags ? ", thresholds from " : "",
               rule->flags ? rule->flags : "");
        if (++alert_rule_count >= MAX_ALERTS) {
            printf("watchdog: maximum number of alerts (%d) reached\n", MAX_ALERTS);
            return false;
        }
    }
    return true;
}
bool alert_begin(void) {
    for (int i = 0; i < alert_rule_count; i++)
        if (!mqtt_subscribe(alert_rules[i].topic)) {
            fprintf(stderr, "alert: failed to subscribe to '%s'\n", alert_rules[i].topic);
            return false;
        }
    return true;
}
void alert_end(void) {
    for (int i = 0; i < alert_rule_count; i++)
        mqtt_unsubscribe(alert_rules[i].topic);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

void message_receive(const char *topic, const char *payload, const int length) {
    topic_receive_message(topic);
    alert_receive_message(topic, payload, length);
}

bool startup_notification(void) {
    if (!action_email_notification("Startup", "")) {
        fprintf(stderr, "watchdog: failed send startup email\n");
        return false;
    }
    char title[256];
    snprintf(title, sizeof(title), "%s: startup", emailConfig.name);
    action_ntfy_notification("min", "", title, ""); // an unreachable ntfy server must not stop the watchdog
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

time_t report_period, report_last = 0;

const struct option config_options[] = {{"config", required_argument, 0, 0},      // config
                                        {"mqtt-client", required_argument, 0, 0}, // mqtt
                                        {"mqtt-server", required_argument, 0, 0},
                                        {"email-name", required_argument, 0, 0}, // email
                                        {"email-from", required_argument, 0, 0},
                                        {"email-to", required_argument, 0, 0},
                                        {"email-subject", required_argument, 0, 0},
                                        {"email-smtp", required_argument, 0, 0},
                                        {"email-username", required_argument, 0, 0},
                                        {"email-password", required_argument, 0, 0},
                                        {"email-use-ssl", required_argument, 0, 0},
                                        {"ntfy-server", required_argument, 0, 0}, // ntfy (ntfy-token: config file only)
                                        {"ntfy-topic", required_argument, 0, 0},
                                        {"report-period", required_argument, 0, 0}, // report
                                        {"debug", required_argument, 0, 0},         // debug
                                        {0, 0, 0, 0}};

bool config(int argc, char *argv[]) {
    if (!config_load(CONFIG_FILE_DEFAULT, argc, argv, config_options))
        return false;
    report_period = (time_t)config_get_integer("report-period", REPORT_PERIOD_DEFAULT);
    printf("report-period=%ld\n", report_period);
    if (!(mqtt_config() && topic_config() && alert_config() && action_email_config() && action_ntfy_config() && action_systemd_config()))
        return false;
    if (topic_monitor_count == 0 && alert_rule_count == 0) {
        fprintf(stderr, "watchdog: no topic.N or alert.N configured for monitoring\n");
        return false;
    }
    return true;
}
bool startup(void) {
    curl_global_init(CURL_GLOBAL_ALL);
    return mqtt_begin(&mqttConfig) && action_email_begin() && action_ntfy_begin() && action_systemd_begin() && startup_notification() &&
           mqtt_message_callback_register(message_receive) && topic_begin() && alert_begin();
}
void cleanup(void) {
    mqtt_message_callback_cancel();
    alert_end();
    topic_end();
    action_systemd_end();
    action_ntfy_end();
    action_email_end();
    mqtt_end();
    curl_global_cleanup();
}
bool process(void) {
    mqtt_poll();
    const bool result = topic_process() && alert_process();
    if (intervalable(report_period, &report_last)) {
        char string[1024];
        if (topic_monitor_count > 0 && topic_stats_to_string(string, sizeof(string)))
            printf("report: %s\n", string);
        if (alert_rule_count > 0 && alert_stats_to_string(string, sizeof(string)))
            printf("report: %s\n", string);
    }
    return result;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define PROCESS_INTERVAL 5
volatile bool running = true;

void signal_handler(const int sig __attribute__((unused))) {
    if (running) {
        printf("stopping\n");
        running = false;
    }
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    printf("starting\n");
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    if (!config(argc, argv))
        return EXIT_FAILURE;
    if (!startup()) {
        cleanup();
        return EXIT_FAILURE;
    }
    while (running) {
        if (!process()) {
            cleanup();
            return EXIT_FAILURE;
        }
        for (int i = 0; i < PROCESS_INTERVAL && running; i++)
            sleep(1);
    }
    cleanup();
    return EXIT_SUCCESS;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
