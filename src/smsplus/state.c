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
 *   Nintendo Gamecube State Management
 *
 ******************************************************************************/

#include "shared.h"

int system_save_state(void *mem)
{
  uint8 padding[16] = {0};
  int i;

  /*** Save SMS Context ***/
  fwrite(sms.wram, 0x2000, 1, mem);
  fwrite(&sms.paused, 1, 1, mem);
  fwrite(&sms.save, 1, 1, mem);
  fwrite(&sms.territory, 1, 1, mem);
  fwrite(&sms.console, 1, 1, mem);
  fwrite(&sms.display, 1, 1, mem);
  fwrite(&sms.fm_detect, 1, 1, mem);
  fwrite(&sms.glasses_3d, 1, 1, mem);
  fwrite(&sms.hlatch, 1, 1, mem);
  fwrite(&sms.use_fm, 1, 1, mem);
  fwrite(&sms.memctrl, 1, 1, mem);
  fwrite(&sms.ioctrl, 1, 1, mem);
  fwrite(&padding, 1, 1, mem);
  fwrite(&sms.sio, 8, 1, mem);
  fwrite(&sms.device, 2, 1, mem);
  fwrite(&sms.gun_offset, 1, 1, mem);
  fwrite(&padding, 1, 1, mem);

  /*** Save VDP state ***/
  fwrite(vdp.vram, 0x4000, 1, mem);
  fwrite(vdp.cram, 0x40, 1, mem);
  fwrite(vdp.reg, 0x10, 1, mem);
  fwrite(&vdp.vscroll, 1, 1, mem);
  fwrite(&vdp.status, 1, 1, mem);
  fwrite(&vdp.latch, 1, 1, mem);
  fwrite(&vdp.pending, 1, 1, mem);
  fwrite(&vdp.addr, 2, 1, mem);
  fwrite(&vdp.code, 1, 1, mem);
  fwrite(&vdp.buffer, 1, 1, mem);
  fwrite(&vdp.pn, 4, 1, mem);
  fwrite(&vdp.ct, 4, 1, mem);
  fwrite(&vdp.pg, 4, 1, mem);
  fwrite(&vdp.sa, 4, 1, mem);
  fwrite(&vdp.sg, 4, 1, mem);
  fwrite(&vdp.ntab, 4, 1, mem);
  fwrite(&vdp.satb, 4, 1, mem);
  fwrite(&vdp.line, 4, 1, mem);
  fwrite(&vdp.left, 4, 1, mem);
  fwrite(&vdp.lpf, 2, 1, mem);
  fwrite(&vdp.height, 1, 1, mem);
  fwrite(&vdp.extended, 1, 1, mem);
  fwrite(&vdp.mode, 1, 1, mem);
  fwrite(&vdp.irq, 1, 1, mem);
  fwrite(&vdp.vint_pending, 1, 1, mem);
  fwrite(&vdp.hint_pending, 1, 1, mem);
  fwrite(&vdp.cram_latch, 2, 1, mem);
  fwrite(&vdp.spr_col, 2, 1, mem);
  fwrite(&vdp.spr_ovr, 1, 1, mem);
  fwrite(&vdp.bd, 1, 1, mem);
  fwrite(&padding, 2, 1, mem);

  /*** Save cart info ***/
  for (i = 0; i < 4; i++)
  {
    fwrite(&cart.fcr[i], 1, 1, mem);
  }

  /*** Save SRAM ***/
  fwrite(cart.sram, 0x8000, 1, mem);

  if (!segaCpuWrite(mem)) return -1;
  fwrite(segaPsg, sizeof(*segaPsg), 1, mem);

  fwrite(&coleco.pio_mode, 1, 1, mem);
  fwrite(&coleco.port53, 1, 1, mem);
  fwrite(&coleco.port7F, 1, 1, mem);
  fwrite(&padding, 5, 1, mem);

  return 0;
}


