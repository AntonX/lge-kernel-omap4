/*
 * BLN extension for leds-keypad-gpio.c
 *
 * Adaptation of neldar's code.
 *
 * Author: AntonX <antonx.git@gmail.com>
 *
 */
 
/*
 * Do not compile this file separately. Include it at the end of leds-keypad-gpio.c.
 */

/*
 * WARNING: 
 * 
 * We cannot keep the LEDs lit during suspend because GPIO power goes down.
 * We need to hold a wakelock when we want LEDs to be on.
 * This BLN implementation only allows short LED pulses to conserve the battery.
 *
 * In order to turn LEDs during suspend we wake the phone with 
 * alarm and hold a wakelock only until a short pulse is finihed. 
 * This is more battery friendly that holding a wakelock all the time.
 *
 * Native BLN-style blinking is not supported.
 *
 */

#ifdef CONFIG_SUPPORT_BLN

//#define DEBUG

#ifdef DEBUG
#define DEBUG_LOG(format,...) \
	printk("%s, " format "\n", __func__, ## __VA_ARGS__);
#else
#define DEBUG_LOG(format,...)
#endif

#define BLN_VERSION 9

static struct led_classdev *bln_keypad_dev;

static bool bln_enabled = false;
static bool bln_on = false; // notificatin active and lights on
static bool bln_ongoing = false; // notificatin active

static bool bln_touch_led_enabled = true;

static bool bln_suspended = false;
static bool bln_unhook = false;

/* 
 * Do not use LED touch timeout now, it does not work with CM 10.1.
 * By default the timeout is hard coded in Android as 5 seconds. 
 * After the lights are turned on the next event will not come until this 
 * timeout is finished. If we turn the lghts off in the driver sooner they 
 * will not be turned back upon touch until previous 5s timeout has 
 * passed because Android thinks the ligts are still on.
 */

//#define USE_TOUCH_TIMEOUT

#ifdef USE_TOUCH_TIMEOUT
static unsigned int bln_timeout = 0;
static struct timer_list bln_timer;
#endif

// timeout

static ktime_t bln_end_time;
static int bln_notification_timeout = 0; // in minutes

// blinking

static struct alarm bln_blink_alarm;
static struct wake_lock bln_blink_wakelock;
static struct workqueue_struct *bln_blink_workqueue;
static struct delayed_work bln_blink_work;
static bool bln_blink_on = false;

static unsigned int bln_blink_period_off = 7000; 
static unsigned int bln_blink_period_on = 40;
static unsigned int bln_blink_strobe_count = 0;
static unsigned int bln_blink_strobe_off = 40;
static bool bln_blink_keep = false;

//

static void bln_write_led(bool on)
{
	DEBUG_LOG("%s", on ? "on" : "off");
	
	bln_unhook = true;	

	keypad_led_store(bln_keypad_dev, (enum led_brightness) (on ? 255 : 0));

	bln_unhook = false;
}

#ifdef USE_TOUCH_TIMEOUT
static void bln_set_timeout(void)
{
	if (bln_timeout) {
		DEBUG_LOG("%dms", bln_timeout);
		mod_timer(&bln_timer, jiffies + msecs_to_jiffies(bln_timeout)); 
	}
}
#endif

int bln_hook_led_write(int value)
{
	int new_value;

	if (bln_unhook) return value;

#ifdef USE_TOUCH_TIMEOUT
	if (value && !bln_on && bln_touch_led_enabled && bln_timeout) 
		bln_set_timeout();
#endif

	if (value) 
		new_value = (bln_on || bln_touch_led_enabled) ? value : 0;
	else
		new_value = bln_on ? 255 : 0;

  return new_value;
}

static inline int _ktime_compare(ktime_t lhs, ktime_t rhs)
{
	if (lhs.tv64 < rhs.tv64) return -1;
	if (lhs.tv64 > rhs.tv64) return 1;
	return 0;
}

static void bln_init_timeout(void)
{
	if (bln_enabled && bln_notification_timeout && !bln_ongoing) {
		DEBUG_LOG("seconds: %u, bln_on: %d", bln_notification_timeout, bln_on);
		bln_end_time = ktime_add(alarm_get_elapsed_realtime(), ktime_set(bln_notification_timeout * 60, 0));
	} else
    DEBUG_LOG("bailing: enabled: %d, timeout: %d, ongoing: %d", bln_enabled, bln_notification_timeout, bln_ongoing);
}

static bool bln_timeout_expired(void)
{
	if (bln_notification_timeout && _ktime_compare(alarm_get_elapsed_realtime(), bln_end_time) >= 0) {
		printk("bln: ending by timeout\n");
		bln_on = false;
		return true;
	}
  DEBUG_LOG("remain: %lldns", ktime_sub(bln_end_time, alarm_get_elapsed_realtime()).tv64);
	return false;
}

static bool bln_start_blinking_ex(bool start, unsigned int timeout_ms)
{
	DEBUG_LOG("start: %d, bln_enabled: %d, bln_on: %d", start, bln_enabled, bln_on);

	if (start) {
		ktime_t delay;
		if (!bln_enabled || !bln_on) return false;
		bln_blink_on = true;
		delay = ktime_add(alarm_get_elapsed_realtime(), ktime_set(timeout_ms/1000, 0));
		alarm_start_range(&bln_blink_alarm, delay, delay);
		return true;
	} else {
		bln_blink_on = false;
		alarm_try_to_cancel(&bln_blink_alarm);
		cancel_delayed_work(&bln_blink_work);
		if (wake_lock_active(&bln_blink_wakelock)) wake_unlock(&bln_blink_wakelock);
		return false;
	}
}

static bool bln_start_blinking(bool start)
{
	return bln_start_blinking_ex(start, bln_blink_period_off);
}

static void bln_blink_queue_handler(struct work_struct *work)
{
	if (!(bln_enabled && bln_on && bln_blink_on && !bln_timeout_expired())) {
		if (wake_lock_active(&bln_blink_wakelock)) wake_unlock(&bln_blink_wakelock);
		return;
	}

	if ( bln_blink_strobe_count > 1 ) {
		int i;
		int period = bln_blink_period_on / bln_blink_strobe_count;
		if (period < 5) period = 5;

		for (i = 0; i < bln_blink_strobe_count; ++i) {
			bln_write_led(true);
			mdelay(period);
			if (i<bln_blink_strobe_count-1) {
				bln_write_led(false);
				mdelay(bln_blink_strobe_off);
			} else
				if (!bln_blink_keep) bln_write_led(false);
		}
	} else {	
		bln_write_led(true);
		mdelay(bln_blink_period_on);
		if (!bln_blink_keep) bln_write_led(false);
	}

	if (bln_enabled && bln_on && bln_blink_on) 
		bln_start_blinking(true);
	    
	wake_unlock(&bln_blink_wakelock);
}

static void bln_blink_alarm_handler(struct alarm *alarm)
{
	DEBUG_LOG("bln_enabled: %d, bln_on: %d, bln_blink_on: %d", bln_enabled, bln_on, bln_blink_on);
	
	if (!bln_enabled || !bln_on) return;

	wake_lock(&bln_blink_wakelock);
	
	queue_delayed_work(bln_blink_workqueue, &bln_blink_work, msecs_to_jiffies(1));
}

#ifdef USE_TOUCH_TIMEOUT
static void bln_timer_callback(unsigned long data)
{
	DEBUG_LOG("timer event, led off");
	bln_write_led(false);
}
#endif

static void bln_enable_notifications(bool on)
{
	DEBUG_LOG("%d, starting: %d", on, bln_enabled && bln_suspended);

	if (on) {
		if (!bln_enabled || !bln_suspended) return;
		bln_on = true;
		bln_init_timeout();
		bln_start_blinking_ex(true, 500); // start blinking and blink early first time
    bln_ongoing = true;
	} else {
		bln_on = false;
    bln_ongoing = false;
		bln_start_blinking(false);
		if (bln_suspended && bln_on) bln_write_led(false);
	}
}

static void bln_early_suspend(struct early_suspend *h)
{
	DEBUG_LOG("suspended, bln_enabled: %d, bln_on: %d", bln_enabled, bln_on);

	bln_suspended = true;

	//bln_start_blinking(true);
	//bln_init_timeout();
}

static void bln_late_resume(struct early_suspend *h)
{
	DEBUG_LOG("resumed");

	bln_suspended = false;
	bln_on = false;

	bln_start_blinking(false);
}

static struct early_suspend bln_suspend_data = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1,
	.suspend = bln_early_suspend,
	.resume = bln_late_resume,
};

