
all: ompinfo.so for-test.pdf

GCC         = gcc-4.6
PLUGIN_DIR  = $(shell $(GCC) -print-file-name=plugin)
INC         = -I$(PLUGIN_DIR)/include
CFLAGS      = $(INC) -fPIC -Wall -ggdb -O2
#CFLAGS     += $(shell pkg-config --cflags --libs libgraph) -lgvc

ompinfo.so: gcc-ompinfo.c
	$(GCC) $(CFLAGS) -lgraph -shared $^ -o $@

for-test.dot: for-test.c ompinfo.so
	$(GCC) -fplugin=./ompinfo.so \
	       -fplugin-arg-ompinfo-output=$(patsubst %.dot,%,$@) \
	       -fopenmp -Wall -S $< #-fdump-tree-all

%.pdf: %.dot
	dot -Tpdf $^ -o $@

clean:
	rm -f ompinfo.so for-test.s for-test.dot for-test.pdf
