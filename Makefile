
CC=gcc
CFLAGS_DEFINES=
CFLAGS_COMMON=-Wall -Wextra -Wpedantic
CFLAGS_STRICT=-Werror \
    -Wstrict-prototypes \
    -Wold-style-definition \
    -Wcast-align -Wcast-qual -Wconversion \
    -Wfloat-equal -Wformat=2 -Wformat-security \
    -Winit-self -Wjump-misses-init \
    -Wlogical-op -Wmissing-include-dirs \
    -Wnested-externs -Wpointer-arith \
    -Wredundant-decls -Wshadow \
    -Wstrict-overflow=2 -Wswitch-default \
    -Wundef \
    -Wunreachable-code -Wunused \
    -Wwrite-strings
CFLAGS_OPT=-O3
CFLAGS_INCLUDES=
CFLAGS=$(CFLAGS_COMMON) $(CFLAGS_STRICT) $(CFLAGS_DEFINES) $(CFLAGS_OPT) $(CFLAGS_INCLUDES)
LDFLAGS=
LIBS=-lmosquitto -lcurl -lsystemd
HOSTNAME=$(shell hostname)

##

TARGET=mqtt-watchdog
SOURCES=include/config_linux.h include/mqtt_linux.h include/util_linux.h include/email_linux.h include/systemd_linux.h

##

all: $(TARGET)

$(TARGET): $(TARGET).c $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(TARGET).c $(LDFLAGS) $(LIBS)
clean:
	rm -f $(TARGET)
format:
	clang-format -i $(TARGET).c include/*.h
test: $(TARGET)
	./$(TARGET)

.PHONY: all clean format test

##

INSTALL=watchdogmqtt
DIR_INSTALL=/usr/local/bin
DIR_DEFAULT=/etc/default
DIR_SYSTEMD=/etc/systemd/system
define install_service_systemd
	-systemctl stop $(2) 2>/dev/null || true
	-systemctl disable $(2) 2>/dev/null || true
	install -m 644 $(1).service $(DIR_SYSTEMD)/$(2).service
	systemctl daemon-reload
	systemctl enable $(2)
	systemctl start $(2) || echo "Warning: Failed to start $(2)"
endef
install_target: $(TARGET)
	install -m 755 $(TARGET) $(DIR_INSTALL)/$(INSTALL)
install_default: $(TARGET).cfg
	install -m 644 $(TARGET).cfg $(DIR_DEFAULT)/$(INSTALL)
install_service: $(TARGET).service
	$(call install_service_systemd,$(TARGET),$(INSTALL))
install: install_target install_default install_service
watch:
	journalctl -u $(TARGET) -f
restart:
	systemctl restart $(INSTALL)
.PHONY: install install_target install_default install_service
.PHONY: watch restart

##

