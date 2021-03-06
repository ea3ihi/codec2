/*---------------------------------------------------------------------------*\

  FILE........: freedv_700.c
  AUTHOR......: David Rowe
  DATE CREATED: May 2020

  Functions that implement the various FreeDV 700 modes.

\*---------------------------------------------------------------------------*/

#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "fsk.h"
#include "fmfsk.h"
#include "codec2.h"
#include "codec2_fdmdv.h"
#include "varicode.h"
#include "freedv_api.h"
#include "freedv_api_internal.h"
#include "comp_prim.h"

#include "codec2_ofdm.h"
#include "ofdm_internal.h"
#include "mpdecode_core.h"
#include "gp_interleaver.h"
#include "interldpc.h"
#include "debug_alloc.h"
#include "filter.h"

extern char *ofdm_statemode[];

void freedv_700c_open(struct freedv *f, int nbit) {
    f->snr_squelch_thresh = 0.0;
    f->squelch_en = 0;

    f->cohpsk = cohpsk_create();
    f->nin = f->nin_prev = COHPSK_NOM_SAMPLES_PER_FRAME;
    f->n_nat_modem_samples = COHPSK_NOM_SAMPLES_PER_FRAME;                       // native modem samples as used by the modem
    f->n_nom_modem_samples = f->n_nat_modem_samples * FREEDV_FS_8000 / COHPSK_FS;// number of samples after native samples are interpolated to 8000 sps
    f->n_max_modem_samples = COHPSK_MAX_SAMPLES_PER_FRAME * FREEDV_FS_8000 / COHPSK_FS + 1;
    f->modem_sample_rate = FREEDV_FS_8000;                                       // note weird sample rate tamed by resampling
    f->clip = 1;
    f->tx_bits = (int*)MALLOC(nbit*sizeof(int));
    assert(f->tx_bits != NULL);
    f->sz_error_pattern = cohpsk_error_pattern_size();
    f->test_frames_diversity = 1;

    f->ptFilter7500to8000 = (struct quisk_cfFilter *)MALLOC(sizeof(struct quisk_cfFilter));
    f->ptFilter8000to7500 = (struct quisk_cfFilter *)MALLOC(sizeof(struct quisk_cfFilter));
    quisk_filt_cfInit(f->ptFilter8000to7500, quiskFilt120t480, sizeof(quiskFilt120t480)/sizeof(float));
    quisk_filt_cfInit(f->ptFilter7500to8000, quiskFilt120t480, sizeof(quiskFilt120t480)/sizeof(float));
}

void freedv_comptx_700c(struct freedv *f, COMP mod_out[]) {
    int    bit, byte, i, j, k;
    int    bits_per_codec_frame, bits_per_modem_frame;
    int    data_flag_index, nspare;
    COMP   tx_fdm[f->n_nat_modem_samples];

    bits_per_codec_frame = codec2_bits_per_frame(f->codec2);
    bits_per_modem_frame = COHPSK_BITS_PER_FRAME;

    byte = 0;
    for (j=0; j<bits_per_modem_frame; j+=bits_per_codec_frame) {

        /* unpack bits, MSB first */

        bit = 7;
        for(i=0; i<bits_per_codec_frame; i++) {
            f->codec_bits[j+i] = (f->packed_codec_bits[byte] >> bit) & 0x1;
            bit--;
            if (bit < 0) {
                bit = 7;
                byte++;
            }
        }
	if (bit != 7)
	    byte++;

        // spare bits in frame that codec defines.  Use these spare
        // bits/frame to send txt messages

        switch(f->mode) {
        case FREEDV_MODE_700C:
            nspare = 0; // and no spare bits for 700C atm
            break;
        default:
            assert(0);
        }

        data_flag_index = codec2_get_spare_bit_index(f->codec2);

        for(k=0; k<nspare; k++) {
            if (f->nvaricode_bits) {
                f->codec_bits[j+data_flag_index+k] = f->tx_varicode_bits[f->varicode_bit_index++];
                //fprintf(stderr, "%d %d\n", j+data_flag_index+k, f->codec_bits[j+data_flag_index+k]);
                f->nvaricode_bits--;
            }
            if (f->nvaricode_bits == 0) {
                /* get new char and encode */
                char s[2];
                if (f->freedv_get_next_tx_char != NULL) {
                    s[0] = (*f->freedv_get_next_tx_char)(f->callback_state);
                    f->nvaricode_bits = varicode_encode(f->tx_varicode_bits, s, VARICODE_MAX_BITS, 1, 1);
                    f->varicode_bit_index = 0;
                }
            }
        }
    }
    /* optionally ovwerwrite the codec bits with test frames */

    if (f->test_frames) {
        cohpsk_get_test_bits(f->cohpsk, f->codec_bits);
    }

    /* cohpsk modulator */

    cohpsk_mod(f->cohpsk, tx_fdm, f->codec_bits, COHPSK_BITS_PER_FRAME);
    if (f->clip) 
        cohpsk_clip(tx_fdm, COHPSK_CLIP, COHPSK_NOM_SAMPLES_PER_FRAME);
    for(i=0; i<f->n_nat_modem_samples; i++)
        mod_out[i] = fcmult(FDMDV_SCALE*NORM_PWR_COHPSK, tx_fdm[i]);
    i = quisk_cfInterpDecim((complex float *)mod_out, f->n_nat_modem_samples, f->ptFilter7500to8000, 16, 15);
    //assert(i == f->n_nom_modem_samples);
    // Caution: assert fails if f->n_nat_modem_samples * 16.0 / 15.0 is not an integer

}

