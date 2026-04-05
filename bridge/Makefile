CXX      = g++
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra -march=skylake
LDFLAGS  =
LIBS     =

# GStreamer + GIO (for D-Bus)
GST_CFLAGS := $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gio-2.0)
GST_LIBS   := $(shell pkg-config --libs   gstreamer-1.0 gstreamer-app-1.0 gio-2.0)

CXXFLAGS += $(GST_CFLAGS)
LIBS     += $(GST_LIBS) -lpthread

TARGET = camera-bridge
SRC    = camera-bridge.cpp

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(HOME)/.local/bin/$(TARGET)
	install -Dm644 camera-bridge.service $(HOME)/.config/systemd/user/camera-bridge.service
	systemctl --user daemon-reload
