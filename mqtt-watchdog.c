
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "include/util_linux.h"

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define CONFIG_FILE_DEFAULT "mqtt-watchdog.cfg"

#define MQTT_CLIENT_DEFAULT "mqtt-watchdog"
#define MQTT_SERVER_DEFAULT "mqtt://localhost"

#define EMAIL_NAME_DEFAULT "MQTT Watchdog"
#define EMAIL_FROM_DEFAULT "no-reply@localhost"
#define EMAIL_TO_DEFAULT ""
#define EMAIL_SUBJECT_DEFAULT "MQTT Watchdog Alert"
#define EMAIL_SMTP_DEFAULT "smtp://localhost:25"
#define EMAIL_USERNAME_DEFAULT ""
#define EMAIL_PASSWORD_DEFAULT ""
#define EMAIL_USE_SSL_DEFAULT false

#define TOPIC_TIMEOUT_LEVEL1_DEFAULT 60
#define TOPIC_TIMEOUT_LEVEL2_DEFAULT 300

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define CONFIG_MAX_ENTRIES 128

#include "include/config_linux.h"

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define MQTT_CONNECT_TIMEOUT 60
#define MQTT_SUBSCRIBE_QOS 0

#include "include/mqtt_linux.h"

MqttConfig mqttConfig;

