# Build one benchmark binary per compiler-flag variant.
# Override: make CC=clang NATIVE=-mcpu=native  (Apple/ARM)
CC      ?= gcc
NATIVE  ?= -march=native
BASE    := -O3 -std=gnu11 -Wall
DRIVER  := -O2 -std=gnu11 -fno-tree-vectorize -Wall

ifeq ($(findstring clang,$(shell $(CC) --version)),clang)
  NOVEC := -fno-vectorize -fno-slp-vectorize
  REPORT = -Rpass=loop-vectorize -Rpass-missed=loop-vectorize -Rpass-analysis=loop-vectorize
  W512  := -mprefer-vector-width=512
else
  NOVEC := -fno-tree-vectorize -fno-tree-slp-vectorize
  REPORT = -fopt-info-vec-optimized -fopt-info-vec-missed
  W512  := -mprefer-vector-width=512
endif

# scalar   : same ISA as native, vectorizer off (fair scalar baseline, keeps scalar FMA)
# auto     : default -O3, baseline ISA (x86-64 = SSE2, 128-bit)
# native   : CPU-targeted ISA (compiler's preferred width, often 256-bit)
# native512: force widest vectors (AVX-512 CPUs only)
# native_fm: native + -ffast-math (allows reassociation -> vectorized reductions)
FLAGS_scalar    := $(BASE) $(NATIVE) $(NOVEC)
FLAGS_auto      := $(BASE)
FLAGS_native    := $(BASE) $(NATIVE)
FLAGS_native512 := $(BASE) $(NATIVE) $(W512)
FLAGS_native_fm := $(BASE) $(NATIVE) -ffast-math

VARIANTS ?= scalar auto native native512 native_fm
BINS := $(addprefix bin/bench_,$(VARIANTS))

all: $(BINS)

bin/bench_%: src/bench.c src/kernels.c src/kernels.h
	@mkdir -p bin reports
	$(CC) $(FLAGS_$*) -c src/kernels.c -o bin/kernels_$*.o $(REPORT) 2> reports/vec_report_$*.txt || (cat reports/vec_report_$*.txt; false)
	$(CC) $(DRIVER) -DVARIANT='"$*"' src/bench.c bin/kernels_$*.o -lm -o $@
	objdump -d --no-show-raw-insn bin/kernels_$*.o > reports/disasm_$*.txt 2>/dev/null || true

clean:
	rm -rf bin reports/vec_report_* reports/disasm_*
.PHONY: all clean
