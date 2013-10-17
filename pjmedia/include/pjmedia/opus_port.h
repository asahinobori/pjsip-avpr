#ifndef __PJMEDIA_OPUS_PORT_H__
#define __PJMEDIA_OPUS_PORT_H__

/**
 * @file opus_port.h
 * @brief OPUS file player and writer.
 */
#include <pjmedia/port.h>
#include <opus.h>
#include <opus_types.h>
#include <opus_multistream.h>
#include <pjmedia/opus_header.h>

typedef long (*audio_read_func)(void *src, float *buffer, int samples);

typedef struct
{
    audio_read_func read_samples;
    void *readdata;
    opus_int64 total_samples_per_channel;
    int rawmode;
    int channels;
    long rate;
    int gain;
    int samplesize;
    int endianness;
    char *infilename;
    int ignorelength;
    int skip;
    int extraout;
    char *comments;
    int comments_length;
    int copy_comments;
} oe_enc_opt;

/**
 * Create a media port to record streams to a OPUS file. Note that the port
 * must be closed properly (with #pjmedia_port_destroy()) so that the OPUS
 * header can be filled with correct values (such as the file length).

 * WAV writer port supports for writing audio in uncompressed 16 bit PCM format
 * or compressed G.711 U-law/A-law format, this needs to be specified in 
 * \a flags param. (check this sentence's meaning later)

 *
 * @param pool		    Pool to create memory buffers for this port.
 * @param filename	    File name.
 * @param clock_rate	    The sampling rate.
 * @param channel_count	    Number of channels.
 * @param samples_per_frame Number of samples per frame.
 * @param bits_per_sample   Number of bits per sample (eg 16).
 * @param flags		    Port creation flags, see
 *			    #pjmedia_file_writer_option.
 * @param buff_size	    Buffer size to be allocated. If the value is 
 *			    zero or negative, the port will use default buffer
 *			    size (which is about 4KB).
 * @param p_port	    Pointer to receive the file port instance.
 *
 * @return		    PJ_SUCCESS on success.
 */
PJ_DECL(pj_status_t) pjmedia_opus_writer_port_create(pj_pool_t *pool,
						    const char *filename,
						    unsigned clock_rate,
						    unsigned channel_count,
						    unsigned samples_per_frame,
						    unsigned bits_per_sample,
						    unsigned flags,
						    pj_ssize_t buff_size,
						    pjmedia_port **p_port);


static long frame_read(void *in, float *buffer, int samples);

void setup_padder(oe_enc_opt *opt, ogg_int64_t *original_samples);
void clear_padder(oe_enc_opt *opt);
void comment_add(char **comments, int* length, char *tag, char *val);

#endif	/* __PJMEDIA_OPUS_PORT_H__ */