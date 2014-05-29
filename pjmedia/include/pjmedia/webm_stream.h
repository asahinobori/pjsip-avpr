#ifndef __PJMEDIA_WEBM_STREAM_H__
#define __PJMEDIA_WEBM_STREAM_H__

/**
 * @file webm_stream.h
 * @brief WEBM file player.
 * @brief WEBM file writer.
 */
#include <pjmedia/port.h>

PJ_BEGIN_DECL

/**
 * WEBM stream data type.
 */
typedef pjmedia_port pjmedia_webm_stream;

/**
 * Opaque data type for WEBM streams. WEBM streams is a collection of
 * zero or more WEBM stream.
 */
typedef struct pjmedia_webm_streams pjmedia_webm_streams;

/**
 * Create webm streams to play an WEBM file. WebM player supports 
 * reading WEBM file with uncompressed video format and 
 * 16 bit PCM or compressed G.711 A-law/U-law audio format.
 *
 * @param pool		Pool to create the streams.
 * @param filename	File name to open.
 * @param flags		WebM streams creation flags.
 * @param p_streams	Pointer to receive the webm streams instance.
 *
 * @return		PJ_SUCCESS on success.
 */
PJ_DECL(pj_status_t)
pjmedia_webm_player_create_streams(pj_pool_t *pool,
                                  const char *filename,
                                  unsigned flags,
                                  pjmedia_webm_streams **p_streams);

/**
 * Create webm streams to write an WEBM file.
 *
 * @param pool		Pool to create the streams.
 * @param filename	File name to open.
 * @param flags		WebM streams creation flags.
 * @param p_streams	Pointer to receive the webm streams instance.
 *
 * @return		PJ_SUCCESS on success.
 */
PJ_DECL(pj_status_t)
pjmedia_webm_writer_create_streams(pj_pool_t *pool,
                                  const char *filename,
                                  unsigned flags,
                                  pjmedia_webm_streams **p_streams);

/**
 * Get the number of WEBM stream.
 *
 * @param streams	The WEBM streams.
 *
 * @return		The number of WEBM stream.
 */
PJ_DECL(unsigned)
pjmedia_webm_streams_get_num_streams(pjmedia_webm_streams *streams);

/**
 * Return the idx-th stream of the WEBM streams.
 *
 * @param streams	The WEBM streams.
 * @param idx	        The stream index.
 *
 * @return		The WEBM stream or NULL if it does not exist.
 */
PJ_DECL(pjmedia_webm_stream *)
pjmedia_webm_streams_get_stream(pjmedia_webm_streams *streams,
                               unsigned idx);

/**
 * Return an WEBM stream with a certain media type from the WEBM streams.
 *
 * @param streams	The WEBM streams.
 * @param start_idx     The starting index.
 * @param media_type    The media type of the stream.
 *
 * @return		The WEBM stream or NULL if it does not exist.
 */
PJ_DECL(pjmedia_webm_stream *)
pjmedia_webm_streams_get_stream_by_media(pjmedia_webm_streams *streams,
                                        unsigned start_idx,
                                        pjmedia_type media_type);

/**
 * Return the media port of an WEBM stream.
 *
 * @param stream	The WEBM stream.
 *
 * @return		The media port.
 */
PJ_INLINE(pjmedia_port *)
pjmedia_webm_stream_get_port(pjmedia_webm_stream *stream)
{
    return (pjmedia_port *)stream;
}

PJ_END_DECL

#endif	/* __PJMEDIA_WEBM_STREAM_H__ */