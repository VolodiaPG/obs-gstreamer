default: build

.PHONY: setup build

setup:
	meson --buildtype=release --libdir=lib --prefix=/usr build

build: 
	ninja -C build

install:
	ninja -C build install
