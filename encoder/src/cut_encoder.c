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

#define AV_OUTPUT_FORMAT "mpegts"
#define AV_OUTPUT_CODEC  "libx264"
#define AUDIO_OUTPUT_CODEC "aac"

#define AV_OUTPUT_BITRATE 2000000
#define AV_OUTPUT_THREADS 16
#define AV_OUTPUT_THREAD_TYPE FF_THREAD_SLICE 

#define AV_FRAMERATE 24

//FFMPEG Input env
typedef struct {
	AVFormatContext *formatCtx;
	AVCodecContext 	*codecCtx;
	AVCodec 		*encoder;
	AVCodec 		*audioencoder;	
	AVCodecContext  *audiocodecCtx;
	AVFrame 		*frame;
	AVPacket 		packet;
} ff_input_t ;

//FFMPEG output env
typedef struct {
	// AVFormatContext *formatCtx;
	AVCodecContext  *codecCtx;
	AVCodec 		*encoder;	
	AVStream 		*outStream;
	AVFrame  		*frame;
	AVProgram 		*program;
	AVPacket 		packet;
} ff_output_t ;


AVFormatContext *formatCtx;

ff_input_t 	ff_input;
ff_output_t *ff_output;

int amount_of_quadrants;
char _keepEncoder = 1;


/**
 * @brief Software usage
 */
void usage(void){
    printf("./encoder [video] [line] [colum] FILE_NAME\n\n");
}

