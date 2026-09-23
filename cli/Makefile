PYTHON ?= python3
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
TOOLS = $(PYTHON) "$(ROOT)/build_tools/run.py"
VERSION ?= $(shell cat "$(ROOT)/VERSION")
JOBS ?= 2
PLATFORM ?=
BUILD_ARGS ?=
ARTIFACTS ?= $(ROOT)/dist/arkit
RELEASE_DIR ?= $(ROOT)/dist/release-$(VERSION)
.DEFAULT_GOAL := build

.PHONY: tools check test native build docker assemble
tools:
	$(TOOLS) --prepare
check:
	$(TOOLS) --check
test:
	$(TOOLS) python -m unittest discover -s "$(ROOT)/tests" -v
native:
	$(TOOLS) python "$(ROOT)/build_tools/build.py" --native-only --jobs "$(JOBS)" $(if $(PLATFORM),--platform "$(PLATFORM)")
build:
	$(TOOLS) python "$(ROOT)/build_tools/build.py" --version "$(VERSION)" --jobs "$(JOBS)" $(if $(PLATFORM),--platform "$(PLATFORM)") $(BUILD_ARGS)
docker:
	$(PYTHON) "$(ROOT)/build_tools/docker_build.py" --platform "$(if $(PLATFORM),$(PLATFORM),all)" --version "$(VERSION)" --jobs "$(JOBS)"
assemble:
	$(PYTHON) "$(ROOT)/build_tools/release.py" --artifacts "$(ARTIFACTS)" --output "$(RELEASE_DIR)" --version "$(VERSION)"
