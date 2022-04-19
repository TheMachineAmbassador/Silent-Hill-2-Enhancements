/*
* Copyright (C) 2022 Gemini
* ===============================================================
* ADX ADPCM decoder
* ---------------------------------------------------------------
* Code for decompressing ADX data to PCM samples. Readapted from
* the following source code:
* https://handwiki.org/wiki/Software:ADX_(file_format)
* ===============================================================
*/
#include <math.h>
#include <algorithm>
#include "criware.h"

void ADXDEC_SetCoeff(CriFileStream* adx)
{
	const double M_SQRT2 = 1.4142135623730951;
	const double M_PI = 3.141592653589793;

	double a, b, c;
	a = M_SQRT2 - cos(2.0 * M_PI * (double)adx->highpass_frequency / (double)adx->sample_rate);
	b = M_SQRT2 - 1.0;
	c = (a - sqrt((a + b) * (a - b))) / b;

	// double coefficient[2];
	adx->coefficient[0] = (short)lrint(c * 2. * (double)(1 << 12));
	adx->coefficient[1] = (short)lrint(-(c * c) * (double)(1 << 12));
	adx->sample_index = 0;
	memset(adx->past_samples, 0, adx->channel_count * 4);
}

typedef struct bitstream
{
	CriFileStream* fp;
	u_long bitpos;
	BYTE read;
} bitstream;

static __inline void bitstream_seek(bitstream* stream, u_long pos)
{
	stream->bitpos = (pos % 8) / 4;
	stream->fp->Seek(pos / 8, FILE_BEGIN);
	stream->fp->Read(&stream->read, 1);
}

static __inline u_long bitstream_read(bitstream* stream, u_long bits)
{
	u_long b = 0;
	if (stream->bitpos == 0)
		b = (stream->read >> 4) & 0xf;
	else
		b = stream->read & 0xF;

	return b;
}

static __inline int sign_extend(u_long base, u_long bits)
{
	int res = base;

	return (res << (32 - bits)) >> (32 - bits);
}

static __inline short sbetole(short a)
{
	u_short b = a;
	return (b >> 8) | (b << 8);
}

// buffer is where the decoded samples will be put
// samples_needed states how many sample 'sets' (one sample from every channel) need to be decoded to fill the buffer
// looping_enabled is a boolean flag to control use of the built-in loop
// Returns the number of sample 'sets' in the buffer that could not be filled (EOS)
unsigned ADXDEC_Decode(CriFileStream* adx, int16_t* buffer, unsigned samples_needed, bool looping_enabled)
{
	unsigned const samples_per_block = (adx->block_size - 2) * 8 / adx->sample_bitdepth;
	int16_t scale[4];

	bitstream stream = { 0 };
	stream.fp = adx;

	if (looping_enabled && !adx->loop_enabled)
		looping_enabled = false;

	// Loop until the requested number of samples are decoded, or the end of file is reached
	while (samples_needed > 0 && adx->sample_index < adx->total_samples)
	{
		// Calculate the number of samples that are left to be decoded in the current block
		unsigned sample_offset = adx->sample_index % samples_per_block;
		unsigned samples_can_get = samples_per_block - sample_offset;

		// Clamp the samples we can get during this run if they won't fit in the buffer
		if (samples_can_get > samples_needed)
			samples_can_get = samples_needed;

		// Clamp the number of samples to be acquired if the stream isn't long enough or the loop trigger is nearby
		if (looping_enabled && adx->sample_index + samples_can_get > adx->loop_end_index)
			samples_can_get = adx->loop_end_index - adx->sample_index;
		else if (adx->sample_index + samples_can_get > adx->total_samples)
			samples_can_get = adx->total_samples - adx->sample_index;

		// Calculate the bit address of the start of the frame that sample_index resides in and record that location
		unsigned long started_at = (adx->copyright_offset + 4 + adx->sample_index / samples_per_block * adx->block_size * adx->channel_count) * 8;

		// Read the scale values from the start of each block in this frame
		for (unsigned i = 0; i < adx->channel_count; ++i)
		{
			adx->Seek(started_at / 8 + adx->block_size * i, SEEK_SET);
			adx->Read(&scale[i], 2);
			scale[i] = sbetole(scale[i]);
		}

		// Pre-calculate the stop value for sample_offset
		unsigned sample_endoffset = sample_offset + samples_can_get;

		// Save the bitstream address of the first sample immediately after the scale in the first block of the frame
		started_at += 16;
		while (sample_offset < sample_endoffset)
		{
			for (unsigned i = 0; i < adx->channel_count; ++i)
			{
				// Predict the next sample
				int sample_prediction = (adx->coefficient[0] * adx->past_samples[i * 2 + 0] + adx->coefficient[1] * adx->past_samples[i * 2 + 1]) >> 12;

				// Seek to the sample offset, read and sign extend it to a 32bit integer
				// The sign extension will also need to include a endian adjustment if there are more than 8 bits
				bitstream_seek(&stream, started_at + adx->sample_bitdepth * sample_offset + adx->block_size * 8 * i);
				int sample_error = bitstream_read(&stream, adx->sample_bitdepth);
				sample_error = sign_extend(sample_error, adx->sample_bitdepth);

				// Scale the error correction value
				sample_error *= scale[i];

				// Calculate the sample by combining the prediction with the error correction
				int sample = sample_error + sample_prediction;

				// Clamp the decoded sample to the valid range for a 16bit integer
				if (sample > SHRT_MAX) sample = SHRT_MAX;
				else if (sample < SHRT_MIN) sample = SHRT_MIN;

				// Update the past samples with the newer sample
				adx->past_samples[i * 2 + 1] = adx->past_samples[i * 2 + 0];
				adx->past_samples[i * 2 + 0] = sample;

				// Save the sample to the buffer then advance one place
				*buffer++ = sample;
			}
			++sample_offset;		// We've decoded one sample from every block, advance block offset by 1
			++adx->sample_index;	// This also means we're one sample further into the stream
			--samples_needed;		// And so there is one less set of samples that needs to be decoded
		}

		// Check if we hit the loop end marker, if we did we need to jump to the loop start
		if (looping_enabled && adx->sample_index == adx->loop_end_index)
			adx->sample_index = adx->loop_start_index;
	}

	return samples_needed;
}
