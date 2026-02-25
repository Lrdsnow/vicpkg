SDK_ROOT = $(HOME)/.anki/vicos-sdk/dist/5.3.0-r07
PREBUILT = $(SDK_ROOT)/prebuilt

CC = $(PREBUILT)/bin/arm-oe-linux-gnueabi-clang
STRIP = $(PREBUILT)/arm-oe-linux-gnueabi/bin/strip

TARGET = vicpkg
SRC = src/vicpkg.c

CFLAGS = -O2 -Wall -Wextra
LDFLAGS = 

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)
	$(STRIP) $(TARGET)

clean:
	rm -f $(TARGET) vicpkg.vpkg

install: $(TARGET)
	scp $(TARGET) root@vector:/anki/bin/

package: $(TARGET)
	@mkdir -p /tmp/vicpkg-build
	@cp -r src/Package/* /tmp/vicpkg-build/
	@mkdir -p /tmp/vicpkg-build/pkg/data/vicpkg/bin
	@cp $(TARGET) /tmp/vicpkg-build/pkg/data/vicpkg/bin/
	@cd /tmp/vicpkg-build && tar -czf vicpkg.vpkg *
	@mv /tmp/vicpkg-build/vicpkg.vpkg .
	@rm -rf /tmp/vicpkg-build
	@echo "Package created: vicpkg.vpkg"

.PHONY: all clean install package