static ssize_t bln_version(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", BLN_VERSION);
}

static ssize_t bln_notification_read(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf,"%u\n", (bln_on ? 1 : 0));
}

static ssize_t bln_notification_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int data;

	if (sscanf(buf, "%u\n", &data) != 1) {
		pr_info("%s: input error\n", __FUNCTION__);
		return size;
	}

	DEBUG_LOG("data: %u", data);
	
	bln_enable_notifications(data != 0);

	return size;
}

static ssize_t bln_notification_timeout_read(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf,"%u\n", bln_notification_timeout);
}

static ssize_t bln_notification_timeout_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int data;

	if (sscanf(buf, "%u\n", &data) != 1) {
		pr_info("%s: input error\n", __FUNCTION__);
		return size;
	}

	DEBUG_LOG("data: %u", data);
	
	bln_notification_timeout = data;
	bln_init_timeout();

	return size;
}

static ssize_t bln_blink_period_off_read(struct device *dev, struct device_attribute *attr, char *buf )
{
        return sprintf( buf, "%u\n", bln_blink_period_off);
}

static ssize_t bln_blink_period_off_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int count = sscanf(buf, "%u\n", &bln_blink_period_off);

	DEBUG_LOG("data: %u\n", bln_blink_period_off);
	
	if (count == 1) {
		if (bln_blink_period_off < 1000) bln_blink_period_off = 1000; 
		if (bln_blink_period_off > 10000) bln_blink_period_off = 10000;
        }
        return size;
}

