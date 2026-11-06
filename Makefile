NDK ?= $(HOME)/android-dev/android-ndk-r27c
TOOLCHAIN := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin
TARGET := armv7a-linux-androideabi29
CXX := $(TOOLCHAIN)/$(TARGET)-clang++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -static-libstdc++
SOURCE := main.cpp
OUTPUT := ir-reader

.PHONY: all clean

all: $(OUTPUT)

$(OUTPUT): $(SOURCE)
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f $(OUTPUT) *.o