bool system_load_state(void *mem)
{
  uint8 padding[16] = {0};
  int i;

  /* Initialize everything */
  system_reset();

  /*** Set SMS Context ***/
  const int current_console = sms.console;
  const int current_display = sms.display;
  sms.console = 0xFF;

  fread(sms.wram, 0x2000, 1, mem);
  fread(&sms.paused, 1, 1, mem);
  fread(&sms.save, 1, 1, mem);
  fread(&sms.territory, 1, 1, mem);
  fread(&sms.console, 1, 1, mem);
  fread(&sms.display, 1, 1, mem);
  fread(&sms.fm_detect, 1, 1, mem);
  fread(&sms.glasses_3d, 1, 1, mem);
  fread(&sms.hlatch, 1, 1, mem);
  fread(&sms.use_fm, 1, 1, mem);
  fread(&sms.memctrl, 1, 1, mem);
  fread(&sms.ioctrl, 1, 1, mem);
  fread(&padding, 1, 1, mem);
  fread(&sms.sio, 8, 1, mem);
  fread(&sms.device, 2, 1, mem);
  fread(&sms.gun_offset, 1, 1, mem);
  fread(&padding, 1, 1, mem);

  if (sms.console != current_console || sms.display != current_display)
  {
      MESSAGE_ERROR("Bad save data\n");
      sms.console = current_console;
      sms.display = current_display;
      system_reset();
      return false;
  }

  /*** Set vdp state ***/
  fread(vdp.vram, 0x4000, 1, mem);
  fread(vdp.cram, 0x40, 1, mem);
  fread(vdp.reg, 0x10, 1, mem);
  fread(&vdp.vscroll, 1, 1, mem);
  fread(&vdp.status, 1, 1, mem);
  fread(&vdp.latch, 1, 1, mem);
  fread(&vdp.pending, 1, 1, mem);
  fread(&vdp.addr, 2, 1, mem);
  fread(&vdp.code, 1, 1, mem);
  fread(&vdp.buffer, 1, 1, mem);
  fread(&vdp.pn, 4, 1, mem);
  fread(&vdp.ct, 4, 1, mem);
  fread(&vdp.pg, 4, 1, mem);
  fread(&vdp.sa, 4, 1, mem);
  fread(&vdp.sg, 4, 1, mem);
  fread(&vdp.ntab, 4, 1, mem);
  fread(&vdp.satb, 4, 1, mem);
  fread(&vdp.line, 4, 1, mem);
  fread(&vdp.left, 4, 1, mem);
  fread(&vdp.lpf, 2, 1, mem);
  fread(&vdp.height, 1, 1, mem);
  fread(&vdp.extended, 1, 1, mem);
  fread(&vdp.mode, 1, 1, mem);
  fread(&vdp.irq, 1, 1, mem);
  fread(&vdp.vint_pending, 1, 1, mem);
  fread(&vdp.hint_pending, 1, 1, mem);
  fread(&vdp.cram_latch, 2, 1, mem);
  fread(&vdp.spr_col, 2, 1, mem);
  fread(&vdp.spr_ovr, 1, 1, mem);
  fread(&vdp.bd, 1, 1, mem);
  fread(&padding, 2, 1, mem);

  // Rebuild viewport dimensions without reallocating the audio streams.
  if (!vdp_init()) return false;

  /*** Set cart info ***/
  for (i = 0; i < 4; i++)
  {
    fread(&cart.fcr[i], 1, 1, mem);
  }

  /*** Set SRAM ***/
  fread(cart.sram, 0x8000, 1, mem);

  if (!segaCpuRead(mem) || fread(segaPsg, sizeof(*segaPsg), 1, mem) != 1) return false;

  fread(&coleco.pio_mode, 1, 1, mem);
  fread(&coleco.port53, 1, 1, mem);
  fread(&coleco.port7F, 1, 1, mem);
  fread(&padding, 5, 1, mem);

  if (sms.console == CONSOLE_COLECO)
  {
    coleco_port_w(0x53, coleco.port53);
    coleco_port_w(0x7F, coleco.port7F);
  }
  else if (sms.console != CONSOLE_SG1000)
  {
    /* Cartridge by default */
    slot.rom    = cart.rom;
    slot.pages  = cart.pages;
    slot.mapper = cart.mapper;
    slot.fcr = &cart.fcr[0];

    /* Restore mapping */
    mapper_reset();
    cpu_readmap[0]  = &slot.rom[0];
    if (slot.mapper != MAPPER_KOREA_MSX)
    {
      mapper_16k_w(0,slot.fcr[0]);
      mapper_16k_w(1,slot.fcr[1]);
      mapper_16k_w(2,slot.fcr[2]);
      mapper_16k_w(3,slot.fcr[3]);
    }
    else
    {
      mapper_8k_w(0,slot.fcr[0]);
      mapper_8k_w(1,slot.fcr[1]);
      mapper_8k_w(2,slot.fcr[2]);
      mapper_8k_w(3,slot.fcr[3]);
    }
  }

  // /* Force full pattern cache update */
  // bg_list_index = 0x200;
  // for(i = 0; i < 0x200; i++)
  // {
  //   bg_name_list[i] = i;
  //   bg_name_dirty[i] = -1;
  // }

  /* Restore palette */
  for(i = 0; i < PALETTE_SIZE; i++)
    palette_sync(i);
  return !ferror(mem) && !feof(mem);
}