void freedv_700d_open(struct freedv *f, struct freedv_advanced *adv) {
        f->snr_squelch_thresh = 0.0;
        f->squelch_en = 0;

        f->ofdm_config = (struct OFDM_CONFIG *) CALLOC(1, sizeof (struct OFDM_CONFIG));
        assert(f->ofdm_config != NULL);

        f->ofdm = ofdm_create(f->ofdm_config);
	assert(f->ofdm != NULL);
        FREE(f->ofdm_config);
        
        /* Get a copy of the actual modem config */
        f->ofdm_config = ofdm_get_config_param();

        f->ofdm_bitsperframe = ofdm_get_bits_per_frame();
        f->ofdm_nuwbits = (f->ofdm_config->ns - 1) * f->ofdm_config->bps - f->ofdm_config->txtbits;
        f->ofdm_ntxtbits = f->ofdm_config->txtbits;

        f->ldpc = (struct LDPC*)MALLOC(sizeof(struct LDPC));
        assert(f->ldpc != NULL);

        set_up_hra_112_112(f->ldpc, f->ofdm_config);
#ifdef __EMBEDDED__
	f->ldpc->max_iter = 10; /* limit LDPC decoder iterations to limit CPU load */
#endif	
	/* Code length 224 divided by 2 bits per symbol = 112 symbols per frame */
        int coded_syms_per_frame = f->ldpc->coded_syms_per_frame;
        
        if (adv == NULL) {
            f->interleave_frames = 1;
        } else {
            assert((adv->interleave_frames >= 0) && (adv->interleave_frames <= 16));
            f->interleave_frames = adv->interleave_frames;
        }

        f->modem_frame_count_tx = f->modem_frame_count_rx = 0;
        
        f->codeword_symbols = (COMP*)MALLOC(sizeof(COMP)*f->interleave_frames*coded_syms_per_frame);
        assert(f->codeword_symbols != NULL);

        f->codeword_amps = (float*)MALLOC(sizeof(float)*f->interleave_frames*coded_syms_per_frame);
        assert(f->codeword_amps != NULL);

        for (int i=0; i<f->interleave_frames*coded_syms_per_frame; i++) {
            f->codeword_symbols[i].real = 0.0;
            f->codeword_symbols[i].imag = 0.0;
            f->codeword_amps[i] = 0.0;
        }

        f->nin = f->nin_prev = ofdm_get_samples_per_frame();
        f->n_nat_modem_samples = ofdm_get_samples_per_frame();
        f->n_nom_modem_samples = ofdm_get_samples_per_frame();
        f->n_max_modem_samples = ofdm_get_max_samples_per_frame();
        f->modem_sample_rate = f->ofdm_config->fs;
        f->clip = 0;
        f->sz_error_pattern = f->ofdm_bitsperframe;

        f->tx_bits = NULL; /* not used for 700D */

	if (f->interleave_frames > 1) {
            /* only allocate this array for interleaver sizes > 1 to save memory on SM1000 port */
            f->mod_out = (COMP*)MALLOC(sizeof(COMP)*f->interleave_frames*f->n_nat_modem_samples);
            assert(f->mod_out != NULL);

            for (int i=0; i<f->interleave_frames*f->n_nat_modem_samples; i++) {
                f->mod_out[i].real = 0.0;
                f->mod_out[i].imag = 0.0;
            }
	}

#ifndef __EMBEDDED__
        /* tx BPF off on embedded platforms, as it consumes significant CPU */
        ofdm_set_tx_bpf(f->ofdm, 1);
#endif
}


