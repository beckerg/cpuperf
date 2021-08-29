# Copyright (c) 2021 Greg Becker.  All rights reserved.

PROG := cpuperf

SRC := clp.c subr.c lfstack.c xoroshiro.c ${patsubst %,%.c,${PROG}}
OBJ := ${SRC:.c=.o}

PROG_VERSION := $(shell git describe --abbrev=8 --dirty --always --tags)
PLATFORM := ${shell uname -s | tr '[:upper:]' '[:lower:]'}

INCLUDE  := -I. -I../src
CFLAGS   += -std=c11 -Wall -O2 -march=native -g ${INCLUDE}
CPPFLAGS += -DPROG_VERSION=\"1.0.0-${PROG_VERSION}\" -DNDEBUG
LDLIBS   += -lpthread

ifeq ($(PLATFORM),linux)
CPPFLAGS += -D_GNU_SOURCE
LDLIBS   += -latomic
endif

.DELETE_ON_ERROR:
.NOTPARALLEL:

.PHONY:	all asan check clean clobber debug distclean maintainer-clean test


all: ${PROG}

asan: CPPFLAGS += -UNDEBUG -DCLP_DEBUG
asan: CFLAGS += -O0 -fno-omit-frame-pointer
asan: CFLAGS += -fsanitize=address -fsanitize=undefined
asan: LDLIBS += -fsanitize=address -fsanitize=undefined
asan: ${PROG}

clean:
	rm -f ${PROG} ${OBJ} *.core
	rm -f $(patsubst %.c,.%.d*,${SRC})

cleandir clobber distclean maintainer-clean: clean

clock: CPPFLAGS += -DUSE_CLOCK=1
clock: ${PROG}

debug: CPPFLAGS += -UNDEBUG -DCLP_DEBUG
debug: CFLAGS += -O0 -fno-omit-frame-pointer
debug: ${PROG}

%: %.o ${OBJ}
	$(LINK.o) $^ $(LOADLIBES) $(LDLIBS) -o $@

${OBJ}: GNUmakefile

.%.d: %.c
	@set -e; rm -f $@; \
	$(CC) -M $(CPPFLAGS) ${INCLUDE} $< > $@.$$$$; \
	sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' < $@.$$$$ > $@; \
	rm -f $@.$$$$

-include $(patsubst %.c,.%.d,${SRC})
