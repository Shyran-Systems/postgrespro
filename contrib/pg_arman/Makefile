PROGRAM = pg_arman
OBJS = backup.o \
	catalog.o \
	data.o \
	delete.o \
	dir.o \
	fetch.o \
	init.o \
	parray.o \
	pg_arman.o \
	restore.o \
	show.o \
	status.o \
	util.o \
	validate.o \
	datapagemap.o \
	parsexlog.o \
	xlogreader.o \
	streamutil.o \
	receivelog.o \
	pgut/pgut.o \
	pgut/pgut-port.o

EXTRA_CLEAN = datapagemap.c datapagemap.h xlogreader.c receivelog.c receivelog.h streamutil.c streamutil.h logging.h

REGRESS = init option show delete backup restore

all: checksrcdir datapagemap.h logging.h receivelog.h streamutil.h pg_arman

MAKE_GLOBAL="../../src/Makefile.global"
TEST_GLOBAL:=$(shell test -e ../../src/Makefile.global)
ifeq ($(.SHELLSTATUS),1)
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

.PHONY: checksrcdir
checksrcdir:
ifndef top_srcdir
	@echo "You must have PostgreSQL source tree available to compile."
	@echo "Pass the path to the PostgreSQL source tree to make, in the top_srcdir"
	@echo "variable: \"make top_srcdir=<path to PostgreSQL source tree>\""
	@exit 1
endif
else
#TODO: fix me
REGRESS =
subdir=contrib/pg_arman
top_builddir=../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
PG_CPPFLAGS = -I$(libpq_srcdir) ${PTHREAD_CFLAGS}
override CPPFLAGS := -DFRONTEND $(CPPFLAGS) $(PG_CPPFLAGS)
PG_LIBS = $(libpq_pgport) ${PTHREAD_CFLAGS}

envtest:
	: top_srcdir=$(top_srcdir)
	: libpq_srcdir = $(libpq_srcdir)

# This rule's only purpose is to give the user instructions on how to pass
# the path to PostgreSQL source tree to the makefile.
.PHONY: checksrcdir
checksrcdir:
ifndef top_srcdir
	@echo "You must have PostgreSQL source tree available to compile."
	@echo "Pass the path to the PostgreSQL source tree to make, in the top_srcdir"
	@echo "variable: \"make top_srcdir=<path to PostgreSQL source tree>\""
	@exit 1
endif

# Those files are symlinked from the PostgreSQL sources.
xlogreader.c: % : $(top_srcdir)/src/backend/access/transam/%
	rm -f $@ && $(LN_S) $< .
datapagemap.c: % : $(top_srcdir)/src/bin/pg_rewind/%
	rm -f $@ && $(LN_S) $< .
datapagemap.h: % : $(top_srcdir)/src/bin/pg_rewind/%
	rm -f  && $(LN_S) $< .
#logging.c: % : $(top_srcdir)/src/bin/pg_rewind/%
#	rm -f  && $(LN_S) $< .
logging.h: % : $(top_srcdir)/src/bin/pg_rewind/%
	rm -f  && $(LN_S) $< .
receivelog.c: % : $(top_srcdir)/src/bin/pg_basebackup/%
	rm -f  && $(LN_S) $< .
receivelog.h: % : $(top_srcdir)/src/bin/pg_basebackup/%
	rm -f  && $(LN_S) $< .
streamutil.c: % : $(top_srcdir)/src/bin/pg_basebackup/%
	rm -f  && $(LN_S) $< .
streamutil.h: % : $(top_srcdir)/src/bin/pg_basebackup/%
	rm -f  && $(LN_S) $< .
