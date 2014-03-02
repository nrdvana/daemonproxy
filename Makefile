MAKE = make
PROVE = prove

all: build/Makefile
	$(MAKE) -C build all

build/Makefile build/config.h: scripts/configure scripts/Makefile.in scripts/config.h.in
	mkdir -p build && cd build && ../scripts/configure

scripts/configure: scripts/configure.ac
	cd scripts && autoconf

clean:
	$(MAKE) -C build clean

dist:
	$(MAKE) -C build dist

test:
	$(MAKE) -C build daemonproxy
	$(PROVE) -j4

install:
	$(MAKE) -C build install

.PHONY: test all clean dist install
