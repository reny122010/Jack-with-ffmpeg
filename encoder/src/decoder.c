/**
 * @file decoder.c
 * @author Caio Marcelo Campoy Guedes
 * @author Igor Gadelha
 * @author Renê Alves
 * @author José Raimundo
 * @date 19 may 2015
 * @brief Videocolaboration PTP test server
 */
#include <stdio.h> //C I/O
#include <stdlib.h> //Memory Manipulation
#include <unistd.h> //Sleep Precision
#include <string.h> //String Manipulation
//-------------------
//Socket Headers
//-------------------
#include <sys/socket.h> //Socket Manipulation
#include <sys/types.h> //Socket Manipulation
#include <netinet/in.h> //Socket Manipulation
#include <arpa/inet.h> //Socket Manipulation
//-------------------
//Concurrency headers
//-------------------
#include <pthread.h> //Thread Manipulation
//-------------------
//Time headers
//-------------------
#include <sys/time.h> //Clock precision
#include <signal.h> //Clock precision
#include <time.h> //Clock precision
 //-------------------
//FFmpeg Headers
//-------------------
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
 //-------------------
//Opencv Headers
//-------------------
#include <cv.h>
#include <highgui.h>
//-------------------
//Common Headers
//-------------------
#include "common.h"

// #define _DBG_PTS
// #define _DBG_SLEEP

 // #define FULLSCREEN

#define SIG_NOTIFIER  	SIGRTMIN
#define UDP_FILENAME	"udp://0.0.0.0:%d"

#define AV_INPUT_FORMAT "mpegts"

#define AV_INPUT_THREADS 16
#define AV_INPUT_THREAD_TYPE FF_THREAD_SLICE 

 char _keepRenderThread = 0;
 char _renderRemainSleeping = 0;
 char _keepDecoder = 1;

 pthread_mutex_t renderThreadLock = PTHREAD_MUTEX_INITIALIZER;
 pthread_cond_t  renderThreadCond = PTHREAD_COND_INITIALIZER;

//FFmpeg Context
AVFormatContext *quadContext = NULL;
AVCodecContext 	*pCodecContext = NULL;
AVCodec 		*avCodec = NULL;
AVInputFormat	*avFormat = NULL;
AVPacket		avPacket;
AVFrame 		*avFrame = NULL;
AVPicture		avPic;
AVDictionaryEntry *dictEntry = NULL;

struct SwsContext * img_convert_ctx; 

//declared globaly
 frameBuffer_t renderBuffer;
 long double minError, maxError, avrgError;

//Utilizada para acordar a thread do render, eh chamada a cada interrupcao do timer.
 void renderTimerHandler (int sig) {
 	_renderRemainSleeping = 0;
 	pthread_cond_signal (&renderThreadCond);
 }

 void handlerToFinish (int sig) {
 	printf ("\nStoping decoder\n");
 	printf ("minError %.9Lf | maxError %.9Lf | avrgError %.9Lf\n", minError, maxError, avrgError);
 	_keepRenderThread = 0;
 	_keepDecoder = 0;
 	renderTimerHandler(0);

 	av_free_packet(&avPacket);
    av_frame_free(&avFrame);
    avcodec_close(pCodecContext);
    
    avformat_close_input(&quadContext);

 	exit(0);
 }

#ifdef __MACH__

int setTimer (long double timerTime) {
 	struct timespec its;

 	its.tv_sec = (int) timerTime;
 	its.tv_nsec = (timerTime - ((int) timerTime)) * 1000000000;

 	if (nanosleep(&its, NULL) == -1) {
        //printf ("MISSED FRAME.. \n");
 		return 0;
 	}

 	return 1;
}

#else
/**
 * @brief Sets the time when the frame will be displayed
 * 
 * @param timerid ADT with the information
 * @param sec Seconds to wait
 * @param nanoSec Nanoseconds to wait
 * @return 0 if failed
 */
 int setTimer (timer_t *timerid, long double timerTime) {
 	struct itimerspec its;

 	its.it_value.tv_sec = (int) timerTime;
 	its.it_value.tv_nsec = (timerTime - ((int) timerTime)) * 1000000000;

 	its.it_interval.tv_sec = 0;
 	its.it_interval.tv_nsec = 0;

 	if (timer_settime(*timerid, 0, &its, NULL) == -1) {
        //printf ("MISSED FRAME.. \n");
 		return 0;
 	}

 	return 1;
 }
