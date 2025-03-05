#Copyright (c) 2015, ARM Limited. All rights reserved.
#
#SPDX-License-Identifier:        BSD-3-Clause
###############################################################################
# Makefile designed by Wonderful Void
# Supports multiple targets and auto-generated dependencies
################################################################################

UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
CLANG = yes
endif

###############################################################################
# Project specific definitions
################################################################################

#List of executable files to build
TARGETS = libprogress64.a hashtable timer rwlock rwlock_r reorder antireplay rwsync rwsync_r reassemble laxrob ringbuf clhlock mcslock lfring qsbr tfrwlock tfrwlock_r pfrwlock stack lfstack msqueue counter mbtrie buckring buckrob skiplock mcas hemlock coroutine fiber blkring linklist verify mcqueue rplock
#The following targets require pthreads and Linux support
ifeq ($(UNAME),Linux)
TARGETS += bm_ringbuf bm_smr bm_mbtrie bm_rob bm_hashtab bm_mcas bm_coroutine bm_fiber bm_lock bm_skiplock
endif
#List object files for each target
OBJECTS_libprogress64.a = p64_ringbuf.o p64_spinlock.o p64_rwlock.o p64_barrier.o p64_hazardptr.o p64_hashtable.o p64_timer.o p64_rwsync.o p64_antireplay.o p64_reorder.o p64_reassemble.o p64_laxrob.o p64_clhlock.o p64_lfring.o p64_rwsync_r.o p64_rwlock_r.o os_abstraction.o thr_idx.o p64_qsbr.o p64_tfrwlock.o p64_tfrwlock_r.o p64_tktlock.o p64_pfrwlock.o p64_semaphore.o p64_rwclhlock.o p64_stack.o p64_msqueue.o p64_counter.o p64_errhnd.o p64_mbtrie.o p64_hopscotch.o p64_buckrob.o p64_buckring.o p64_cuckooht.o p64_skiplock.o p64_mcslock.o p64_mcas.o p64_hemlock.o p64_coroutine.o p64_fiber.o p64_lfstack.o p64_blkring.o ver_lfstack.o ver_msqueue.o ver_clhlock.o ver_mcslock.o ver_blkring.o ver_hemlock.o ver_barrier.o ver_buckring1.o ver_buckring2.o ver_cuckooht1.o ver_cuckooht2.o ver_ringbuf.o ver_hopscotch1.o ver_spinlock.o
OBJECTS_libprogress64.a += p64_linklist.o ver_linklist.o
OBJECTS_libprogress64.a += p64_mcqueue.o ver_mcqueue.o
OBJECTS_libprogress64.a += p64_rplock.o ver_rplock.o
OBJECTS_rplock = rplock.o
OBJECTS_mcqueue = mcqueue.o
OBJECTS_hashtable = hashtable.o
OBJECTS_timer = timer.o
OBJECTS_rwlock = rwlock.o
OBJECTS_rwlock_r = rwlock_r.o
OBJECTS_reorder = reorder.o
OBJECTS_antireplay = antireplay.o
OBJECTS_rwsync = rwsync.o
OBJECTS_rwsync_r = rwsync_r.o
OBJECTS_reassemble = reassemble.o
OBJECTS_laxrob = laxrob.o
OBJECTS_ringbuf = ringbuf.o
OBJECTS_clhlock = clhlock.o
OBJECTS_hemlock = hemlock.o
OBJECTS_mcslock = mcslock.o
OBJECTS_lfring = lfring.o
OBJECTS_qsbr = qsbr.o
OBJECTS_tfrwlock = tfrwlock.o
OBJECTS_tfrwlock_r = tfrwlock_r.o
OBJECTS_pfrwlock = pfrwlock.o
OBJECTS_stack = stack.o
OBJECTS_msqueue = msqueue.o
OBJECTS_counter = counter.o
OBJECTS_mbtrie = mbtrie.o
OBJECTS_buckring = buckring.o
OBJECTS_buckrob = buckrob.o
OBJECTS_skiplock = skiplock.o
OBJECTS_coroutine = coroutine.o
OBJECTS_fiber = fiber.o
OBJECTS_mcas = mcas.o
OBJECTS_lfstack = lfstack.o
OBJECTS_bm_ringbuf = bm_ringbuf.o
OBJECTS_bm_smr = bm_smr.o
OBJECTS_bm_lock = bm_lock.o
OBJECTS_bm_mbtrie = bm_mbtrie.o
OBJECTS_bm_rob = bm_rob.o
OBJECTS_bm_hashtab = bm_hashtab.o
OBJECTS_bm_skiplock = bm_skiplock.o
OBJECTS_bm_mcas = bm_mcas.o
OBJECTS_bm_coroutine = bm_coroutine.o
OBJECTS_bm_fiber = bm_fiber.o
OBJECTS_blkring = blkring.o
OBJECTS_linklist = linklist.o
OBJECTS_verify = verify.o

