# Silicon Path for Windows — builds with MinGW-w64 g++ (MSYS2 UCRT64) and mingw32-make.
#
#   mingw32-make            build SiliconPath.exe, spcheck.exe and uishot.exe into build/
#   mingw32-make check      build, then run the example check and the hand-written tests
#   mingw32-make clean
#
# Everything is linked statically, so the .exe files run on any Windows 10/11 PC
# without MSYS2 installed.

CXX      ?= g++
WINDRES  ?= windres
BUILD    := build
CXXFLAGS := -std=c++20 -O2 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -municode -MMD -MP
LDFLAGS  := -static -municode
UI_LIBS  := -ld2d1 -ldwrite -lwindowscodecs -lole32 -luuid -lcomdlg32 -lshell32 -lgdi32 -luser32 -limm32 -ldwmapi

# mingw32-make runs recipes in sh when MSYS2's sh.exe is on PATH, and in cmd.exe
# otherwise (for example when it is started from PowerShell). cmd.exe echoes the quotes.
ifeq ($(shell echo "sh"),"sh")
  native = $(subst /,\,$(1))
  MKDIR  = if not exist "$(call native,$(1))" mkdir "$(call native,$(1))"
  RMDIR  = if exist "$(call native,$(1))" rmdir /s /q "$(call native,$(1))"
else
  native = $(1)
  MKDIR  = mkdir -p "$(1)"
  RMDIR  = rm -rf "$(1)"
endif

CORE_SRC := $(wildcard src/core/*.cpp)
UI_SRC   := $(wildcard src/ui/*.cpp)
CORE_OBJ := $(patsubst src/%.cpp,$(BUILD)/%.o,$(CORE_SRC))
UI_OBJ   := $(patsubst src/%.cpp,$(BUILD)/%.o,$(UI_SRC))
CHECK_OBJ := $(patsubst src/%.cpp,$(BUILD)/%.o,$(wildcard src/spcheck/*.cpp)) $(BUILD)/ui/Examples.o
APP_OBJ  := $(BUILD)/app/main.o $(BUILD)/app/resources.o
SHOT_OBJ := $(BUILD)/app/uishot.o

.PHONY: all check clean icon dirs

all: $(BUILD)/SiliconPath.exe $(BUILD)/spcheck.exe $(BUILD)/uishot.exe

# Creates the output folders once, before any parallel compile needs them.
dirs:
	@$(call MKDIR,$(BUILD)/core)
	@$(call MKDIR,$(BUILD)/ui)
	@$(call MKDIR,$(BUILD)/app)
	@$(call MKDIR,$(BUILD)/spcheck)

$(BUILD)/%.o: src/%.cpp | dirs
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/app/resources.o: res/app.rc res/app.ico res/app.manifest | dirs
	$(WINDRES) -I res res/app.rc -O coff -o $@

$(BUILD)/spcheck.exe: $(CHECK_OBJ) $(CORE_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^

$(BUILD)/SiliconPath.exe: $(APP_OBJ) $(UI_OBJ) $(CORE_OBJ)
	$(CXX) $(LDFLAGS) -mwindows -o $@ $^ $(UI_LIBS)

$(BUILD)/uishot.exe: $(SHOT_OBJ) $(UI_OBJ) $(CORE_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(UI_LIBS)

check: $(BUILD)/spcheck.exe
	$(call native,$(BUILD)/spcheck.exe) verify
	$(call native,$(BUILD)/spcheck.exe) hand

# The icon is committed; this redraws it (the same design as scripts/make_icon.swift).
icon: $(BUILD)/make_icon.exe
	$(call native,$(BUILD)/make_icon.exe) res/app.ico

$(BUILD)/make_icon.exe: tools/make_icon.cpp | dirs
	$(CXX) -std=c++20 -O2 -municode -static -o $@ $< -ld2d1 -ldwrite -lwindowscodecs -lole32 -luuid

clean:
	$(call RMDIR,$(BUILD))

-include $(wildcard $(BUILD)/*/*.d)
