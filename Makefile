CC:=gcc
INCLUDES:=$(shell pkg-config --cflags libavformat libavcodec libswresample libswscale libavutil sdl)
CFLAGS:=-Wall -ggdb
LDFLAGS:=$(shell pkg-config --libs libavformat libavcodec libswresample libswscale libavutil sdl) -lm
EXE:=player

#
# This is here to prevent Make from deleting secondary files.
#
.SECONDARY:
	
#
# $< is the first dependency in the dependency list
# $@ is the target name
#
all: dirs $(addprefix bin/, $(EXE)) tags

dirs:
	mkdir -p obj
	mkdir -p bin

tags: *.c
	ctags *.c

bin/%: obj/%.o
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@ -lpthread 

obj/%.o : %.c
	$(CC) $(CFLAGS) $< $(INCLUDES) -c -o $@ -lpthread 

clean:
	rm -f obj/*
	rm -f bin/*
	rmdir --ignore-fail-on-non-empty obj
	rmdir --ignore-fail-on-non-empty bin

