#include <stdio.h>
#include <stdlib.h>    
#include <math.h>
#include <unistd.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>

#include <assert.h>
#include <inttypes.h>
#include "common.h"

#define TO_AUDIO_BUFFER_SIZE 1990656*sizeof(jack_sample_t)
#define TO_JACK_BUFFER_SIZE 2048*sizeof(jack_sample_t)
#define AUDIO_PROCESS_BLOCK_SIZE 256*sizeof(jack_sample_t)
#define READ_INPUT_FRAME_RATE 31.0

//#define _DBG_PTS

typedef struct {
	long double samplePTS;
} audio_sync_sample_t;

typedef struct {
	int numBuffers;

	// These two ringbuffers above must walk togheter
	jack_ringbuffer_t **to_audio_buffers; //Refer to audio data
	jack_ringbuffer_t **sync_buffers;  //Refer to audio_sync_sample_t data
} audioBuffer_t ;

audioBuffer_t to_audio_buffer;
audioBuffer_t to_jack_buffer;

typedef struct {
	jack_client_t *client;
	jack_nframes_t nframes;
	jack_port_t **ports;
	int nOutputPorts;

} jack_ctx_t;

long unsigned int start_time;

typedef struct {
	AVFormatContext *avInputCtx;
	AVCodec *avCodec;
	AVCodecContext *avCodecCtx;
	AVPacket avPacket;
	AVDictionary *avDic;
	AVDictionaryEntry *avDicentry;
	AVFrame *avFrame;
	int audioIndexStream;
	char *udp_address;
	long double ptsDelay;
} ff_ctx_t ;

jack_ctx_t jackCtx;
ff_ctx_t *ff_ctx;

typedef jack_default_audio_sample_t jack_sample_t;

void InitBF(int num_channels, audioBuffer_t *buffer, int size){
	int i;

	buffer->numBuffers = num_channels;
	buffer->to_audio_buffers = malloc (num_channels*sizeof(jack_ringbuffer_t*));
	buffer->sync_buffers = malloc (num_channels*sizeof(jack_ringbuffer_t*));

	for (i = 0; i < num_channels; ++i){
		buffer->to_audio_buffers[i] = jack_ringbuffer_create(size);
		buffer->sync_buffers[i] = jack_ringbuffer_create(size);
	}
}

void ProduceSyncToBuffer(audioBuffer_t *buffer, int channel, uint8_t *data, int data_size) {
	if(buffer != NULL){
		jack_ringbuffer_write (buffer->sync_buffers[channel], (char*) data, data_size);
		return;
	}
	return;
}

int PeekSyncFromBuffer (audioBuffer_t *buffer, int channel, uint8_t *data_out, int data_size) {
	if(buffer != NULL) {
		jack_ringbuffer_peek(buffer->sync_buffers[channel], (char*) data_out, data_size);
		return data_size;
	}
	return -1;
}

int ConsumeSyncFromBuffer(audioBuffer_t *buffer, int channel, uint8_t *data_out, int data_size) {
	if(buffer != NULL) {
		jack_ringbuffer_read(buffer->sync_buffers[channel], (char*) data_out, data_size);
		return data_size;
	}
	return -1;
}

void ProduceAudioToBuffer(audioBuffer_t *buffer, int channel, uint8_t *data, int data_size) {
	if(buffer != NULL){
		jack_ringbuffer_write (buffer->to_audio_buffers[channel], (char*) data, data_size);
		return;
	}
	return;
}

int ConsumeAudioFromBuffer(audioBuffer_t *buffer, int channel, uint8_t *data_out, int data_size) {
	if(buffer != NULL) {
		jack_ringbuffer_read(buffer->to_audio_buffers[channel], (char*) data_out, data_size);	
		return data_size;
	}
	return -1;
}

void InitFF(ff_ctx_t *file, char * ip, char *pts){
	file->avInputCtx = avformat_alloc_context();
	file->avCodec = NULL;
	file->avCodecCtx = NULL;
	file->avDic = NULL;
	file->avDicentry = NULL;
	file->avFrame = NULL;

	file->udp_address = ip;
	file->ptsDelay = atof(pts);
}

int jack_process(jack_nframes_t nframes, void *notused){
	int	i;
    jackCtx.nframes = nframes;
    jack_sample_t *buffers[jackCtx.nOutputPorts];
    int bytes_in_buffer;

    for (i = 0; i < jackCtx.nOutputPorts; i++){
    	buffers[i] = (jack_sample_t *) jack_port_get_buffer (jackCtx.ports[i], nframes);
    }

    for (i = 0; i < jackCtx.nOutputPorts; i++){
    	bytes_in_buffer = jack_ringbuffer_read_space (to_jack_buffer.to_audio_buffers[i]);
    	if (bytes_in_buffer < nframes*sizeof(jack_sample_t))
    		continue;

    	printf ("Jack process, bytes in buffer = %d\n", bytes_in_buffer);
		ConsumeAudioFromBuffer (&to_jack_buffer, i, (uint8_t*) buffers[i], nframes*sizeof(jack_sample_t));
    }

	return 0;
}

#define OUTPUTPORTNAME "out%d"

