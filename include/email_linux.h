
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <curl/curl.h>

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

struct UploadData {
    const char *data;
    size_t remaining_size;
};

size_t __curl_payload_read_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
    struct UploadData *upload_data = (struct UploadData *)userdata;
    if (upload_data->remaining_size == 0)
        return 0;
    const size_t buffer_size = size * nitems;
    const size_t to_copy = (buffer_size < upload_data->remaining_size) ? buffer_size : upload_data->remaining_size;
    memcpy(buffer, upload_data->data, to_copy);
    upload_data->data += to_copy;
    upload_data->remaining_size -= to_copy;
    return to_copy;
}

bool email_send(const char *server, const char *username, const char *password, const bool use_ssl, const char *name, const char *from, const char *to, const char *subject,
                const char *content) {
    bool result = false;
    char payload[4096] = {0};
    size_t payload_size = snprintf(payload, sizeof(payload),
                                   "From: %s <%s>\r\n"
                                   "To: %s\r\n"
                                   "Subject: %s\r\n"
                                   "MIME-Version: 1.0\r\n"
                                   "Content-Type: text/plain; charset=UTF-8\r\n"
                                   "\r\n"
                                   "%s\r\n",
                                   name, from, to, subject, content);
    if (payload_size >= sizeof(payload)) {
        fprintf(stderr, "email_send: payload too large, sending truncated\n");
        /* ignore */
    }
    struct UploadData upload_data = {.data = payload, .remaining_size = strlen(payload)};
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "email_send: failed to initialize curl\n");
        return false;
    }
    struct curl_slist *recipients = curl_slist_append(NULL, to);
    curl_easy_setopt(curl, CURLOPT_URL, server);
    if (username && *username)
        curl_easy_setopt(curl, CURLOPT_USERNAME, username);
    if (password && *password)
        curl_easy_setopt(curl, CURLOPT_PASSWORD, password);
    if (use_ssl) {
        curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, from);
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, __curl_payload_read_callback);
    curl_easy_setopt(curl, CURLOPT_READDATA, &upload_data);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
        fprintf(stderr, "email_send: send failed: %s\n", curl_easy_strerror(res));
    else
        result = true;
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);
    return result;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
