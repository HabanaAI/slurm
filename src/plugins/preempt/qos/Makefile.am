# Makefile for preempt/qos plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic

AM_CPPFLAGS = -DSLURM_PLUGIN_DEBUG -I$(top_srcdir)

pkglib_LTLIBRARIES = preempt_qos.la

preempt_qos_la_SOURCES = preempt_qos.c

preempt_qos_la_LDFLAGS = $(PLUGIN_FLAGS)
