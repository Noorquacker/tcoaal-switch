# TCOAAL -> Nintendo Switch port pipeline.
#
#   make GAME="/path/to/The Coffin of Andy and Leyley"   # everything: build/tcoaal.nro
#   make host                                           # host runtime + jsbc
#   make romfs GAME=...                                 # decrypt + patch + compile scripts
#   make switch                                         # Switch .nro from build/romfs
#   make nsp                                            # installable .nsp (needs hacbrewpack + prod.keys)
#   make run                                            # run build/romfs with the host runtime
#
# GAME may point at the game folder or its www/ folder. Only your own copy is used;
# nothing from the game is part of this repository.

BUILD    := $(CURDIR)/build
# NRO icon, downloaded at build time (not stored in the repo). Override with ICON_URL=... or ICON_URL=
ICON_URL ?= https://cdn2.steamgriddb.com/icon/b759b1649138a906c4ec2b2516ae5084/32/256x256.png
HOSTDIR  := $(BUILD)/host
ROMFS    := $(BUILD)/romfs
PYTHON   ?= python3

.PHONY: all host romfs switch nsp run clean
all: switch

host:
	cmake -S runtime -B $(HOSTDIR) -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
	cmake --build $(HOSTDIR) -j

romfs: host
	@test -n "$(GAME)" || (echo 'usage: make romfs GAME="/path/to/game"' && false)
	$(PYTHON) -I tools/build_romfs.py "$(GAME)" $(ROMFS) --jsbc $(HOSTDIR)/jsbc

# elf2nro wants a 256x256 JPEG; falls back to the libnx default icon if this fails
$(BUILD)/icon.jpg:
	@mkdir -p $(BUILD)
	@if [ -n "$(ICON_URL)" ] && curl -fsSL "$(ICON_URL)" -o $(BUILD)/icon-src.png \
	    && $(PYTHON) -I tools/make_icon.py $(BUILD)/icon-src.png $@; then echo "icon: $(ICON_URL)"; \
	 else echo "icon: download/convert failed, using default"; rm -f $@; fi

switch: $(BUILD)/icon.jpg
	@test -d $(ROMFS) || $(MAKE) romfs
	$(MAKE) -C runtime -f Makefile.switch ROMFS=$(ROMFS) OUT=$(BUILD) -j$$(nproc) \
	    $$(test -f $(BUILD)/icon.jpg && echo ICON=$(BUILD)/icon.jpg)

# installable NSP; needs hacbrewpack and prod.keys (KEYS=/path/to/prod.keys to override)
nsp: $(BUILD)/icon.jpg
	@test -d $(ROMFS) || $(MAKE) romfs
	$(MAKE) -C runtime -f Makefile.switch ROMFS=$(ROMFS) OUT=$(BUILD) -j$$(nproc) nsp \
	    $$(test -f $(BUILD)/icon.jpg && echo ICON=$(BUILD)/icon.jpg)

run: host
	$(HOSTDIR)/tcoaal $(ROMFS) $(BUILD)/save

clean:
	rm -rf $(BUILD)
