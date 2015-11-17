CC=cc
CFLAGS=-DSTANDALONE -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS -DTARGET_POSIX -D_LINUX -fPIC -DPIC -D_REENTRANT -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -U_FORTIFY_SOURCE -Wall -g -DHAVE_LIBOPENMAX=2 -DOMX -DOMX_SKIP64BIT -ftree-vectorize -pipe -DUSE_EXTERNAL_OMX -DHAVE_LIBBCM_HOST -DUSE_EXTERNAL_LIBBCM_HOST -DUSE_VCHIQ_ARM -Wno-psabi -I/opt/vc/include/ -I/opt/vc/include/interface/vcos/pthreads -I/opt/vc/include/interface/vmcs_host/linux -I/opt/vc/src/hello_pi/libs/ilclient `freetype-config --cflags` `pkg-config --cflags harfbuzz fontconfig` -I/usr/include/fontconfig -g -Wno-deprecated-declarations -O3 -pipe -march=armv6zk -mfpu=vfp -mfloat-abi=hard
LDFLAGS=-g -Wl,--whole-archive -lilclient -L/opt/vc/lib/ -L/usr/local/lib -lGLESv2 -lEGL -lopenmaxil -lbcm_host -lvcos -lvchiq_arm -lpthread -lrt -L/opt/vc/src/hello_pi/libs/ilclient -Wl,--no-whole-archive -rdynamic -lavformat -lavcodec -ldl -lasound -lavutil -lm -lfdk-aac -lcrypto -lswresample -lz `freetype-config --libs` `pkg-config --libs harfbuzz fontconfig` -pipe
DEP_LIBS=/opt/vc/src/hello_pi/libs/ilclient/libilclient.a
SOURCES=stream.c hooks.c mpegts.c httplivestreaming.c state.c log.c text.c timestamp.c subtitle.c
HEADERS=hooks.h mpegts.h httplivestreaming.h state.h log.h text.h timestamp.h subtitle.h
OBJECTS=$(SOURCES:.c=.o)
EXECUTABLE=picam

all: $(SOURCES) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS) $(DEP_LIBS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

%.o: %.c $(HEADERS)
	$(CC) -c $< -o $@ $(CFLAGS)

.PHONY: clean

clean:
	rm -f $(EXECUTABLE) $(OBJECTS)
