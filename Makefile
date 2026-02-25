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
	rm -f $(TARGET) $(TARGET).vpkg

install: $(TARGET)
	scp $(TARGET) root@vector:/data/vicpkg/bin/$(TARGET)

package: $(TARGET)
	@mkdir -p /tmp/$(TARGET)-build
	@cp -r src/Package/* /tmp/$(TARGET)-build/
	@mkdir -p /tmp/$(TARGET)-build/pkg/data/vicpkg/bin
	@cp $(TARGET) /tmp/$(TARGET)-build/pkg/data/vicpkg/bin/
	@cd /tmp/$(TARGET)-build && tar -czf $(TARGET).vpkg *
	@mv /tmp/$(TARGET)-build/$(TARGET).vpkg .
	@rm -rf /tmp/$(TARGET)-build
	@echo "Package created: $(TARGET).vpkg"

.PHONY: all clean install package