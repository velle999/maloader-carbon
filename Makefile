VERSION=0.4
# The target is Halo's i386 slice on an i686 box.
BITS=32

GCC_EXTRA_FLAGS=-m$(BITS)
GCCFLAGS+=-g -Iinclude -Wall -MMD -fno-omit-frame-pointer -O $(GCC_EXTRA_FLAGS)
ifeq ($(USE_LIBCXX), 1)
GCCFLAGS+=-stdlib=libc++ -DUSE_LIBCXX
CXX_LDFLAGS+=-lc++ -lsupc++
CC=clang
CXX=clang++
endif
CXXFLAGS=$(GCCFLAGS) -W --std=c++11
# Darwin's off_t is 64 bits even on i386; libmac's Darwin structures and
# the offsets games pass need Linux's to match.
CFLAGS=$(GCCFLAGS) -fPIC -D_FILE_OFFSET_BITS=64 -Wno-multichar -Ithird_party/SDL2/include -D_REENTRANT

EXES=libmac.so extract macho2elf ld-mac

MAC_C_SRCS=$(wildcard mach/*.c)
MAC_CXX_SRCS=$(wildcard mach/*.cc)
MAC_C_BINS=$(MAC_C_SRCS:.c=.c.bin)
MAC_CXX_BINS=$(MAC_CXX_SRCS:.cc=.cc.bin)
MACBINS=$(MAC_C_BINS) $(MAC_CXX_BINS)
MACTXTS=$(MACBINS:.bin=.txt)

OS=$(shell uname)

ifeq ($(OS), Linux)
MAC_TOOL_DIR=/usr/i686-apple-darwin10
MAC_BIN_DIR=$(MAC_TOOL_DIR)/usr/bin
MAC_CC=PATH=$(MAC_BIN_DIR) ./ld-mac $(MAC_BIN_DIR)/gcc --sysroot=$(MAC_TOOL_DIR)
MAC_CXX=PATH=$(MAC_BIN_DIR) ./ld-mac $(MAC_BIN_DIR)/g++ --sysroot=$(MAC_TOOL_DIR)
MAC_OTOOL=./ld-mac $(MAC_BIN_DIR)/otool
MAC_TARGETS=ld-mac $(MACBINS) $(MACTXTS)
else
MAC_CC=$(CC)
MAC_CXX=$(CXX)
MAC_OTOOL=otool
MAC_TARGETS=$(MACBINS) $(MACTXTS)
endif

all: $(EXES)

profile:
	$(MAKE) clean
	$(MAKE) all GCC_EXTRA_FLAGS=-pg

release:
	$(MAKE) clean
	$(MAKE) all "GCC_EXTRA_FLAGS=-DNOLOG -DNDEBUG"

both:
	$(MAKE) clean
	$(MAKE) BITS=32 all
	mv ld-mac ld-mac32
	mv libmac.so libmac32.so
	$(MAKE) clean
	$(MAKE) BITS=64 all

mach: $(MAC_TARGETS)

check: all mach
	./runtests.sh

check-all: check
	rm -f $(MACBINS)
	MACOSX_DEPLOYMENT_TARGET=10.5 make mach
	MACOSX_DEPLOYMENT_TARGET=10.5 ./runtests.sh

$(MAC_C_BINS): %.c.bin: %.c
	$(MAC_CC) -g -arch i386 -arch x86_64 $^ -o $@

$(MAC_CXX_BINS): %.cc.bin: %.cc
	$(MAC_CXX) -g -arch i386 -arch x86_64 $^ -o $@

$(MACTXTS): %.txt: %.bin
	$(MAC_OTOOL) -hLltvV $^ > $@

#ok: macho2elf
#	./genelf.sh
#	touch $@

extract: extract.o fat.o
	$(CXX) $^ -o $@ -g -I. -W -Wall $(GCC_EXTRA_FLAGS) $(CXX_LDFLAGS)

macho2elf: macho2elf.o mach-o.o fat.o log.o
	$(CXX) $^ -o $@ -g $(GCC_EXTRA_FLAGS) $(CXX_LDFLAGS)

# -no-pie: a 32-bit kernel loads a PIE at 0x400000, inside the Mac image's
# __TEXT/__DATA, and mapping those segments would overwrite the loader.
ld-mac: ld-mac.o mach-o.o fat.o log.o
	$(CXX) $^ -o $@ -g -no-pie -ldl -lpthread $(GCC_EXTRA_FLAGS) $(CXX_LDFLAGS)

# TODO(hamaji): autotoolize?
# hle/: CoreFoundation and the rest of what Mac OS X gives the game.
HLE_OBJS=$(patsubst %.c,%.o,$(wildcard hle/*.c))
libmac.so: libmac/mac.o libmac/strmode.c $(HLE_OBJS)
	$(CC) -shared $^ $(CFLAGS) -o $@ $(GCC_EXTRA_FLAGS) $(LDFLAGS) -lpthread -lm -ldl -l:libSDL2-2.0.so.0 -l:libGL.so.1

tests/cf_test: tests/cf_test.c libmac.so hle/cf.h
	$(CC) $(CFLAGS) -o $@ tests/cf_test.c ./libmac.so -Wl,-rpath,$(CURDIR)

tests/files_test: tests/files_test.c libmac.so hle/carbon.h
	$(CC) $(CFLAGS) -o $@ tests/files_test.c ./libmac.so -Wl,-rpath,$(CURDIR)

tests/crypto_test: tests/crypto_test.c tests/crypto_vectors.h hle/crypto.c
	$(CC) $(CFLAGS) -Itests -o $@ tests/crypto_test.c hle/crypto.c

tests/arb_test: tests/arb_test.c hle/arb_program.c hle/arb_program.h
	$(CC) $(CFLAGS) -o $@ tests/arb_test.c hle/arb_program.c

tests/sound_test: tests/sound_test.c hle/sound_mix.c hle/sound_mix.h
	$(CC) $(CFLAGS) -o $@ tests/sound_test.c hle/sound_mix.c

tests/fault_test: tests/fault_test.c hle/game_faults.c hle/game_faults.h
	$(CC) $(CFLAGS) -o $@ tests/fault_test.c hle/game_faults.c

tests/profile_test: tests/profile_test.c hle/profile.c hle/profile.h
	$(CC) $(CFLAGS) -o $@ tests/profile_test.c hle/profile.c -ldl -lpthread

tests/var_ranges_test: tests/var_ranges_test.c hle/var_ranges.c hle/var_ranges.h
	$(CC) $(CFLAGS) -o $@ tests/var_ranges_test.c hle/var_ranges.c

tests/cast_test: tests/cast_test.cc hle/cxx_cast.c
	$(CC) $(CFLAGS) -c -o tests/cxx_cast.o hle/cxx_cast.c
	$(CXX) $(CXXFLAGS) -m32 -o $@ tests/cast_test.cc tests/cxx_cast.o -ldl -lpthread

tests/combine3_test: tests/combine3_test.c hle/gl_combine3.c hle/gl_combine3.h
	$(CC) $(CFLAGS) -o $@ tests/combine3_test.c hle/gl_combine3.c -ldl -lm -l:libSDL2-2.0.so.0 -l:libGL.so.1

dist:
	cd /tmp && rm -fr maloader-$(VERSION) && git clone git@github.com:shinh/maloader.git && rm -fr maloader/.git && mv maloader maloader-$(VERSION) && tar -cvzf maloader-$(VERSION).tar.gz maloader-$(VERSION)

clean:
	rm -f *.o *.d */*.o */*.d $(EXES)

-include *.d */*.d
