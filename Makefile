# lg290p-rtk: project chores. `make help` lists targets.
SHELL := /bin/bash
.DEFAULT_GOAL := help
PRESET ?= dev
PREFIX ?= /usr/local
# Extra CMake arguments, e.g. CMAKE_ARGS=-DCMAKE_PREFIX_PATH=$$HOME/.local for a user-installed PROJ.
CMAKE_ARGS ?=

.PHONY: help setup build test check fmt fmt-check lint tidy docs hooks install clean grids

help: ## Show this help
	@grep -hE '^[a-zA-Z_-]+:.*?## ' $(MAKEFILE_LIST) | \
	  awk 'BEGIN{FS=":.*?## "}{printf "  \033[36m%-12s\033[0m %s\n", $$1, $$2}'

setup: hooks test ## One-command bootstrap: hooks, build, tests

build: ## Configure and build (PRESET=dev: Debug + ASan/UBSan + -Werror; PRESET=release)
	cmake --preset $(PRESET) $(CMAKE_ARGS)
	cmake --build --preset $(PRESET)

test: build ## Run all tests (unit, simulated receiver, fake caster, CLI end-to-end)
	ctest --preset $(PRESET) -j8

check: lint test tidy docs ## Everything CI checks

fmt: ## Format C++ with clang-format (Google style)
	git ls-files '*.h' '*.cc' | xargs clang-format -i

fmt-check: ## Fail if any C++ file is not clang-formatted
	git ls-files '*.h' '*.cc' | xargs clang-format --dry-run --Werror

lint: fmt-check ## fmt-check, include guards, shell scripts
	python3 tools/check-header-guards.py
	@for f in $$(git ls-files '*.sh' 'tools/githooks/*'); do bash -n "$$f" || exit 1; done
	@if command -v shellcheck >/dev/null; then shellcheck -S warning $$(git ls-files '*.sh' 'tools/githooks/*'); \
	  else echo "lint: shellcheck not installed, skipped"; fi

tidy: build ## clang-tidy naming/style checks (uses the compile database of PRESET)
	@files="$$(git ls-files 'src/*.cc' 'apps/*.cc' 'tests/*.cc')"; \
	if command -v run-clang-tidy >/dev/null; then \
	  run-clang-tidy -quiet -p build/$(PRESET) -extra-arg=-Wno-unknown-warning-option $$files; \
	else \
	  for f in $$files; do clang-tidy -quiet -p build/$(PRESET) -extra-arg=-Wno-unknown-warning-option "$$f" || exit 1; done; \
	fi

docs: ## API docs with Doxygen (fails on warnings): build/docs/html/index.html
	@mkdir -p build/docs
	doxygen Doxyfile
	@echo "docs: $(CURDIR)/build/docs/html/index.html"

hooks: ## Install the git hooks (pre-commit: format + guards on staged files)
	git config core.hooksPath tools/githooks

install: ## Build release and install library, headers, CMake package and CLI into PREFIX
	cmake --preset release -DCMAKE_INSTALL_PREFIX=$(PREFIX) $(CMAKE_ARGS)
	cmake --build --preset release
	cmake --install build/release

grids: ## Download the swisstopo PROJ grids (LV95 / LHN95 / LN02) into the user PROJ directory
	projsync --source-id ch_swisstopo

clean: ## Remove the build directory of this checkout
	@test -f CMakeLists.txt && test -d build && rm -r -- ./build || true
