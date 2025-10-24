CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -O2
INCLUDES := -Iinclude
LDFLAGS ?=
LIBS ?= -lpq

-include config.mk

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SYSTEMD_UNITDIR ?= /etc/systemd/system
SYSCONFDIR ?= /etc/address-matching-service
RUNDIR ?= bin

SRC := src/main.c src/address_matcher.c
OBJ := $(SRC:.c=.o)
TARGET := $(RUNDIR)/address_matching_service

.PHONY: all clean install uninstall distclean

all: $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $(OBJ) -o $@ $(LDFLAGS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/
	install -d $(DESTDIR)$(SYSTEMD_UNITDIR)
	sed \
		-e "s|@BINDIR@|$(BINDIR)|g" \
		-e "s|@SYSCONFDIR@|$(SYSCONFDIR)|g" \
		packaging/address-matching-service.service.in > $(DESTDIR)$(SYSTEMD_UNITDIR)/address-matching-service.service
	install -d $(DESTDIR)$(SYSCONFDIR)
	install -m 644 config/address-matching-service.env.example $(DESTDIR)$(SYSCONFDIR)/env.example

uninstall:
	@if [ -f "$(DESTDIR)$(BINDIR)/address_matching_service" ]; then \
		rm -f "$(DESTDIR)$(BINDIR)/address_matching_service"; \
	fi
	@if [ -f "$(DESTDIR)$(SYSTEMD_UNITDIR)/address-matching-service.service" ]; then \
		rm -f "$(DESTDIR)$(SYSTEMD_UNITDIR)/address-matching-service.service"; \
	fi
	@if [ -f "$(DESTDIR)$(SYSCONFDIR)/env.example" ]; then \
		rm -f "$(DESTDIR)$(SYSCONFDIR)/env.example"; \
	fi
	@if [ -d "$(DESTDIR)$(SYSCONFDIR)" ]; then \
		rmdir --ignore-fail-on-non-empty "$(DESTDIR)$(SYSCONFDIR)" 2>/dev/null || true; \
	fi

clean:
	rm -f $(OBJ)
	rm -f $(TARGET)

distclean: clean
	rm -f config.mk
