# Makefile for backfill scheduler plugin

AUTOMAKE_OPTIONS = foreign

AM_CXXFLAGS = -fexceptions

PLUGIN_FLAGS = -module -avoid-version --export-dynamic

AM_CPPFLAGS = -DSLURM_PLUGIN_DEBUG -I$(top_srcdir)

pkglib_LTLIBRARIES = sched_backfill.la

sched_backfill_la_SOURCES = backfill_wrapper.c	\
			backfill.c	\
			backfill.h	\
			oracle.c	\
			oracle.h
sched_backfill_la_LDFLAGS = $(PLUGIN_FLAGS)
