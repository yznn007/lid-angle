CC = clang
MACOSX_DEPLOYMENT_TARGET ?= 11.0
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
TARGET_FLAGS = -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET)
LDFLAGS ?=
FRAMEWORKS = -framework IOKit -framework CoreFoundation
PREFIX ?= $(HOME)/.local

.PHONY: all clean install uninstall

all: lid-angle

lid-angle: main.c
	"$(CC)" $(CFLAGS) $(TARGET_FLAGS) $(LDFLAGS) "$<" $(FRAMEWORKS) -o "$@"

clean:
	$(RM) lid-angle

install: lid-angle
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 0755 lid-angle "$(DESTDIR)$(PREFIX)/bin/lid-angle"

uninstall:
	$(RM) "$(DESTDIR)$(PREFIX)/bin/lid-angle"