static ssize_t bln_blink_period_on_read(struct device *dev, struct device_attribute *attr, char *buf )
{
        return sprintf( buf, "%u\n", bln_blink_period_on);
}

static ssize_t bln_blink_period_on_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int count = sscanf(buf, "%u\n", &bln_blink_period_on);

	DEBUG_LOG("data: %u\n", bln_blink_period_on);
	
	if (count == 1) {
		if (bln_blink_period_on < 10) bln_blink_period_on = 10; 
		if (bln_blink_period_on > 1000) bln_blink_period_on = 1000;
        }
        return size;
}


static ssize_t bln_blink_strobe_read(struct device *dev, struct device_attribute *attr, char *buf )
{
        return sprintf( buf, "%u %u\n", bln_blink_strobe_count, bln_blink_strobe_off);
}

static ssize_t bln_blink_strobe_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int count = sscanf(buf, "%u %u\n", &bln_blink_strobe_count, &bln_blink_strobe_off );
	
	DEBUG_LOG("data: %u %u\n", bln_blink_strobe_count, bln_blink_strobe_off);
	
	if (count >= 1) {
		if (bln_blink_strobe_count > 10) bln_blink_strobe_count = 10;
	}
	if (count >= 2) {
		if (bln_blink_strobe_off > 100) bln_blink_strobe_off = 100;
        }
        return size;
}

static ssize_t bln_blink_keep_read(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", bln_blink_keep ? 1 : 0);
}

static ssize_t bln_blink_keep_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int data;

	if (sscanf(buf, "%u\n", &data) == 1) {
		DEBUG_LOG("data: %u", data);
		bln_blink_keep = data != 0;
	}
	return size;
}

static ssize_t bln_enabled_read(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", bln_enabled ? 1 : 0);
}

static ssize_t bln_enabled_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int data;

	if (sscanf(buf, "%u\n", &data) != 1) {
		pr_info("%s: input error\n", __FUNCTION__);
		return size;
	}

	DEBUG_LOG("data: %u", data);

	if (data != 0) {
		bln_enabled = true;
	} else {
		bln_enabled = false;
		if (bln_on) bln_enable_notifications(false);
	}

	return size;
}

static ssize_t bln_touch_led_enable_read(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", bln_touch_led_enabled ? 1 : 0);
}

static ssize_t bln_touch_led_enable_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
        unsigned int data;

	if (sscanf(buf, "%u\n", &data ) == 1) {
		DEBUG_LOG("data: %u", data);
		if (data != 0) {
			if (!bln_touch_led_enabled) {
				bln_touch_led_enabled = true;
				bln_write_led(true);
			}
                } else {
			if (bln_touch_led_enabled) { 
				bln_write_led(false);
				bln_touch_led_enabled = false;
			}
                }
        }
        return size;
}

#ifdef USE_TOUCH_TIMEOUT
static ssize_t bln_led_timeout_read(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", bln_timeout);
}

static ssize_t bln_led_timeout_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int data;
	if (sscanf(buf, "%u\n", &data ) == 1 )
		if (data >= 0) {
			bln_timeout = data;
			DEBUG_LOG("%u", bln_timeout);
		}
        return size;
}
#endif

