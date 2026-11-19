CC ?= cc
PYTHON ?= python3
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror
CORE = src/gauge_core.c
CORE_LINK = $(CORE) src/diag_link.c
HEADERS = src/gauge_core.h src/drive_frame.h src/diagnostic_lease.h src/diag_link.h
# Host workers carry the HSFZ-then-MOST link, with a fake DSI selected by ID6_FAKE_MOST.
MOST = src/most/most_link.c src/most/dsi_client.c src/most/most_proto.c src/most/dsi_bind.c
MOST_HEADERS = src/most/most_link.h src/most/dsi_client.h src/most/most_proto.h src/most/dsi_abi.h
FAKE_DSI = tests/fake_dsi.c tests/fake_dme.c tests/fake_dsi.h tests/fake_dme.h
HOST_LINK = $(CC) $(CFLAGS) -pthread -DMOST_HOST -Isrc -Itests
HUD = src/hud/hud_app.c src/hud/hud_proto.c src/hud/hud_strip.c src/hud/hud_scene.c src/hud/hud_sender.c src/hud/hud_nav.c src/hud/hud_nav_demo.c src/hud/hud_text.c src/hud/hud_rg.c src/hud/hud_gear.c src/hud/hud_cds.c src/hud/hud_cam.c
HUD_HEADERS = src/hud/hud_app.h src/hud/hud_proto.h src/hud/hud_strip.h src/hud/hud_scene.h src/hud/hud_sender.h src/hud/hud_nav.h src/hud/hud_nav_demo.h src/hud/hud_text.h src/hud/hud_glyphs.h src/hud/hud_rg.h src/hud/hud_gear.h src/hud/hud_cds.h src/hud/hud_cam.h

.PHONY: test-hud test clean

build:
	mkdir -p build

build/shift-deck: src/collector.c $(CORE_LINK) $(MOST) tests/fake_most_env.c $(FAKE_DSI) $(HEADERS) $(MOST_HEADERS) | build
	$(HOST_LINK) $(filter %.c,$^) -lm -o $@

build/test-most: tests/test_most.c $(MOST) $(FAKE_DSI) $(MOST_HEADERS) | build
	$(CC) $(CFLAGS) -pthread -DMOST_HOST -Isrc -Itests $(filter %.c,$^) -o $@

build/test-diag-link: tests/test_diag_link.c $(CORE_LINK) $(MOST) tests/fake_most_env.c $(FAKE_DSI) $(HEADERS) $(MOST_HEADERS) | build
	$(HOST_LINK) $(filter %.c,$^) -lm -o $@

build/test-hud: tests/test_hud.c $(HUD) $(HUD_HEADERS) | build
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -lm -o $@

# CarPlay route guidance: the file reader and librghook's writer (built for the host, hooks renamed).
build/test-hud-rg: tests/test_hud_rg.c src/hud/hud_rg.c src/rghook/rghook.c src/hud/hud_rg.h src/hud/hud_nav.h | build
	$(CC) $(CFLAGS) -pthread -DRGHOOK_HOST_TEST -Isrc -Isrc/hud $(filter %.c,$^) -lm -o $@

# Host build: --dump works; the car modes find no DSI library unless ID6_FAKE_MOST is set.
build/hud-worker: src/hud/hud_worker.c src/hud/hud_most.c $(HUD) $(MOST) tests/fake_most_env.c $(FAKE_DSI) $(HUD_HEADERS) src/hud/hud_most.h $(MOST_HEADERS) | build
	$(HOST_LINK) -Isrc/hud $(filter %.c,$^) -lm -o $@

test-hud: build/test-hud build/test-hud-rg build/hud-worker
	./build/test-hud
	./build/test-hud-rg
	$(PYTHON) -B tests/test_hud_worker.py

test: test-hud build/shift-deck build/test-most build/test-diag-link
	./build/test-most
	./build/test-diag-link
	$(PYTHON) -B tests/test_collector.py

clean:
	rm -rf build
