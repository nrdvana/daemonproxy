MAKE = make

all: build/Makefile
	$(MAKE) -C build

build/Makefile build/config.h: autoconf/configure autoconf/Makefile.in autoconf/config.h.in
	mkdir -p build && cd build && ../autoconf/configure

autoconf/configure: autoconf/configure.ac
	cd autoconf && autoconf

clean:
	$(MAKE) -C build clean
distclean:
	rm -r build
test:
	$(MAKE) -C build test
install:
	$(MAKE) -C build install
