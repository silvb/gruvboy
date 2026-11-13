IMAGE      := gruvboy-toolchain
BUILD      := build
SRC        := tools/clicktest/main.cpp
APP_NAME   := GruvboyClickTest
PACKAGE    := $(BUILD)/package/App/$(APP_NAME)

# SDL 1.2 with the `miao` audio driver, which writes directly to the vendor
# audio API. Onion's stock SDL does not include this driver.
ONION_TAG  := v4.3.1-1
SDL_PATH   := static/packages/App/Music%20Player%20(GMU)/App/Gmu/lib/libSDL-1.2.so.0
SDL_URL    := https://raw.githubusercontent.com/OnionUI/Onion/$(ONION_TAG)/$(SDL_PATH)
SDL_SHA256 := 709aacd04144c107f804ded558f607a3de1da5ad73b201813fa1a1d696831631
SDL_LIB    := third_party/miyoo/libSDL-1.2.so.0

CXXFLAGS   := -std=c++17 -O2 -Wall -Wextra

.PHONY: desktop selftest image miyoo package sd clean

desktop: $(BUILD)/desktop/clicktest

$(BUILD)/desktop/clicktest: $(SRC)
	@mkdir -p $(@D)
	clang++ $(CXXFLAGS) $(shell sdl-config --cflags) $< -o $@ $(shell sdl-config --libs)

selftest: desktop
	$(BUILD)/desktop/clicktest --selftest $(BUILD)/desktop/selftest.bmp

image:
	docker build -t $(IMAGE) docker

miyoo: $(BUILD)/miyoo/clicktest

$(BUILD)/miyoo/clicktest: $(SRC)
	@mkdir -p $(@D)
	docker run --rm -v "$(CURDIR)":/work $(IMAGE) sh -c '$${CROSS_COMPILE}g++ \
		-std=c++17 -O2 -Wall -Wextra -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
		$$($$SYSROOT/usr/bin/sdl-config --cflags) $(SRC) -o $@ \
		-L$$SYSROOT/usr/lib -lSDL -lpthread -static-libstdc++ -static-libgcc \
		&& $${CROSS_COMPILE}strip $@'

$(SDL_LIB):
	@mkdir -p $(@D)
	curl --fail --location --retry 3 --output $@.tmp "$(SDL_URL)"
	echo "$(SDL_SHA256)  $@.tmp" | shasum -a 256 --check
	mv $@.tmp $@

package: $(BUILD)/miyoo/clicktest $(SDL_LIB)
	rm -rf $(PACKAGE)
	mkdir -p $(PACKAGE)/lib
	cp packaging/clicktest/launch.sh packaging/clicktest/config.json packaging/clicktest/icon.png $(BUILD)/miyoo/clicktest $(PACKAGE)/
	cp $(SDL_LIB) $(PACKAGE)/lib/

# Usage: make sd SD=/Volumes/YOUR_CARD
sd: package
	@test -n "$(SD)" || (echo "Set SD to the mounted card, e.g. make sd SD=/Volumes/ONION" && exit 1)
	@test -d "$(SD)/.tmp_update" || (echo "$(SD) does not look like an Onion SD card" && exit 1)
	mkdir -p "$(SD)/App/$(APP_NAME)"
	rsync -rt --delete --exclude='*.log' $(PACKAGE)/ "$(SD)/App/$(APP_NAME)/"
	sync
	@echo "Installed to $(SD)/App/$(APP_NAME). Eject the card before removing it."

clean:
	rm -rf $(BUILD)
