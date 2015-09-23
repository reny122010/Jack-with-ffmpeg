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
#include <ctype.h> //Type Convertion
#include <string.h> //String manipulation
#include <errno.h>
#include <signal.h> //Clock precision
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
//FFmpeg Headers
//-------------------
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
//Encoder and Decoder Common headers (frame type, frame buffer type)
//-------------------
#include "common.h"

//TODO: This can, and should be adaptative. Easy mod :)
#define AV_OUTPUT_FORMAT "mpegts"

#define OUTPUT_FRAMERATE 24.0

char _keepEncoder = 1;

//FFMPEG Input env
typedef struct {
	AVFormatContext *formatCtx;
	AVCodecContext 	*codecCtx;
	AVCodecContext 	*audiocodecCtx;
	AVCodec 		*encoder;
	AVCodec 		*audioencoder;
	AVFrame 		*frame;
	AVPacket 		packet;
} ff_input_t ;

//FFMPEG output env
typedef struct {
	AVFormatContext *formatCtx;
	AVCodecContext  *codecCtx;
	AVCodec 		*encoder;
	AVStream 		*outStream;
	AVFrame  		*frame;
	AVProgram 		*program;
	AVPacket 		packet;
	AVDictionary	*fmtOption;
} ff_output_t ;

/**
 * @brief Software usage
 */
void usage(void){
    printf("./sender [video] [N * <udp://[IP ADDR]:[PORT]>]\n\n");
}

void handlerToFinish (int sig) {
 	printf ("\nStoping sender\n");
 	exit(0);
 }

/**
 * @brief The main function
 * 
 * @param argc Amount of arguments received by the main function
 * @param argv The arguments that the main received
 *             Expects: ./Application_name <port> <1...n IP addresses>
 * 
 * @return 0 if finished correctly
 */

