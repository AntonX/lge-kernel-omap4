/*
 * Analog Headset Amplifier and Microphone Pre-Aamplifier Control with Voodoo Sound Emulation for twl6040
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

/*
 * Do not compile this file separately. Include it at the end of twl6040.c
 */

#ifdef CONFIG_TWL6040_VOODOO_SOUND

//#define DEBUG

#ifndef DEBUG_LOG
#ifdef DEBUG
#define DEBUG_LOG(format,...)\
	printk ("%s, " format "\n", __func__, ## __VA_ARGS__);
#else
#define DEBUG_LOG(format,...)
#endif
#endif

static bool voodoo_enable = false;
static struct snd_soc_codec * voodoo_codec;
static bool voodoo_hpvol_enable = true;
static bool voodoo_hpvol_mute = false;
static bool voodoo_capturing = false;
//

#define VOODOO_DEFAULT_HP_LEVEL_PRIV 	18
#define VOODOO_DEFAULT_HP_LEVEL 	(VOODOO_DEFAULT_HP_LEVEL_PRIV*2)

unsigned short voodoo_hp_level = VOODOO_DEFAULT_HP_LEVEL;
unsigned short voodoo_hp_level_priv = VOODOO_DEFAULT_HP_LEVEL_PRIV;
unsigned short voodoo_hp_abs_level;
unsigned voodoo_hp_orig_level[2];

//

#define MIC_PREAMP_GAIN_MAX		24
#define MIC_PREAMP_GAIN_STEP		6
#define MIC_PREAMP_DEFAULT_GAIN		18

static bool voodoo_mic_preamp_attn[2] = {false, false};
static int voodoo_mic_preamp_gain[2] = {-1, -1};

#define MIC_MAIN	0
#define MIC_SUB 	1

//

bool unhook_write = false;

//

static int voodoo_translate_hpvol_priv(int vol)
{
	// twl6040 operates in 0..30dB domain in 2dB steps
	vol /= 2;
	if ( vol > 0x0F ) vol = 0x0F;
	return vol;
}

static int voodoo_translate_to_hpvol_priv(int vol)
{
	// twl6040 operates in 0..30dB domain in 2dB steps
	vol *= 2;
	if ( vol > 30 ) vol = 30;
	return vol;
}

static int voodoo_translate_hpvol(int vol)
{
	// voodoo audio operates in -57..+5 = 62db domain, so do the best we can scaling
	vol /= 4;
	if ( vol > 0x0F ) vol = 0x0F;
	return vol;
}

static int voodoo_translate_to_hpvol(int vol)
{
	vol *= 4;
	if ( vol > 62 ) vol = 62;
	return vol;
}

static void voodoo_write_volume(int volume_left, int volume_right, bool fade)
{
	u8 reg, val_l, val_r, cur_val_l, cur_val_r;
	u8 step_l, step_r;
	bool done_l = false, done_r = false;
	int i;

	val_l = volume_left & 0x0F;	
	val_r = volume_right & 0x0F;	

	reg = twl6040_read_reg_cache(voodoo_codec, TWL6040_REG_HSGAIN);
  	
  	cur_val_l = (~reg & TWL6040_HSL_VOL_MASK) >> TWL6040_HSL_VOL_SHIFT;
  	cur_val_r = (~reg & TWL6040_HSR_VOL_MASK) >> TWL6040_HSR_VOL_SHIFT;

	if (val_l == cur_val_l && val_r == cur_val_r) return;
	
	if ( fade ) {
		step_l = val_l > cur_val_l ? +1 : -1;
		step_r = val_r > cur_val_r ? +1 : -1;

		for ( i = 0; i < 16 && ! (done_l && done_r); ++i) {
			DEBUG_LOG("fading, val: %u/%u, cur_val: %u/%u", (unsigned) val_l, (unsigned) val_r, (unsigned) cur_val_l, (unsigned) cur_val_r);
			if (cur_val_l != val_l) cur_val_l += step_l; else done_l = true;	
			if (cur_val_r != val_r) cur_val_r += step_r; else done_r = true;	
			reg &= (u8) ~(TWL6040_HSL_VOL_MASK | TWL6040_HSR_VOL_MASK);
			twl6040_write(voodoo_codec, TWL6040_REG_HSGAIN, reg | 
		                     	((~cur_val_l << TWL6040_HSL_VOL_SHIFT) & TWL6040_HSL_VOL_MASK) | 
		                     	((~cur_val_r << TWL6040_HSR_VOL_SHIFT) & TWL6040_HSR_VOL_MASK));
			usleep_range(1000,1500);
		}
	} else {
		DEBUG_LOG("no fading, val: %u/%u", (unsigned) val_l, (unsigned) val_r);
		reg &= (u8) ~(TWL6040_HSL_VOL_MASK | TWL6040_HSR_VOL_MASK);
		twl6040_write(voodoo_codec, TWL6040_REG_HSGAIN, reg | 
					((~val_l << TWL6040_HSL_VOL_SHIFT) & TWL6040_HSL_VOL_MASK) | 
					((~val_r << TWL6040_HSR_VOL_SHIFT) & TWL6040_HSR_VOL_MASK));
	}
}

void voodoo_enable_hpvol(bool enable)
{
	// disable custom amp gain when capture stream is active assuming we are in a call

	struct twl6040_data *priv;
	struct twl6040_output *headset;
  
        if (enable == voodoo_hpvol_enable) return;

	priv = snd_soc_codec_get_drvdata(voodoo_codec);
	headset = &priv->headset;

	if (enable) {
		DEBUG_LOG("hpvol enabled, vol: %d", voodoo_hp_abs_level );
		headset->left_vol = headset->right_vol = voodoo_hp_abs_level;
	} else {
		DEBUG_LOG("hpvol disabled, vol: %d/%d", voodoo_hp_orig_level[0], voodoo_hp_orig_level[1] );
		headset->left_vol = voodoo_hp_orig_level[0]; 
		headset->right_vol = voodoo_hp_orig_level[1];
	}
	voodoo_hpvol_enable = enable;

	if (headset->active && !voodoo_hpvol_mute) {
  		voodoo_write_volume(headset->left_vol, headset->right_vol, false);
  	}
}

void voodoo_hook_enable_capture(bool enable)
{
	DEBUG_LOG("enable: %d", (int) enable);
	voodoo_capturing = enable;
	voodoo_enable_hpvol(!enable);
}

void voodoo_hook_hpvol(void)
{
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(voodoo_codec);
	struct twl6040_output *headset = &priv->headset;

	voodoo_hp_orig_level[0] = headset->left_vol;
	voodoo_hp_orig_level[1] = headset->right_vol;

	voodoo_hpvol_mute = false;

	DEBUG_LOG("level: %d, enabled: %d", voodoo_hp_abs_level, (int) voodoo_hpvol_enable);

	if (!voodoo_hpvol_enable) return;

	headset->left_vol = headset->right_vol = voodoo_hp_abs_level;	
} 

void voodoo_hook_hpvol_mute(bool mute)
{
	DEBUG_LOG("mute: %d", (int) mute);

	voodoo_hpvol_mute = mute;
}

static unsigned int voodoo_set_mic_gain_bits(unsigned int val)
{
       	unsigned short gain, mask, shift;         	

       	// preamp 6dB attenuation, 1 bit, shifted 6/7 for main/sub mics
       	
       	shift = 6;
       	mask = 3;
       	gain = 0;

       	if (voodoo_mic_preamp_attn[0]) gain |= 1;
       	if (voodoo_mic_preamp_attn[1]) gain |= 2;

       	val &= ~(mask << shift);
       	val |= (gain << shift);

       	// preamp gains, 3 bits, shifted 0/3 for main/sub mics
       	
       	shift = 3;
       	mask = 7;
       	
       	if (voodoo_mic_preamp_gain[0] >= 0) {
	       	gain = (voodoo_mic_preamp_gain[0] / MIC_PREAMP_GAIN_STEP); // << 0
	       	if (gain > MIC_PREAMP_GAIN_MAX / MIC_PREAMP_GAIN_STEP) gain = MIC_PREAMP_GAIN_MAX / MIC_PREAMP_GAIN_STEP;
	       	val &= ~mask; // << 0
       		val |= gain; // << 0
	}

       	if (voodoo_mic_preamp_gain[1] >= 0) {
       		gain = (voodoo_mic_preamp_gain[1] / MIC_PREAMP_GAIN_STEP);
       		if (gain > MIC_PREAMP_GAIN_MAX / MIC_PREAMP_GAIN_STEP) gain = MIC_PREAMP_GAIN_MAX / MIC_PREAMP_GAIN_STEP;
       		val &= ~(mask << shift);
       		val |= (gain << shift);
	}

       	return val;
}

static void voodoo_update_mic_gain(void)
{
	unsigned int v, newv;

	v = twl6040_read_reg_cache(voodoo_codec, TWL6040_REG_MICGAIN);
	newv = voodoo_set_mic_gain_bits(v);

	if (newv != v) {
		unhook_write = true;
	
		twl6040_write(voodoo_codec, TWL6040_REG_MICGAIN, newv);

		unhook_write = false;

		DEBUG_LOG("val: 0x%0X, newval: 0x%0X", v, newv);
	}
}

bool voodoo_hook_write(unsigned int reg, unsigned int * value)
{
	unsigned int v;

	if (unhook_write) return true;

	switch (reg) {
		case TWL6040_REG_MICGAIN :
			if (voodoo_mic_preamp_gain >= 0) {
				v = voodoo_set_mic_gain_bits(*value);
				DEBUG_LOG("TWL6040_REG_MICGAIN, old value: 0x%0X, new value: 0x%0X, gain: %d/%d", *value, v, (v>>3)&7, v&7);
				*value = v;
			} else
				DEBUG_LOG("TWL6040_REG_MICGAIN, ignoring, value: 0x%0X, gain: %d/%d", *value, (*value>>3)&7, *value&7);
		break;
	}

	return true;
}

static void voodoo_update_hpvol(int vol)
{
	struct twl6040_data *priv = snd_soc_codec_get_drvdata(voodoo_codec);
	struct twl6040_output *headset = &priv->headset;

	DEBUG_LOG("vol: %d", vol );

	if (vol == voodoo_hp_abs_level) {
		DEBUG_LOG("same volume %d, bailing", (int) voodoo_hp_abs_level );
		return;
	}

	voodoo_hp_abs_level = vol;

	if (!voodoo_hpvol_enable) return;

	headset->left_vol = headset->right_vol = voodoo_hp_abs_level;

	if (headset->active && !voodoo_hpvol_mute)
		voodoo_write_volume(voodoo_hp_abs_level, voodoo_hp_abs_level, true);
}

//

#define DECLARE_BOOL_SHOW(name) 					       \
static ssize_t name##_show(struct device *dev, struct device_attribute *attr, char *buf) \
{									                 \
	return sprintf(buf,"%u\n",(name ? 1 : 0));			                 \
}

static ssize_t voodoo_headphone_amplifier_level_show(struct device *dev,
					             struct device_attribute *attr,
					             char *buf)
{
	return sprintf(buf, "%u\n", voodoo_hp_level);
}

static ssize_t voodoo_headphone_amplifier_level_store(struct device *dev,
					              struct device_attribute *attr,
					              const char *buf, size_t size)
{
	unsigned short vol;
	if (sscanf(buf, "%hu", &vol) == 1) {

		if (vol < 0) vol = 0;
		if (vol > 62) vol = 62;
		
		voodoo_hp_level = vol;

		vol = voodoo_translate_hpvol(vol);

		voodoo_hp_level_priv = voodoo_translate_to_hpvol_priv(vol);

		voodoo_update_hpvol(vol);
	}
	return size;
}

static ssize_t voodoo_headphone_amplifier_level_show_priv(struct device *dev,
					                  struct device_attribute *attr,
					                  char *buf)
{
	return sprintf(buf, "%u\n", voodoo_hp_level_priv);
}

static ssize_t voodoo_headphone_amplifier_level_store_priv(struct device *dev,
					                   struct device_attribute *attr,
					                   const char *buf, size_t size)
{
	unsigned short vol;
	if (sscanf(buf, "%hu", &vol) == 1) {

		if (vol < 0) vol = 0;
		if (vol > 30) vol = 30;

		voodoo_hp_level_priv = vol;

		vol = voodoo_translate_hpvol_priv(vol);

		voodoo_hp_level = voodoo_translate_to_hpvol(vol);

		voodoo_update_hpvol(vol);
	}
	return size;
}

static ssize_t voodoo_mic_preamp_attn_main_show(struct device *dev,
					                  struct device_attribute *attr,
					                  char *buf)
{
	return sprintf(buf, "%d\n", (int) voodoo_mic_preamp_attn[MIC_MAIN]);
}

static ssize_t voodoo_mic_preamp_attn_sub_show(struct device *dev,
					                  struct device_attribute *attr,
					                  char *buf)
{
	return sprintf(buf, "%d\n", (int) voodoo_mic_preamp_attn[MIC_SUB]);
}

static void voodoo_mic_preamp_attn_store_ex(int mic, int val)
{
	voodoo_mic_preamp_attn[mic] = val != 0;

	if (voodoo_mic_preamp_gain[mic] < 0) voodoo_mic_preamp_attn[mic] = false;

	DEBUG_LOG("attenuation[%d]: %d", mic, (int) voodoo_mic_preamp_attn[mic]);

	voodoo_update_mic_gain();
}

static ssize_t voodoo_mic_preamp_attn_main_store(struct device *dev,
					                   struct device_attribute *attr,
					                   const char *buf, size_t size)
{
	unsigned short val;
	if (sscanf(buf, "%hu", &val) == 1)
		voodoo_mic_preamp_attn_store_ex(MIC_MAIN, val);
	return size;
}

static ssize_t voodoo_mic_preamp_attn_sub_store(struct device *dev,
					                   struct device_attribute *attr,
					                   const char *buf, size_t size)
{
	unsigned short val;
	if (sscanf(buf, "%hu", &val) == 1)
		voodoo_mic_preamp_attn_store_ex(MIC_SUB, val);
	return size;
}


static ssize_t voodoo_mic_preamp_gain_main_show(struct device *dev,
					                  struct device_attribute *attr,
					                  char *buf)
{
	return sprintf(buf, "%d\n", voodoo_mic_preamp_gain[MIC_MAIN]);
}

static ssize_t voodoo_mic_preamp_gain_sub_show(struct device *dev,
					                  struct device_attribute *attr,
					                  char *buf)
{
	return sprintf(buf, "%d\n", voodoo_mic_preamp_gain[MIC_SUB]);
}

static void voodoo_mic_preamp_gain_store_ex(int mic, int val)
{

	if (val < 0) val = -1; else if (val > MIC_PREAMP_GAIN_MAX) val = MIC_PREAMP_GAIN_MAX;
	else val = (val / MIC_PREAMP_GAIN_STEP) * MIC_PREAMP_GAIN_STEP;
	
	voodoo_mic_preamp_gain[mic] = val;

	if (voodoo_mic_preamp_gain[mic] < 0) voodoo_mic_preamp_attn[mic] = false;

	DEBUG_LOG("gain[%d]: %d, attn: %d", mic, voodoo_mic_preamp_gain[mic], (int) voodoo_mic_preamp_attn[mic]);

	voodoo_update_mic_gain();
}

static ssize_t voodoo_mic_preamp_gain_main_store(struct device *dev,
					                   struct device_attribute *attr,
					                   const char *buf, size_t size)
{
	int val;
	if (sscanf(buf, "%d", &val) == 1)
		voodoo_mic_preamp_gain_store_ex(MIC_MAIN, val);
	return size;
}

static ssize_t voodoo_mic_preamp_gain_sub_store(struct device *dev,
					                   struct device_attribute *attr,
					                   const char *buf, size_t size)
{
	int val;
	if (sscanf(buf, "%d", &val) == 1)
		voodoo_mic_preamp_gain_store_ex(MIC_SUB, val);
	return size;
}

#define VOODOO_SOUND_VERSION 10

static ssize_t voodoo_sound_version(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", VOODOO_SOUND_VERSION);
}

//

DECLARE_BOOL_SHOW(voodoo_enable);

static void voodoo_update_enable(void);

static ssize_t voodoo_enable_store(struct device *dev,
			           struct device_attribute *attr, const char *buf,
			           size_t size)
{
	unsigned short state;
	bool bool_state;
	if (sscanf(buf, "%hu", &state) == 1) {
		bool_state = state == 0 ? false : true;
		if (state != voodoo_enable) {
			voodoo_enable = bool_state;
			voodoo_update_enable();
		}
	}
	return size;
}

//

static DEVICE_ATTR(headphone_amplifier_level, S_IRUGO | S_IWUGO,
		   voodoo_headphone_amplifier_level_show,
		   voodoo_headphone_amplifier_level_store);

static DEVICE_ATTR(twl6040_headphone_amplifier_level, S_IRUGO | S_IWUGO,
		   voodoo_headphone_amplifier_level_show_priv,
		   voodoo_headphone_amplifier_level_store_priv);

static DEVICE_ATTR(twl6040_mic_preamp_attn_main, S_IRUGO | S_IWUGO,
		   voodoo_mic_preamp_attn_main_show,
		   voodoo_mic_preamp_attn_main_store);

static DEVICE_ATTR(twl6040_mic_preamp_attn_sub, S_IRUGO | S_IWUGO,
		   voodoo_mic_preamp_attn_sub_show,
		   voodoo_mic_preamp_attn_sub_store);

static DEVICE_ATTR(twl6040_mic_preamp_gain_main, S_IRUGO | S_IWUGO,
		   voodoo_mic_preamp_gain_main_show,
		   voodoo_mic_preamp_gain_main_store);

static DEVICE_ATTR(twl6040_mic_preamp_gain_sub, S_IRUGO | S_IWUGO,
		   voodoo_mic_preamp_gain_sub_show,
		   voodoo_mic_preamp_gain_sub_store);

static DEVICE_ATTR(version, S_IRUGO, voodoo_sound_version, NULL);

static DEVICE_ATTR(enable, S_IRUGO | S_IWUGO, voodoo_enable_show, voodoo_enable_store);

//

static struct attribute *voodoo_sound_attributes[] = {
	&dev_attr_headphone_amplifier_level.attr,
	&dev_attr_version.attr,
	&dev_attr_twl6040_headphone_amplifier_level.attr,
	&dev_attr_twl6040_mic_preamp_attn_main.attr,
	&dev_attr_twl6040_mic_preamp_attn_sub.attr,
	&dev_attr_twl6040_mic_preamp_gain_main.attr,
	&dev_attr_twl6040_mic_preamp_gain_sub.attr,
	NULL
};

static struct attribute *voodoo_sound_control_attributes[] = {
	&dev_attr_enable.attr,
	NULL
};

static struct attribute_group voodoo_sound_group = {
	.attrs = voodoo_sound_attributes,
};

static struct attribute_group voodoo_sound_control_group = {
	.attrs = voodoo_sound_control_attributes,
};

static struct miscdevice voodoo_sound_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "voodoo_sound",
};