//

static DEVICE_ATTR(enabled, S_IRUGO | S_IWUGO,
		   bln_enabled_read,
		   bln_enabled_write);

static DEVICE_ATTR(notification_led, S_IRUGO | S_IWUGO,
		   bln_notification_read,
		   bln_notification_write);

static DEVICE_ATTR(blink_period_off, S_IRUGO | S_IWUGO, 
                   bln_blink_period_off_read, 
                   bln_blink_period_off_write );

static DEVICE_ATTR(blink_period_on, S_IRUGO | S_IWUGO, 
                   bln_blink_period_on_read, 
                   bln_blink_period_on_write );

static DEVICE_ATTR(blink_strobe, S_IRUGO | S_IWUGO, 
                   bln_blink_strobe_read, 
                   bln_blink_strobe_write );

static DEVICE_ATTR(blink_keep, S_IRUGO | S_IWUGO, 
                   bln_blink_keep_read, 
                   bln_blink_keep_write );

static DEVICE_ATTR(notification_timeout, S_IRUGO | S_IWUGO, 
                   bln_notification_timeout_read, 
                   bln_notification_timeout_write );
                   
//static DEVICE_ATTR(enable_touch_lights, S_IRUGO | S_IWUGO, 
static DEVICE_ATTR(enable_touch_ex, S_IRUGO | S_IWUGO, 
                   bln_touch_led_enable_read, 
                   bln_touch_led_enable_write );

#ifdef USE_TOUCH_TIMEOUT
static DEVICE_ATTR(led_touch_timeout, S_IRUGO | S_IWUGO, 
                   bln_led_timeout_read, 
                   bln_led_timeout_write );
#endif

static DEVICE_ATTR(version, S_IRUGO , bln_version, NULL);

static struct attribute *bln_notification_attributes[] = {
	&dev_attr_version.attr,
	&dev_attr_enabled.attr,
	&dev_attr_notification_led.attr,
	&dev_attr_notification_timeout.attr,
	&dev_attr_blink_period_on.attr,
	&dev_attr_blink_period_off.attr,
//	&dev_attr_enable_touch_lights.attr,
	&dev_attr_enable_touch_ex.attr,
	&dev_attr_blink_strobe.attr,
	&dev_attr_blink_keep.attr,
#ifdef USE_TOUCH_TIMEOUT	
	&dev_attr_led_touch_timeout.attr,
#endif
	NULL
};

static struct attribute_group bln_notification_group = {
	.attrs  = bln_notification_attributes,
};

static struct miscdevice bln_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "backlightnotification",
};

int bln_control_register(struct led_classdev *pdev)
{
	int ret;

	bln_keypad_dev = pdev;

	bln_suspended = bln_on = bln_ongoing = false;

	pr_info("%s misc_register(%s)\n", __FUNCTION__, bln_device.name);
	ret = misc_register(&bln_device);
	if (ret) {
		pr_err("%s misc_register(%s) fail\n", __FUNCTION__, bln_device.name);
		return 1;
	}

	/* add the bln attributes */
	if (sysfs_create_group(&bln_device.this_device->kobj, &bln_notification_group) < 0) {
		pr_err("%s sysfs_create_group fail\n", __FUNCTION__);
		pr_err("Failed to create sysfs group for device (%s)!\n", bln_device.name);
		return 1;
	}

	register_early_suspend(&bln_suspend_data);

#ifdef USE_TOUCH_TIMEOUT
	setup_timer(&bln_timer, bln_timer_callback, 0);
#endif

    	bln_blink_workqueue = create_singlethread_workqueue("bln_blink_work");
    	INIT_DELAYED_WORK(&bln_blink_work, bln_blink_queue_handler);
	
	alarm_init(&bln_blink_alarm, ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP, bln_blink_alarm_handler);
  	wake_lock_init(&bln_blink_wakelock, WAKE_LOCK_SUSPEND, "bln_blink_wake");

	return 0;
}

void bln_control_deregister(void)
{
#ifdef USE_TOUCH_TIMEOUT
	del_timer(&bln_timer);
#endif
	bln_on = bln_ongoing = false;
	cancel_delayed_work(&bln_blink_work);
	alarm_try_to_cancel(&bln_blink_alarm);
  	wake_lock_destroy(&bln_blink_wakelock);
	destroy_workqueue(bln_blink_workqueue);
	misc_deregister(&bln_device);
}

#endif //CONFIG_SUPPORT_BLN
