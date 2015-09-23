
#ifdef __MACH__

#include <mach/mach_time.h>

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 0

int clock_gettime(int clk_id, struct timespec *t){
	struct timeval tm;
    	int result;

	result = gettimeofday (&tm, NULL);
	
	t->tv_sec = tm.tv_sec;
	t->tv_nsec = tm.tv_usec * 1000;

	return result;
}
#else

#include <time.h>

#endif

#define BUFFER_SIZE 720 //Gives 30sec at 24FPS

typedef struct {
	unsigned int frame_count;
	unsigned int packet_order;
	unsigned int packet_size;
	
	struct timespec frameTimeVal;
	
	unsigned int frame_height;
	unsigned int frame_width;
	unsigned int frame_bitDepth;
	unsigned int frame_channels;
	unsigned int frame_size;
} frameHeader_t ;

typedef struct {
	frameHeader_t frameHeader;
	char *frame_data;
} frame_t ;

typedef struct {
        struct timespec frameTimeVal;
        unsigned int frame_count;
} UDP_PTSframe_t ;

//OBS: can be dynamicaly alocated
typedef struct {
        frame_t frameBuffer[BUFFER_SIZE];
        unsigned int consumerPosition;
        unsigned int producerPosition;
} frameBuffer_t ;

// pthread_mutex_t bufferLock = PTHREAD_MUTEX_INITIALIZER;

long double getSystemTime (struct timespec *timeS) {
    struct timespec *timeValue;

    if (timeS == NULL) {
        timeValue = malloc (sizeof(struct timespec));
    
        if (clock_gettime(CLOCK_REALTIME, timeValue) != 0 ) {
                printf ("SYSTEM TIME NOT SET. getSystemTime() failed\n");
                return -1;
        }

        return timeValue->tv_sec + (timeValue->tv_nsec / 1000000000.0);
    }
    else {
        if (clock_gettime(CLOCK_REALTIME, timeS) != 0 ) {
                printf ("SYSTEM TIME NOT SET. getSystemTime() failed\n");
                return -1;
        }

        return timeS->tv_sec + (timeS->tv_nsec / 1000000000.0);
    }
}

/**
 * @brief Initialize a ADT of type frameBuffer_t
 * 
 * @param buffer Buffer data structure to be initialized
 */
void initializeBuffer(frameBuffer_t *buffer){
        buffer->consumerPosition = 0;
        buffer->producerPosition = 0;
}
/**
 * @brief [brief description]
 * 
 * @param buffer The ADT where the data will be copied
 * @param newFrame The frame to be copied
 */
 void addFrameToBuffer (frameBuffer_t *buffer, frame_t *newFrame) {

    memcpy(&buffer->frameBuffer[buffer->producerPosition].frameHeader, &newFrame->frameHeader, sizeof(frameHeader_t));

    if (buffer->frameBuffer[buffer->producerPosition].frame_data == NULL) {
        buffer->frameBuffer[buffer->producerPosition].frame_data = malloc (newFrame->frameHeader.frame_size);
    }
    else {
        buffer->frameBuffer[buffer->producerPosition].frame_data = realloc (buffer->frameBuffer[buffer->producerPosition].frame_data, newFrame->frameHeader.frame_size);
    }

    memcpy (buffer->frameBuffer[buffer->producerPosition].frame_data, newFrame->frame_data, newFrame->frameHeader.frame_size);

    buffer->producerPosition++;

    if (buffer->producerPosition == BUFFER_SIZE) {
      printf ("buffer rewind %d %d\n", buffer->consumerPosition, buffer->producerPosition);
      buffer->producerPosition = 0;
    }
    if (buffer->producerPosition == buffer->consumerPosition) {
        printf ("Buffer OVERFLOW..\n");
    }
}

void waitForFrame (frameBuffer_t *buffer) {
    while (buffer->producerPosition - buffer->consumerPosition < 1);
}