static struct miscdevice voodoo_sound_control_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "voodoo_sound_control",
};

//

void voodoo_pcm_remove()
{
	printk("Voodoo sound: removing driver v%d\n", VOODOO_SOUND_VERSION);
	sysfs_remove_group(&voodoo_sound_device.this_device->kobj, &voodoo_sound_group);
	misc_deregister(&voodoo_sound_device);
}

static void voodoo_update_enable(void)
{
	if (voodoo_enable) {
		printk("Voodoo sound: initializing driver v%d\n", VOODOO_SOUND_VERSION);

		misc_register(&voodoo_sound_device);
		if (sysfs_create_group(&voodoo_sound_device.this_device->kobj, &voodoo_sound_group) < 0) {
			printk("%s sysfs_create_group fail\n", __FUNCTION__);
			pr_err("Failed to create sysfs group for (%s)!\n", voodoo_sound_device.name);
		}
	} else
		voodoo_pcm_remove();
}

void voodoo_pcm_probe(struct snd_soc_codec *codec)
{
	voodoo_enable = true;
	voodoo_update_enable();

	voodoo_hp_abs_level = voodoo_translate_hpvol_priv(voodoo_hp_level_priv);

	misc_register(&voodoo_sound_control_device);
	if (sysfs_create_group(&voodoo_sound_control_device.this_device->kobj, &voodoo_sound_control_group) < 0) {
		printk("%s sysfs_create_group fail\n", __FUNCTION__);
		pr_err("Failed to create sysfs group for device (%s)!\n",
		       voodoo_sound_control_device.name);
	}

	voodoo_codec = codec;
}

#endif //CONFIG_TWL6040_VOODOO_SOUND
