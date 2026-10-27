PYTHON ?= python3
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
VERSION ?= $(shell cat "$(ROOT)/VERSION")
JOBS ?= 2
PLATFORM ?=
BUILD_ARGS ?=
ARTIFACTS ?= $(ROOT)/dist/arkit
RELEASE_DIR ?= $(ROOT)/dist/release-$(VERSION)
.DEFAULT_GOAL := build

.PHONY: tools check test native build docker assemble
tools:
	$(PYTHON) "$(ROOT)/build_tools/toolchain.py"
check:
	$(PYTHON) "$(ROOT)/build_tools/toolchain.py" --check
test:
	$(PYTHON) -m unittest discover -s "$(ROOT)/tests" -v
	$(MAKE) native PYTHON="$(PYTHON)" JOBS="$(JOBS)"
native:
	$(PYTHON) "$(ROOT)/build_tools/build.py" --native-only --jobs "$(JOBS)" $(if $(PLATFORM),--platform "$(PLATFORM)") $(BUILD_ARGS)
build:
	$(PYTHON) "$(ROOT)/build_tools/build.py" --version "$(VERSION)" --jobs "$(JOBS)" $(if $(PLATFORM),--platform "$(PLATFORM)") $(BUILD_ARGS)
docker:
	$(PYTHON) "$(ROOT)/build_tools/docker_build.py" --platform "$(if $(PLATFORM),$(PLATFORM),all)" --version "$(VERSION)" --jobs "$(JOBS)"
assemble:
	$(PYTHON) "$(ROOT)/build_tools/release.py" --artifacts "$(ARTIFACTS)" --output "$(RELEASE_DIR)" --version "$(VERSION)"
