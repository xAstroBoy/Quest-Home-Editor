# Makefile — Root Linux & macOS build automation for Quest Home Editor
#
# Targets:
#   all (default)  - Configure and build all binaries (editor + CLI cooker)
#   editor         - Build "Quest Home Editor" GUI application
#   cooker         - Build "hsl_cook" CLI cooker
#   install-deps   - Install required dependencies on Debian/Ubuntu systems
#   appimage       - Package Linux build into a portable .AppImage
#   tarball        - Package Linux build into a portable .tar.gz archive
#   docker         - Build Linux binaries reproducibly inside a Docker container
#   clean          - Clean build directories and package artifacts

BUILD_DIR ?= build
BUILD_TYPE ?= Release
GENERATOR ?= $(shell command -v ninja >/dev/null 2>&1 && echo "-G Ninja")
CMAKE_FLAGS ?= -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
NPROC ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

.PHONY: all editor cooker install-deps appimage tarball docker clean

all:
	@cmake -S QuestHomeEditor -B $(BUILD_DIR) $(GENERATOR) $(CMAKE_FLAGS)
	@cmake --build $(BUILD_DIR) -j$(NPROC)
	@echo "\n=== Build complete ==="
	@echo "  Editor GUI: ./$(BUILD_DIR)/Quest Home Editor"
	@echo "  CLI Cooker: ./$(BUILD_DIR)/hsl_cook\n"

editor:
	@cmake -S QuestHomeEditor -B $(BUILD_DIR) $(GENERATOR) $(CMAKE_FLAGS)
	@cmake --build $(BUILD_DIR) --target hsr_renderer -j$(NPROC)

cooker:
	@cmake -S QuestHomeEditor -B $(BUILD_DIR) $(GENERATOR) $(CMAKE_FLAGS)
	@cmake --build $(BUILD_DIR) --target hsl_cook -j$(NPROC)

install-deps:
	sudo apt-get update && sudo apt-get install -y \
		cmake ninja-build libvulkan-dev \
		libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
		libwayland-dev libxkbcommon-dev pkg-config

appimage: all
	@echo "Creating AppImage..."
	@rm -rf AppDir
	@mkdir -p AppDir/usr/bin/fonts
	@cp "$(BUILD_DIR)/Quest Home Editor" AppDir/usr/bin/
	@cp -r QuestHomeEditor/third_party/fonts/* AppDir/usr/bin/fonts/
	@cp QuestHomeEditor/app.png AppDir/hsr_renderer.png
	@printf "[Desktop Entry]\nType=Application\nName=Quest Home Editor\nExec=hsr_renderer\nIcon=hsr_renderer\nCategories=Graphics;\n" > AppDir/hsr_renderer.desktop
	@printf "#!/bin/sh\nHERE=\"\$$(dirname \"\$$(readlink -f \"\$$0\")\")\"\nexec \"\$$HERE/usr/bin/Quest Home Editor\" \"\$$@\"\n" > AppDir/AppRun
	@chmod +x AppDir/AppRun
	@if [ ! -x ./appimagetool ]; then \
		curl -sSfL -o appimagetool https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage; \
		chmod +x appimagetool; \
	fi
	@ARCH=x86_64 ./appimagetool --appimage-extract-and-run AppDir Quest-Home-Editor-linux-x86_64.AppImage
	@echo "Built AppImage: Quest-Home-Editor-linux-x86_64.AppImage"

tarball: all
	@echo "Creating distribution tarball..."
	@mkdir -p dist/fonts
	@cp "$(BUILD_DIR)/Quest Home Editor" dist/
	@cp "$(BUILD_DIR)/hsl_cook" dist/
	@cp -r QuestHomeEditor/third_party/fonts/* dist/fonts/
	@tar -czf Quest-Home-Editor-linux-x86_64.tar.gz -C dist .
	@rm -rf dist
	@echo "Built tarball: Quest-Home-Editor-linux-x86_64.tar.gz"

docker:
	docker build -f Dockerfile.linux -t quest-home-editor-build .
	docker run --rm -v "$(PWD)/dist:/out" quest-home-editor-build sh -c "cp -r /app/build/* /out/"

clean:
	rm -rf $(BUILD_DIR) AppDir dist appimagetool *.AppImage *.tar.gz
