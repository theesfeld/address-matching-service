CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -O2
INCLUDES := -Iinclude
LDFLAGS ?=
LIBS ?=

-include config.mk

PQ_CFLAGS ?=
PQ_LDFLAGS ?=
PQ_LIBS ?=
PG_CONFIG ?= pg_config

ifeq ($(PQ_CFLAGS),)
ifneq ($(shell command -v $(PG_CONFIG) 2>/dev/null),)
PQ_CFLAGS := -I$(shell $(PG_CONFIG) --includedir)
PQ_LDFLAGS := -L$(shell $(PG_CONFIG) --libdir)
PQ_LIBS := -lpq
else
PQ_CFLAGS := -I/usr/include/postgresql
PQ_LDFLAGS :=
PQ_LIBS := -lpq
endif
endif

CFLAGS += $(PQ_CFLAGS)
LDFLAGS += $(PQ_LDFLAGS)
LIBS += $(PQ_LIBS)

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