/**
 * @brief Initialize frame initialization timer
 * 
 * @param timerid Timer information
 * @return -1 if failed
 */
 int initTimer (timer_t *timerid) {
 	struct sigevent sev;

	//SET TIMER SIGNAL
 	signal(SIG_NOTIFIER, &renderTimerHandler);

    //Create timer
 	sev.sigev_notify = SIGEV_SIGNAL;
 	sev.sigev_signo = SIG_NOTIFIER;
 	if (timer_create(CLOCK_REALTIME, &sev, timerid) < 0) {
 		printf ("timer_create () failed.. \n");
 		return -1; 
 	}

	//printf("render timer ID is 0x%lx\n", (long) *timerid);
 }
#endif
/**
 * @brief Thread function
 * 
 * @param threadArgs Thread arguments
 * @return NULL
 */
 void *renderThreadFunction (void *threadArgs) {
 	struct timespec timeValue;

 	int key = 0;

 	long unsigned int frame_counter = 0;

 	long secTimerVal;
 	long  nsecTimerVal;

 	long double localTime, frameTime, diffTime;

 	frame_t frameToRender;

 	IplImage* cvFrame;

#ifndef __MACH__
 	timer_t renderTimer;
 	initTimer (&renderTimer);
#endif

 	cvNamedWindow("FOGO - player", CV_WINDOW_OPENGL);
#ifdef FULLSCREEN
 	cvSetWindowProperty("FOGO - player", CV_WND_PROP_FULLSCREEN, CV_WINDOW_FULLSCREEN);
#endif

 	printf ("Render Running...\n");

 	while (_keepRenderThread) {

 		//Busy wait here
 		consumeFrameFromBuffer(&renderBuffer, &frameToRender);

		cvFrame = cvCreateImage(cvSize(frameToRender.frameHeader.frame_width, frameToRender.frameHeader.frame_height), frameToRender.frameHeader.frame_bitDepth, frameToRender.frameHeader.frame_channels);
		memcpy (cvFrame->imageData, frameToRender.frame_data, frameToRender.frameHeader.frame_size);
		free (frameToRender.frame_data);

		getSystemTime(&timeValue);

		localTime = timeValue.tv_sec + ((long double)timeValue.tv_nsec / 1000000000.);
		frameTime = frameToRender.frameHeader.frameTimeVal.tv_sec + ((long double)frameToRender.frameHeader.frameTimeVal.tv_nsec / 1000000000.);
		diffTime = frameTime - localTime;

#ifdef _DBG_SLEEP
		printf ("SLEEP  ***  LT %.9Lf | FT %.9Lf | DT %.9Lf\n", localTime, frameTime, diffTime);
#endif

#ifdef __MACH__
		
		//wait for frame time
		if (!setTimer (diffTime)) {
			printf ("MISSED FRAME %lu LT %.9Lf | FT %.9Lf | DT %.9Lf\n", frame_counter, localTime, frameTime, diffTime);
			frame_counter++;
			cvReleaseImage (&cvFrame);
			// free (frameToRender->frame_data);
			continue;
		}
#else

		//wait for frame time
		if (setTimer (&renderTimer, diffTime)) {
			//Put thread to wait signal conditions

			pthread_mutex_lock(&renderThreadLock);
			_renderRemainSleeping = 1;

			while (_renderRemainSleeping)
				pthread_cond_wait(&renderThreadCond, &renderThreadLock);

			pthread_mutex_unlock(&renderThreadLock);
		}
		else {
			printf ("MISSED FRAME %lu LT %.9Lf | FT %.9Lf | DT %.9Lf\n", frame_counter, localTime, frameTime, diffTime);
			frame_counter++;
			cvReleaseImage (&cvFrame);
			// free (frameToRender->frame_data);
			continue;
		}

#endif

		//TIME TO RENDER FRAME!!!

		getSystemTime(&timeValue);
		
		cvShowImage ("FOGO - player", cvFrame);
		cvWaitKey(1);

		localTime = timeValue.tv_sec + ((long double)timeValue.tv_nsec / 1000000000.);
		frameTime = frameToRender.frameHeader.frameTimeVal.tv_sec + ((long double)frameToRender.frameHeader.frameTimeVal.tv_nsec / 1000000000.);
		diffTime = frameTime - localTime;

		if (avrgError == 0) {
			avrgError = diffTime;
			minError = diffTime;
			maxError = diffTime;
		}
		else {
			avrgError = (avrgError + diffTime) / 2;

			if (diffTime > minError)
				minError = diffTime;
			if (diffTime < maxError)
				maxError = diffTime;
		}
		
		// free (frameToRender->frame_data);
		cvReleaseImage (&cvFrame);
		frame_counter++;
	}

	pthread_exit(NULL);
}