int main(int argc, char** argv){
	int amount_of_quadrants = argc - 2;
	
	char *videoFileName;
	char quadFileNames[amount_of_quadrants][30];

	int i = 0, k, j;

	long unsigned int inc = 0;

	int videoOutputStreamIndex;
	int frameFinished, gotPacket;

	ff_input_t 	ff_input;
	ff_output_t ff_output[amount_of_quadrants];
	UDP_PTSframe_t PTS_frame;

	struct tm *start_time_tm;
	char start_time_str[64];
	long unsigned int start_time;
	time_t start_timer_t;

	long double nowTime, lastTime, adaptativet;
	long double pktt1, pktt2, deltapkt;


	signal (SIGTERM, handlerToFinish);
	signal (SIGINT, handlerToFinish);


    if(argc != amount_of_quadrants + 2){
        usage();    
        return -1;
    }

    videoFileName = argv[1];
 
    for (i = 0; i < amount_of_quadrants; i++)
        strcpy (quadFileNames[i], argv[i + 2]);

	av_register_all();
	avformat_network_init();
	ff_input.formatCtx = avformat_alloc_context();
	//Initialize Input
	if (avformat_open_input (&ff_input.formatCtx, videoFileName, NULL, NULL) != 0) {
		printf ("Cold not open input video file at %s\n", videoFileName);
		return -1;
	}

	if (avformat_find_stream_info(ff_input.formatCtx, NULL) < 0) {
		printf ("Cold not get stream info\n");
		return -1;
	}

	if (ff_input.formatCtx->nb_streams != amount_of_quadrants) {
		printf ("Quadrant numbers do not match streams in opened file. Aborting...\n");
		return -1;
	}

	av_dump_format(ff_input.formatCtx, 0, videoFileName, 0);

	//Get system time and append as metadata
	getSystemTime (&PTS_frame.frameTimeVal); //Must be the same for all output contexts
	start_time = PTS_frame.frameTimeVal.tv_sec;
	start_timer_t = (time_t) start_time;
	start_time_tm = localtime (&start_timer_t);
	strftime(start_time_str, sizeof start_time_str, "%Y-%m-%d %H:%M:%S", start_time_tm);
	printf("1\n");
	//Initialize Output
	for (i = 0; amount_of_quadrants-1 > i; i++) {
		if (avformat_alloc_output_context2(&ff_output[i].formatCtx, NULL, AV_OUTPUT_FORMAT, quadFileNames[i]) < 0) {
			printf ("could not create output context\n");
			return -1;
		}

		ff_output[i].outStream = avformat_new_stream (ff_output[i].formatCtx, NULL);
		if (ff_output[i].outStream == NULL) {
			printf ("Could not create output stream\n");
			return -1;
		}

		videoOutputStreamIndex = ff_output[i].outStream->index;
		ff_output[i].codecCtx = ff_output[i].outStream->codec;

		ff_output[i].codecCtx->codec_type 	= AVMEDIA_TYPE_VIDEO;
		ff_output[i].codecCtx->height 		= ff_input.formatCtx->streams[i]->codec->height;
		ff_output[i].codecCtx->width 		= ff_input.formatCtx->streams[i]->codec->width;
		ff_output[i].codecCtx->pix_fmt		= ff_input.formatCtx->streams[i]->codec->pix_fmt;

		ff_output[i].codecCtx->codec_id		= ff_input.formatCtx->streams[i]->codec->codec_id;
		ff_output[i].codecCtx->bit_rate		= ff_input.formatCtx->streams[i]->codec->bit_rate;
		ff_output[i].codecCtx->bit_rate_tolerance	= ff_input.formatCtx->streams[i]->codec->bit_rate_tolerance;

		ff_output[i].outStream->start_time 	= ff_input.formatCtx->streams[i]->start_time;
		ff_output[i].outStream->duration 	= ff_input.formatCtx->streams[i]->duration;
		ff_output[i].outStream->nb_frames 	= ff_input.formatCtx->streams[i]->nb_frames;

		ff_output[i].outStream->codec->time_base.num = ff_input.formatCtx->streams[i]->codec->time_base.num;
		ff_output[i].outStream->codec->time_base.den = ff_input.formatCtx->streams[i]->codec->time_base.den;
		ff_output[i].outStream->time_base.num = ff_input.formatCtx->streams[i]->time_base.num;
		ff_output[i].outStream->time_base.den = ff_input.formatCtx->streams[i]->time_base.den;

		//Set realtimestamps as metadata
		if (strcmp (AV_OUTPUT_FORMAT, "webm") == 0) {
			//Set realtimestamps as metadata
			ff_output[i].formatCtx->start_time_realtime = start_time;
			av_dict_set (&ff_output[i].formatCtx->metadata, "creation_time", start_time_str, 0);
		}

		if (strcmp (AV_OUTPUT_FORMAT, "mpegts") == 0) {
			//Set realtimestamps as metadata
			ff_output[i].formatCtx->start_time_realtime = start_time;
			av_dict_set (&ff_output[i].formatCtx->metadata, "service_name", start_time_str, 0);
		}

		av_dump_format (ff_output[i].formatCtx, 0, quadFileNames[i], 1);

		//Open output context
		if (avio_open (&ff_output[i].formatCtx->pb, quadFileNames[i], AVIO_FLAG_WRITE)) {
			printf ("avio_open failed %s\n", quadFileNames[i]);
			return -1;
		}
	
		//Write format context header
		if (avformat_write_header (ff_output[i].formatCtx, &ff_output[i].formatCtx->metadata)) {
			printf ("fail to write outstream header\n");
			return -1;
		}

		printf ("OUTPUT TO %s, at %lu\n", quadFileNames[i], start_time);
	}
	av_find_best_stream(ff_input.formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &ff_input.audioencoder, 0);
	ff_input.audiocodecCtx = ff_input.formatCtx->streams[i]->codec;

	if (avcodec_open2 (ff_input.audiocodecCtx, ff_input.audioencoder, NULL) < 0) {
        printf ("Could not open input codec\n");
        return -1;
    }

    if (avformat_alloc_output_context2(&ff_output[i].formatCtx, NULL,"mp3" , quadFileNames[i]) < 0) {
			printf ("could not create output context\n");
			return -1;
	}
	ff_output[i].outStream = avformat_new_stream (ff_output[i].formatCtx, NULL);
    if (ff_output[i].outStream == NULL) {
        printf ("Could not create output stream\n");
        return -1;
    }
    ff_output[i].codecCtx = ff_output[i].outStream->codec;
    ff_output[i].codecCtx->codec_id = ff_input.audiocodecCtx->codec_id;
	ff_output[i].codecCtx = ff_output[i].outStream->codec;
    ff_output[i].codecCtx->codec_type = AVMEDIA_TYPE_AUDIO;
    ff_output[i].codecCtx->sample_fmt = ff_input.audiocodecCtx->sample_fmt;
    ff_output[i].codecCtx->sample_rate = ff_input.audiocodecCtx->sample_rate;
    ff_output[i].codecCtx->channel_layout = ff_input.audiocodecCtx->channel_layout;
    ff_output[i].codecCtx->channels = av_get_channel_layout_nb_channels(ff_output[i].codecCtx->channel_layout);
    ff_output[i].codecCtx->bit_rate = ff_input.audiocodecCtx->bit_rate;
    ff_output[i].codecCtx->sample_aspect_ratio = ff_input.audiocodecCtx->sample_aspect_ratio;
    ff_output[i].codecCtx->max_b_frames = ff_input.audiocodecCtx->max_b_frames;
    ff_output[i].outStream->sample_aspect_ratio = ff_output[i].codecCtx->sample_aspect_ratio;

  	av_dump_format (ff_output[i].formatCtx, 0, quadFileNames[i], 1);

  	if (strcmp (AV_OUTPUT_FORMAT, "mpegts") == 0) {
		//Set realtimestamps as metadata
		ff_output[i].formatCtx->start_time_realtime = start_time;
		av_dict_set (&ff_output[i].formatCtx->metadata, "service_name", start_time_str, 0);
	}

	//Open output context
	if (avio_open (&ff_output[i].formatCtx->pb, quadFileNames[i], AVIO_FLAG_WRITE)) {
		printf ("avio_open failed %s\n", quadFileNames[i]);
		return -1;
	}

	//Write format context header
	if (avformat_write_header (ff_output[i].formatCtx, &ff_output[i].formatCtx->metadata)) {
		printf ("fail to write outstream header\n");
		return -1;
	}

	lastTime = getSystemTime (NULL);

	printf("Sending video streams...\n");
	while(av_read_frame (ff_input.formatCtx, &ff_input.packet) >= 0 && _keepEncoder) {
		pktt1 = getSystemTime(NULL);
		if(ff_input.packet.stream_index == amount_of_quadrants-1){
			i = ff_input.packet.stream_index;

			av_packet_ref  (&ff_output[i].packet, &ff_input.packet); 
            ff_output[i].packet.stream_index = 0;
	        if (av_write_frame(ff_output[i].formatCtx, &ff_output[i].packet) < 0) {
	            printf ("Unable to write to output stream..\n");
	            pthread_exit(NULL);
	        }
		}else{

			i = ff_input.packet.stream_index;
			 av_packet_ref	(&ff_output[i].packet, &ff_input.packet); 

			 ff_output[i].packet.stream_index = videoOutputStreamIndex;

			if (av_write_frame (ff_output[i].formatCtx, &ff_output[i].packet) < 0) {
				printf ("Unable to write to output stream..\n");
				pthread_exit(NULL);
			}

			nowTime = getSystemTime (NULL);	
			adaptativet = ((1/(OUTPUT_FRAMERATE * (amount_of_quadrants-1)))-(nowTime - lastTime));

			adaptativeSleep (adaptativet);
			lastTime = getSystemTime (NULL);
		}
		
		

	}

	return 0;
}
