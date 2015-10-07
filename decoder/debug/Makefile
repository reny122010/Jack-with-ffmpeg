RM = rm -fv

UNAME_S := $(shell uname -s)
USER_OBJS :=

ifeq ($(UNAME_S),Darwin)

LFLAGS = -pthread -framework OpenGL
FFMFLAGS = `pkg-config --cflags libavcodec` `pkg-config --cflags libavformat` `pkg-config --cflags libavutil` `pkg-config --cflags libswscale`
LIBS = -lvpx `sdl2-config --libs` `pkg-config --libs libavformat` `pkg-config --libs libavcodec` `pkg-config --libs libavutil` `pkg-config --libs libswscale`

endif
ifeq ($(UNAME_S),Linux)

LFLAGS = -pthread -lrt -lGL
FFMFLAGS = `pkg-config --cflags libavcodec` `pkg-config --cflags libavformat` `pkg-config --cflags libavutil` `pkg-config --cflags libswscale`
LIBS = -ljack `pkg-config --libs libavformat` `pkg-config --libs libavcodec` `pkg-config --libs libavutil` `pkg-config --libs libswscale`

endif

all: decoder

decoder:
	gcc ../src/decoder.c  $(FFMFLAGS) $(LFLAGS) $(LIBS) -g -o decoder


clean:
	@echo 'Cleaning object files'
	$(RM) decoder
