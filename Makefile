# markit build wrapper: `make` equivalents without touching cmake directly.
# Targets: help (default), build, test, clean.

BUILD_DIR ?= build
JOBS ?= $(shell nproc 2>/dev/null || echo 4)

.DEFAULT_GOAL := help

.PHONY: help build test clean

help: ## Show this help.
	@echo "Usage: make [BUILD_DIR=build] [JOBS=N] <target>"
	@echo "Targets:"
	@echo "  build   Configure (if needed) and build the project."
	@echo "  test    Build, then run the test suite."
	@echo "  clean   Remove the build directory and test output."

build: ## Configure (if needed) and build the project.
	cmake -S . -B $(BUILD_DIR)
	cmake --build $(BUILD_DIR) -j$(JOBS)

test: build ## Build, then run the test suite.
	ctest --test-dir $(BUILD_DIR) --output-on-failure

clean: ## Remove the build directory and test output.
	rm -rf $(BUILD_DIR) Testing/
