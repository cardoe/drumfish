
bin_PROGRAMS =

simavr_LIBS = $(shell pwd)/../simavr/simavr/obj-${shell $(CC) -dumpmachine}

EVENT_CFLAGS = $(shell pkg-config --cflags libevent)
EVENT_LIBS = $(shell pkg-config --libs libevent)

BUILD_CFLAGS = -std=gnu99 -g \
  -Wall -Wextra -W \
  -Wbad-function-cast \
  -Waggregate-return -Wcast-align \
  -Wfloat-equal -Wimplicit \
  -Wmissing-declarations -Wmissing-prototypes \
  -Wnested-externs -Wpointer-arith \
  -Wsign-compare -Wchar-subscripts \
  -Wstrict-prototypes -Wshadow \
  -Wformat-security -Wtype-limits \
  -Werror=format-security \
  -Wformat -Wformat=2 -Wmissing-braces \
  -Wreturn-type -Wunused-label \
  -Wcast-qual -Wdisabled-optimization \
  -Wwrite-strings -Wmissing-format-attribute
BUILD_CFLAGS += -I../simavr/simavr/sim/
BUILD_CFLAGS += $(EVENT_CFLAGS)
BUILD_CFLAGS += $(CFLAGS)

# Rules to build drumfish
bin_PROGRAMS += drumfish
drumfish_SOURCES = drumfish.c flash.c m128rfa1.c uart_pty.c df_log.c
drumfish_OBJS = $(drumfish_SOURCES:.c=.o)
drumfish_LDFLAGS = $(LDFLAGS)
drumfish_LDADD = -L$(simavr_LIBS) $(EVENT_LIBS) -lsimavr -lelf
drumfish_LDADD += -pthread -lutil $(LDADD)

# Very basic quiet rules
ifneq ($(V),)
	Q=
else
	Q=@
endif

.PHONY: all
all: $(bin_PROGRAMS)

%.o: %.c
	@echo "  CC $@"
	$(Q)$(CC) $(BUILD_CFLAGS) -o $@ -c $<

%: .libs/%
	-@cp ../run_wrapper.sh $@

.libs/drumfish: $(drumfish_OBJS)
	-@mkdir -p $(@D)
	@echo "  CCLD $(@F)"
	$(Q)$(CC) $($(@F)_LDFLAGS) -o $@ $^ $($(@F)_LDADD)

.PHONY: clean
clean:
	$(Q)rm -f $(drumfish_OBJS)
	$(Q)rm -f $(bin_PROGRAMS)
