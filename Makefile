CC ?= cc
BUILD_DIR ?= build
PREFIX ?= /usr/local

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS += $(shell pkg-config --cflags libdrm libdrm_amdgpu 2>/dev/null)
LDLIBS += $(shell pkg-config --libs libdrm libdrm_amdgpu 2>/dev/null)
LDLIBS += -ldrm_amdgpu -ldrm

BINS := $(BUILD_DIR)/amdgpu-poc $(BUILD_DIR)/amdgpu-regs2 $(BUILD_DIR)/amdgpu-gfx-pm4 $(BUILD_DIR)/amdgpu-raw-triangle $(BUILD_DIR)/radv-triangle
GFX1201_BIN := $(BUILD_DIR)/amdgpu-raw-triangle-gfx1201

.PHONY: all clean install install-gfx1201 shaders gfx1201

all: $(BINS)

gfx1201: $(GFX1201_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/amdgpu-poc: src/amdgpu_poc.c src/common.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/amdgpu_poc.c $(LDLIBS)

$(BUILD_DIR)/amdgpu-regs2: src/amdgpu_regs2.c src/common.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/amdgpu_regs2.c

$(BUILD_DIR)/amdgpu-gfx-pm4: src/amdgpu_gfx_pm4.c src/common.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/amdgpu_gfx_pm4.c $(LDLIBS)

$(BUILD_DIR)/amdgpu-raw-triangle: src/amdgpu_raw_triangle.c src/common.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/amdgpu_raw_triangle.c $(LDLIBS)

$(BUILD_DIR)/amdgpu-raw-triangle-gfx1201: src/amdgpu_raw_triangle_gfx1201.c src/common.h $(BUILD_DIR)/gfx12-raw-triangle-ngg.bin $(BUILD_DIR)/gfx12-raw-triangle-ps.bin | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/amdgpu_raw_triangle_gfx1201.c $(LDLIBS)

$(BUILD_DIR)/radv-triangle: examples/radv_triangle.c src/common.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ examples/radv_triangle.c -lvulkan

shaders: $(BUILD_DIR)/nop.bin $(BUILD_DIR)/store-output.bin $(BUILD_DIR)/add-output.bin $(BUILD_DIR)/mul-output.bin $(BUILD_DIR)/sub-output.bin $(BUILD_DIR)/and-or-xor-output.bin $(BUILD_DIR)/shl-shr-output.bin $(BUILD_DIR)/load-add-store.bin $(BUILD_DIR)/multi-thread-addtid.bin $(BUILD_DIR)/branch-select.bin $(BUILD_DIR)/atomic-add.bin $(BUILD_DIR)/gfx-vs.bin $(BUILD_DIR)/gfx-ps-store.bin $(BUILD_DIR)/gfx-ps-store-only.bin $(BUILD_DIR)/trap-test.bin

$(BUILD_DIR)/nop.bin: shaders/nop.s tools/extract-text.sh | $(BUILD_DIR)
	sh tools/extract-text.sh shaders/nop.s $@

$(BUILD_DIR)/store-output.bin: shaders/store-output.s tools/extract-text.sh | $(BUILD_DIR)
	sh tools/extract-text.sh shaders/store-output.s $@

$(BUILD_DIR)/add-output.bin: shaders/add-output.s tools/extract-text.sh | $(BUILD_DIR)
	sh tools/extract-text.sh shaders/add-output.s $@

$(BUILD_DIR)/mul-output.bin: shaders/mul-output.s tools/extract-text.sh | $(BUILD_DIR)
	sh tools/extract-text.sh shaders/mul-output.s $@

$(BUILD_DIR)/sub-output.bin: shaders/sub-output.s tools/extract-text.sh | $(BUILD_DIR)
	sh tools/extract-text.sh shaders/sub-output.s $@

$(BUILD_DIR)/and-or-xor-output.bin: shaders/and-or-xor-output.s tools/extract-text.sh | $(BUILD_DIR)
	sh tools/extract-text.sh shaders/and-or-xor-output.s $@

$(BUILD_DIR)/shl-shr-output.bin: shaders/shl-shr-output.s tools/extract-text.sh | $(BUILD_DIR)
	sh tools/extract-text.sh shaders/shl-shr-output.s $@

$(BUILD_DIR)/load-add-store.bin: shaders/load-add-store.s tools/extract-text.sh | $(BUILD_DIR)
	sh tools/extract-text.sh shaders/load-add-store.s $@

$(BUILD_DIR)/multi-thread-addtid.bin: shaders/multi-thread-addtid.s tools/extract-text.sh | $(BUILD_DIR)
	sh tools/extract-text.sh shaders/multi-thread-addtid.s $@

$(BUILD_DIR)/branch-select.bin: shaders/branch-select.s tools/extract-text.sh | $(BUILD_DIR)
	sh tools/extract-text.sh shaders/branch-select.s $@

$(BUILD_DIR)/atomic-add.bin: shaders/atomic-add.s tools/extract-text.sh | $(BUILD_DIR)
	sh tools/extract-text.sh shaders/atomic-add.s $@

$(BUILD_DIR)/gfx-vs.bin: shaders/gfx-vs.s tools/extract-text.sh | $(BUILD_DIR)
	sh tools/extract-text.sh shaders/gfx-vs.s $@

$(BUILD_DIR)/gfx-ps-store.bin: shaders/gfx-ps-store.s tools/extract-text.sh | $(BUILD_DIR)
	sh tools/extract-text.sh shaders/gfx-ps-store.s $@

$(BUILD_DIR)/gfx-ps-store-only.bin: shaders/gfx-ps-store-only.s tools/extract-text.sh | $(BUILD_DIR)
	sh tools/extract-text.sh shaders/gfx-ps-store-only.s $@

$(BUILD_DIR)/gfx12-raw-triangle-ngg.bin: shaders/gfx12/raw-triangle-ngg.s tools/extract-text.sh | $(BUILD_DIR)
	AMDGPU_MCPU=gfx1201 sh tools/extract-text.sh shaders/gfx12/raw-triangle-ngg.s $@

$(BUILD_DIR)/gfx12-raw-triangle-ps.bin: shaders/gfx12/raw-triangle-ps.s tools/extract-text.sh | $(BUILD_DIR)
	AMDGPU_MCPU=gfx1201 sh tools/extract-text.sh shaders/gfx12/raw-triangle-ps.s $@

$(BUILD_DIR)/trap-test.bin: shaders/trap-test.s tools/extract-text.sh | $(BUILD_DIR)
	sh tools/extract-text.sh shaders/trap-test.s $@

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BINS) $(DESTDIR)$(PREFIX)/bin

install-gfx1201: gfx1201
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(GFX1201_BIN) $(DESTDIR)$(PREFIX)/bin
	install -m 644 $(BUILD_DIR)/gfx12-raw-triangle-ngg.bin $(BUILD_DIR)/gfx12-raw-triangle-ps.bin $(DESTDIR)$(PREFIX)/bin

clean:
	rm -rf $(BUILD_DIR)
