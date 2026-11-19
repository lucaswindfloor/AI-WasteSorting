CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic
BUILD_DIR ?= build

SOURCES := $(wildcard src/m01_acquisition/*.cpp src/m01_acquisition/adapters/*.cpp)
HEADERS := $(wildcard src/m01_acquisition/*.h)
TEST := $(BUILD_DIR)/m01_acquisition_test

.PHONY: test
test: $(TEST)
	$(TEST)

$(TEST): $(SOURCES) $(HEADERS) tests/m01_acquisition/acquisition_test.cpp
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -Isrc/m01_acquisition \
		$(SOURCES) tests/m01_acquisition/acquisition_test.cpp $(LDFLAGS) $(LDLIBS) -o $@
