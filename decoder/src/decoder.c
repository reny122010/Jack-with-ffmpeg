#include <stdio.h>
#include <stdlib.h>    
#include <math.h>
#include <unistd.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <jack/jack.h>

#include <assert.h>
#include <inttypes.h>
#include "common.h"

#define AUDIO_BUFFER_SIZE 50000

typedef struct {
	struct timespec frameTimeVal;
	int data_size;
	jack_default_audio_sample_t *audio_data;
} audio_sync_sample_t;

typedef struct {
	int numElements;
	int producerPosition;
	int consumerPosition;

	audio_sync_sample_t buffer[AUDIO_BUFFER_SIZE];
} audioBuffer_t ;

typedef struct {
	jack_client_t *client;
	jack_nframes_t nframes;
	jack_port_t **ports;
	int nOutputPorts;

} jack_ctx_t;

jack_ctx_t jackCtx;

long unsigned int start_time;

typedef struct FF_File
{
	AVFormatContext *avInputCtx;
	AVCodec *avCodec;
	AVCodecContext *avCodecCtx;
	AVPacket avPacket;
	AVDictionary *avDic;
	AVDictionaryEntry *avDicentry;
	AVFrame *avFrame;
	int audioIndexStream;
	char *udp_address;
}ff_file;

audioBuffer_t **AudioBuffer;
FILE * arquivo_consumidor;

typedef jack_default_audio_sample_t jack_sample_t;

void InitBF(int channels){
	audioBuffer_t *buffer;
	int i;

	for (i = 0; i < channels; ++i){
		AudioBuffer[i] = malloc(sizeof(audioBuffer_t));
		buffer = AudioBuffer[i];
		buffer->numElements = 0;
		buffer->producerPosition = 0;
		buffer->consumerPosition = 0;
	}
	
}

void ProducerAudioToBuffer(int channels,uint16_t * data,int data_size){
	audioBuffer_t * p_buffer = AudioBuffer[channels];

	if(p_buffer != NULL){
		if(!(p_buffer->producerPosition < 0)){
			if(!(p_buffer->producerPosition == p_buffer->consumerPosition && p_buffer->numElements == AUDIO_BUFFER_SIZE-1)){
				printf("Buffer Overflow\n");
			}
				p_buffer->buffer[p_buffer->producerPosition].audio_data = (jack_default_audio_sample_t *) data;
				p_buffer->buffer[p_buffer->producerPosition].data_size = data_size;
				if(p_buffer->numElements != AUDIO_BUFFER_SIZE-1){
					p_buffer->numElements++;
				}
				p_buffer->producerPosition = (++p_buffer->producerPosition)%AUDIO_BUFFER_SIZE;
		}
		printf("Invalide position\n");		
	}
	printf("Buffer NULL\n");
}

int ConsumerAudioToBuffer(int channels, jack_default_audio_sample_t * data_out){
	audioBuffer_t * p_buffer = AudioBuffer[channels];
	
	if(p_buffer != NULL){
		if(p_buffer->numElements > 0){
			data_out = p_buffer->buffer[p_buffer->consumerPosition].audio_data;
			p_buffer->numElements--;
			p_buffer->consumerPosition = (++p_buffer->consumerPosition)%AUDIO_BUFFER_SIZE;
			return p_buffer->buffer[p_buffer->consumerPosition].data_size;
		}
		return -1;
	}
	return -1;
}

void InitFF(ff_file * file, char * ip){
	file->avInputCtx = avformat_alloc_context();
	file->avCodec = NULL;
	file->avCodecCtx = NULL;
	file->avDic = NULL;
	file->avDicentry = NULL;
	file->avFrame = NULL;

	file->udp_address = ip;

}

typedef jack_default_audio_sample_t sample_t;
typedef jack_nframes_t nframes_t;

int jack_process(nframes_t nframes, void *notused){
	int	i;
    jackCtx.nframes = nframes;
    sample_t *in;
    sample_t *buffers[jackCtx.nOutputPorts];

    for (i=0; i < jackCtx.nOutputPorts; i++){
        if (jackCtx.ports[i]==NULL){
	    	return 0;
    	}
    }
    for (i = 0; i < jackCtx.nOutputPorts; i++){
    	buffers[i] = (sample_t *) jack_port_get_buffer (jackCtx.ports[i], nframes);
    }

    nframes_t j;
    
    for (i = 0; i < jackCtx.nOutputPorts; i++){
    	int aux_i = 0;
    	for (j = 0; j < nframes; ++j){
    		if(0 < ConsumerAudioToBuffer(i, in)){
    			buffers[i][aux_i] = *in;
    			aux_i++;	
    		}    		            	
    	}    	
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


int main(int argc, char **argv){	
	struct 				tm start_time_tm;
	const char **ports1;
	int outputPorts;

	if(argc < 3){
		printf("./<audio_decoder> udp://[IP]:[PORT] [ptsDelay]\n");
		return 0;
	}

	ff_file *file;
	int data_size;

	file = malloc(sizeof(ff_file));

	av_register_all();
	avformat_network_init();

	InitFF(file, argv[1]);
	

	if (avformat_open_input (&file->avInputCtx, file->udp_address, NULL , &file->avDic) != 0) {
		printf ("Cloud not open UDP input stream at %s\n", file->udp_address);
		return -1;
	}

	if (avformat_find_stream_info(file->avInputCtx, NULL) < 0) {
		printf ("Cloud not get stream info\n");
		return -1;
	}

	if (file->audioIndexStream = av_find_best_stream(file->avInputCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &file->avCodec, 0) < 0) {
		printf ("No audio streams found\n");
		return -1;
	}

	file->avDicentry = av_dict_get(file->avInputCtx->metadata, "service_name", NULL, 0);

	if(file->avDicentry != NULL){
		strptime( file->avDicentry->value, "%Y-%m-%d %H:%M:%S", &start_time_tm);
		start_time = mktime(&start_time_tm);
	}
	
	file->avCodecCtx = file->avInputCtx->streams[file->audioIndexStream]->codec;
	file->avCodec = avcodec_find_decoder(file->avCodecCtx->codec_id);

	av_dump_format(file->avInputCtx, 0, file->udp_address, 0);

	if (avcodec_open2 (file->avCodecCtx, file->avCodec, NULL) < 0) {
		return -1;
	}

	outputPorts = file->avCodecCtx->channels;
	AudioBuffer = malloc(sizeof(AudioBuffer)*file->avCodecCtx->channels);
	InitBF(file->avCodecCtx->channels);

	if (init_jack(&jackCtx, outputPorts)){
		return 1;
	}
    
    av_init_packet(&file->avPacket);

	static AVFrame frame;
	int frameFinished;
	int nb, ch, linesize;

	while(av_read_frame (file->avInputCtx, &file->avPacket)>=0){
		if(file->avPacket.stream_index == file->audioIndexStream ){
			int len = avcodec_decode_audio4(file->avCodecCtx,&frame,&frameFinished,&file->avPacket);
            int data_size = av_samples_get_buffer_size(&linesize, file->avCodecCtx->channels, frame.nb_samples,file->avCodecCtx->sample_fmt, 1);

            for (nb = 0; nb < linesize/(sizeof(uint16_t));nb++){
	            for (ch = 0; ch < file->avCodecCtx->channels; ch++){
	               	ProducerAudioToBuffer(ch,(uint16_t *) ((uint16_t *) frame.extended_data[ch])[nb],sizeof(uint16_t));
	            }
	        }
		}
	}
}