frame_t* skipToValidFrame (frameBuffer_t *buffer, long unsigned int *frame_count) {
    struct timespec timeVal;
    long double localTime, frameTime;
    long double diffTime;

    frame_t *result;

    waitForFrame (buffer);

    getSystemTime(&timeVal);
    localTime = timeVal.tv_sec + ((long double)timeVal.tv_nsec / 1000000000.);
    frameTime = buffer->frameBuffer[buffer->consumerPosition].frameHeader.frameTimeVal.tv_sec + ((long double)buffer->frameBuffer[buffer->consumerPosition].frameHeader.frameTimeVal.tv_nsec / 1000000000.);
    diffTime = frameTime - localTime;
    
    //Check timing
    while (diffTime < 0.0) {
        free (buffer->frameBuffer[buffer->consumerPosition].frame_data);
        buffer->frameBuffer[buffer->consumerPosition].frame_data = NULL;

        // printf ("SKIPED FRAME %lu LT %.9Lf | FT %.9Lf | DT %.9Lf\n", *frame_count, localTime, frameTime, diffTime);
        *frame_count += 1;

        buffer->consumerPosition++;
        if (buffer->consumerPosition == BUFFER_SIZE) {
            // printf ("buffer cons %d\n", buffer->consumerPosition);
            buffer->consumerPosition = 0;
        }
        
        waitForFrame (buffer);

        getSystemTime(&timeVal);
        localTime = timeVal.tv_sec + ((long double)timeVal.tv_nsec / 1000000000.);
        frameTime = buffer->frameBuffer[buffer->consumerPosition].frameHeader.frameTimeVal.tv_sec + ((long double)buffer->frameBuffer[buffer->consumerPosition].frameHeader.frameTimeVal.tv_nsec / 1000000000.);
        diffTime = frameTime - localTime;
    }

    result = &buffer->frameBuffer[buffer->consumerPosition];

    buffer->consumerPosition++;
    if (buffer->consumerPosition == BUFFER_SIZE) {
        // printf ("buffer cons %d\n", buffer->consumerPosition);
        buffer->consumerPosition = 0;
    }

    return result;
}

/**
 * @brief Extracts a frame from the queue
 * 
 * @param buffer ADT where the queue is located
 * @return The first frame from the queue
 */
frame_t* consumeFrameFromBuffer(frameBuffer_t *buffer, frame_t *cpyFrame) {
    frame_t *result;

    if (cpyFrame == NULL) {
        result = &buffer->frameBuffer[buffer->consumerPosition++];

        if (buffer->consumerPosition == BUFFER_SIZE) {
            // printf ("buffer cons %d\n", buffer->consumerPosition);
            buffer->consumerPosition = 0;
        }

        return result;

    }
    else {
        //Copy header
        memcpy (&cpyFrame->frameHeader, &buffer->frameBuffer[buffer->consumerPosition].frameHeader, sizeof(frameHeader_t));

        //Copy data, assume that cpyFrame->frame_data is NULL
        cpyFrame->frame_data = malloc (cpyFrame->frameHeader.frame_size);
        memcpy (cpyFrame->frame_data, buffer->frameBuffer[buffer->consumerPosition].frame_data, cpyFrame->frameHeader.frame_size);

        free (buffer->frameBuffer[buffer->consumerPosition].frame_data);
        buffer->frameBuffer[buffer->consumerPosition].frame_data = NULL;

        buffer->consumerPosition++;

        if (buffer->consumerPosition == BUFFER_SIZE) {
            // printf ("buffer cons %d\n", buffer->consumerPosition);
            buffer->consumerPosition = 0;
        }

        return NULL;
    }
}

int adaptativeSleep (long double timerTime) {
 	struct timespec its;

 	its.tv_sec = (int) timerTime;
 	its.tv_nsec = (timerTime - ((int) timerTime)) * 1000000000;
 	if (nanosleep(&its, NULL) == -1) {
        //printf ("MISSED FRAME.. \n");
 		return 0;
 	}

 	return 1;
}
