CC ?= gcc
CXX ?= g++
DEBUG ?= 0
ENABLE_DYNARMIC ?= 0
DYNARMIC_SOURCE_DIR ?= third_party/dynarmic
DYNARMIC_BUILD_DIR ?= build/dynarmic
DYNARMIC_PREFIX ?= $(CURDIR)/build/dynarmic-install
BUILD_DIR ?= build/x86_64/$(if $(filter 1,$(DEBUG)),debug,release)/$(if $(filter 1,$(ENABLE_DYNARMIC)),dynarmic,unicorn)
BIN_DIR ?= bin
CFLAGS := -fPIC -O3 -Wall -Wextra -DNETWORK_SUPPORT -DVMRP -I./header
CXXFLAGS := $(CFLAGS) -std=c++20
LDFLAGS := -lpthread -lm -fPIC
SOURCES := network.c fileLib.c vmrp.c utils.c rbtree.c bridge.c memory.c cJSON.c guest_memory.c arm_runtime.c backend_unicorn.c main.c
OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SOURCES))

ifeq ($(ENABLE_DYNARMIC),1)
CFLAGS += -DVMRP_ENABLE_DYNARMIC
CXXFLAGS += -DVMRP_ENABLE_DYNARMIC -I$(DYNARMIC_PREFIX)/include
OBJS += $(BUILD_DIR)/backend_dynarmic.o
LDFLAGS += -L$(DYNARMIC_PREFIX)/lib -ldynarmic -lmcl -lfmt -lZydis \
	$(DYNARMIC_BUILD_DIR)/externals/zydis/zycore/libZycore.a -lrt
LINKER := $(CXX)
else
LINKER := $(CC)
endif

LDFLAGS += -lSDL2 -lSDL2_mixer -L./lib -lunicorn

ifeq ($(DEBUG),1)
	CFLAGS += -DDEBUG
	OBJS += $(BUILD_DIR)/debug.o $(BUILD_DIR)/trace.o
	LDFLAGS += -lcapstone
endif

main: $(BIN_DIR)/main
	@:

$(BIN_DIR)/main: $(OBJS) | $(BIN_DIR)
	$(LINKER) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BIN_DIR):
	mkdir -p $@

.PHONY: main dynarmic dynarmic-lib clean
dynarmic: dynarmic-lib
	$(MAKE) ENABLE_DYNARMIC=1

dynarmic-lib:
	cmake -S $(DYNARMIC_SOURCE_DIR) -B $(DYNARMIC_BUILD_DIR) \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=$(DYNARMIC_PREFIX) \
		-DBUILD_TESTING=OFF \
		-DDYNARMIC_TESTS=OFF \
		-DDYNARMIC_USE_LLVM=OFF \
		-DDYNARMIC_USE_BUNDLED_EXTERNALS=ON
	cmake --build $(DYNARMIC_BUILD_DIR) --target install -j$$(nproc)
clean:
	-rm -rf build/x86_64
