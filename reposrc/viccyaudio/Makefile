SDK_ROOT = $(HOME)/.anki/vicos-sdk/dist/5.3.0-r07
PREBUILT = $(SDK_ROOT)/prebuilt

CC = $(PREBUILT)/bin/arm-oe-linux-gnueabi-clang
STRIP = $(PREBUILT)/arm-oe-linux-gnueabi/bin/strip

TARGET = viccyaudio
SRC = src/main.c

CFLAGS = -O2 -Wall -Wextra
LDFLAGS = 

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)
	$(STRIP) $(TARGET)

clean:
	rm -f $(TARGET) viccyaudio.vpkg

install: $(TARGET)
	scp $(TARGET) root@vector:/anki/bin/

package: $(TARGET)
	@mkdir -p /tmp/viccyaudio-build
	@cp -r src/Package/* /tmp/viccyaudio-build/
	@mkdir -p /tmp/viccyaudio-build/pkg/data/vicpkg/bin
	@cp $(TARGET) /tmp/viccyaudio-build/pkg/data/vicpkg/bin/
	@cd /tmp/viccyaudio-build && tar -czf viccyaudio.vpkg *
	@mv /tmp/viccyaudio-build/viccyaudio.vpkg .
	@rm -rf /tmp/viccyaudio-build
	@echo "Package created: viccyaudio.vpkg"

.PHONY: all clean install package