bool mqtt_config() {
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
    const char *from;
    const char *name;
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
bool action_email_config() {
    emailConfig.smtp = config_get_string("email-smtp", EMAIL_SMTP_DEFAULT);
    emailConfig.username = config_get_string("email-username", EMAIL_USERNAME_DEFAULT);
    emailConfig.password = config_get_string("email-password", EMAIL_PASSWORD_DEFAULT);
    emailConfig.use_ssl = config_get_bool("email-use-ssl", EMAIL_USE_SSL_DEFAULT);
    emailConfig.from = config_get_string("email-from", EMAIL_NAME_DEFAULT);
    emailConfig.name = config_get_string("email-name", EMAIL_FROM_DEFAULT);
    emailConfig.to = config_get_string("email-to", EMAIL_TO_DEFAULT);
    emailConfig.subject = config_get_string("email-subject", EMAIL_SUBJECT_DEFAULT);
    return true;
}
bool action_email_begin() { return true; }
void action_email_end() {}

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
bool action_systemd_config() { return true; }
bool action_systemd_begin() { return true; }
bool action_systemd_end() { return true; }

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define MAX_TOPICS 32
typedef struct {
    const char *topic;             // MQTT topic to monitor
    const char *service_name;      // Systemd service name (can be NULL)
    time_t warning_seconds;        // Level 1 (notification) threshold in seconds
    time_t restart_seconds;        // Level 2 (restart) threshold in seconds
    time_t last_message;           // Timestamp of last received message
    bool warned;                   // Flag to track if warning has been sent
    bool restarted;                // Flag to track if service has been restarted
                                   //
    unsigned long level1_timeouts; // Count of level 1 timeouts for this topic
    unsigned long level2_timeouts; // Count of level 2 timeouts for this topic
    time_t last_level1_time;       // Timestamp of last level 1 timeout
    time_t last_level2_time;       // Timestamp of last level 2 timeout
} TopicMonitor;
TopicMonitor topic_monitors[MAX_TOPICS];
size_t topic_monitor_count = 0;
bool topic_debug = false;
unsigned long topic_timeout_level1 = 0, topic_timeout_level2 = 0;

void topic_receive_message(const char *topic) {
    for (size_t i = 0; i < topic_monitor_count; i++) {
        TopicMonitor *monitor = &topic_monitors[i];
        if (strcmp(monitor->topic, topic) == 0) {
            monitor->last_message = time(NULL);
            if (topic_debug)
                printf("topic: message received for '%s'\n", topic);
            break;
        }
    }
}
bool topic_process() {
    char subject[256];
    const time_t now = time(NULL);
    for (size_t i = 0; i < topic_monitor_count; i++) {
        TopicMonitor *monitor = &topic_monitors[i];
        const time_t seconds_since_last = now - monitor->last_message;
        // Level 2
        if (seconds_since_last >= monitor->restart_seconds && !monitor->restarted) {
            printf("topic: restart threshold exceeded for '%s' (%ld seconds)\n", monitor->topic, seconds_since_last);
            snprintf(subject, sizeof(subject), "Alert '%s' level-2 timeout (%ld seconds) [notify+restart]", monitor->topic, seconds_since_last);
            action_email_notification(subject, "");
            action_systemd_service_restart(monitor->service_name);
            monitor->warned = true;
            monitor->restarted = true;
            monitor->level2_timeouts++;
            monitor->last_level2_time = now;
            topic_timeout_level2++;
        }
        // Level 1
        else if (seconds_since_last >= monitor->warning_seconds && !monitor->warned) {
            printf("topic: warning threshold exceeded for '%s' (%ld seconds)\n", monitor->topic, seconds_since_last);
            snprintf(subject, sizeof(subject), "Alert '%s' level-1 timeout (%ld seconds) [notify]", monitor->topic, seconds_since_last);
            action_email_notification(subject, "");
            monitor->warned = true;
            monitor->level1_timeouts++;
            monitor->last_level1_time = now;
            topic_timeout_level1++;
        }
        if (seconds_since_last < monitor->warning_seconds)
            monitor->warned = monitor->restarted = false;
    }
    return true;
}
bool topic_stats_to_string(char *buffer, size_t size) {
    size_t offset = snprintf(buffer, size, "L1=%lu, L2=%lu: ", topic_timeout_level1, topic_timeout_level2);
    for (size_t i = 0; i < topic_monitor_count; i++) {
        TopicMonitor *monitor = &topic_monitors[i];
        offset += snprintf(buffer + offset, size - offset, "%s%s", (i == 0 ? "" : ", "), monitor->topic);
        if (monitor->level1_timeouts > 0 || monitor->level2_timeouts > 0) {
            offset += snprintf(buffer + offset, size - offset, " (");
            if (monitor->level1_timeouts > 0) {
                offset += snprintf(buffer + offset, size - offset, "L1=%lu/", monitor->level1_timeouts);
                offset += strftime(buffer + offset, size - offset, "%Y-%m-%dT%H:%M:%S", localtime(&monitor->last_level1_time));
            }
            if (monitor->level2_timeouts > 0) {
                if (monitor->level1_timeouts > 0)
                    offset += snprintf(buffer + offset, size - offset, ", ");
                offset += snprintf(buffer + offset, size - offset, "L2=%lu/", monitor->level2_timeouts);
                offset += strftime(buffer + offset, size - offset, "%Y-%m-%dT%H:%M:%S", localtime(&monitor->last_level2_time));
            }
            offset += snprintf(buffer + offset, size - offset, ")");
        }
        if (offset >= size - 1) {
            buffer[size - 1] = '\0';
            return false;
        }
    }
    return true;
}
bool topic_config() {
    char buffer[64];
    topic_monitor_count = 0;
    topic_debug = config_get_bool("debug", false);
    const time_t now = time(NULL);
    for (size_t i = 0; i < MAX_TOPICS; i++) {
        snprintf(buffer, sizeof(buffer), "topic.%d.name", i);
        const char *topic = config_get_string(buffer, NULL);
        if (topic == NULL)
            continue;
        TopicMonitor *monitor = &topic_monitors[topic_monitor_count];
        monitor->topic = topic;
        snprintf(buffer, sizeof(buffer), "topic.%d.service", i);
        monitor->service_name = config_get_string(buffer, NULL);
        snprintf(buffer, sizeof(buffer), "topic.%d.warning", i);
        monitor->warning_seconds = (time_t)config_get_integer(buffer, TOPIC_TIMEOUT_LEVEL1_DEFAULT);
        snprintf(buffer, sizeof(buffer), "topic.%d.restart", i);
        monitor->restart_seconds = (time_t)config_get_integer(buffer, TOPIC_TIMEOUT_LEVEL2_DEFAULT);
        monitor->last_message = now;
        monitor->warned = false;
        monitor->restarted = false;
        monitor->level1_timeouts = 0;
        monitor->level2_timeouts = 0;
        monitor->last_level1_time = 0;
        monitor->last_level2_time = 0;
        printf("topic: monitoring '%s' (warning=%lds, restart=%lds, service=%s)\n", monitor->topic, monitor->warning_seconds, monitor->restart_seconds,
               monitor->service_name ? monitor->service_name : "n/a");
        if (++topic_monitor_count >= MAX_TOPICS) {
            printf("watchdog: maximum number of topics (%d) reached\n", MAX_TOPICS);
            return false;
        }
    }
    if (topic_monitor_count == 0) {
        fprintf(stderr, "topic: none configured for monitoring\n");
        return false;
    }
    return true;
}
bool topic_begin() {
    if (!action_email_notification("Startup", "")) {
        fprintf(stderr, "topic: failed send startup email\n");
        return false;
    }
    if (!mqtt_message_callback_register(topic_receive_message))
        return false;
    for (size_t i = 0; i < topic_monitor_count; i++) {
        TopicMonitor *monitor = &topic_monitors[i];
        if (!mqtt_subscribe(monitor->topic)) {
            fprintf(stderr, "topic: failed to subscribe to '%s'\n", monitor->topic);
            return false;
        }
    }
    return true;
}
void topic_end() {
    mqtt_message_callback_cancel();
    for (size_t i = 0; i < topic_monitor_count; i++) {
        TopicMonitor *monitor = &topic_monitors[i];
        mqtt_unsubscribe(monitor->topic);
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define NOTIFY_INTERVAL 30
time_t notify_last = 0;

const struct option config_options[] = {{"config", required_argument, 0, 0},      // config
                                        {"mqtt-client", required_argument, 0, 0}, // mqtt
                                        {"mqtt-server", required_argument, 0, 0},
                                        {"email-from", required_argument, 0, 0}, // email
                                        {"email-to", required_argument, 0, 0},
                                        {"email-subject", required_argument, 0, 0},
                                        {"email-smtp", required_argument, 0, 0},
                                        {"email-username", required_argument, 0, 0},
                                        {"email-password", required_argument, 0, 0},
                                        {"email-use-ssl", required_argument, 0, 0},
                                        {"debug", required_argument, 0, 0}, // debug
                                        {0, 0, 0, 0}};

bool config(const int argc, const char *argv[]) {
    if (!config_load(CONFIG_FILE_DEFAULT, argc, argv, config_options))
        return false;
    return mqtt_config() && topic_config() && action_email_config() && action_systemd_config();
}
bool startup() {
    curl_global_init(CURL_GLOBAL_ALL);
    return mqtt_begin(&mqttConfig) && action_email_begin() && action_systemd_begin() && topic_begin();
}
void cleanup() {
    topic_end();
    action_systemd_end();
    action_email_end();
    mqtt_end();
    curl_global_cleanup();
}
bool process() {
    const bool result = topic_process();
    if (intervalable(NOTIFY_INTERVAL, &notify_last)) {
        char string[1024];
        if (topic_stats_to_string(string, sizeof(string)))
            printf("notify: %s\n", string);
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

int main(const int argc, const char *argv[]) {
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
        sleep(PROCESS_INTERVAL);
    }
    cleanup();
    return EXIT_SUCCESS;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
