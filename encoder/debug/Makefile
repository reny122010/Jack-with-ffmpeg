RM = rm -fv

UNAME_S := $(shell uname -s)
USER_OBJS :=

ifeq ($(UNAME_S),Darwin)

LFLAGS = -pthread  
CVFLAGS = -ggdb `pkg-config --cflags opencv`
FFMFLAGS = `pkg-config --cflags libavcodec` `pkg-config --cflags libavformat` `pkg-config --cflags libavutil` `pkg-config --cflags libswscale`
LIBS = -lvpx `pkg-config --libs opencv` `pkg-config --libs libavformat` `pkg-config --libs libavcodec` `pkg-config --libs libavutil` `pkg-config --libs libswscale`

endif
ifeq ($(UNAME_S),Linux)

LFLAGS = -pthread -lrt
CVFLAGS = -ggdb `pkg-config --cflags opencv`
FFMFLAGS = `pkg-config --cflags libavcodec` `pkg-config --cflags libavformat` `pkg-config --cflags libavutil` `pkg-config --cflags libswscale`
LIBS = -lvpx `pkg-config --libs opencv` `pkg-config --libs libavformat` `pkg-config --libs libavcodec` `pkg-config --libs libavutil` `pkg-config --libs libswscale`

endif

all: encoder_onthefly decoder cut_encoder sender

encoder_onthefly:
	gcc ../src/encoder_onthefly.c $(CVFLAGS) $(FFMFLAGS) $(LFLAGS) $(LIBS) -o encoder_onthefly
cut_encoder:
	gcc ../src/cut_encoder.c $(CVFLAGS) $(FFMFLAGS) $(LFLAGS) $(LIBS) -o cut_encoder
sender:
	gcc ../src/sender.c $(CVFLAGS) $(FFMFLAGS) $(LFLAGS) $(LIBS) -o sender
decoder:
	gcc ../src/decoder.c $(CVFLAGS) $(FFMFLAGS) $(LFLAGS) $(LIBS) -o decoder 
clean:
	@echo 'Cleaning object files'
	$(RM) encoder_onthefly decoder cut_encoder sender
