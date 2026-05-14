# Version from file
VERSION := $(shell cat version.txt)
DEFAULT_APP_UUID_KEY := 44444444-4444-4444-8444-444444444444
UUID_FROM_FILE := $(strip $(shell [ -f uuid.txt ] && tr -d '\r\n' < uuid.txt))
APP_UUID_KEY_RESOLVED := $(if $(strip $(APP_UUID_KEY)),$(strip $(APP_UUID_KEY)),$(if $(UUID_FROM_FILE),$(UUID_FROM_FILE),$(DEFAULT_APP_UUID_KEY)))

## Build production firmware for pico_w using APP_UUID_KEY env var, then uuid.txt, then default
.PHONY: build
build:
	@echo "Using APP_UUID_KEY: $(APP_UUID_KEY_RESOLVED)"
	./build.sh pico_w release "$(APP_UUID_KEY_RESOLVED)"

## Build debug firmware for pico_w using APP_UUID_KEY env var, then uuid.txt, then default
.PHONY: debug
debug:
	@echo "Using APP_UUID_KEY: $(APP_UUID_KEY_RESOLVED)"
	./build.sh pico_w debug "$(APP_UUID_KEY_RESOLVED)"

## Tag this version
.PHONY: tag
tag:
	git tag $(VERSION) && git push origin $(VERSION) && \
	echo "Tagged: $(VERSION)"
