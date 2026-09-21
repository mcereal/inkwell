# Thin wrappers over cmake/ctest. The build types live in CMakeLists.txt; nothing here
# configures anything the plain cmake commands would not.

BUILD ?= build

.PHONY: all test debug format clean

all: debug

debug:
	cmake -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(BUILD)

# The verify step, and what CI runs: the unit suite plus the check that no layer reaches
# upward. Run it before every push.
test: debug
	ctest --test-dir $(BUILD) --output-on-failure

format:
	clang-format -i $$(git ls-files '*.c' '*.h')

clean:
	rm -rf $(BUILD)