int init_jack(jack_ctx_t *ctx, int outputPorts)	{
	int i;
	char outputPortName[30];
        
	if ((ctx->client = jack_client_open("FogoAudio", JackNullOption, NULL)) == 0){
		return 1;
	}

	ctx->nOutputPorts = outputPorts;
			
	jack_set_process_callback (ctx->client, jack_process, 0);
    ctx->ports = malloc (outputPorts * sizeof(jack_port_t*));

    for (i = 0; i < outputPorts; i++ ) {
    	sprintf (outputPortName, OUTPUTPORTNAME, i);
    	ctx->ports[i] = jack_port_register (ctx->client, outputPortName, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    }

	if (jack_activate (ctx->client)){
		return 2;
	}

	return 0;
}


int _keep_audioThread = 1;

typedef struct {
	int channel;
	int process_block_size;
} threadArgs_t ;

void *audioThreadFunction (void *threadArgs) {
	threadArgs_t args;
	memcpy (&args, (char*) threadArgs, sizeof(args));

	int channel = args.channel;
	int process_block_size = args.process_block_size;

	int i;
	int sample_rate = ff_ctx->avCodecCtx->sample_rate;
	
	audio_sync_sample_t sync_sample[process_block_size];
	jack_sample_t 		sample_data[process_block_size];

	long double nowTime, endTime, diffTime;

	printf ("audioThread initialized for channel %d, with process size %d\n", channel, process_block_size );

	int bytes_in_buffer;

	while(_keep_audioThread) {

		bytes_in_buffer = jack_ringbuffer_read_space (to_audio_buffer.sync_buffers[channel]);
		if ( bytes_in_buffer <= process_block_size*sizeof(audio_sync_sample_t))
			continue;

		ConsumeSyncFromBuffer(&to_audio_buffer, channel, (uint8_t*) sync_sample, process_block_size*sizeof(audio_sync_sample_t));

		nowTime = getSystemTime(NULL);
	    diffTime = sync_sample[0].samplePTS - nowTime;

	    if (diffTime > 0.0) {
	    	printf ("bytes in buffer %d - difftime %Lf\n", bytes_in_buffer, diffTime);
	    	adaptativeSleep (diffTime);
	    }
	    else {
	    	printf ("DISCARDED BLOCK difftime %Lf\n", diffTime);
	    	ConsumeAudioFromBuffer(&to_audio_buffer, channel, (uint8_t*) sample_data, process_block_size*sizeof(jack_sample_t)); //Discard sample
	    	continue;
	    }

	    ConsumeAudioFromBuffer(&to_audio_buffer, channel, (uint8_t*) sample_data, process_block_size*sizeof(jack_sample_t));
	    ProduceAudioToBuffer(&to_jack_buffer, channel, (uint8_t*) sample_data, process_block_size*sizeof(jack_sample_t));
	}
}

int main(int argc, char **argv){	
	struct 				tm start_time_tm;
	int outputPorts;

	pthread_t *audioThreads;
	pthread_attr_t custom_sched_attr;	
	int fifo_max_prio = 0;
	int fifo_min_prio = 0;
	int fifo_mid_prio = 0;	
	struct sched_param fifo_param;

	if(argc < 3){
		printf("./<audio_decoder> udp://[IP]:[PORT] [ptsDelay]\n");
		return 0;
	}

	ff_ctx = malloc(sizeof(ff_ctx_t));

	av_register_all();
	avformat_network_init();

	InitFF(ff_ctx, argv[1], argv[2]);
	

	if (avformat_open_input (&ff_ctx->avInputCtx, ff_ctx->udp_address, NULL , &ff_ctx->avDic) != 0) {
		printf ("Cloud not open UDP input stream at %s\n", ff_ctx->udp_address);
		return -1;
	}

	if (avformat_find_stream_info(ff_ctx->avInputCtx, NULL) < 0) {
		printf ("Cloud not get stream info\n");
		return -1;
	}

	if (ff_ctx->audioIndexStream = av_find_best_stream(ff_ctx->avInputCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &ff_ctx->avCodec, 0) < 0) {
		printf ("No audio streams found\n");
		return -1;
	}

	printf ("Audio stream found at %d\n", ff_ctx->audioIndexStream);

	ff_ctx->avDicentry = av_dict_get(ff_ctx->avInputCtx->metadata, "service_name", NULL, 0);

	if(ff_ctx->avDicentry != NULL){
		strptime( ff_ctx->avDicentry->value, "%Y-%m-%d %H:%M:%S", &start_time_tm);
		start_time = mktime(&start_time_tm);
	}
	else {
		start_time = getSystemTime(NULL);
	}
	
	ff_ctx->avCodecCtx = ff_ctx->avInputCtx->streams[ff_ctx->audioIndexStream]->codec;
	ff_ctx->avCodec = avcodec_find_decoder(ff_ctx->avCodecCtx->codec_id);

	av_dump_format(ff_ctx->avInputCtx, 0, ff_ctx->udp_address, 0);

	if (avcodec_open2 (ff_ctx->avCodecCtx, ff_ctx->avCodec, NULL) < 0) {
		return -1;
	}

	outputPorts = ff_ctx->avCodecCtx->channels;
	InitBF(ff_ctx->avCodecCtx->channels, &to_audio_buffer, TO_AUDIO_BUFFER_SIZE);
	InitBF(ff_ctx->avCodecCtx->channels, &to_jack_buffer, TO_JACK_BUFFER_SIZE);

	//One thread for each channel
	audioThreads = malloc (sizeof(pthread_t)*outputPorts);

	pthread_attr_init(&custom_sched_attr);	
 	pthread_attr_setinheritsched(&custom_sched_attr, PTHREAD_INHERIT_SCHED /* PTHREAD_EXPLICIT_SCHED */);

 	//Options below only are applied when PTHREAD_EXPLICIT_SCHED is used!
 	pthread_attr_setscope(&custom_sched_attr, PTHREAD_SCOPE_SYSTEM );	
 	pthread_attr_setschedpolicy(&custom_sched_attr, SCHED_FIFO);	

 	fifo_max_prio = sched_get_priority_max(SCHED_FIFO);	
 	fifo_min_prio = sched_get_priority_min(SCHED_FIFO);	
 	fifo_mid_prio = (fifo_min_prio + fifo_max_prio) / 2;	
 	fifo_param.sched_priority = fifo_mid_prio;	
 	pthread_attr_setschedparam(&custom_sched_attr, &fifo_param);

 	int i;
 	threadArgs_t args[outputPorts];
 	for (i = 0; i < outputPorts; i++) {
 		args[i].channel = i;
 		args[i].process_block_size = AUDIO_PROCESS_BLOCK_SIZE;
 		if (pthread_create(&audioThreads[i], &custom_sched_attr, audioThreadFunction, &args[i])) {
 			printf ("Unable to create audio_thread %d\n", i);
 			return 0;
 		}
 	}
    
    av_init_packet(&ff_ctx->avPacket);

	static AVFrame frame;
	int frameFinished;
	int nb, ch;

	char samplebuf[30];
	av_get_sample_fmt_string (samplebuf, 30, ff_ctx->avCodecCtx->sample_fmt);
	printf ("Audio sample format is %s\n", samplebuf);

	audio_sync_sample_t **sync_samples;
	sync_samples = malloc (outputPorts*sizeof(audio_sync_sample_t*));

	long double initPTS, PTS, frame_pts_offset;
	unsigned long int frame_count, framePTS, sample_count;

	int sample_rate = ff_ctx->avCodecCtx->sample_rate;

	if (init_jack(&jackCtx, outputPorts)) {
		return 1;
	}

	while(av_read_frame (ff_ctx->avInputCtx, &ff_ctx->avPacket)>=0) {

		if(ff_ctx->avPacket.stream_index == ff_ctx->audioIndexStream ) {
			int contador = 0;
			long double time_1 = getSystemTime(NULL);

			int len = avcodec_decode_audio4 (ff_ctx->avCodecCtx, &frame, &frameFinished, &ff_ctx->avPacket);

			if (frameFinished) {
				int data_size = frame.nb_samples * av_get_bytes_per_sample(frame.format);
				int sync_size = frame.nb_samples * sizeof (audio_sync_sample_t);

				framePTS = av_frame_get_best_effort_timestamp (&frame);

				frame_count = framePTS - ff_ctx->avInputCtx->streams[ff_ctx->audioIndexStream]->start_time;
				frame_pts_offset = frame_count * av_q2d(ff_ctx->avInputCtx->streams[ff_ctx->audioIndexStream]->time_base) ; //frame_count * (timebase.num/timebase.den)

				initPTS = start_time + frame_pts_offset + ff_ctx->ptsDelay;

#ifdef _DBG_PTS
				printf ("frame decoded PTS %lu, frame count %lu, TB %d/%d, PTS %Lf\n", framePTS, frame_count, ff_ctx->avInputCtx->streams[ff_ctx->audioIndexStream]->time_base.num, ff_ctx->avInputCtx->streams[ff_ctx->audioIndexStream]->time_base.den, initPTS);
#endif

				//Build sync info data, sample timing
				for (ch = 0; ch < ff_ctx->avCodecCtx->channels; ch++) {
					sync_samples[ch] =  malloc(sync_size);

					PTS = initPTS;

					for (sample_count = 0; sample_count < frame.nb_samples; sample_count++) {
						PTS += (1/(float) sample_rate);
						sync_samples[ch][sample_count].samplePTS = PTS;
					}
				}

#ifdef _DBG_PTS
				printf ("ended samples PTS %Lf\n", PTS);
#endif
				for (ch = 0; ch < ff_ctx->avCodecCtx->channels; ch++) {
					ProduceSyncToBuffer (&to_audio_buffer, ch, (uint8_t*) sync_samples[ch], sync_size);
					ProduceAudioToBuffer(&to_audio_buffer, ch, (uint8_t*) frame.extended_data[ch], data_size);

					free(sync_samples[ch]);
				}
			}

	       	long double time_2 = getSystemTime(NULL);
	       	adaptativeSleep( (1/READ_INPUT_FRAME_RATE) - (time_2 - time_1));
		}
	}
}