LIBS = libprogress64.a
LIBS += -lm
DEBUG ?= 0
ASSERT ?= 0

ifeq ($(VERIFY),yes)
DEFINE += -DVERIFY
endif
ifeq ($(DEBUG),0)
CCFLAGS += -O2
else
CCFLAGS += -O0
endif
ifeq ($(ASSERT),0)
DEFINE += -DNDEBUG#disable assertions
else
CCFLAGS += -fsanitize=address -fsanitize=undefined
LDFLAGS += -fsanitize=address -fsanitize=undefined
ifneq ($(CLANG),yes)
ifneq ($(UNAME),Darwin)
LDFLAGS += -static-libasan -static-libubsan
else
#GCC on macOS seems to have only one library for all sanitizers
LDFLAGS += -static-libsan
endif
endif
endif
CCFLAGS += -std=c11
LDFLAGS += -std=c11
#Enable when compiling for Armv8.1a (make ATOMICS=yes)
ifeq ($(ATOMICS),yes)
ARCH ?= armv8.1-a
CCFLAGS += -march=$(ARCH)+lse -D__ARM_FEATURE_ATOMICS
endif
ifneq ($(ARCH),)
#GCC target architecture
CCFLAGS += -march=$(ARCH)
endif
ifneq ($(TARGET),)
#LLVM/Clang target architecture
CCFLAGS += --target=$(TARGET)
LDFLAGS += --target=$(TARGET)
endif
CCFLAGS += -g -ggdb -Wall -Wextra
CCFLAGS += -fomit-frame-pointer
ifneq ($(CLANG),yes)
CCFLAGS += -falign-loops=32 -falign-jumps=32 -falign-functions=32
endif
CCFLAGS += -fstrict-aliasing -fno-stack-check -fno-stack-protector
LDFLAGS += -g -ggdb -pthread

#Where to find the source files
VPATH += src examples benchmarks
VPATH += wip
#Where to find include files
INCLUDE += include src

#Default to non-verbose mode (echo command lines)
VERB = @

#Location of object and other derived/temporary files
OBJDIR = obj#Must not be .

###############################################################################
# Make actions (phony targets)
################################################################################

.PHONY : default all clean tags etags run

default:
	@echo "Make targets:"
	@echo "all         build all targets ($(TARGETS))"
	@echo "clean       remove derived files"
	@echo "tags        generate vi tags file"
	@echo "etags       generate emacs tags file"
	@echo "run         run all examples, tests and benchmarks"

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

run : $(TARGETS_bin)
	for cmd in $(TARGETS_bin); do echo Running $$cmd; ./$$cmd || break; done

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
LD = $(CROSS_COMPILE)clang
else
CC = $(CROSS_COMPILE)gcc
CXX = $(CROSS_COMPILE)g++
AS = $(CROSS_COMPILE)as
AR = $(CROSS_COMPILE)ar
LD = $(CROSS_COMPILE)gcc
endif
#GROUPSTART = -Wl,--start-group
#GROUPEND = -Wl,--end-group
ifneq ($(UNAME),Darwin)
ARFLAGS = -rcD
else
#Deterministic option not supported by macOS
ARFLAGS = -rc
endif
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

$(TARGETS_bin) : $(TARGETS_lib)
	@echo "--- Linking $@ from $(OBJECTS_$@) $(LIBS)"
	$(VERB)$(LD) $(LDFLAGS) $(LDOUT) $(addprefix $(OBJDIR)/,$(OBJECTS_$@)) $(GROUPSTART) $(LIBS) $(GROUPEND) $(LDMAP)

################################################################################
# Include generated dependencies
################################################################################

-include $(patsubst %.o,%.d,$(OBJECTS))
# DO NOT DELETE
