IDF_PATH    ?= $(HOME)/esp-idf/6.0
IDF_EXPORTS  = $(IDF_PATH)/export.sh
TARGET      ?= esp32s3

# Serial port for flash / monitor / erase-flash. Leave unset to let idf.py
# auto-detect; pass on the command line (`make flash PORT=/dev/ttyACM0`) or
# pin in Makefile.local.
PORT        ?=
IDFPY_PORT   = $(if $(PORT),-p $(PORT))

# Allow developers to keep local overrides in an untracked file.
-include Makefile.local

# Build sdkconfig defaults chains.
#   - sdkconfig.defaults                  (committed, common)
#   - sdkconfig.defaults.<target>         (committed, per-target — picked up
#     automatically by IDF, but listed explicitly here so we can chain
#     sdkconfig.defaults.local after it)
#   - sdkconfig.defaults.local            (gitignored, host-specific overrides;
#     the last entry wins for duplicate keys so local values trump committed
#     defaults)
SDKCONFIG_DEFAULTS_HW   = sdkconfig.defaults
ifneq (,$(wildcard sdkconfig.defaults.$(TARGET)))
SDKCONFIG_DEFAULTS_HW  := $(SDKCONFIG_DEFAULTS_HW);sdkconfig.defaults.$(TARGET)
endif
ifneq (,$(wildcard sdkconfig.defaults.local))
SDKCONFIG_DEFAULTS_HW  := $(SDKCONFIG_DEFAULTS_HW);sdkconfig.defaults.local
endif

.PHONY: build clean set-target flash flash-monitor monitor monitor-log erase-flash \
        build-docker docker-shell docker-clean

# Capture serial output non-interactively (for CI / agent contexts where
# idf.py monitor refuses to attach without a TTY). SECONDS defaults to 8.
MONITOR_LOG_SECONDS ?= 8

# --- Target builds ----------------------------------------------------------

set-target:
	bash -c "source $(IDF_EXPORTS) && idf.py -DSDKCONFIG_DEFAULTS='$(SDKCONFIG_DEFAULTS_HW)' set-target $(TARGET)"

build:
	bash -c "source $(IDF_EXPORTS) && idf.py -DSDKCONFIG_DEFAULTS='$(SDKCONFIG_DEFAULTS_HW)' build"

flash:
	bash -c "source $(IDF_EXPORTS) && idf.py $(IDFPY_PORT) flash"

monitor:
	bash -c "source $(IDF_EXPORTS) && idf.py $(IDFPY_PORT) monitor"

# Non-interactive log capture: resets the target and reads stdout for
# MONITOR_LOG_SECONDS seconds. Requires pyserial.
monitor-log:
	python3 tools/monitor_log.py --port $(if $(PORT),$(PORT),/dev/ttyACM0) --seconds $(MONITOR_LOG_SECONDS)

# Convenience: flash + monitor in a single idf.py invocation (faster than
# `make flash monitor` because the IDF env is sourced once).
flash-monitor:
	bash -c "source $(IDF_EXPORTS) && idf.py $(IDFPY_PORT) flash monitor"

erase-flash:
	bash -c "source $(IDF_EXPORTS) && idf.py $(IDFPY_PORT) erase-flash"

clean:
	bash -c "source $(IDF_EXPORTS) && idf.py fullclean"

# --- Docker build (mirrors CI) ----------------------------------------------
DOCKER_IMAGE         ?= espressif/idf:release-v6.0
DOCKER_WORKDIR       ?= /work
DOCKER_BUILD_DIR     ?= build-docker
DOCKER_SDKCONFIG     ?= sdkconfig.docker
DOCKER_TARGET_CHIP   ?= esp32s3

define docker-run
	docker run --rm -t \
		-u $$(id -u):$$(id -g) \
		-v "$(CURDIR):$(DOCKER_WORKDIR)" \
		-w $(DOCKER_WORKDIR) \
		-e HOME=/tmp \
		-e CI=true \
		$(DOCKER_IMAGE) \
		bash -c '\
			git config --global --add safe.directory "$(DOCKER_WORKDIR)" && \
			. "$$IDF_PATH/export.sh" && \
			$(1)'
endef

build-docker:
	$(call docker-run, \
		idf.py -B $(DOCKER_BUILD_DIR) -DSDKCONFIG=$(DOCKER_SDKCONFIG) \
		       -DSDKCONFIG_DEFAULTS=sdkconfig.defaults \
		       set-target $(DOCKER_TARGET_CHIP) && \
		idf.py -B $(DOCKER_BUILD_DIR) -DSDKCONFIG=$(DOCKER_SDKCONFIG) \
		       -DSDKCONFIG_DEFAULTS=sdkconfig.defaults build)

docker-clean:
	$(call docker-run, \
		idf.py -B $(DOCKER_BUILD_DIR) -DSDKCONFIG=$(DOCKER_SDKCONFIG) \
		       fullclean)
	@rm -f $(DOCKER_SDKCONFIG)

docker-shell:
	docker run --rm -it \
		-u $$(id -u):$$(id -g) \
		-v "$(CURDIR):$(DOCKER_WORKDIR)" \
		-w $(DOCKER_WORKDIR) \
		-e HOME=/tmp \
		$(DOCKER_IMAGE) \
		bash -c '\
			git config --global --add safe.directory "$(DOCKER_WORKDIR)" && \
			. "$$IDF_PATH/export.sh" && \
			exec bash'
