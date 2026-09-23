PYTHON ?= python3
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
UE_ROOT ?=
.DEFAULT_GOAL := help

.PHONY: help plugin plugin-source plugin-build plugin-test cli cli-docker cli-test test
help:
	@printf '%s\n' \
	  'make plugin                         Package Unreal plugin sources' \
	  'make plugin-build UE_ROOT=/path     Compile with that Unreal Engine SDK' \
	  'make cli                            Build the standalone CLI for this host' \
	  'make cli-docker PLATFORM=linux-x64   Build the Linux publication package' \
	  'make test                           Run both test suites' \
	  'make plugin-test / make cli-test     Run either test suite'

plugin: plugin-source
plugin-source:
	$(PYTHON) "$(ROOT)/plugins/ue/tools/package_plugin.py"
plugin-build:
	$(PYTHON) "$(ROOT)/plugins/ue/tools/build_plugin.py" --engine "$(UE_ROOT)"
plugin-test:
	$(PYTHON) -m unittest discover -s "$(ROOT)/plugins/ue/tests" -v
cli:
	$(MAKE) -C "$(ROOT)/cli" build PYTHON="$(PYTHON)"
cli-docker:
	$(MAKE) -C "$(ROOT)/cli" docker PYTHON="$(PYTHON)"
cli-test:
	$(MAKE) -C "$(ROOT)/cli" test PYTHON="$(PYTHON)"
test: plugin-test cli-test
