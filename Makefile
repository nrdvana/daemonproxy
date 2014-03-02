MAKE = make
PROVE = prove
PERL = perl

all: build/Makefile
	$(MAKE) -C build -j4 all

build/Makefile build/config.h: scripts/configure scripts/Makefile.in scripts/config.h.in
	mkdir -p build && cd build && ../scripts/configure

scripts/configure: scripts/configure.ac
	cd scripts && autoconf

clean:
	$(MAKE) -C build clean

dist:
	env PROJ_ROOT=. $(PERL) scripts/build_dist_tarball.pl

test:
	$(MAKE) -C build daemonproxy
	$(PROVE) -j4

install:
	$(MAKE) -C build install

.PHONY: test all clean dist install
