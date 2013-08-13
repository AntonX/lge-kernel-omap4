/*
 * Analog Headset Amplifier Control with Voodoo Sound Emulation for twl6040
 *
 * Author:	 AntonX <antonx.git@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#ifndef __TWL6040_VOODOO_H__
#define __TWL6040_VOODOO_H__

void voodoo_pcm_probe(struct snd_soc_codec *codec);
void voodoo_pcm_remove(void);

void voodoo_hook_enable_capture(bool enable);
void voodoo_hook_hpvol(void);
bool voodoo_hook_write(unsigned int reg, unsigned int * value);
void voodoo_hook_hpvol_mute(bool mute);

#endif


