#ifndef __BLN_CONTROL_H_
#define __BLN_CONTROL_H_

#include <linux/earlysuspend.h>
#include <linux/miscdevice.h>
#include <linux/wakelock.h>
#include <linux/delay.h>
#include <linux/android_alarm.h>
#include <linux/time.h>

int bln_control_register(struct led_classdev *pdev);
void bln_control_deregister(void);

int bln_hook_led_write(int value); // ret: 1 - turn LED on, 0 - turn LED off, -1 - do nothing

#endif
