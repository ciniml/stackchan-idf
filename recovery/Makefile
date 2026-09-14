IDF_PATH    ?= $(HOME)/esp-idf/5.5.4
IDF_EXPORTS  = $(IDF_PATH)/export.sh
TARGET      ?= esp32s3

# Board variant — flash サイズと board kind だけが異なる (PSRAM は使わない)。
#   cores3 / stopwatch : 16 MB
#   atoms3r / atoms3   :  8 MB
BOARD       ?= cores3
BUILD_DIR    = build-$(BOARD)

PORT        ?=
IDFPY_PORT   = $(if $(PORT),-p $(PORT))

-include Makefile.local
-include ../Makefile.local

SDKCONFIG_DEFAULTS_HW = sdkconfig.defaults
ifneq (,$(wildcard sdkconfig.defaults.$(BOARD)))
SDKCONFIG_DEFAULTS_HW := $(SDKCONFIG_DEFAULTS_HW);sdkconfig.defaults.$(BOARD)
endif
ifneq (,$(wildcard sdkconfig.defaults.local))
SDKCONFIG_DEFAULTS_HW := $(SDKCONFIG_DEFAULTS_HW);sdkconfig.defaults.local
endif

IDFPY = bash -c "source $(IDF_EXPORTS) && idf.py -B $(BUILD_DIR) -DSDKCONFIG=$(BUILD_DIR)/sdkconfig -DSDKCONFIG_DEFAULTS='$(SDKCONFIG_DEFAULTS_HW)' $(1)"

.PHONY: set-target build size size-components flash monitor flash-monitor clean

set-target:
	$(call IDFPY,set-target $(TARGET))

build:
	$(call IDFPY,build)

size:
	$(call IDFPY,size)

size-components:
	$(call IDFPY,size-components)

flash:
	bash -c "source $(IDF_EXPORTS) && idf.py -B $(BUILD_DIR) $(IDFPY_PORT) flash"

monitor:
	bash -c "source $(IDF_EXPORTS) && idf.py -B $(BUILD_DIR) $(IDFPY_PORT) monitor"

flash-monitor:
	bash -c "source $(IDF_EXPORTS) && idf.py -B $(BUILD_DIR) $(IDFPY_PORT) flash monitor"

clean:
	$(call IDFPY,fullclean)
