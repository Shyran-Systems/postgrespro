# contrib/Makefile

subdir = contrib
top_builddir = ..
include $(top_builddir)/src/Makefile.global

SUBDIRS = \
		adminpack	\
		auth_delay	\
		auto_explain	\
		btree_gin	\
		btree_gist	\
		chkpass		\
		citext		\
		cube		\
		dblink		\
		dict_int	\
		dict_xsyn	\
		dump_stat   \
		earthdistance	\
		file_fdw	\
		fuzzystrmatch	\
		hstore		\
		hunspell_en_us \
		hunspell_fr \
		hunspell_nl_nl \
		hunspell_ru_ru \
		intagg		\
		intarray	\
		isn		\
		jsquery \
		lo		\
		ltree		\
		oid2name	\
		pageinspect	\
		passwordcheck	\
		pg_arman	\
		pg_buffercache	\
		pg_freespacemap \
		pg_prewarm	\
		pg_standby	\
		pg_stat_statements \
		pg_trgm		\
		pg_variables	\
		pgcrypto	\
		pgrowlocks	\
		pgstattuple	\
		postgres_fdw	\
		seg		\
		shared_ispell \
		spi		\
		sr_plan \
		tablefunc	\
		tcn		\
		test_decoding	\
		tsm_system_rows \
		tsm_system_time \
		tsearch2	\
		unaccent	\
		vacuumlo    \
		pg_pathman

ifeq ($(with_openssl),yes)
SUBDIRS += sslinfo
else
ALWAYS_SUBDIRS += sslinfo
endif

ifneq ($(with_uuid),no)
SUBDIRS += uuid-ossp
else
ALWAYS_SUBDIRS += uuid-ossp
endif

ifeq ($(with_libxml),yes)
SUBDIRS += xml2
else
ALWAYS_SUBDIRS += xml2
endif

ifeq ($(with_selinux),yes)
SUBDIRS += sepgsql
else
ALWAYS_SUBDIRS += sepgsql
endif

ifeq ($(with_perl),yes)
SUBDIRS += hstore_plperl
else
ALWAYS_SUBDIRS += hstore_plperl
endif

ifeq ($(with_python),yes)
SUBDIRS += hstore_plpython ltree_plpython
else
ALWAYS_SUBDIRS += hstore_plpython ltree_plpython
endif

# Missing:
#		start-scripts	\ (does not have a makefile)


$(recurse)
$(recurse_always)
