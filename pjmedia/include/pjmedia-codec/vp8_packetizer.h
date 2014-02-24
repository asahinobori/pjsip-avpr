#ifndef __PJMEDIA_VP8_PACKETIZER_H__
#define __PJMEDIA_VP8_PACKETIZER_H__

/**
 * @file vp8_packetizer.h
 * @brief Packetizes/unpacketizes VP8 bitstream into RTP payload.
 */

#include <pj/pool.h>
#include <pj/types.h>

PJ_BEGIN_DECL

/**
 * Opaque declaration for VP8 packetizer.
 */
typedef struct pjmedia_vp8_packetizer pjmedia_vp8_packetizer;


/**
 * Enumeration of VP8 packetization modes.
 */
typedef enum
{
    /**
     * VP8 RTP packetization using Draft IETF payload-vp8-10.
     */
    PJMEDIA_VP8_PACKETIZER_MODE_DRAFTVP8,

} pjmedia_vp8_packetizer_mode;


/**
 * VP8 packetizer configuration.
 */
typedef struct pjmedia_vp8_packetizer_cfg
{
    /**
     * Maximum payload length.
     * Default: PJMEDIA_MAX_MTU
     */
    int	mtu;

    /**
     * Packetization mode.
     * Default: PJMEDIA_H263_PACKETIZER_MODE_RFC4629
     */
    pjmedia_vp8_packetizer_mode mode;

} pjmedia_vp8_packetizer_cfg;


/**
 * Create VP8 packetizer.
 *
 * @param pool		The memory pool.
 * @param cfg		Packetizer settings, if NULL, default setting
 *			will be used.
 * @param p_pktz	Pointer to receive the packetizer.
 *
 * @return		PJ_SUCCESS on success.
 */
PJ_DECL(pj_status_t) pjmedia_vp8_packetizer_create(
				    pj_pool_t *pool,
				    const pjmedia_vp8_packetizer_cfg *cfg,
				    pjmedia_vp8_packetizer **p_pktz);


/**
 * Generate an RTP payload from a VP8 picture bitstream. Note that this
 * function will apply in-place processing, so the bitstream may be modified
 * during the packetization.
 *
 * @param pktz		The packetizer.
 * @param bits		The picture bitstream to be packetized.
 * @param bits_len	The length of the bitstream.
 * @param bits_pos	The bitstream offset to be packetized.
 * @param payload	The output payload.
 * @param payload_len	The output payload length.
 *
 * @return		PJ_SUCCESS on success.
 */
PJ_DECL(pj_status_t) pjmedia_vp8_packetize(pjmedia_vp8_packetizer *pktz,
					    pj_uint8_t *bits,
                                            pj_size_t bits_len,
                                            unsigned *bits_pos,
                                            const pj_uint8_t **payload,
                                            pj_size_t *payload_len);


/**
 * Append an RTP payload to an VP8 picture bitstream. Note that in case of
 * noticing packet lost, application should keep calling this function with
 * payload pointer set to NULL, as the packetizer need to update its internal
 * state.
 *
 * @param pktz		The packetizer.
 * @param payload	The payload to be unpacketized.
 * @param payload_len	The payload length.
 * @param bits		The bitstream buffer.
 * @param bits_size	The bitstream buffer size.
 * @param bits_pos	The bitstream offset to put the unpacketized payload
 *			in the bitstream, upon return, this will be updated
 *			to the latest offset as a result of the unpacketized
 *			payload.
 *
 * @return		PJ_SUCCESS on success.
 */
PJ_DECL(pj_status_t) pjmedia_vp8_unpacketize(pjmedia_vp8_packetizer *pktz,
					      const pj_uint8_t *payload,
                                              pj_size_t payload_len,
                                              pj_uint8_t *bits,
                                              pj_size_t bits_size,
					      unsigned *bits_pos);


PJ_END_DECL


#endif	/* __PJMEDIA_VP8_PACKETIZER_H__ */