/*
  Ok so when interleaved, we take the interleaver length of input samples,
  and output that many modem samples, e.g. for interleaver of length 4:

  record input speech 1234
  freedv tx              |
  play modem sig         1234
  record modem sig       1234
  freedv_rx                   |
  play output speech          1234        
  time axis --------->123456789012---->

  So a sample of input speech at time 1 is ouput at time 9.  We assume
  the freedv_tx and freedv_rx and propogation time over channel (from
  when a modem signal is played at the HF tx to when it is recorded at
  the HF rx) is zero.

  The freedv tx interface ouputs n_nom_modem_samples, which a single
  OFDM modem frame, 112 payload bits or 4 speech codec frames.  So
  this function must always have 1280 speech samples as input, and
  1280 modem samples as output, regradless of interleaver_frames.  For
  interleaver_frames > 1, we need to buffer samples.
*/

void freedv_comptx_700d(struct freedv *f, COMP mod_out[]) {
    int    bit, byte, i, j, k;
    int    nspare;
 
    int data_bits_per_frame = f->ldpc->data_bits_per_frame;
    int bits_per_interleaved_frame = f->interleave_frames*data_bits_per_frame;
    uint8_t tx_bits[bits_per_interleaved_frame];
    int bits_per_codec_frame = codec2_bits_per_frame(f->codec2);

    byte = 0;
    for (j=0; j<bits_per_interleaved_frame; j+=bits_per_codec_frame) {

        /* unpack bits, MSB first */

        bit = 7;
        for(i=0; i<bits_per_codec_frame; i++) {
            tx_bits[j+i] = (f->packed_codec_bits_tx[byte] >> bit) & 0x1;
            bit--;
            if (bit < 0) {
                bit = 7;
                byte++;
            }
        }
	if (bit != 7)
	    byte++;
    }

    assert(byte <= f->nbyte_packed_codec_bits);
    
    // Generate Varicode txt bits. Txt bits in OFDM frame come just
    // after Unique Word (UW).  Txt bits aren't protected by FEC, and need to be
    // added to each frame after interleaver as done it's thing

    nspare = f->ofdm_ntxtbits*f->interleave_frames;
    uint8_t txt_bits[nspare];

    for(k=0; k<nspare; k++) {
        if (f->nvaricode_bits == 0) {
            /* get new char and encode */
            char s[2];
            if (f->freedv_get_next_tx_char != NULL) {
                s[0] = (*f->freedv_get_next_tx_char)(f->callback_state);
                f->nvaricode_bits = varicode_encode(f->tx_varicode_bits, s, VARICODE_MAX_BITS, 1, 1);
                f->varicode_bit_index = 0;
            }
        }
        if (f->nvaricode_bits) {
            txt_bits[k] = f->tx_varicode_bits[f->varicode_bit_index++];
            f->nvaricode_bits--;
        }
    }

    /* optionally replace codec payload bits with test frames known to rx */

    if (f->test_frames) {
        uint8_t payload_data_bits[data_bits_per_frame];
        ofdm_generate_payload_data_bits(payload_data_bits, data_bits_per_frame);

        for (j=0; j<f->interleave_frames; j++) {
            for(i=0; i<data_bits_per_frame; i++) {
                tx_bits[j*data_bits_per_frame + i] = payload_data_bits[i];
            }
        }
    }

    /* OK now ready to LDPC encode, interleave, and OFDM modulate */
    
    complex float tx_sams[f->interleave_frames*f->n_nat_modem_samples];
    COMP asam;
    
    ofdm_ldpc_interleave_tx(f->ofdm, f->ldpc, tx_sams, tx_bits, txt_bits, f->interleave_frames, f->ofdm_config);

    for(i=0; i<f->interleave_frames*f->n_nat_modem_samples; i++) {
        asam.real = crealf(tx_sams[i]);
        asam.imag = cimagf(tx_sams[i]);
        mod_out[i] = fcmult(OFDM_AMP_SCALE*NORM_PWR_OFDM, asam);
    }

    if (f->clip) {
        //fprintf(stderr, "clip ");
        cohpsk_clip(mod_out, OFDM_CLIP, f->interleave_frames*f->n_nat_modem_samples);
    }
}

