# markit build wrapper: `make` equivalents without touching cmake directly.
# Targets: help (default), build, test, bench, release, minsize, size-report,
# clean.

BUILD_DIR ?= build
JOBS ?= $(shell nproc 2>/dev/null || echo 4)
BUILD_TYPE ?=
CMAKE_ARGS ?=
CMAKE_BUILD_TYPE_ARG = $(if $(BUILD_TYPE),-DCMAKE_BUILD_TYPE=$(BUILD_TYPE),)
BENCH_ARGS ?= --warmup 2 --samples 5

.DEFAULT_GOAL := help

.PHONY: help build test bench release minsize size-report clean

help: ## Show this help.
	@echo "Usage: make [BUILD_DIR=build] [JOBS=N] <target>"
	@echo "Targets:"
	@echo "  build   Configure (if needed) and build the project."
	@echo "  test    Build, then run the test suite."
	@echo "  bench   Build and run the opt-in performance benchmark."
	@echo "  release Build an optimized production tree without tests."
	@echo "  minsize Build a size-oriented production tree without tests."
	@echo "  size-report Print size information for BUILD_DIR/markit."
	@echo "  clean   Remove the build directory and test output."

build: ## Configure (if needed) and build the project.
	cmake -S . -B $(BUILD_DIR) $(CMAKE_BUILD_TYPE_ARG) $(CMAKE_ARGS)
	cmake --build $(BUILD_DIR) -j$(JOBS)

test: build ## Build, then run the test suite.
	ctest --test-dir $(BUILD_DIR) --output-on-failure

bench: ## Build and run the opt-in performance benchmark.
	$(MAKE) BUILD_DIR=build-bench BUILD_TYPE=RelWithDebInfo CMAKE_ARGS="-DMARKIT_BUILD_TESTS=OFF -DMARKIT_BUILD_BENCHMARKS=ON" build
	./build-bench/markit-bench $(BENCH_ARGS)

release: ## Build an optimized production tree without tests.
	$(MAKE) BUILD_DIR=build-release BUILD_TYPE=Release CMAKE_ARGS="-DMARKIT_BUILD_TESTS=OFF -DMARKIT_BUILD_BENCHMARKS=OFF -DMARKIT_BUILD_DEPENDENCY_TOOLS=OFF" build

minsize: ## Build a size-oriented production tree without tests.
	$(MAKE) BUILD_DIR=build-minsize BUILD_TYPE=MinSizeRel CMAKE_ARGS="-DMARKIT_BUILD_TESTS=OFF -DMARKIT_BUILD_BENCHMARKS=OFF -DMARKIT_BUILD_DEPENDENCY_TOOLS=OFF" build

size-report: ## Print size information for the selected build tree.
	size $(BUILD_DIR)/markit

clean: ## Remove the build directory and test output.
	rm -rf $(BUILD_DIR) Testing/
