/******************************************************************************
 *  Sega Master System / GameGear Emulator
 *  Copyright (C) 1998-2007  Charles MacDonald
 *
 *  additionnal code by Eke-Eke (SMS Plus GX)
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *   Sound emulation.
 *
 ******************************************************************************/

#include "../shared.h"

snd_t snd;
// static int16 **fm_buffer;
static int16 **psg_buffer;
static int lines_per_frame;
static int samples_per_line;


// Keep the MIT-licensed emu76489 PSG in stereo for Game Gear.
SNG *segaPsg;

int sound_init(void)
{
  snd.fps = (sms.display == DISPLAY_NTSC) ? FPS_NTSC : FPS_PAL;
  snd.psg_clock = (sms.display == DISPLAY_NTSC) ? CLOCK_NTSC : CLOCK_PAL;
  snd.sample_rate = option.sndrate;
  if (snd.sample_rate < 8000 || snd.sample_rate > 48000) return 0;

  snd.sample_count = (snd.sample_rate / snd.fps) + 1;
  snd.buffer_size = snd.sample_count * sizeof(int16);
  snd.done_so_far = 0;
  lines_per_frame = (sms.display == DISPLAY_NTSC) ? 262 : 313;
  samples_per_line = snd.sample_count / lines_per_frame;

  for (int i = 0; i < STREAM_MAX; ++i)
  {
    snd.stream[i] = calloc(1, snd.buffer_size);
    if (!snd.stream[i]) { sound_shutdown(); return 0; }
  }
  psg_buffer = &snd.stream[STREAM_PSG_L];
  segaPsg = SNG_new(snd.psg_clock, snd.sample_rate);
  if (!segaPsg) { sound_shutdown(); return 0; }
  snd.enabled = 1;
  return 1;
}

void sound_shutdown(void)
{
  snd.enabled = 0;
  for (int i = 0; i < STREAM_MAX; ++i)
  {
    free(snd.stream[i]);
    snd.stream[i] = NULL;
  }
  if (segaPsg) SNG_delete(segaPsg);
  segaPsg = NULL;
}

void sound_reset(void)
{
  if (segaPsg) SNG_reset(segaPsg);
}

static void renderPsg(int16 **streams, int frames)
{
  for (int i = 0; i < frames; ++i)
  {
    int32_t output[2];
    SNG_calc_stereo(segaPsg, output);
    streams[0][i] = (int16_t)output[0];
    streams[1][i] = (int16_t)output[1];
  }
}

void sound_update(int line)
{
  int16 *psg[2];
  // int16 *fm[2];

  if(!snd.enabled)
    return;

  /* Finish buffers at end of frame */
  if(line == lines_per_frame - 1)
  {
    psg[0] = psg_buffer[0] + snd.done_so_far;
    psg[1] = psg_buffer[1] + snd.done_so_far;
    // fm[0]  = fm_buffer[0] + snd.done_so_far;
    // fm[1]  = fm_buffer[1] + snd.done_so_far;

    /* Generate SN76489 sample data */
    renderPsg(psg, snd.sample_count - snd.done_so_far);

#if 0
    /* Generate YM2413 sample data */
    FM_Update(fm, snd.sample_count - snd.done_so_far);
#endif

    /* Mix streams into output buffer */
    if (snd.mixer_callback)
      snd.mixer_callback(snd.stream, snd.output, snd.sample_count);

    /* Reset */
    snd.done_so_far = 0;
  }
  else
  {
    /* Do a tiny bit */
    psg[0] = psg_buffer[0] + snd.done_so_far;
    psg[1] = psg_buffer[1] + snd.done_so_far;
    // fm[0]  = fm_buffer[0] + snd.done_so_far;
    // fm[1]  = fm_buffer[1] + snd.done_so_far;

    /* Generate SN76489 sample data */
    renderPsg(psg, samples_per_line);

#if 0
    /* Generate YM2413 sample data */
    FM_Update(fm, samples_per_line);
#endif

    /* Sum total */
    snd.done_so_far += samples_per_line;
  }
}

/* Generic FM+PSG stereo mixer callback */
void sound_mixer_callback(int16 **stream, int16 **output, int length)
{
  int i;
  for(i = 0; i < length; i++)
  {
    //int16 temp = (fm_buffer[0][i] + fm_buffer[1][i]) / 2;
    //int16 temp = psg_buffer[1][i];
    output[0][i] = psg_buffer[0][i] * 2.75f;
    output[1][i] = psg_buffer[1][i] * 2.75f;
  }
}


/*--------------------------------------------------------------------------*/
/* Sound chip access handlers                                               */
/*--------------------------------------------------------------------------*/

void psg_stereo_w(int data)
{
  if(!snd.enabled) return;
  SNG_writeGGIO(segaPsg, data);
}

void stream_update(int which, int position)
{
}


void psg_write(int data)
{
  if(!snd.enabled) return;
  SNG_writeIO(segaPsg, data);
}

/*--------------------------------------------------------------------------*/
/* Mark III FM Unit / Master System (J) built-in FM handlers                */
/*--------------------------------------------------------------------------*/

int fmunit_detect_r(void)
{
  return sms.fm_detect;
}

void fmunit_detect_w(int data)
{
  if(!snd.enabled || !sms.use_fm) return;
  sms.fm_detect = data;
}

void fmunit_write(int offset, int data)
{
  if(!snd.enabled || !sms.use_fm) return;
  MESSAGE_ERROR("FM Not supported write %02x at %04x\n", data, offset);
  // FM_Write(offset, data);
}
