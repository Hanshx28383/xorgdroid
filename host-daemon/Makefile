# language: Makefile, file: host-daemon/Makefile, target: Linux Xorg, gcc
# Build the X11 daemon and the test producer. Artifacts go to $(BUILD_DIR)/host-daemon/.
# Can be invoked directly (make -C host-daemon) or from the top-level Makefile.

ROOT      := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
BUILD_DIR ?= $(ROOT)/build
OUT_DIR   := $(BUILD_DIR)/host-daemon

CC            ?= gcc
CFLAGS_COMMON ?= -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Wshadow -Wformat=2 -I$(ROOT)/include
CFLAGS        := $(CFLAGS_COMMON) -I. -Igles_stream
LDFLAGS       ?=
# Link libGL.so.1 directly (no dev symlink libGL.so) so the build works in
# containers without libgl1-mesa-dev; NVIDIA hosts provide libGL.so.1 at runtime.
LDLIBS        := -lX11 -lXext -l:libGL.so.1 -lpthread

DAEMON   := $(OUT_DIR)/x11droid-daemon
PRODUCER := $(OUT_DIR)/test_producer

.PHONY: all run producer clean

all: $(DAEMON) $(PRODUCER)

$(OUT_DIR):
	@mkdir -p $@

$(DAEMON): x11droid_daemon.c uinput_input.c uinput_input.h fb_proto.h \
           glx_render.c glx_render.h \
           gles_stream/gles_server.c gles_stream/gles_server.h \
           gles_stream/gles_stream_proto.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ x11droid_daemon.c uinput_input.c glx_render.c \
		gles_stream/gles_server.c \
		$(LDFLAGS) $(LDLIBS) -ldl

$(PRODUCER): test_producer.c fb_proto.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ test_producer.c $(LDFLAGS)

run: $(DAEMON)
	exec $(DAEMON)

producer: $(PRODUCER)
	exec $(PRODUCER)

clean:
	rm -f $(DAEMON) $(PRODUCER)
