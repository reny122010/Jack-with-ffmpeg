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

#define AV_OUTPUT_FORMAT "webm"
#define AV_OUTPUT_CODEC  "libvpx-vp9"

#define AV_OUTPUT_BITRATE 200000
#define AV_OUTPUT_THREADS 16
#define AV_OUTPUT_THREAD_TYPE FF_THREAD_SLICE 

#define AV_FRAMERATE 24

char _keepEncoder = 1;

//FFMPEG Input env
typedef struct {
	AVFormatContext *formatCtx;
	AVCodecContext 	*codecCtx;
	AVCodec 		*encoder;
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
    printf("./encoder [video] [line] [colum] <udp://[IP ADDR]:[PORT]>\n\n");
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
	
	int quadrant_start = 2;

	int amount_of_quadrants = argc - 4;
	int quadrant_line, quadrant_column;
	
	char *videoFileName = argv[1];
	char quadFileNames[amount_of_quadrants][30];

	int i = 0, k, j;

	long unsigned int inc = 0;

	int videoStreamIndex;
	int videoOutputStreamIndex;
	int frameFinished, gotPacket;

	ff_input_t 	ff_input;
	ff_output_t ff_output[amount_of_quadrants];
	UDP_PTSframe_t PTS_frame;

	struct tm *start_time_tm;
	char start_time_str[64];
	long unsigned int start_time;
	time_t start_timer_t;
	
	//Crop env
	int tam_quad = sqrt(amount_of_quadrants);
	int frist = 1, marginLeft = 0, marginTop = 0;
	int width , height;

    if(argc < 5){
        usage();    
        return -1;
    }

    quadrant_line = atoi(argv[2]);
    quadrant_column = atoi(argv[3]);
 
    if((quadrant_line * quadrant_column) != amount_of_quadrants){
        printf("It's missing or exceeded the amount of OUTPUTs, check them!\n");
        return -1;
    }
 
    for (i = 0; i < amount_of_quadrants; i++)
        strcpy (quadFileNames[i], argv[i + 4]);

	av_register_all();
	avformat_network_init();

	//Initialize Input
	if (avformat_open_input (&ff_input.formatCtx, videoFileName, NULL, NULL) != 0) {
		printf ("Cold not open input video file at %s\n", videoFileName);
		return -1;
	}

	if (avformat_find_stream_info(ff_input.formatCtx, NULL) < 0) {
		printf ("Cold not get stream info\n");
		return -1;
	}

	for (i = 0; i < ff_input.formatCtx->nb_streams; i++)
		av_dump_format(ff_input.formatCtx, i, videoFileName, 0);

	videoStreamIndex = av_find_best_stream(ff_input.formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &ff_input.encoder, 0);
	if (videoStreamIndex < 0) {
		printf ("no video streams found\n");
		return -1;
	}

	ff_input.codecCtx = ff_input.formatCtx->streams[videoStreamIndex]->codec;

	if (avcodec_open2 (ff_input.codecCtx, ff_input.encoder, NULL) < 0) {
		printf ("Could not open input codec\n");
		return -1;
	}

	//Get system time and append as metadata
	getSystemTime (&PTS_frame.frameTimeVal); //Must be the same for all output contexts
	start_time = PTS_frame.frameTimeVal.tv_sec;
	start_timer_t = (time_t) start_time;
	start_time_tm = localtime (&start_timer_t);
	strftime(start_time_str, sizeof start_time_str, "%Y-%m-%d %H:%M:%S", start_time_tm);

	//Initialize Output
	for (i = 0; i < amount_of_quadrants; i++) {
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
		ff_output[i].encoder = avcodec_find_encoder_by_name (AV_OUTPUT_CODEC);
		if (ff_output[i].encoder == NULL) {
			printf ("Codec %s not found..\n", AV_OUTPUT_CODEC);
			return -1;
		}

		//Sliced sizes
		width = ff_input.codecCtx->width/quadrant_column;
        height = ff_input.codecCtx->height/quadrant_line;

		ff_output[i].codecCtx->codec_type 	= AVMEDIA_TYPE_VIDEO;
		ff_output[i].codecCtx->height 		= height;
		ff_output[i].codecCtx->width 		= width;
		ff_output[i].codecCtx->pix_fmt		= ff_input.codecCtx->pix_fmt;

		//Maintain input aspect ratio for codec and stream info, and b_frames for codec info
		ff_output[i].codecCtx->sample_aspect_ratio = ff_input.codecCtx->sample_aspect_ratio;
		ff_output[i].codecCtx->max_b_frames = ff_input.codecCtx->max_b_frames;
		ff_output[i].outStream->sample_aspect_ratio = ff_output[i].codecCtx->sample_aspect_ratio;
		
		//Set custom BIT RATE and THREADs 
		ff_output[i].codecCtx->bit_rate 	= AV_OUTPUT_BITRATE;
		ff_output[i].codecCtx->thread_count = AV_OUTPUT_THREADS;
		ff_output[i].codecCtx->thread_type  = AV_OUTPUT_THREAD_TYPE;

		//Set custo timebase for codec and streams
		ff_output[i].codecCtx->time_base.num = 1;
		ff_output[i].codecCtx->time_base.den = AV_FRAMERATE;
		ff_output[i].outStream->time_base.num = 1;
		ff_output[i].outStream->time_base.den = 90000;

		//ff_output[i].codecCtx->profile = FF_PROFILE_H264_BASELINE;

		//Set realtimestamps as metadata
		//TODO: This only works for WEBM format, must be format independent
		ff_output[i].formatCtx->start_time_realtime = start_time;
		av_dict_set (&ff_output[i].formatCtx->metadata, "creation_time", start_time_str, 0);

		//Open codec
		if (avcodec_open2(ff_output[i].codecCtx, ff_output[i].encoder, NULL)) {
			printf ("Could not open output codec...\n");
			return -1;
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


	printf("Generating video streams...\n");
	while(av_read_frame (ff_input.formatCtx, &ff_input.packet) >= 0 && _keepEncoder) {

		if (ff_input.packet.stream_index == videoStreamIndex) {

			ff_input.frame = av_frame_alloc();
			avcodec_decode_video2 (ff_input.codecCtx, ff_input.frame, &frameFinished, &ff_input.packet);

			if (frameFinished) {
				//TODO: Slice inputFrame and fill avQuadFrames[quadrant]
				//By now, inputFrame are replicated to all quadrants

				ff_input.frame->pts = av_frame_get_best_effort_timestamp (ff_input.frame);
				
				i = 0;
				for ( k = 0; k < quadrant_line; ++k) {
                    for (j = 0; j < quadrant_column; ++j) {
            			ff_output[i].frame = av_frame_alloc();

            			//make the cut quadrant ff_output[i]!
            			av_picture_crop((AVPicture *)ff_output[i].frame, (AVPicture *)ff_input.frame,       
            							ff_input.formatCtx->streams[videoStreamIndex]->codec->pix_fmt, marginTop, marginLeft);
            			
            			ff_output[i].frame->width = width; // updates the new width
						ff_output[i].frame->height = height; // updates the new height
						ff_output[i].frame->format = ff_input.frame->format;

						ff_output[i].frame->pts = start_time + inc++;

						ff_output[i].packet.data = NULL;
						ff_output[i].packet.size = 0;
						av_init_packet (&ff_output[i].packet);

						avcodec_encode_video2 (ff_output[i].codecCtx, &ff_output[i].packet, ff_output[i].frame, &gotPacket);

						ff_output[i].packet.stream_index = videoOutputStreamIndex;
						av_packet_rescale_ts (&ff_output[i].packet,
										 ff_output[i].codecCtx->time_base,
										 ff_output[i].formatCtx->streams[videoOutputStreamIndex]->time_base);						

						if (gotPacket) {
							// printf ("frame writen %d\n", i);
							if (av_write_frame (ff_output[i].formatCtx, &ff_output[i].packet) < 0) {
								printf ("Unable to write to output stream..\n");
								pthread_exit(NULL);
							}
						}

						i++;
						marginLeft += width;	

            		}
            		marginLeft = 0;
            		marginTop += height;
            	}
            	marginTop = 0; 
            	i = 0;
			}
			av_frame_free (&ff_input.frame);
		}
	}

	return 0;
}
