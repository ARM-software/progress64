#Copyright (c) 2015, ARM Limited. All rights reserved.
#
#SPDX-License-Identifier:        BSD-3-Clause
###############################################################################
# Makefile designed by Wonderful Void
# Supports multiple targets and auto-generated dependencies
################################################################################

###############################################################################
# Project specific definitions
################################################################################

#List of executable files to build
TARGETS = libprogress64.a hashtable timer rwlock reorder antireplay rwsync reassemble
#List object files for each target
OBJECTS_libprogress64.a = p64_ringbuf.o p64_spinlock.o p64_rwlock.o p64_barrier.o p64_hazardptr.o p64_hashtable.o p64_timer.o p64_rwsync.o p64_antireplay.o p64_reorder.o p64_reassemble.o
OBJECTS_hashtable = p64_hazardptr.o p64_hashtable.o hashtable.o
OBJECTS_timer = p64_timer.o timer.o
OBJECTS_rwlock = p64_rwlock.o rwlock.o
OBJECTS_reorder = p64_reorder.o reorder.o
OBJECTS_antireplay = p64_antireplay.o antireplay.o
OBJECTS_rwsync = p64_rwsync.o rwsync.o
OBJECTS_reassemble = p64_reassemble.o reassemble.o

DEBUG ?= 0
ASSERT ?= 0

ifeq ($(DEBUG),0)
CCFLAGS += -O2
else
CCFLAGS += -O0
endif
ifeq ($(ASSERT),0)
DEFINE += -DNDEBUG#disable assertions
endif
DEFINE += -D_GNU_SOURCE
CCFLAGS += -std=c99
#CCFLAGS += -march=armv8.1-a
CCFLAGS += -g -ggdb -Wall
CCFLAGS += -fomit-frame-pointer
CCFLAGS += -fstrict-aliasing -fno-stack-check -fno-stack-protector
LDFLAGS += -g -ggdb -pthread
LIBS = -lrt

#Where to find the source files
VPATH += src examples
#Where to find include files
INCLUDE += include src

#Default to non-verbose mode (echo command lines)
VERB = @

#Location of object and other derived/temporary files
OBJDIR = obj#Must not be .

###############################################################################
# Make actions (phony targets)
################################################################################

.PHONY : default all clean tags etags

default:
	@echo "Make targets:"
	@echo "all         build all targets ($(TARGETS))"
	@echo "clean       remove derived files"
	@echo "tags        generate vi tags file"
	@echo "etags       generate emacs tags file"

all : $(TARGETS)

#Make sure we don't remove current directory with all source files
ifeq ($(OBJDIR),.)
$(error invalid OBJDIR=$(OBJDIR))
endif
ifeq ($(TARGETS),.)
$(error invalid TARGETS=$(TARGETS))
endif
clean:
	@echo "--- Removing derived files"
	$(VERB)-rm -rf $(OBJDIR) $(TARGETS) tags TAGS perf.data perf.data.old

tags :
	$(VERB)ctags -R .

etags :
	$(VERB)ctags -e -R .

################################################################################
# Setup tool commands and flags
################################################################################

#Defaults to be overriden by compiler makefragment
CCOUT = -o $@
ASOUT = -o $@
AROUT = $@
LDOUT = -o $@

ifeq ($(CLANG),yes)
CC = $(CROSS_COMPILE)clang
CXX = $(CROSS_COMPILE)clang++
AS = $(CROSS_COMPILE)as
AR = $(CROSS_COMPILE)ar
LD = $(CROSS_COMPILE)clang++
else
CC = $(CROSS_COMPILE)gcc
CXX = $(CROSS_COMPILE)g++
AS = $(CROSS_COMPILE)as
AR = $(CROSS_COMPILE)ar
LD = $(CROSS_COMPILE)g++
endif
#GROUPSTART = -Wl,--start-group
#GROUPEND = -Wl,--end-group
ARFLAGS = -rcD
BIN2C = bin2c

#Important compilation flags
CCFLAGS += -c -MMD -MP

################################################################################
# Post-process some variables and definitions, generate dependencies
################################################################################

CCFLAGS += $(DEFINE) $(addprefix -I,$(INCLUDE))
#Generate list of all object files (for all targets)
override OBJECTS := $(addprefix $(OBJDIR)/,$(foreach var,$(TARGETS),$(OBJECTS_$(var))))
#Generate target:objects dependencies for all targets
$(foreach target,$(TARGETS),$(eval $(target) : $$(addprefix $$(OBJDIR)/,$$(OBJECTS_$(target)))))
#Special dependency for object files on object directory
$(OBJECTS) : | $(OBJDIR)

TARGETS_lib = $(filter lib%,$(TARGETS))
TARGETS_bin = $(filter-out lib%,$(TARGETS))

################################################################################
# Build recipes
################################################################################

$(OBJDIR) :
	$(VERB)mkdir -p $(OBJDIR)

#Keep intermediate pcap C-files
.PRECIOUS : $(OBJDIR)/%_pcap.c

$(OBJDIR)/%_pcap.o : $(OBJDIR)/%_pcap.c
	@echo "--- Compiling $<"
	$(VERB)$(CC) $(CCFLAGS) $(CCOUT) $<

$(OBJDIR)/%_pcap.c : %.pcap
	@echo "--- Generating $@"
	$(VERB)$(BIN2C) -n $(notdir $(basename $@)) -o $@ $<

$(OBJDIR)/%.o : %.cc
	@echo "--- Compiling $<"
	$(VERB)$(CXX) $(CXXFLAGS) $(CCFLAGS) $(CCFLAGS_$(basename $<)) $(CCOUT) $<

$(OBJDIR)/%.o : %.c
	@echo "--- Compiling $<"
	$(VERB)$(CC) $(CCFLAGS) $(CCFLAGS_$(basename $<)) $(CCOUT) $<

$(OBJDIR)/%.o : %.s
	@echo "--- Compiling $<"
	$(VERB)$(AS) $(ASFLAGS) $(ASONLYFLAGS) $(ASOUT) $<

$(OBJDIR)/%.o : %.S
	@echo "--- Compiling $<"
	$(VERB)$(CC) $(CCFLAGS) $(addprefix $(ASPREFIX),$(ASFLAGS)) $(CCOUT) $<

$(TARGETS_lib) :
	@echo "--- Archiving $@ from $(OBJECTS_$@)"
	$(VERB)$(AR) $(ARFLAGS) $(AROUT) $(addprefix $(OBJDIR)/,$(OBJECTS_$@))

$(TARGETS_bin) :
	@echo "--- Linking $@ from $(OBJECTS_$@) $(LIBS)"
	$(VERB)$(LD) $(LDFLAGS) $(LDOUT) $(addprefix $(OBJDIR)/,$(OBJECTS_$@)) $(GROUPSTART) $(LIBS) $(GROUPEND) $(LDMAP)

################################################################################
# Include generated dependencies
################################################################################

-include $(patsubst %.o,%.d,$(OBJECTS))
# DO NOT DELETE
