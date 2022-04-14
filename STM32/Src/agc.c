#include "stm32h7xx_hal.h"
#include "agc.h"
#include "settings.h"
#include "audio_filters.h"
#include "vad.h"
#include "trx_manager.h"

// Private variables
static IRAM2 AGC_RX_Instance AGC_RX1 = {0};
static IRAM2 AGC_RX_Instance AGC_RX2 = {0};
static IRAM2 AGC_TX_Instance AGC_TX = {0};

// Run AGC on data block
void DoRxAGC(float32_t *agcBuffer_i, float32_t *agcBuffer_q, uint_fast16_t blockSize, AUDIO_PROC_RX_NUM rx_id, uint_fast8_t mode, bool stereo)
{
	// RX1 or RX2
	AGC_RX_Instance *AGC = &AGC_RX1;
	bool VAD_Muting = VAD_RX1_Muting;
	if (rx_id == AUDIO_RX2)
	{
		AGC = &AGC_RX2;
		VAD_Muting = VAD_RX2_Muting;
	}

	// higher speed in settings - higher speed of AGC processing
	float32_t RX_AGC_STEPSIZE_UP = 0.0f;
	float32_t RX_AGC_STEPSIZE_DOWN = 0.0f;
	if (mode == TRX_MODE_CW)
	{
		RX_AGC_STEPSIZE_UP = 200.0f / (float32_t)TRX.RX_AGC_CW_speed;
		RX_AGC_STEPSIZE_DOWN = 20.0f / (float32_t)TRX.RX_AGC_CW_speed;
	}
	else
	{
		RX_AGC_STEPSIZE_UP = 1000.0f / (float32_t)TRX.RX_AGC_SSB_speed;
		RX_AGC_STEPSIZE_DOWN = 20.0f / (float32_t)TRX.RX_AGC_SSB_speed;
	}

	// do k-weighting (for LKFS)
	if (rx_id == AUDIO_RX1)
	{
		arm_biquad_cascade_df2T_f32_single(&AGC_RX1_KW_HSHELF_FILTER, agcBuffer_i, AGC->agcBuffer_kw, blockSize);
		arm_biquad_cascade_df2T_f32_single(&AGC_RX1_KW_HPASS_FILTER, agcBuffer_i, AGC->agcBuffer_kw, blockSize);
	}
	else
	{
		arm_biquad_cascade_df2T_f32_single(&AGC_RX2_KW_HSHELF_FILTER, agcBuffer_i, AGC->agcBuffer_kw, blockSize);
		arm_biquad_cascade_df2T_f32_single(&AGC_RX2_KW_HPASS_FILTER, agcBuffer_i, AGC->agcBuffer_kw, blockSize);
	}

	// do ring buffer
	static uint32_t ring_position = 0;
	// save new data to ring buffer
	dma_memcpy(&(AGC->ringbuffer[ring_position * blockSize]), agcBuffer_i, sizeof(float32_t) * blockSize);
	// move ring buffer index
	ring_position++;
	if (ring_position >= AGC_RINGBUFFER_TAPS_SIZE)
		ring_position = 0;
	// get old data to process
	if (!stereo)
		dma_memcpy(agcBuffer_i, &(AGC->ringbuffer[ring_position * blockSize]), sizeof(float32_t) * blockSize);

	// calculate the magnitude in dBFS
	float32_t AGC_RX_magnitude = 0;
	arm_rms_f32(AGC->agcBuffer_kw, blockSize, &AGC_RX_magnitude);
	if (AGC_RX_magnitude == 0.0f)
		AGC_RX_magnitude = 0.001f;
	float32_t full_scale_rate = AGC_RX_magnitude / FLOAT_FULL_SCALE_POW;
	float32_t AGC_RX_dbFS = rate2dbV(full_scale_rate);

	// gain target
	float32_t gain_target = (float32_t)TRX.AGC_GAIN_TARGET;
	if (mode == TRX_MODE_CW)
		gain_target += CW_ADD_GAIN_AF;

	// move the gain one step
	if (!WM8731_Muting && !VAD_Muting)
	{
		float32_t diff = (gain_target - (AGC_RX_dbFS + AGC->need_gain_db));

		// hold time limiter
		if (fabsf(diff) < AGC_HOLDTIME_LIMITER_DB && AGC->hold_time < TRX.RX_AGC_Hold)
		{
			AGC->hold_time += AGC_HOLDTIME_STEP;
		}

		// move
		if (diff > 0)
		{
			if ((HAL_GetTick() - AGC->last_agc_peak_time) > AGC->hold_time)
			{
				AGC->need_gain_db += diff / RX_AGC_STEPSIZE_UP;

				if (diff > AGC_HOLDTIME_LIMITER_DB && AGC->hold_time > 0)
					AGC->hold_time -= AGC_HOLDTIME_STEP;
			}
		}
		else
		{
			AGC->need_gain_db += diff / RX_AGC_STEPSIZE_DOWN;
			AGC->last_agc_peak_time = HAL_GetTick();
		}

		// overload (clipping), sharply reduce the gain
		if ((AGC_RX_dbFS + AGC->need_gain_db) > (gain_target + AGC_CLIPPING))
		{
			AGC->need_gain_db = gain_target - AGC_RX_dbFS;
			// println("AGC overload ", diff);
		}

		// println("AGC HoldTime ", AGC->need_gain_db, " ", diff, " ", AGC->hold_time);
	}

	// AGC off, not adjustable
	if ((rx_id == AUDIO_RX1 && !CurrentVFO->AGC) || (rx_id == AUDIO_RX2 && !SecondaryVFO->AGC))
		AGC->need_gain_db = 1.0f;

	// Muting if need
	float32_t current_need_gain = AGC->need_gain_db;
	if (WM8731_Muting || VAD_Muting)
	{
		current_need_gain = -200.0f;
		// AGC->need_gain_db -= 10.0f;
	}

	// gain limitation
	if (AGC->need_gain_db > (float32_t)TRX.RX_AGC_Max_gain)
		AGC->need_gain_db = (float32_t)TRX.RX_AGC_Max_gain;

	// apply gain
	// println("cur agc: ", AGC_RX_dbFS, " need: ", AGC->need_gain_db);
	if (fabsf(AGC->need_gain_db_old - current_need_gain) > 0.0f) // gain changed
	{
		float32_t gainApplyStep = 0;
		if (AGC->need_gain_db_old > current_need_gain)
			gainApplyStep = -(AGC->need_gain_db_old - current_need_gain) / (float32_t)blockSize;
		if (AGC->need_gain_db_old < current_need_gain)
			gainApplyStep = (current_need_gain - AGC->need_gain_db_old) / (float32_t)blockSize;
		float32_t val_prev = 0.0f;
		bool zero_cross = false;
		for (uint_fast16_t i = 0; i < blockSize; i++)
		{
			if (val_prev < 0.0f && agcBuffer_i[i] > 0.0f)
				zero_cross = true;
			else if (val_prev > 0.0f && agcBuffer_i[i] < 0.0f)
				zero_cross = true;
			if (zero_cross)
				AGC->need_gain_db_old += gainApplyStep;

			agcBuffer_i[i] = agcBuffer_i[i] * db2rateV(AGC->need_gain_db_old);
			if (stereo)
				agcBuffer_q[i] = agcBuffer_q[i] * db2rateV(AGC->need_gain_db_old);
			val_prev = agcBuffer_i[i];
		}
	}
	else // gain did not change, apply gain across all samples
	{
		arm_scale_f32(agcBuffer_i, db2rateV(AGC->need_gain_db), agcBuffer_i, blockSize);
		if (stereo)
			arm_scale_f32(agcBuffer_q, db2rateV(AGC->need_gain_db), agcBuffer_q, blockSize);
	}
}