int main (int argc, char *argv[]) {
	
	char _renderThreadIsRunning = 0;

	char *avPicBuffer = NULL;

	char *UDPaddress = argv[1];

	long double ptsDelay;
	long unsigned int ptsDelay_sec, ptsDelay_nsec;

	long double PTS;
	long unsigned int PTS_sec, PTS_nsec;

	long double frame_pts_offset;

	int port;

	int videoStreamIndex, i;
	int frameFinished;

	frame_t renderFrame;

	long unsigned int start_time, framePTS;
	long unsigned int frame_count = 0;
	struct tm start_time_tm;
	struct timespec start_timespec;

	//render thread
	pthread_t renderThread;

	//received frames will be here
	frameHeader_t 	frameHeader;

	if (argc <= 2) {
		printf ("./decoder udp://[IP]:[PORT] [pts_delay in micro seconds]\n");
		return -1;
	}
	else {
		ptsDelay = atof(argv[2]);
		ptsDelay_sec = (unsigned int) ptsDelay;
		ptsDelay_nsec = (ptsDelay - ptsDelay_sec) * 1000000000;
	}

	signal (SIGTERM, handlerToFinish);
	signal (SIGINT, handlerToFinish);

	printf ("FOGO Player - Waiting stream at %s | ptsDelay %lu.%lu\n\n", argv[1], ptsDelay_sec, ptsDelay_nsec);
	av_register_all();
	avformat_network_init();

	if (avformat_open_input (&quadContext, UDPaddress, NULL , NULL) != 0) {
		printf ("Cold not open UDP input stream at %s\n", UDPaddress);
		return -1;
	}

	if (avformat_find_stream_info(quadContext, NULL) < 0) {
		printf ("Cold not get stream info\n");
		return -1;
	}

	videoStreamIndex = av_find_best_stream(quadContext, AVMEDIA_TYPE_VIDEO, -1, -1, &avCodec, 0);
	if (videoStreamIndex < 0) {
		printf ("no video streams found\n");
		return -1;
	}

	av_dump_format(quadContext, videoStreamIndex, UDPaddress, 0);

	dictEntry = av_dict_get (quadContext->metadata, "creation_time", NULL, 0);
	if (dictEntry != NULL) {
		printf ("%s : %s\n", dictEntry->key, dictEntry->value);
		
		strptime(dictEntry->value, "%Y-%m-%d %H:%M:%S", &start_time_tm);
		start_time = mktime(&start_time_tm);
	}
	else {
		//TODO: Check if there is a better way to get this timestamp reference. Problem here is that mpegts always present a service_name = program1 by default, and that
		//confuses strptime () func.
		dictEntry = av_dict_get (quadContext->programs[0]->metadata, "service_name", NULL, 0);
		if (dictEntry != NULL) {
		printf ("%s : %s\n", dictEntry->key, dictEntry->value);
		
		strptime(dictEntry->value, "%Y-%m-%d %H:%M:%S", &start_time_tm);
		start_time = mktime(&start_time_tm);
		}
		else {
			getSystemTime (&start_timespec);
			start_time = start_timespec.tv_sec;

			printf ("could not get startime from dict entry - using localtime %lu\n", start_time);
		}
	}

	pCodecContext = quadContext->streams[videoStreamIndex]->codec;
	pCodecContext->thread_count = AV_INPUT_THREADS;
	pCodecContext->thread_type = AV_INPUT_THREAD_TYPE;

	if (avcodec_open2 (pCodecContext, avCodec, NULL) < 0) {
		printf ("Could not open codec\n");
		return -1;
	}

	if (!_renderThreadIsRunning) {
		if (pthread_create(&renderThread, NULL, renderThreadFunction, NULL)) {
			printf("unable to create render thread...\n");
			return 0;
		}
		_keepRenderThread = 1;
		_renderThreadIsRunning = 1;
	}

	renderFrame.frameHeader.frame_size = avpicture_get_size(PIX_FMT_RGB24, pCodecContext->width, pCodecContext->height);

	printf("RECIVING at %s, start time %lu\n", UDPaddress, start_time);
	while(av_read_frame (quadContext, &avPacket) >= 0 && _keepDecoder) {
		if (avPacket.stream_index == videoStreamIndex) {
			//decode video frame
			avFrame = av_frame_alloc();

			avcodec_decode_video2 (pCodecContext, avFrame, &frameFinished, &avPacket);

			if (frameFinished) {


				framePTS = av_frame_get_best_effort_timestamp (avFrame);

				img_convert_ctx = sws_getContext (	quadContext->streams[videoStreamIndex]->codec->width, 
					quadContext->streams[videoStreamIndex]->codec->height, 
					quadContext->streams[videoStreamIndex]->codec->pix_fmt, 
					quadContext->streams[videoStreamIndex]->codec->width, 
					quadContext->streams[videoStreamIndex]->codec->height, 
					PIX_FMT_BGR24, SWS_BICUBIC, NULL, NULL, NULL);

				avPicBuffer = av_malloc (renderFrame.frameHeader.frame_size);
				renderFrame.frame_data = av_malloc (renderFrame.frameHeader.frame_size);

				avpicture_fill(&avPic, avPicBuffer, PIX_FMT_RGB24, quadContext->streams[videoStreamIndex]->codec->width, 
				quadContext->streams[videoStreamIndex]->codec->height);

				sws_scale(img_convert_ctx, (uint8_t const * const *) avFrame->data, avFrame->linesize, 0, quadContext->streams[videoStreamIndex]->codec->height, 
					avPic.data, avPic.linesize);

				memcpy (renderFrame.frame_data, avPic.data[0], renderFrame.frameHeader.frame_size); 

				renderFrame.frameHeader.frame_height = quadContext->streams[videoStreamIndex]->codec->height;
				renderFrame.frameHeader.frame_width = quadContext->streams[videoStreamIndex]->codec->width;
				renderFrame.frameHeader.frame_bitDepth = IPL_DEPTH_8U;
				renderFrame.frameHeader.frame_channels = 3;

				//TODO: REVIEW
				frame_count = framePTS - quadContext->streams[videoStreamIndex]->start_time;
				// frame_count = frame_count;
				frame_pts_offset = frame_count * av_q2d(quadContext->streams[videoStreamIndex]->time_base) ; //frame_count * (timebase.num/timebase.den)

				PTS = start_time + frame_pts_offset + ptsDelay;

				PTS_sec = (unsigned int) PTS;
				PTS_nsec = (PTS - PTS_sec) * 1000000000;

				renderFrame.frameHeader.frameTimeVal.tv_sec = PTS_sec;
				renderFrame.frameHeader.frameTimeVal.tv_nsec = PTS_nsec;
#ifdef _DBG_PTS
				printf ("frame decoded PTS %lu, frame count %lu, TB %d/%d, PTS %Lf\n", framePTS, frame_count, quadContext->streams[videoStreamIndex]->time_base.num, quadContext->streams[videoStreamIndex]->time_base.den, PTS);
#endif
				addFrameToBuffer (&renderBuffer, &renderFrame);

				av_frame_free (&avFrame);
				av_free(avPicBuffer);
				av_free(renderFrame.frame_data);
			}
			av_frame_free (&avFrame);
		}
		av_free_packet(&avPacket);
	}

	handlerToFinish(0);

	return 0;
}

