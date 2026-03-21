CC = gcc
INCLUDES = -I. -Icore -Ihydra -Istorage -Ibridge -Irouting

ifeq ($(OS),Windows_NT)
    CFLAGS   = -std=c11 -O2 -Wall $(INCLUDES) -DPOGLS_WINDOWS
    PTHREAD  =
    EXE      = .exe
    MKDIR    = if not exist C:\Temp\pogls_delta_test mkdir C:\Temp\pogls_delta_test
    DELTA_DIR = C:\Temp\pogls_delta_test
    RM       = del /Q
else
    CFLAGS   = -std=c11 -O2 -Wall $(INCLUDES) -D_GNU_SOURCE
    PTHREAD  = -pthread
    LDFLAGS  = -lm
    EXE      =
    MKDIR    = mkdir -p /tmp/pogls_delta_test
    DELTA_DIR = /tmp/pogls_delta_test
    RM       = rm -f
endif

OPT = -O3 -march=native -funroll-loops

.PHONY: bench test_routing test_l3 test_castle test_wire test_gpu clean all

all: bench

bench:
	$(CC) $(OPT) $(CFLAGS) bench/pogls_bench_win.c $(LDFLAGS) \
	    -o bench/batch_bench$(EXE)
	bench/batch_bench$(EXE)

test_routing:
	$(CC) $(CFLAGS) tests/pogls_adaptive_v2_test.c $(LDFLAGS) \
	    -o tests/t_routing$(EXE) && tests/t_routing$(EXE)

test_l3:
	$(CC) $(CFLAGS) tests/pogls_l3_test.c $(LDFLAGS) \
	    -o tests/t_l3$(EXE) && tests/t_l3$(EXE)

test_castle:
	$(CC) $(CFLAGS) tests/pogls_infinity_castle_test.c $(LDFLAGS) \
	    -o tests/t_castle$(EXE) && tests/t_castle$(EXE)

test_wire:
	$(CC) $(CFLAGS) -I. -Irouting -Icore -Istorage \
	    tests/test_pipeline_wire.c $(LDFLAGS) \
	    -o tests/t_wire$(EXE) && tests/t_wire$(EXE)

test_gpu:
	$(CC) $(OPT) -D_POSIX_C_SOURCE=200809L $(CFLAGS) \
	    tests/test_gpu_pipeline.c $(LDFLAGS) \
	    -o tests/t_gpu$(EXE) && tests/t_gpu$(EXE)

clean:
	$(RM) bench/batch_bench$(EXE)
	$(RM) tests/t_routing$(EXE) tests/t_l3$(EXE) tests/t_castle$(EXE)
	$(RM) tests/t_wire$(EXE) tests/t_gpu$(EXE)

test_hydra_batch:
	$(CC) $(CFLAGS) -I. tests/test_hydra_batch.c $(LDFLAGS) \
	    -o tests/t_hydra_batch$(EXE) && tests/t_hydra_batch$(EXE)
