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
M04_SOURCES := $(wildcard src/m04_tracking/*.cpp)
M04_HEADERS := $(wildcard src/m04_tracking/*.h)
M04_TEST_SOURCES := $(wildcard tests/m04_tracking/*.cpp)
M04_TEST_HEADERS := $(wildcard tests/m04_tracking/*.h)
M05_SOURCES := $(wildcard src/m05_eligibility/*.cpp)
M05_HEADERS := $(wildcard src/m05_eligibility/*.h)
M05_TEST_SOURCES := $(wildcard tests/m05_eligibility/*.cpp)
M05_TEST_HEADERS := $(wildcard tests/m05_eligibility/*.h)
M06_SOURCES := $(wildcard src/m06_scheduling/*.cpp)
M06_HEADERS := $(wildcard src/m06_scheduling/*.h)
M06_TEST_SOURCES := $(wildcard tests/m06_scheduling/*.cpp)
M06_TEST_HEADERS := $(wildcard tests/m06_scheduling/*.h)
M07_SOURCES := $(wildcard src/m07_execution/*.cpp)
M07_HEADERS := $(wildcard src/m07_execution/*.h)
M07_TEST_SOURCES := $(wildcard tests/m07_execution/*.cpp)
M07_TEST_HEADERS := $(wildcard tests/m07_execution/*.h)
M01_TEST := $(BUILD_DIR)/m01_acquisition_test
M02_TEST := $(BUILD_DIR)/m02_calibration_test
M03_TEST := $(BUILD_DIR)/m03_inference_test
M04_TEST := $(BUILD_DIR)/m04_tracking_test
M05_TEST := $(BUILD_DIR)/m05_eligibility_test
M06_TEST := $(BUILD_DIR)/m06_scheduling_test
M07_TEST := $(BUILD_DIR)/m07_execution_test

.PHONY: test
test: $(M01_TEST) $(M02_TEST) $(M03_TEST) $(M04_TEST) $(M05_TEST) $(M06_TEST) $(M07_TEST) m08-test m09-test
	$(M01_TEST)
	$(M02_TEST)
	$(M03_TEST)
	$(M04_TEST)
	$(M05_TEST)
	$(M06_TEST)
	$(M07_TEST)

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

$(M04_TEST): $(M01_SOURCES) $(M01_HEADERS) $(M02_SOURCES) $(M02_HEADERS) $(M03_SOURCES) $(M03_HEADERS) $(M04_SOURCES) $(M04_HEADERS) $(M04_TEST_SOURCES) $(M04_TEST_HEADERS) $(M02_TEST_HEADERS) $(M03_TEST_HEADERS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -Isrc \
		$(M01_SOURCES) $(M02_SOURCES) $(M03_SOURCES) $(M04_SOURCES) $(M04_TEST_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(M05_TEST): $(M01_HEADERS) $(M02_HEADERS) $(M03_HEADERS) $(M04_SOURCES) $(M04_HEADERS) $(M05_SOURCES) $(M05_HEADERS) $(M05_TEST_SOURCES) $(M05_TEST_HEADERS) $(M02_TEST_HEADERS) $(M03_TEST_HEADERS) $(M04_TEST_HEADERS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -Isrc \
		$(M04_SOURCES) $(M05_SOURCES) $(M05_TEST_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(M06_TEST): $(M01_HEADERS) $(M02_HEADERS) $(M03_HEADERS) $(M04_HEADERS) $(M05_SOURCES) $(M05_HEADERS) $(M06_SOURCES) $(M06_HEADERS) $(M06_TEST_SOURCES) $(M06_TEST_HEADERS) $(M02_TEST_HEADERS) $(M03_TEST_HEADERS) $(M04_TEST_HEADERS) $(M05_TEST_HEADERS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -Isrc \
		$(M05_SOURCES) $(M06_SOURCES) $(M06_TEST_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(M07_TEST): $(M01_HEADERS) $(M02_HEADERS) $(M03_HEADERS) $(M04_HEADERS) $(M05_SOURCES) $(M05_HEADERS) $(M06_SOURCES) $(M06_HEADERS) $(M07_SOURCES) $(M07_HEADERS) $(M07_TEST_SOURCES) $(M07_TEST_HEADERS) $(M02_TEST_HEADERS) $(M03_TEST_HEADERS) $(M04_TEST_HEADERS) $(M05_TEST_HEADERS) $(M06_TEST_HEADERS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -Isrc \
		$(M05_SOURCES) $(M06_SOURCES) $(M07_SOURCES) $(M07_TEST_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

M08_SOURCES := $(wildcard src/m08_hmi/*.cpp)
M08_HEADERS := $(wildcard src/m08_hmi/*.h)
M08_TEST_SOURCES := $(wildcard tests/m08_hmi/*.cpp)
M08_TEST_HEADERS := $(wildcard tests/m08_hmi/*.h)
M08_TEST := $(BUILD_DIR)/m08_hmi_test
M08_DEMO := $(BUILD_DIR)/m08_hmi_demo

.PHONY: m08-test m08-demo
m08-test: $(M08_TEST)
	$(M08_TEST)

m08-demo: $(M08_DEMO)

$(M08_TEST): $(M08_SOURCES) $(M08_HEADERS) $(M08_TEST_SOURCES) $(M08_TEST_HEADERS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -Isrc \
		$(M08_SOURCES) $(M08_TEST_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(M08_DEMO): $(M08_SOURCES) $(M08_HEADERS) $(M08_TEST_HEADERS) web/m08_hmi/demo_bridge.cpp
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -Isrc -Itests/m08_hmi \
		$(M08_SOURCES) web/m08_hmi/demo_bridge.cpp $(LDFLAGS) $(LDLIBS) -o $@

M09_SOURCES := $(wildcard src/m09_archive/*.cpp src/m09_archive/*/*.cpp)
M09_TEST_SOURCES := $(wildcard tests/m09_archive/*.cpp)
M09_TEST := $(BUILD_DIR)/m09_archive_test
M09_TEST_INPUTS := $(M09_SOURCES) $(M09_TEST_SOURCES) $(M05_SOURCES) $(M06_SOURCES) $(M07_SOURCES) $(M08_SOURCES)
M09_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/m09-obj/%.o,$(M09_TEST_INPUTS))

.PHONY: m09-test
m09-test: $(M09_TEST)
	$(M09_TEST)

$(M09_TEST): $(M09_OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -lcrypto -o $@

$(BUILD_DIR)/m09-obj/%.o: %.cpp
	mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -Isrc -Itests -MMD -MP -c $< -o $@

-include $(M09_OBJECTS:.o=.d)

M01_SIM := $(BUILD_DIR)/m01_sim
M01_SIM_SOURCES := $(wildcard sim/m01/*.cpp) $(M01_SOURCES) $(M02_SOURCES) $(M03_SOURCES) \
	$(M04_SOURCES) $(M05_SOURCES) $(M06_SOURCES) $(M07_SOURCES)
M01_SIM_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/m01-sim-obj/%.o,$(M01_SIM_SOURCES))
M01_SIM_ARCHIVE := $(addprefix $(BUILD_DIR)/m09-obj/src/m09_archive/,archive_service.o \
	adapters/m01_capture_adapter.o adapters/m07_intent_store_adapter.o evidence/evidence_index.o storage/archive_recovery.o \
	storage/local_files.o storage/manifest_store.o storage/record_codec.o storage/segment_store.o storage/storage_pressure.o \
	storage/command_journal.o)

.PHONY: m01-test m01-sim m01-sim-test m01-pick-test
m01-test: $(M01_TEST)
	$(M01_TEST)

m01-sim: $(M01_SIM)

m01-sim-test: $(M01_SIM)
	python3 tests/m01_sim/scenarios.py "$(M01_SIM)"

m01-pick-test: $(M01_SIM)
	python3 tests/m01_sim/pick_scenarios.py "$(M01_SIM)"

$(M01_SIM): $(M01_SIM_OBJECTS) $(M01_SIM_ARCHIVE)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -lcrypto -o $@

$(BUILD_DIR)/m01-sim-obj/%.o: %.cpp
	mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -Isrc -MMD -MP -c $< -o $@

-include $(M01_SIM_OBJECTS:.o=.d) $(M01_SIM_ARCHIVE:.o=.d)
