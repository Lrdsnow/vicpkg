all: package

clean:
	rm -f ffmpeg.vpkg

install:
	scp src/ffmpeg root@vector:/anki/bin/

package:
	@mkdir -p /tmp/ffmpeg-build
	@cp -r src/Package/* /tmp/ffmpeg-build/
	@mkdir -p /tmp/ffmpeg-build/pkg/data/vicpkg/bin
	@cp src/ffmpeg /tmp/ffmpeg-build/pkg/data/vicpkg/bin/
	@cp src/ffmpeg_g /tmp/ffmpeg-build/pkg/data/vicpkg/bin/
	@cp src/ffprobe /tmp/ffmpeg-build/pkg/data/vicpkg/bin/
	@cp src/ffprobe_g /tmp/ffmpeg-build/pkg/data/vicpkg/bin/
	@mkdir -p /tmp/ffmpeg-build/pkg/data/vicpkg/doc/ffmpeg/
	@cp src/SOURCE_INFO.txt /tmp/ffmpeg-build/pkg/data/vicpkg/doc/ffmpeg/
	@cp src/COPYING.LGPLv2.1 /tmp/ffmpeg-build/pkg/data/vicpkg/doc/ffmpeg/
	@cd /tmp/ffmpeg-build && tar -czf ffmpeg.vpkg *
	@mv /tmp/ffmpeg-build/ffmpeg.vpkg .
	@rm -rf /tmp/ffmpeg-build
	@echo "Package created: ffmpeg.vpkg"

.PHONY: all clean install package