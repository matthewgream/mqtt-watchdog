
CC = gcc
CFLAGS = -O6 -Wall -Wextra -Wpedantic
LDFLAGS = -lmosquitto -lcurl -lsystemd
TARGET = mqtt-watchdog
SOURCES=include/config_linux.h include/mqtt_linux.h include/util_linux.h include/email_linux.h include/systemd_linux.h
HOSTNAME = $(shell hostname)

##

all: $(TARGET)

$(TARGET): $(TARGET).c $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(TARGET).c $(LDFLAGS)
clean:
	rm -f $(TARGET)
format:
	clang-format -i *.c include/*.h
test: $(TARGET)
	./$(TARGET)

.PHONY: all clean format test

##

SYSTEMD_DIR = /etc/systemd/system
UDEVRULES_DIR = /etc/udev/rules.d
define install_systemd_service
	-systemctl stop $(1) 2>/dev/null || true
	-systemctl disable $(1) 2>/dev/null || true
	cp $(2).service $(SYSTEMD_DIR)/$(1).service
	systemctl daemon-reload
	systemctl enable $(1)
	systemctl start $(1) || echo "Warning: Failed to start $(1)"
endef
install_systemd_service: $(TARGET).service
	$(call install_systemd_service,$(TARGET),$(TARGET))
install:  install_systemd_service
watch:
	journalctl -u $(TARGET) -f
restart:
	systemctl restart $(TARGET)
.PHONY: install install_systemd_service
.PHONY: watch restart

