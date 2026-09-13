# Makefile - Build the reverse-hashing benchmark.
#
# TARGETS
#   make          Build the benchmark program.
#   make run      Build the program and then run it.
#   make clean    Remove the build files.
#
# NOTE ON BUILD OUTPUT
# All build files go in the "build" folder. The source folder stays clean.
# The object files keep the same sub-path as the source files. Thus two
# source files with the same name do not overwrite each other.
#
# NOTE ON OPTIMISATION
# The challenge rules allow up to -O3 for gcc. The rules do not allow -Ofast.
# This Makefile uses -O3. A high optimisation level gives a fair measure of
# the true speed of each algorithm.

CC      := cc
CFLAGS  := -O3 -Wall -Wextra -std=c11
LDFLAGS := -pthread

# Extra compile flags. Enable the prism SIMD path with:
#   make PRISM_SIMD=1
# The prism SIMD path is a SHA-256 reimplementation, so keep it off until the
# course staff confirms it counts as "the provided implementation".
ifeq ($(PRISM_SIMD),1)
CFLAGS += -DPRISM_SIMD
endif

# The folder for all build output.
BUILD   := build

# The name of the benchmark program. It goes in the build folder.
TARGET  := $(BUILD)/benchmark

# All source files of the project.
SOURCES := \
	src/benchmark.c \
	src/cases.c \
	src/hash.c \
	src/registry.c \
	src/solvers/linear.c \
	src/solvers/threaded.c \
	src/solvers/dynamic.c \
	src/solvers/hybrid.c \
	src/solvers/prism.c

# The object files. Each source file becomes one object file in the build
# folder. For example, "src/hash.c" becomes "build/src/hash.o".
OBJECTS := $(SOURCES:%.c=$(BUILD)/%.o)

# The default target. It builds the benchmark program.
all: $(TARGET)

# Link all object files into the benchmark program.
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(LDFLAGS)

# Compile one source file into one object file. The "mkdir" command makes
# the destination folder if it does not exist yet.
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Build and run the benchmark.
run: $(TARGET)
	./$(TARGET)

# Remove all build files. This removes the whole build folder.
clean:
	rm -rf $(BUILD)

.PHONY: all run clean