// Run TX AGC on data block
void DoTxAGC(float32_t *agcBuffer_i, uint_fast16_t blockSize, float32_t target, uint_fast8_t mode)
{
	// higher speed in settings - higher speed of AGC processing
	float32_t TX_AGC_STEPSIZE_UP = 0.0f;
	float32_t TX_AGC_STEPSIZE_DOWN = 0.0f;
	switch (mode)
	{
    case TRX_MODE_NFM:
	case TRX_MODE_WFM:
	case TRX_MODE_AM:
	case TRX_MODE_SAM:
		TX_AGC_STEPSIZE_UP = 200.0f / (float32_t)TRX.TX_Compressor_speed_AMFM;
		TX_AGC_STEPSIZE_DOWN = 20.0f / (float32_t)TRX.TX_Compressor_speed_AMFM;
		break;

	case TRX_MODE_LSB:
	case TRX_MODE_USB:
	case TRX_MODE_LOOPBACK:
	default:
		TX_AGC_STEPSIZE_UP = 200.0f / (float32_t)TRX.TX_Compressor_speed_SSB;
		TX_AGC_STEPSIZE_DOWN = 20.0f / (float32_t)TRX.TX_Compressor_speed_SSB;
		break;
	}

	// do ring buffer
	static uint32_t ring_position = 0;
	// save new data to ring buffer
	// dma_memcpy(&agc_ringbuffer_i[ring_position * blockSize], agcBuffer_i, sizeof(float32_t) * blockSize);
	// move ring buffer index
	ring_position++;
	if (ring_position >= AGC_RINGBUFFER_TAPS_SIZE)
		ring_position = 0;
	// get old data to process
	// dma_memcpy(agcBuffer_i, &agc_ringbuffer_i[ring_position * blockSize], sizeof(float32_t) * blockSize);

	// calculate the magnitude
	float32_t AGC_TX_I_magnitude = 0;
	float32_t ampl_max_i = 0.0f;
	float32_t ampl_min_i = 0.0f;
	uint32_t tmp_index;
	arm_max_no_idx_f32(agcBuffer_i, blockSize, &ampl_max_i);
	arm_min_f32(agcBuffer_i, blockSize, &ampl_min_i, &tmp_index);
	if (ampl_max_i > -ampl_min_i)
		AGC_TX_I_magnitude = ampl_max_i;
	else
		AGC_TX_I_magnitude = -ampl_min_i;
	if (AGC_TX_I_magnitude == 0.0f)
		AGC_TX_I_magnitude = 0.000001f;

	float32_t AGC_TX_dbFS = rate2dbV(AGC_TX_I_magnitude);
	if (AGC_TX_dbFS < -100.0f)
		AGC_TX_dbFS = -100.0f;

	// mic noise threshold (gate), below it - mute
	if (AGC_TX_dbFS < TRX.MIC_NOISE_GATE && TRX.MIC_NOISE_GATE > -120)
	{ //-120db - disabled
		target = 0.0f;
		TRX_MIC_BELOW_NOISEGATE = true;
	}
	else
	{
		// println(AGC_TX_I_magnitude, " ", AGC_TX_dbFS, " ", *AGC_need_gain_db);
		TRX_MIC_BELOW_NOISEGATE = false;
	}

	// move the gain one step
	if (target > 0.0f)
	{
		float32_t diff = (target - (AGC_TX_dbFS + AGC_TX.need_gain_db));
		if (diff > 0)
			AGC_TX.need_gain_db += diff / TX_AGC_STEPSIZE_UP;
		else
			AGC_TX.need_gain_db += diff / TX_AGC_STEPSIZE_DOWN;

		// overload (clipping), sharply reduce the gain
		if ((AGC_TX_dbFS + AGC_TX.need_gain_db) > target)
		{
			AGC_TX.need_gain_db = target - AGC_TX_dbFS;
			// sendToDebug_float32(diff,false);
		}
	}

	// gain limitation
	switch (mode)
	{
	case TRX_MODE_NFM:
	case TRX_MODE_WFM:
	case TRX_MODE_AM:
	case TRX_MODE_SAM:
		if (AGC_TX.need_gain_db > TRX.TX_Compressor_maxgain_AMFM)
			AGC_TX.need_gain_db = TRX.TX_Compressor_maxgain_AMFM;
		break;

	case TRX_MODE_LSB:
	case TRX_MODE_USB:
	case TRX_MODE_LOOPBACK:
	default:
		if (AGC_TX.need_gain_db > TRX.TX_Compressor_maxgain_SSB)
			AGC_TX.need_gain_db = TRX.TX_Compressor_maxgain_SSB;
		break;
	}

	// apply gain
	// println(*AGC_need_gain_db);
	if (target == 0.0f) // zero gain (mute)
	{
#define zero_gain 200.0f
		float32_t gainApplyStep = 0;
		if (AGC_TX.need_gain_db_old > zero_gain)
			gainApplyStep = -(AGC_TX.need_gain_db_old - zero_gain) / (float32_t)blockSize;
		if (AGC_TX.need_gain_db_old < zero_gain)
			gainApplyStep = (zero_gain - AGC_TX.need_gain_db_old) / (float32_t)blockSize;
		float32_t val_prev = 0.0f;
		bool zero_cross = false;
		for (uint_fast16_t i = 0; i < blockSize; i++)
		{
			if (val_prev < 0.0f && agcBuffer_i[i] > 0.0f)
				zero_cross = true;
			else if (val_prev > 0.0f && agcBuffer_i[i] < 0.0f)
				zero_cross = true;
			if (zero_cross)
				AGC_TX.need_gain_db_old += gainApplyStep;

			if (AGC_TX.need_gain_db_old >= zero_gain)
				agcBuffer_i[i] = agcBuffer_i[i] * db2rateV(AGC_TX.need_gain_db_old);
			else
				agcBuffer_i[i] = 0.0f;

			val_prev = agcBuffer_i[i];
		}
	}
	else if (fabsf(AGC_TX.need_gain_db_old - AGC_TX.need_gain_db) > 0.0f) // gain changed
	{
		float32_t gainApplyStep = 0;
		if (AGC_TX.need_gain_db_old > AGC_TX.need_gain_db)
			gainApplyStep = -(AGC_TX.need_gain_db_old - AGC_TX.need_gain_db) / (float32_t)blockSize;
		if (AGC_TX.need_gain_db_old < AGC_TX.need_gain_db)
			gainApplyStep = (AGC_TX.need_gain_db - AGC_TX.need_gain_db_old) / (float32_t)blockSize;
		float32_t val_prev = 0.0f;
		bool zero_cross = false;
		for (uint_fast16_t i = 0; i < blockSize; i++)
		{
			if (val_prev < 0.0f && agcBuffer_i[i] > 0.0f)
				zero_cross = true;
			else if (val_prev > 0.0f && agcBuffer_i[i] < 0.0f)
				zero_cross = true;
			if (zero_cross)
				AGC_TX.need_gain_db_old += gainApplyStep;

			agcBuffer_i[i] = agcBuffer_i[i] * db2rateV(AGC_TX.need_gain_db_old);
			val_prev = agcBuffer_i[i];
		}
	}
	else // gain did not change, apply gain across all samples
	{
		arm_scale_f32(agcBuffer_i, db2rateV(AGC_TX.need_gain_db), agcBuffer_i, blockSize);
	}
}

void ResetAGC(void)
{
	// AGC_RX1.need_gain_db = 0.0f;
	// AGC_RX2.need_gain_db = 0.0f;
	dma_memset(AGC_RX1.ringbuffer, 0x00, sizeof(AGC_RX1.ringbuffer));
	dma_memset(AGC_RX2.ringbuffer, 0x00, sizeof(AGC_RX2.ringbuffer));
}