void handlerToFinish (int sig) {
	int i;

	//close inputs
 	av_free_packet(&ff_input.packet);
    av_frame_free(&ff_input.frame);
    avcodec_close(ff_input.codecCtx);
    avformat_close_input(&ff_input.formatCtx);

    //close inputs
    // avformat_free_context(formatCtx);
    for (i = 0; i < amount_of_quadrants; i++) {
		//close inputs
	 	av_free_packet(&ff_output[i].packet);
	    av_frame_free(&ff_output[i].frame);
	    avcodec_close(ff_output[i].codecCtx);
    }

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
	int quadrant_line, quadrant_column;
	
	char *videoFileName = argv[1];
	char quadFileName[64];

	int i = 0, k, j;

	long unsigned int inc = 0;
	long unsigned int incaudio = 0;

	int videoStreamIndex;
	int audioStreamIndex= -1;
	int frameFinished, gotPacket;

	AVDictionary	*codecOptions = NULL;
	
	UDP_PTSframe_t PTS_frame;

	struct tm *start_time_tm;
	char start_time_str[64];
	long unsigned int start_time;
	time_t start_timer_t;
	
	//Crop env
	int tam_quad;
	int frist = 1, marginLeft = 0, marginTop = 0;
	int width , height;

    if(argc < 4){
        usage();    
        return -1;
    }

    signal (SIGTERM, handlerToFinish);
	signal (SIGINT, handlerToFinish);

    tam_quad = sqrt(amount_of_quadrants);
    quadrant_line = atoi(argv[2]);
    quadrant_column = atoi(argv[3]);
    amount_of_quadrants = (quadrant_line * quadrant_column) + 1;

    strcpy (quadFileName, argv[4]);

    //Allocat output streams context
    ff_output = malloc (sizeof(ff_output_t) * amount_of_quadrants);

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

	av_dump_format(ff_input.formatCtx, 0, videoFileName, 0);

	videoStreamIndex = av_find_best_stream(ff_input.formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &ff_input.encoder, 0);
	if (videoStreamIndex < 0) {
		printf ("no video streams found\n");
		return -1;
	}

	audioStreamIndex = av_find_best_stream(ff_input.formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &ff_input.audioencoder, 0);
    if (audioStreamIndex < 0) {
        printf ("no audio streams found\n");
        return -1;
    }
    printf ("VIDEO ST %d, AUDIO ST %d\n", videoStreamIndex, audioStreamIndex);

    ff_input.audiocodecCtx = ff_input.formatCtx->streams[audioStreamIndex]->codec;
	ff_input.codecCtx = ff_input.formatCtx->streams[videoStreamIndex]->codec;

	if (avcodec_open2 (ff_input.audiocodecCtx, ff_input.audioencoder, NULL) < 0) {
        printf ("Could not open input codec\n");
        return -1;
    }

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

	if (avformat_alloc_output_context2(&formatCtx, NULL, AV_OUTPUT_FORMAT, quadFileName) < 0) {
			printf ("could not create output context\n");
			return -1;
	}

	//Initialize Video Output Streams
	for (i = 0; i < amount_of_quadrants - 1; i++) {

		ff_output[i].outStream = avformat_new_stream (formatCtx, NULL);
		if (ff_output[i].outStream == NULL) {
			printf ("Could not create output stream\n");
			return -1;
		}

		ff_output[i].outStream->id = formatCtx->nb_streams - 1;

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

		if (strcmp (AV_OUTPUT_CODEC, "libvpx") == 0) {
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
			ff_output[i].outStream->time_base.den = 10000;			
		}

		if (strcmp (AV_OUTPUT_CODEC, "libx264") == 0) {
			// ff_output[i].codecCtx->profile = FF_PROFILE_H264_MAIN;
			// av_dict_set(&codecOptions, "profile","main",0);

			//Set custom BIT RATE and THREADs 
			ff_output[i].codecCtx->bit_rate 	= AV_OUTPUT_BITRATE;
			ff_output[i].codecCtx->thread_count = AV_OUTPUT_THREADS;
			ff_output[i].codecCtx->thread_type  = AV_OUTPUT_THREAD_TYPE;

			ff_output[i].codecCtx->bit_rate_tolerance = 0;
			ff_output[i].codecCtx->rc_max_rate = 0;
			ff_output[i].codecCtx->rc_buffer_size = 0;
			ff_output[i].codecCtx->gop_size = 40;
			ff_output[i].codecCtx->max_b_frames = 3;
			ff_output[i].codecCtx->b_frame_strategy = 1;
			ff_output[i].codecCtx->coder_type = 1;
			ff_output[i].codecCtx->me_cmp = 1;
			ff_output[i].codecCtx->me_range = 16;
			ff_output[i].codecCtx->qmin = 10;
			ff_output[i].codecCtx->qmax = 51;
			ff_output[i].codecCtx->scenechange_threshold = 40;
			ff_output[i].codecCtx->flags |= CODEC_FLAG_LOOP_FILTER;
			ff_output[i].codecCtx->me_method = ME_HEX;
			ff_output[i].codecCtx->me_subpel_quality = 5;
			ff_output[i].codecCtx->i_quant_factor = 0.71;
			ff_output[i].codecCtx->qcompress = 0.6;
			ff_output[i].codecCtx->max_qdiff = 4;

			//Set custo timebase for codec and streams
			ff_output[i].codecCtx->time_base.num = 1;
			ff_output[i].codecCtx->time_base.den = 24;
			ff_output[i].outStream->time_base.num = 1;
			ff_output[i].outStream->time_base.den = 90000;		
		}

		formatCtx->start_time_realtime = start_time;
		av_dict_set (&formatCtx->metadata, "service_name", start_time_str, 0);
		av_dict_set (&formatCtx->metadata, "creation_time", start_time_str, 0);

		//Open codec
		if (avcodec_open2(ff_output[i].codecCtx, ff_output[i].encoder, &codecOptions)) {
			printf ("Could not open output codec...\n");
			return -1;
		}
	}

	//Initializing Audio Output
	i = amount_of_quadrants-1; //Last stream
	ff_output[i].outStream = avformat_new_stream (formatCtx, NULL);
	if (ff_output[i].outStream == NULL) {
		printf ("Could not create output stream\n");
		return -1;
	}

	ff_output[i].outStream->id = formatCtx->nb_streams - 1;

	ff_output[i].codecCtx = ff_output[i].outStream->codec;
	ff_output[i].encoder = avcodec_find_encoder (ff_input.audiocodecCtx->codec_id);
	if (ff_output[i].encoder == NULL) {
		printf ("Codec %s not found..\n", AUDIO_OUTPUT_CODEC);
		return -1;
	}
  
    ff_output[i].codecCtx = ff_output[amount_of_quadrants-1].outStream->codec;
    ff_output[i].codecCtx->codec_id = ff_input.audiocodecCtx->codec_id;
    ff_output[i].codecCtx->codec_type = AVMEDIA_TYPE_AUDIO;
    ff_output[i].codecCtx->sample_fmt = ff_input.audiocodecCtx->sample_fmt;
    ff_output[i].codecCtx->sample_rate = ff_input.audiocodecCtx->sample_rate;
    ff_output[i].codecCtx->channel_layout = ff_input.audiocodecCtx->channel_layout;
    ff_output[i].codecCtx->channels = av_get_channel_layout_nb_channels(ff_output[amount_of_quadrants-1].codecCtx->channel_layout);
    ff_output[i].codecCtx->bit_rate = ff_input.audiocodecCtx->bit_rate;  
    ff_output[i].codecCtx->sample_aspect_ratio = ff_input.audiocodecCtx->sample_aspect_ratio;
    ff_output[i].codecCtx->max_b_frames = ff_input.audiocodecCtx->max_b_frames;
    ff_output[i].outStream->sample_aspect_ratio = ff_output[i].codecCtx->sample_aspect_ratio;

    ff_output[i].outStream->time_base.num = ff_input.formatCtx->streams[audioStreamIndex]->time_base.num;
	ff_output[i].outStream->time_base.den = ff_input.formatCtx->streams[audioStreamIndex]->time_base.den;

	ff_output[i].codecCtx->time_base.num = ff_input.audiocodecCtx->time_base.num;
	ff_output[i].codecCtx->time_base.den = ff_input.audiocodecCtx->time_base.den;

	printf("sample_rate %d\n", ff_input.audiocodecCtx->sample_rate);

	//Open codec
	if (avcodec_open2(ff_output[i].codecCtx, ff_output[i].encoder, &codecOptions)) {
		printf ("Could not open output codec...\n");
		return -1;
	}

	av_dump_format (formatCtx, 0, quadFileName, 1);

	//Open output context
	if (avio_open (&formatCtx->pb, quadFileName, AVIO_FLAG_WRITE)) {
		printf ("avio_open failed %s\n", quadFileName);
		return -1;
	}
	
	//Write format context header
	if (avformat_write_header (formatCtx, &formatCtx->metadata)) {
		printf ("fail to write outstream header\n");
		return -1;
	}

	printf ("OUTPUT TO %s, at %lu\n", quadFileName, start_time);


	incaudio = 0;
	printf("Generating video streams...\n");
	while(av_read_frame (ff_input.formatCtx, &ff_input.packet) >= 0 && _keepEncoder) {
		if (ff_input.packet.stream_index == audioStreamIndex)
		{
			av_packet_ref  (&ff_output[amount_of_quadrants-1].packet, &ff_input.packet); 
            ff_output[amount_of_quadrants-1].packet.stream_index = amount_of_quadrants-1;
            ff_output[amount_of_quadrants-1].packet.pts = incaudio;

            // printf("%lu\n", ff_output[amount_of_quadrants-1].packet.pts);
            // if(gotPacket){
            	if (av_write_frame(formatCtx, &ff_output[amount_of_quadrants-1].packet) < 0) {
	                printf ("Unable to write to output stream..\n");
	                pthread_exit(NULL);
            	// }
            }            
            incaudio += 2880;
		}

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

						ff_output[i].frame->pts = inc;

						ff_output[i].packet.data = NULL;
						ff_output[i].packet.size = 0;
						av_init_packet (&ff_output[i].packet);

						avcodec_encode_video2 (ff_output[i].codecCtx, &ff_output[i].packet, ff_output[i].frame, &gotPacket);

						if (gotPacket) {
							ff_output[i].packet.stream_index = i;
							av_packet_rescale_ts (&ff_output[i].packet,
													ff_output[i].codecCtx->time_base,
													ff_output[i].outStream->time_base);

							if (av_write_frame (formatCtx, &ff_output[i].packet) < 0) {
								printf ("Unable to write to output stream..\n");
								pthread_exit(NULL);
							}

						}

						av_frame_free (&ff_output[i].frame);	

						i++;
						marginLeft += width;	

            		}
            		marginLeft = 0;
            		marginTop += height;
            	}
            	marginTop = 0; 
            	i = 0;
            	inc++;
			}
			av_frame_free (&ff_input.frame);
		}
	}

	return 0;
}
