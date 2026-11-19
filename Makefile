CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic
BUILD_DIR ?= build

M01_SOURCES := $(wildcard src/m01_acquisition/*.cpp src/m01_acquisition/adapters/*.cpp)
M01_HEADERS := $(wildcard src/m01_acquisition/*.h)
M02_SOURCES := $(wildcard src/m02_calibration/*.cpp)
M02_HEADERS := $(wildcard src/m02_calibration/*.h)
M02_TEST_SOURCES := $(wildcard tests/m02_calibration/*.cpp)
M02_TEST_HEADERS := $(wildcard tests/m02_calibration/*.h)
M03_SOURCES := $(wildcard src/m03_inference/*.cpp)
M03_HEADERS := $(wildcard src/m03_inference/*.h)
M03_TEST_SOURCES := $(wildcard tests/m03_inference/*.cpp)
M03_TEST_HEADERS := $(wildcard tests/m03_inference/*.h)
M01_TEST := $(BUILD_DIR)/m01_acquisition_test
M02_TEST := $(BUILD_DIR)/m02_calibration_test
M03_TEST := $(BUILD_DIR)/m03_inference_test

.PHONY: test
test: $(M01_TEST) $(M02_TEST) $(M03_TEST)
	$(M01_TEST)
	$(M02_TEST)
	$(M03_TEST)

$(M01_TEST): $(M01_SOURCES) $(M01_HEADERS) tests/m01_acquisition/acquisition_test.cpp
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -Isrc/m01_acquisition \
		$(M01_SOURCES) tests/m01_acquisition/acquisition_test.cpp $(LDFLAGS) $(LDLIBS) -o $@

$(M02_TEST): $(M01_SOURCES) $(M01_HEADERS) $(M02_SOURCES) $(M02_HEADERS) $(M02_TEST_SOURCES) $(M02_TEST_HEADERS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -Isrc \
		$(M01_SOURCES) $(M02_SOURCES) $(M02_TEST_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(M03_TEST): $(M01_SOURCES) $(M01_HEADERS) $(M02_SOURCES) $(M02_HEADERS) $(M03_SOURCES) $(M03_HEADERS) $(M03_TEST_SOURCES) $(M03_TEST_HEADERS) $(M02_TEST_HEADERS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -Isrc \
		$(M01_SOURCES) $(M02_SOURCES) $(M03_SOURCES) $(M03_TEST_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