int freedv_comprx_700c(struct freedv *f, COMP demod_in_8kHz[]) {
    int                 bits_per_codec_frame, bytes_per_codec_frame;
    int                 i, j, bit, byte, k;
    int                 data_flag_index, n_ascii, nspare;
    short               abit[1];
    char                ascii_out;
    float rx_bits[COHPSK_BITS_PER_FRAME]; /* soft decn rx bits */
    int   sync;
    int   frames;
    int   rx_status;

    bits_per_codec_frame  = codec2_bits_per_frame(f->codec2);
    bytes_per_codec_frame = (bits_per_codec_frame + 7) / 8;
    frames = f->n_codec_bits / bits_per_codec_frame;

    rx_status = 0;
    
    // quisk_cfInterpDecim() modifies input data so lets make a copy just in case there
    // is no sync and we need to echo inpout to output

    // freedv_nin(f): input samples at Fs=8000 Hz
    // f->nin: input samples at Fs=7500 Hz
    
    COMP demod_in[freedv_nin(f)];
    for(i=0; i<freedv_nin(f); i++)
        demod_in[i] = demod_in_8kHz[i];

    i = quisk_cfInterpDecim((complex float *)demod_in, freedv_nin(f), f->ptFilter8000to7500, 15, 16);

    for(i=0; i<f->nin; i++)
        demod_in[i] = fcmult(1.0/FDMDV_SCALE, demod_in[i]);
    
    cohpsk_demod(f->cohpsk, rx_bits, &sync, demod_in, &f->nin);

    f->sync = sync;
    cohpsk_get_demod_stats(f->cohpsk, &f->stats);
    f->snr_est = f->stats.snr_est;

    memset(f->packed_codec_bits, 0, bytes_per_codec_frame * frames);

    if (sync) {
        rx_status = RX_SYNC;
        
        if (f->test_frames == 0) {
            data_flag_index = codec2_get_spare_bit_index(f->codec2);

            /* optional smoothing of codec symbols */

            if (f->smooth_symbols) {

                for(i=0; i<bits_per_codec_frame; i++) {
                    rx_bits[i] += rx_bits[i+bits_per_codec_frame];
                    rx_bits[i+bits_per_codec_frame] = rx_bits[i];
                }
            }

            byte = 0;
            for (j=0; j<COHPSK_BITS_PER_FRAME; j+=bits_per_codec_frame) {

                /* extract txt msg data bit(s) */

                switch(f->mode) {
                case FREEDV_MODE_700C:
                    nspare = 0; // and no spare bits for 700C atm
                    break;
                default:
                    assert(0);
                }

                for(k=0; k<nspare; k++)  {
                    abit[0] = rx_bits[data_flag_index+j+k] < 0.0;

                    n_ascii = varicode_decode(&f->varicode_dec_states, &ascii_out, abit, 1, 1);
                    if (n_ascii && (f->freedv_put_next_rx_char != NULL)) {
                        (*f->freedv_put_next_rx_char)(f->callback_state, ascii_out);
                    }
                }

                /* pack bits, MSB received first */

                bit = 7;
                for(i=0; i<bits_per_codec_frame; i++) {
                    f->packed_codec_bits[byte] |= ((rx_bits[j+i] < 0.0) << bit);
                    bit--;
                    if (bit < 0) {
                        bit = 7;
                        byte++;
                    }
                }
		if (bit != 7)
		    byte++;

            }
            rx_status |= RX_BITS;
        }
        else {
            //fprintf(stderr, " freedv_api:  f->test_frames_diversity: %d\n", f->test_frames_diversity);

            if (f->test_frames_diversity) {
                /* normal operation - error pattern on frame after diveristy combination */
                short error_pattern[COHPSK_BITS_PER_FRAME];
                int   bit_errors;

                /* test data, lets see if we can sync to the test data sequence */

                char rx_bits_char[COHPSK_BITS_PER_FRAME];
                for(i=0; i<COHPSK_BITS_PER_FRAME; i++)
                    rx_bits_char[i] = rx_bits[i] < 0.0;
                cohpsk_put_test_bits(f->cohpsk, &f->test_frame_sync_state, error_pattern, &bit_errors, rx_bits_char, 0);
                if (f->test_frame_sync_state) {
                    f->total_bit_errors += bit_errors;
                    f->total_bits       += COHPSK_BITS_PER_FRAME;
                    if (f->freedv_put_error_pattern != NULL) {
                        (*f->freedv_put_error_pattern)(f->error_pattern_callback_state, error_pattern, COHPSK_BITS_PER_FRAME);
                    }
                }
            } 
            else {
                /* calculate error pattern on uncombined carriers - test mode to spot any carrier specific issues like
                   tx passband filtering */

                short error_pattern[2*COHPSK_BITS_PER_FRAME];
                char  rx_bits_char[COHPSK_BITS_PER_FRAME];
                int   bit_errors_lower, bit_errors_upper;

                /* lower group of carriers */

                float *rx_bits_lower = cohpsk_get_rx_bits_lower(f->cohpsk);
                for(i=0; i<COHPSK_BITS_PER_FRAME; i++) {
                    rx_bits_char[i] = rx_bits_lower[i] < 0.0;
                }
                cohpsk_put_test_bits(f->cohpsk, &f->test_frame_sync_state, error_pattern, &bit_errors_lower, rx_bits_char, 0);

                /* upper group of carriers */

                float *rx_bits_upper = cohpsk_get_rx_bits_upper(f->cohpsk);
                for(i=0; i<COHPSK_BITS_PER_FRAME; i++) {
                    rx_bits_char[i] = rx_bits_upper[i] < 0.0;
                }
                cohpsk_put_test_bits(f->cohpsk, &f->test_frame_sync_state_upper, &error_pattern[COHPSK_BITS_PER_FRAME], &bit_errors_upper, rx_bits_char, 1);
                //                fprintf(stderr, " freedv_api:  f->test_frame_sync_state: %d f->test_frame_sync_state_upper: %d\n", 
                //        f->test_frame_sync_state, f->test_frame_sync_state_upper);

                /* combine total errors and call callback */

                if (f->test_frame_sync_state && f->test_frame_sync_state_upper) {
                    f->total_bit_errors += bit_errors_lower + bit_errors_upper;
                    f->total_bits       += 2*COHPSK_BITS_PER_FRAME;
                    if (f->freedv_put_error_pattern != NULL) {
                        (*f->freedv_put_error_pattern)(f->error_pattern_callback_state, error_pattern, 2*COHPSK_BITS_PER_FRAME);
                    }
                }

            }
        }

    }

    return rx_status;
}

