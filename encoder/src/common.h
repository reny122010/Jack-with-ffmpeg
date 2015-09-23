/**
 * @file common.h
 * @author Caio Marcelo Campoy Guedes
 * @author Igor Gadelha
 * @author Renê Alves
 * @author José Raimundo
 * @date 19 may 2015
 * @brief Videocolaboration PTP test server
 */

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

pthread_mutex_t bufferLock = PTHREAD_MUTEX_INITIALIZER;

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
    pthread_mutex_lock(&bufferLock);

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

    pthread_mutex_unlock(&bufferLock);
}

/**
 * @brief Extracts a frame from the queue
 * 
 * @param buffer ADT where the queue is located
 * @return The first frame from the queue
 */
 void consumeFrameFromBuffer(frameBuffer_t *buffer, frame_t *result) {

    //Espera ocupada
    while (buffer->producerPosition - buffer->consumerPosition < 1);

    //Copy header
    memcpy (&result->frameHeader, &buffer->frameBuffer[buffer->consumerPosition].frameHeader, sizeof(frameHeader_t));

    //Copy data, assume that result->frame_data is NULL
    result->frame_data = malloc (result->frameHeader.frame_size);
    memcpy (result->frame_data, buffer->frameBuffer[buffer->consumerPosition].frame_data, result->frameHeader.frame_size);

    free (buffer->frameBuffer[buffer->consumerPosition].frame_data);
    buffer->frameBuffer[buffer->consumerPosition].frame_data = NULL;

    buffer->consumerPosition++;

    if (buffer->consumerPosition == BUFFER_SIZE) {
        // printf ("buffer cons %d\n", buffer->consumerPosition);
        buffer->consumerPosition = 0;
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

