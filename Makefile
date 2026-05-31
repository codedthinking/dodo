PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=dodo
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# ---- dodoc standalone CLI (no DuckDB dependency) ----
DODOC_BUILD_DIR := build/dodoc
DODOC_SOURCES := src/cli/dodoc.cpp src/core/dodo_core.cpp
DODOC_HEADERS := src/core/dodo_core.hpp src/core/string_utils.hpp
CXX ?= c++
CXXFLAGS ?= -O2 -std=c++17

.PHONY: dodoc dodoc-install dodoc-clean

dodoc: $(DODOC_BUILD_DIR)/dodoc

$(DODOC_BUILD_DIR)/dodoc: $(DODOC_SOURCES) $(DODOC_HEADERS)
	@mkdir -p $(DODOC_BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Isrc/core -o $@ $(DODOC_SOURCES)

dodoc-install: dodoc
	install -m 755 $(DODOC_BUILD_DIR)/dodoc /usr/local/bin/dodoc

dodoc-clean:
	rm -rf $(DODOC_BUILD_DIR)

# ---- E2E tests across client interfaces ----
.PHONY: e2e e2e-cli e2e-python e2e-r e2e-node

e2e: e2e-cli e2e-python e2e-r e2e-node

e2e-cli:
	bash test/e2e/test_cli.sh

e2e-python:
	uv run --python 3.13 --with 'duckdb==1.5.2' --with pyyaml test/e2e/test_python.py

e2e-r:
	Rscript test/e2e/test_r.R $(PROJ_DIR)

e2e-node:
	node test/e2e/test_node.js
