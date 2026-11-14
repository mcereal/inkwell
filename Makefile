# Thin wrappers over cmake/ctest. The build types live in CMakeLists.txt; nothing here
# configures anything the plain cmake commands would not.

BUILD ?= build

.PHONY: all venv test debug format format-check clean

all: debug

# The host tools in requirements-dev.txt - Mbed TLS's generator Python and the pinned
# clang-format - in a venv of their own, remade when that file changes.
VENV = $(BUILD)/venv
VENV_STAMP = $(VENV)/.installed

venv: $(VENV_STAMP)
$(VENV_STAMP): requirements-dev.txt
	python3 -m venv $(VENV)
	$(VENV)/bin/pip install --quiet --disable-pip-version-check -r requirements-dev.txt
	@touch $@

debug: $(VENV_STAMP)
	cmake -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Debug \
	  -DPython3_EXECUTABLE=$(abspath $(VENV))/bin/python3
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
FORMAT_FILES = $$(git ls-files '*.c' '*.h' '*.m' ':!:third_party/*')
CLANG_FORMAT ?= $(VENV)/bin/clang-format

format: $(VENV_STAMP)
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

# What CI runs: the same files, changing nothing, failing on the first that differs.
format-check: $(VENV_STAMP)
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

clean:
	rm -rf $(BUILD)