/*
  700D demod function that can support complex (float) or real (short)
  samples.  The real short samples are useful for low memory overhead,
  such at the SM1000.
*/

int freedv_comp_short_rx_700d(struct freedv *f, void *demod_in_8kHz, int demod_in_is_short, float gain) {
    int   bits_per_codec_frame, bytes_per_codec_frame;
    int   i, j, bit, byte, k;
    int   n_ascii;
    char  ascii_out;
    int   frames;
    struct OFDM *ofdm = f->ofdm;
    struct LDPC *ldpc = f->ldpc;
    
    int    data_bits_per_frame = ldpc->data_bits_per_frame;
    int    coded_bits_per_frame = ldpc->coded_bits_per_frame;
    int    coded_syms_per_frame = ldpc->coded_syms_per_frame;
    int    interleave_frames = f->interleave_frames;
    COMP  *codeword_symbols = f->codeword_symbols;
    float *codeword_amps = f->codeword_amps;
    int    rx_bits[f->ofdm_bitsperframe];
    short txt_bits[f->ofdm_ntxtbits];
    COMP  payload_syms[coded_syms_per_frame];
    float payload_amps[coded_syms_per_frame];
   
    bits_per_codec_frame  = codec2_bits_per_frame(f->codec2);
    bytes_per_codec_frame = (bits_per_codec_frame + 7) / 8;
    frames = f->n_codec_bits / bits_per_codec_frame;

    int Nerrs_raw = 0;
    int Nerrs_coded = 0;
    int iter = 0;
    int parityCheckCount = 0;
    uint8_t rx_uw[f->ofdm_nuwbits];

    float new_gain = gain / OFDM_AMP_SCALE;
    
    assert((demod_in_is_short == 0) || (demod_in_is_short == 1));

    f->sync = f->stats.sync = 0;
    int rx_status = 0;
    
    /* TODO estimate this properly from signal */    
    float EsNo = 3.0;
    
    /* looking for modem sync */
    
    if (ofdm->sync_state == search) {
        if (demod_in_is_short)
            ofdm_sync_search_shorts(f->ofdm, (short*)demod_in_8kHz, new_gain);
        else
            ofdm_sync_search(f->ofdm, (COMP*)demod_in_8kHz);
    }

     /* OK modem is in sync */
    
    if ((ofdm->sync_state == synced) || (ofdm->sync_state == trial)) {
        rx_status |= RX_SYNC;
        if (ofdm->sync_state == trial) rx_status |= RX_TRIAL_SYNC;
    
        if (demod_in_is_short)
            ofdm_demod_shorts(ofdm, rx_bits, (short*)demod_in_8kHz, new_gain);
        else
            ofdm_demod(ofdm, rx_bits, (COMP*)demod_in_8kHz);
            
        ofdm_disassemble_modem_frame(ofdm, rx_uw, payload_syms, payload_amps, txt_bits);

        f->sync = 1;
        ofdm_get_demod_stats(f->ofdm, &f->stats);
        f->snr_est = f->stats.snr_est;

        assert((f->ofdm_nuwbits+f->ofdm_ntxtbits+coded_bits_per_frame) == f->ofdm_bitsperframe);

        /* now we need to buffer for de-interleaving -------------------------------------*/
                
        /* shift interleaved symbol buffers to make room for new symbols */
                
        for(i=0, j=coded_syms_per_frame; j<interleave_frames*coded_syms_per_frame; i++,j++) {
            codeword_symbols[i] = codeword_symbols[j];
            codeword_amps[i] = codeword_amps[j];
        }

        /* newest symbols at end of buffer (uses final i from last loop), note we 
           change COMP formats from what modem uses internally */
                
        for(i=(interleave_frames-1)*coded_syms_per_frame,j=0; i<interleave_frames*coded_syms_per_frame; i++,j++) {
            codeword_symbols[i] = payload_syms[j];
            codeword_amps[i]    = payload_amps[j];
         }
               
        /* run de-interleaver */
                
        COMP  codeword_symbols_de[interleave_frames*coded_syms_per_frame];
        float codeword_amps_de[interleave_frames*coded_syms_per_frame];
        gp_deinterleave_comp (codeword_symbols_de, codeword_symbols, interleave_frames*coded_syms_per_frame);
        gp_deinterleave_float(codeword_amps_de   , codeword_amps   , interleave_frames*coded_syms_per_frame);

        float llr[coded_bits_per_frame];
        uint8_t out_char[coded_bits_per_frame];

        interleaver_sync_state_machine(ofdm, ldpc, f->ofdm_config, codeword_symbols_de, codeword_amps_de, EsNo,
                                       interleave_frames, &iter, &parityCheckCount, &Nerrs_coded);
                                         
        if ((ofdm->sync_state_interleaver == synced) && (ofdm->frame_count_interleaver == interleave_frames)) {
            ofdm->frame_count_interleaver = 0;

            if (f->test_frames) {
                int tmp[interleave_frames];
                Nerrs_raw = count_uncoded_errors(ldpc, f->ofdm_config, tmp, interleave_frames, codeword_symbols_de);
                f->total_bit_errors += Nerrs_raw;
                f->total_bits       += f->ofdm_bitsperframe*interleave_frames;
            }

            memset(f->packed_codec_bits, 0, bytes_per_codec_frame * frames);
            byte = 0; f->modem_frame_count_rx = 0;
            
            for (j=0; j<interleave_frames; j++) {
                symbols_to_llrs(llr, &codeword_symbols_de[j*coded_syms_per_frame],
                                &codeword_amps_de[j*coded_syms_per_frame],
                                EsNo, ofdm->mean_amp, coded_syms_per_frame);               
                iter = run_ldpc_decoder(ldpc, out_char, llr, &parityCheckCount);

                if (parityCheckCount != ldpc->NumberParityBits) rx_status |= RX_BIT_ERRORS;
                
                if (f->test_frames) {
                    uint8_t payload_data_bits[data_bits_per_frame];
                    ofdm_generate_payload_data_bits(payload_data_bits, data_bits_per_frame);
                    Nerrs_coded = count_errors(payload_data_bits, out_char, data_bits_per_frame);
                    f->total_bit_errors_coded += Nerrs_coded;
                    f->total_bits_coded       += data_bits_per_frame;
                } else {

                    /* a frame of valid Codec 2 bits, pack into Codec 2 frame  */

                    for (i=0; i<data_bits_per_frame; i+=bits_per_codec_frame) {

                        /* pack bits, MSB received first */

                        bit = 7;
                        for(k=0; k<bits_per_codec_frame; k++) {
                            f->packed_codec_bits[byte] |= (out_char[i+k] << bit);
                            bit--;
                            if (bit < 0) {
                                bit = 7;
                                byte++;
                            }
                        }
                        if (bit != 7)
                            byte++;
                    }
                    
                }
            } /* for interleave frames ... */

            /* make sure we don't overrun packed byte array */

            assert(byte <= f->nbyte_packed_codec_bits);
                   
            rx_status |= RX_BITS;

        } /* if interleaver synced ..... */

        /* If modem is synced we can decode txt bits */        
        for(k=0; k<f->ofdm_ntxtbits; k++)  { 
            //fprintf(stderr, "txt_bits[%d] = %d\n", k, rx_bits[i]);
            n_ascii = varicode_decode(&f->varicode_dec_states, &ascii_out, &txt_bits[k], 1, 1);
            if (n_ascii && (f->freedv_put_next_rx_char != NULL)) {
                (*f->freedv_put_next_rx_char)(f->callback_state, ascii_out);
            }
        }

        /* estimate uncoded BER from UW */
        for(i=0; i<f->ofdm_nuwbits; i++) {         
            if (rx_uw[i] != ofdm->tx_uw[i]) {
                f->total_bit_errors++;
            }
        }
        f->total_bits += f->ofdm_nuwbits;          

    }
    
    /* iterate state machine and update nin for next call */
    
    f->nin = ofdm_get_nin(ofdm);
    ofdm_sync_state_machine(ofdm, rx_uw);

    if ((f->verbose && (ofdm->last_sync_state == search)) || (f->verbose == 2)) {
        fprintf(stderr, "%3d nin: %4d st: %-6s euw: %2d %1d f: %5.1f phbw: %d snr: %4.1f %2d eraw: %3d ecdd: %3d iter: %3d pcc: %3d rxst: %d\n",
                f->frames++, ofdm->nin, ofdm_statemode[ofdm->last_sync_state], ofdm->uw_errors, ofdm->sync_counter, 
		(double)ofdm->foff_est_hz, ofdm->phase_est_bandwidth,
                f->snr_est, ofdm->frame_count_interleaver,
                Nerrs_raw, Nerrs_coded, iter, parityCheckCount, rx_status);
    }
    
    return rx_status;
}

