.PHONY: all build install clean

all: build

build/build.ninja:
	meson setup build

build: build/build.ninja
	ninja -C build

install: build/build.ninja
	ninja -C build install

clean:
	rm -rf build
