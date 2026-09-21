# Thin wrappers over cmake/ctest. The build types live in CMakeLists.txt; nothing here
# configures anything the plain cmake commands would not.

BUILD ?= build

.PHONY: all test debug format format-check clean

all: debug

debug:
	cmake -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(BUILD)

# The verify step, and what CI runs: the unit suite plus the check that no layer reaches
# upward. Run it before every push.
test: debug
	ctest --test-dir $(BUILD) --output-on-failure

# What clang-format is allowed to touch, named once.
#
# third_party/ is excluded and that is not tidiness: a vendored file is upstream's, and
# reformatting one rewrites every line of a 3.6 MB generated source into a diff nobody can read
# - which is also what scripts/check-vendor.py then refuses, because the digest no longer
# matches the README that vouches for it. Submodules were never reachable here (git ls-files
# does not descend into one); the vendored file is.
#
# It is a variable, and `format-check` exists, because the workflow used to carry its own copy
# of this list - so the exclusion landed here and CI went on checking the vendored file and
# failed. One list, two targets, and nothing to keep in step.
FORMAT_FILES = $$(git ls-files '*.c' '*.h' ':!:third_party/*')
CLANG_FORMAT ?= clang-format

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

# What CI runs: the same files, changing nothing, failing on the first that differs.
format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

clean:
	rm -rf $(BUILD)
