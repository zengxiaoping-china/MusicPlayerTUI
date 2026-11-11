CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS += -D_GNU_SOURCE -Isrc -MMD -MP
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wformat=2 -pthread
LDLIBS += -pthread

FFMPEG_PKGS := libavformat libavcodec libavutil libswresample libpulse-simple
FFMPEG_CFLAGS := $(shell pkg-config --cflags $(FFMPEG_PKGS) 2>/dev/null)
FFMPEG_LIBS := $(shell pkg-config --libs $(FFMPEG_PKGS) 2>/dev/null)

ifneq ($(strip $(FFMPEG_LIBS)),)
CPPFLAGS += -DHAVE_FFMPEG=1 $(FFMPEG_CFLAGS)
LDLIBS += $(FFMPEG_LIBS)
endif
FFMPEG_ENABLED := $(if $(strip $(FFMPEG_LIBS)),1,0)

BUILD_DIR := build
CONFIG_STAMP := $(BUILD_DIR)/config
CONFIG_VALUE := $(shell cat $(CONFIG_STAMP) 2>/dev/null)

ifneq ($(CONFIG_VALUE),$(FFMPEG_ENABLED))
CONFIG_DEPS := config-force
else
CONFIG_DEPS := $(CONFIG_STAMP)
endif

TARGET := musicplayertui
SOURCES := $(wildcard src/*.c)
OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))
DEPS := $(OBJECTS:.o=.d)

.PHONY: all clean run install config-force

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(BUILD_DIR)/%.o: src/%.c $(CONFIG_DEPS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(CONFIG_STAMP):
	@mkdir -p $(BUILD_DIR)
	@printf '%s\n' '$(FFMPEG_ENABLED)' > '$@'

config-force:
	@mkdir -p $(BUILD_DIR)
	@printf '%s\n' '$(FFMPEG_ENABLED)' > '$(CONFIG_STAMP)'

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: $(TARGET)
	./$(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

PREFIX ?= /usr/local

-include $(DEPS)
