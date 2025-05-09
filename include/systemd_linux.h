
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <systemd/sd-bus.h>

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

bool systemd_service_restart(const char *service_name) {
    sd_bus *bus = NULL;
    int r = sd_bus_open_system(&bus);
    if (r < 0) {
        fprintf(stderr, "systemd_service_restart: system bus connection failed: %s\n", strerror(-r));
        return false;
    }
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    r = sd_bus_call_method(bus,
                           "org.freedesktop.systemd1",         // Service
                           "/org/freedesktop/systemd1",        // Object path
                           "org.freedesktop.systemd1.Manager", // Interface
                           "RestartUnit",                      // Method
                           &error,                             // Error
                           &m,                                 // Response message
                           "ss",                               // Signature (string, string)
                           service_name,                       // Unit name
                           "replace");                         // Mode
    if (r < 0) {
        fprintf(stderr, "systemd_service_restart: restart service '%s' failed: %s\n", service_name, error.message);
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return false;
    }
    sd_bus_message_unref(m);
    sd_bus_unref(bus);
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
