ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
BUILD := $(ROOT)/build/native
CXX := xcrun clang++
CXXFLAGS := -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Wno-unused-parameter -mmacosx-version-min=14.0 -I$(ROOT)/native/vendor -I$(ROOT)/native/src -MMD -MP
SOURCES := $(wildcard $(ROOT)/native/src/*.cpp)
OBJECTS := $(patsubst $(ROOT)/native/src/%.cpp,$(BUILD)/%.o,$(SOURCES)) $(BUILD)/platform.o
.DEFAULT_GOAL := all
.PHONY: all
all: $(BUILD)/astrolibrary
$(BUILD)/astrolibrary: $(OBJECTS)
	$(CXX) $(OBJECTS) -mmacosx-version-min=14.0 -framework Foundation -framework AppKit -framework ImageIO -framework CoreGraphics -lsqlite3 -lz -o $@
$(BUILD)/%.o: $(ROOT)/native/src/%.cpp $(ROOT)/native/Makefile
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD)/platform.o: $(ROOT)/native/src/platform.mm $(ROOT)/native/Makefile
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -fobjc-arc -c $< -o $@
-include $(OBJECTS:.o=.d)
