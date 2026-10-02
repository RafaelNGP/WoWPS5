# Sandbox elevation helper (from ps5-native-app-boilerplate dd44bbd,
# examples/sandbox-elevation/payload), built as an elfldr payload with the
# public payload SDK. It accepts only the title in main.cpp's target_title_id.
ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
include /opt/ps5-payload-sdk/toolchain/prospero.mk
endif

OUTPUT ?= sandbox-elevator.elf
CXXFLAGS := -std=c++20 -O2 -g -Wall -Wextra -Werror
LDFLAGS := -Wl,--build-id=none

.PHONY: all clean
all: $(OUTPUT)

$(OUTPUT): main.cpp ../protocol.hpp
	@mkdir -p -- $(@D)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<
	python3 validate-elevation-helper.py $@

clean:
	rm -f -- $(OUTPUT)
