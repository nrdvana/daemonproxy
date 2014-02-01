MAKE = make
PROVE = prove

all: build/Makefile
	$(MAKE) -C build daemonproxy

build/Makefile build/config.h: autoconf/configure autoconf/Makefile.in autoconf/config.h.in
	mkdir -p build && cd build && ../autoconf/configure

autoconf/configure: autoconf/configure.ac
	cd autoconf && autoconf

clean:
	$(MAKE) -C build clean

distclean:
	rm -r build

test:
	$(MAKE) -C build daemonproxy test
	$(PROVE) -j4

install:
	$(MAKE) -C build install

.PHONY: test all clean distclean install
