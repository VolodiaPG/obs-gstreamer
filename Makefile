default: build

.PHONY: setup build

setup:
	meson --buildtype=release build

build: 
	ninja -C build

install:
	ninja -C build install
