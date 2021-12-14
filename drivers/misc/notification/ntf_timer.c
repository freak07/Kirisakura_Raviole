/*
 * Copyright (C) 2018 Pal Zoltan Illes
 *
 * Licensed under the GPL-2 or later.
 */

/**
    alarm scheduled tasks for notifications
    - flashlight
    ...
*/

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/spinlock.h>

#include <linux/alarmtimer.h>
#include <linux/notification/notification.h>
#include <linux/uci/uci.h>

//HTC_START
//#include <linux/htc_flashlight.h>

#ifdef LOCKING
static DEFINE_SPINLOCK(blink_spinlock);
#endif

#if 1
static int init_done = 0;
static struct alarm flash_blink_rtc;
static struct alarm flash_blink_unidle_smp_cpu_rtc;
static struct alarm vib_rtc;
static struct work_struct flash_blink_work;
static struct work_struct flash_start_blink_work;
static struct work_struct flash_stop_blink_work;
static struct workqueue_struct *flash_blink_workqueue;
static struct workqueue_struct *flash_start_blink_workqueue;
static struct workqueue_struct *flash_stop_blink_workqueue;
static struct workqueue_struct *vib_workqueue;

// storing current executing cpu number to queue blink work on it...
static int smp_processor = -1;

static int currently_torch_mode = 0;
static int currently_blinking = 0;
void ntf_set_cam_flashlight(bool on) {
	currently_torch_mode = on?1:0;
	currently_blinking = 0;
}
EXPORT_SYMBOL(ntf_set_cam_flashlight);


#define DEFAULT_BLINK_NUMBER 46
#define DEFAULT_BLINK_WAIT_SEC 4
#define DEFAULT_WAIT_INC 1
#define DEFAULT_WAIT_INC_MAX 8

// default switches
static const int flash_blink_on  = 0;
static const int flash_blink_bright  = 1; // apply bright flash on each X number
static const int flash_blink_bright_number  = 5; // X number when bright flash should be done
static const int flash_blink_number = DEFAULT_BLINK_NUMBER;
static const int flash_blink_wait_sec = DEFAULT_BLINK_WAIT_SEC;
static const int flash_blink_wait_inc = DEFAULT_WAIT_INC;
static const int flash_blink_wait_inc_max = DEFAULT_WAIT_INC_MAX;
#ifdef CONFIG_UCI_NOTIFICATIONS_DETECT_VIBRATIONS
static const int haptic_mode = 1; // 0 - always blink, 1 - only blink with haptic vibration notifications
#else
static const int haptic_mode = 0; // 0 - always blink, 1 - only blink with haptic vibration notifications
#endif
static const int flash_only_face_down = 1;

// dim mode switches
static const int dim_mode = 1; // 0 - no , 1 - darker dim flash, 2 - fully off dim
static const int dim_use_period = 1; // 0 - don't use dimming period hours, 1 - use hours, dim only between them
static const int dim_start_hour = 22; // start hour
static const int dim_end_hour = 6; // end hour

#define DEFAULT_VIB_SLOW 12
#define DEFAULT_VIB_LENGTH 250

// on off:
static const int vib_notification_reminder = 0;
// how oftern vib
static const int vib_notification_slowness = DEFAULT_VIB_SLOW;
// how long vibration motor should be on for one reminder buzz...
static const int vib_notification_length = DEFAULT_VIB_LENGTH;



static bool in_call = false;

static bool flash_start_queued = false;


// uci properties --------------------------------
static int uci_flash_ignore_vibration = 0;
static int uci_flash_haptic_mode = haptic_mode;
static int uci_flash_blink_bright = flash_blink_bright;
static int uci_flash_blink_bright_number = flash_blink_bright_number;
static int uci_flash_blink_bright_strong = 0;
static int uci_flash_blink_number = flash_blink_number;
static int uci_flash_blink_wait_sec = flash_blink_wait_sec;
static int uci_flash_blink_wait_inc = flash_blink_wait_inc;
static int uci_flash_blink_wait_inc_max = flash_blink_wait_inc_max;
static int uci_flash_blink = flash_blink_on;
static int uci_flash_dim_mode = dim_mode;
static int uci_flash_dim_use_period = dim_use_period;
static int uci_flash_dim_start_hour = dim_start_hour;
static int uci_flash_dim_end_hour = dim_end_hour;
static int uci_flash_only_face_down = flash_only_face_down;

static int uci_vib_notification_reminder = vib_notification_reminder;
static int uci_vib_notification_slowness = vib_notification_slowness;
static int uci_vib_notification_length = vib_notification_length;

static void uci_user_listener(void) {
	uci_flash_ignore_vibration = uci_get_user_property_int_mm("flash_ignore_vibration", 0, 0, 1);
	uci_flash_haptic_mode = uci_get_user_property_int_mm("flash_haptic_mode", haptic_mode, 0, 1);

	uci_flash_blink_bright = uci_get_user_property_int_mm("flash_blink_bright", flash_blink_bright, 0, 1);
	uci_flash_blink_bright_number = uci_get_user_property_int_mm("flash_blink_bright_number", flash_blink_bright_number, 1, 10);
	uci_flash_blink_bright_strong = uci_get_user_property_int_mm("flash_blink_bright_strong", 0, 0, 1);
	uci_flash_blink_number = uci_get_user_property_int_mm("flash_blink_number", flash_blink_number, 0, 50);
	uci_flash_blink_wait_sec = uci_get_user_property_int_mm("flash_blink_wait_sec", flash_blink_wait_sec, 1, 50);
	uci_flash_blink_wait_inc = uci_get_user_property_int_mm("flash_blink_wait_inc", flash_blink_wait_inc, 0, 1);
	uci_flash_blink_wait_inc_max = uci_get_user_property_int_mm("flash_blink_wait_inc_max", flash_blink_wait_inc_max, 1, 8);
	uci_flash_dim_mode = uci_get_user_property_int_mm("flash_dim_mode", dim_mode, 0, 2);
	uci_flash_dim_use_period = uci_get_user_property_int_mm("flash_dim_use_period", dim_use_period, 0, 1);
	uci_flash_dim_start_hour =uci_get_user_property_int_mm("flash_dim_start_hour", dim_start_hour, 0, 23);
	uci_flash_dim_end_hour =uci_get_user_property_int_mm("flash_dim_end_hour", dim_end_hour, 0, 23);
	uci_flash_only_face_down =uci_get_user_property_int_mm("flash_only_face_down", flash_only_face_down, 0, 1);
	uci_flash_blink =!!uci_get_user_property_int_mm("flash_blink", flash_blink_on, 0, 1);

	uci_vib_notification_slowness = uci_get_user_property_int_mm("vib_notification_slowness", vib_notification_slowness, 0, 30);
	uci_vib_notification_length = uci_get_user_property_int_mm("vib_notification_length", vib_notification_length, 0, 500);
	uci_vib_notification_reminder = !!uci_get_user_property_int_mm("vib_notification_reminder", vib_notification_reminder, 0, 1);

}

static int get_flash_ignore_vibration(void) {
	return uci_flash_ignore_vibration;
}
static int uci_get_flash_haptic_mode(void) {
#ifdef CONFIG_UCI_NOTIFICATIONS_DETECT_VIBRATIONS
	return uci_flash_haptic_mode;
#else
	return haptic_mode;
#endif
}
static int uci_get_flash_blink_bright(void) {
	return uci_flash_blink_bright;
}
static int uci_get_flash_blink_bright_number(void) {
	return uci_flash_blink_bright_number;
}
static int uci_get_flash_blink_bright_strong(void) {
	return uci_flash_blink_bright_strong;
}
static int uci_get_flash_blink_number(void) {
	return uci_flash_blink_number;
}
static int uci_get_flash_blink_wait_sec(void) {
	return uci_flash_blink_wait_sec;
}
static int uci_get_flash_blink_wait_inc(void) {
	return uci_flash_blink_wait_inc;
}
static int uci_get_flash_blink_wait_inc_max(void) {
	return uci_flash_blink_wait_inc_max;
}

static int uci_get_flash_dim_mode(void) {
	return uci_flash_dim_mode;
}
static int uci_get_flash_dim_use_period(void) {
	return uci_flash_dim_use_period;
}
static int uci_get_flash_dim_start_hour(void) {
	return uci_flash_dim_start_hour;
}
static int uci_get_flash_dim_end_hour(void) {
	return uci_flash_dim_end_hour;
}
static int uci_get_flash_only_face_down(void) {
	return uci_flash_only_face_down;
}

extern bool ntf_face_down;
extern bool ntf_proximity;
extern bool ntf_silent;
extern bool ntf_ringing;

void flash_blink(bool haptic);
void flash_stop_blink(void);

static int smart_get_flash_blink_on(void) {
	int ret = 0;
	int level = smart_get_notification_level(NOTIF_FLASHLIGHT);
	if (uci_flash_blink) {
		if (level!=NOTIF_STOP) {
			ret = 1;
		}
	}
	pr_info("%s smart_notif =========== level: %d  uci_get_flash_blink_on() %d \n",__func__, level, ret);
	return ret;
}
static int smart_get_flash_dim_mode(void) {
	int ret = uci_get_flash_dim_mode();
	int level = smart_get_notification_level(NOTIF_FLASHLIGHT);
	if (!ret) { // not yet dimming...
		if (level==NOTIF_DIM) {
			ret = 1; // should DIM!
		}
	}
	pr_info("%s smart_notif =========== level: %d  flash_dim_mode %d \n",__func__, level, ret);
	return ret;
}
static int smart_get_flash_dim_use_period(void) {
	int ret = uci_get_flash_dim_use_period();
	int level = smart_get_notification_level(NOTIF_FLASHLIGHT);
	if (ret) { // using period for dimming...
		if (level==NOTIF_DIM) {
			ret = 0; // should dim anyway, override to Not use Dim period
		}
	}
	pr_info("%s smart_notif =========== level: %d  flash_dim_use_period %d \n",__func__, level, ret);
	return ret;
}
static int smart_get_flash_blink_wait_sec(void) {
	int ret = uci_get_flash_blink_wait_sec();
	int level = smart_get_notification_level(NOTIF_FLASHLIGHT);
	if (level!=NOTIF_DEFAULT) {
		ret = ret * 2;
	}
	pr_info("%s smart_notif =========== level: %d  flash_blink_wait_sec %d \n",__func__, level, ret);
	return ret;
}
static int smart_get_flash_blink_bright_number(void) {
	int ret = uci_get_flash_blink_bright_number();
	int level = smart_get_notification_level(NOTIF_FLASHLIGHT);
	if (level!=NOTIF_DEFAULT) {
		ret = ret * 2;
	}
	pr_info("%s smart_notif =========== level: %d  flash_blink_bright_number %d \n",__func__, level, ret);
	return ret;
}


static int current_blink_num = 0;
static int interrupt_retime = 0;
//static DEFINE_MUTEX(flash_blink_lock);


int get_hour_of_day(void) {
	struct timespec64 ts;
	unsigned long local_time;
	ktime_get_real_ts64(&ts);
	local_time = (u32)(ts.tv_sec - (sys_tz.tz_minuteswest * 60));
	return (local_time / 3600) % (24);
}

int is_dim_blink_needed(void)
{
	int hour = get_hour_of_day();
	int in_dim_period = 0;
	int start_hour = uci_get_flash_dim_start_hour();
	int end_hour = uci_get_flash_dim_end_hour();

	if (!smart_get_flash_dim_mode()) return 0;

	pr_info("%s hour %d\n",__func__,hour);
	in_dim_period = (smart_get_flash_dim_use_period() && ( (start_hour>end_hour && ( (hour<24 && hour>=start_hour) || (hour>=0 && hour<end_hour) )) || (start_hour<end_hour && hour>=start_hour && hour<end_hour)));

	if (smart_get_flash_dim_mode() && (!smart_get_flash_dim_use_period() || (smart_get_flash_dim_use_period() && in_dim_period))) return smart_get_flash_dim_mode();
	return 0;
}


static int uci_get_vib_notification_slowness(void) {
	return uci_vib_notification_slowness;
}
static int uci_get_vib_notification_length(void) {
	return uci_vib_notification_length;
}

int get_vib_notification_reminder(void) {
	return uci_vib_notification_reminder;
}
EXPORT_SYMBOL(get_vib_notification_reminder);
int get_vib_notification_slowness(void) {
	return uci_vib_notification_slowness;
}
EXPORT_SYMBOL(get_vib_notification_slowness);
int get_vib_notification_length(void) {
	return uci_vib_notification_length;
}
EXPORT_SYMBOL(get_vib_notification_length);

static int smart_get_vib_notification_reminder(void) {
	int ret = 0;
	if (ntf_silent) return 0; // do not vibrate in silent mode at all, regadless of configurations
	if (uci_vib_notification_reminder) {
		int level = smart_get_notification_level(NOTIF_VIB_REMINDER);
		if (level!=NOTIF_STOP) {
			pr_info("%s smart_notif =========== level: %d vib_notification_reminder %d \n",__func__, level, ret);
			ret = 1;
		}
	}
	return ret;
}

static int smart_get_vib_notification_slowness(void) {
	int ret = uci_get_vib_notification_slowness();
	int level = smart_get_notification_level(NOTIF_VIB_REMINDER);
	if (level!=NOTIF_DEFAULT) {
		pr_info("%s smart_notif =========== level: %d vib_notification_slowness %d \n",__func__, level, ret);
		ret = ret * 2;
	}
	return ret;
}

//extern void boosted_vib(int time);

#define DIM_USEC 2
#define BRIGHT_USEC 1550

void precise_delay(int usec) {
	ktime_t start, end;
	s64 diff;
	start = ktime_get();
	while (1) {
		end = ktime_get();
		diff = ktime_to_us(ktime_sub(end, start));
		if (diff>=usec) return;
	}
}

extern void qpnp_torch_main(int led0, int led1);

// should be true if phone was not in flashlight ready state, like not on a table face down. Then next flashblink start should reschedule work.
static bool in_no_flash_long_alarm_wake_time = false;

void do_flash_blink(void) {
	ktime_t wakeup_time;
	int count = 0;
	int limit = 3;
	int dim = 0;
	int bright = 0;
	int flash_next = 0;
	int vib_slowness = smart_get_vib_notification_slowness();

	pr_info("%s ########################## flash_blink ############################# \n",__func__);
	alarm_cancel(&flash_blink_unidle_smp_cpu_rtc); // stop pending alarm... no need to unidle cpu in that alarm...

	if (currently_torch_mode || interrupt_retime || in_call) return;

	dim = is_dim_blink_needed();
	pr_info("%s dim %d\n",__func__,dim);

	if (dim==2) {
		currently_blinking = 0;
		goto exit;
	}
	if (dim == 0 && uci_get_flash_blink_bright() && current_blink_num % smart_get_flash_blink_bright_number() == 0) {
		bright = 1;
	}

	if (uci_get_flash_blink_bright() && ntf_ringing) bright = 1;

	qpnp_torch_main(0,0);

	if (uci_get_flash_blink_wait_inc() && !dim) {
		// while in the first fast paced periodicity, don't do that much of flashing in one blink...
		if (current_blink_num < 16) limit = 3;
		if (current_blink_num > 30) limit = 3;
		if (current_blink_num > 40) limit = 4;
	}

	limit -= dim * 2;


	if ((uci_get_flash_only_face_down() && ntf_face_down) || !uci_get_flash_only_face_down()) {
		flash_next = 1; // should flash next time, alarm wait normal... if no flashing is being done, vibrating reminder wait period should be waited instead!
		while (count++<limit) {
#if 1
			// always set light brightest...vary with time length only
			qpnp_torch_main(300,0);  // [o] [ ]
#endif
#if 0
			qpnp_torch_main(150*(bright+1),0);  // [o] [ ]
#endif
			if (uci_get_flash_blink_bright_strong()) {
				precise_delay(1120 -(dim * DIM_USEC) +(bright * (BRIGHT_USEC+100)));
			} else {
				precise_delay(520 -(dim * DIM_USEC) +(bright * BRIGHT_USEC));
			}
			qpnp_torch_main(0,0);	// [ ] [ ]
			precise_delay(20000);

			if (bright)
			{
				qpnp_torch_main(300,0);  // [o] [ ]
				if (uci_get_flash_blink_bright_strong()) {
					precise_delay(1120 -(dim * DIM_USEC) +(bright * (BRIGHT_USEC+100)));
				} else {
					precise_delay(520 -(dim * DIM_USEC) +(bright * BRIGHT_USEC));
				}
				qpnp_torch_main(0,0);	// [ ] [ ]
				udelay(15000);
			}
		}
	} else {
		pr_info("%s skipping flashing because of not face down\n",__func__);
	}

	if (!ntf_ringing) {
	if (smart_get_vib_notification_reminder() && current_blink_num % vib_slowness == (vib_slowness - 1)) {
		// call vibration from a real time alarm thread, otherwise it can get stuck vibrating
		if (alarm_try_to_cancel(&vib_rtc)>=0) { // stop pending alarm...
			alarm_start_relative(&vib_rtc, ms_to_ktime(1000)); // start new...
		}
	}
	}

#ifdef LOCKING
//	mutex_lock(&flash_blink_lock);
	spin_lock(&blink_spinlock);
#endif
	// make sure this part is running in a few cycles, as blocking the lock will block vibrator driver resulting in kernel freeze and panic
	{
		int max_blink = uci_get_flash_blink_number();
		pr_info("%s flash_blink lock - maxblink %d crnt %d interrupt %d \n",__func__, max_blink, current_blink_num, interrupt_retime);
		if ( (max_blink > 0 && current_blink_num > max_blink) || interrupt_retime) {
			currently_blinking = 0;
			goto exit;
		}
	}
	current_blink_num++;
	if (smart_get_flash_blink_on()) // only reschedule if still flashblink is on...
	{
		ktime_t curr_time;
		int multiplicator;
		int calc_with_blink_num;
		curr_time = ktime_get();
		multiplicator = 1;
		calc_with_blink_num = current_blink_num;
		if (!flash_next) {
			in_no_flash_long_alarm_wake_time = true;
			// won't need flashing next, skip a few blinks till next is a vibrating notifiaction, also count multiplicator, to multiply wait time...
			while (current_blink_num % vib_slowness != (vib_slowness - 1)) {
				current_blink_num++;
				multiplicator++;
			}
		} else {
			in_no_flash_long_alarm_wake_time = false;
		}
		wakeup_time = ktime_add_us(curr_time,
			( (smart_get_flash_blink_wait_sec() + min(max(((calc_with_blink_num-6)/4),0),uci_get_flash_blink_wait_inc_max()) * uci_get_flash_blink_wait_inc()) * 1000LL * 1000LL) * multiplicator); // msec to usec 
		pr_info("%s: Flash_next %d -- Current Time tv_sec: %ld, Alarm set to tv_sec: %ld\n",
			__func__, flash_next,
			ktime_to_timespec64(curr_time).tv_sec,
			ktime_to_timespec64(wakeup_time).tv_sec);

			if (alarm_try_to_cancel(&flash_blink_rtc)>=0)
			{ // stop pending alarm...
				flash_start_queued = true;
				pr_info("%s: flash next alarm queued...##",__func__);
				alarm_start_relative(&flash_blink_rtc, ms_to_ktime(
						( (smart_get_flash_blink_wait_sec() + min(max(((calc_with_blink_num-6)/4),0),uci_get_flash_blink_wait_inc_max()) * uci_get_flash_blink_wait_inc()) * 1000LL) * multiplicator
					)); // start new...
			}

	} else {
			alarm_cancel(&flash_blink_rtc); // stop pending alarm...
	}


exit:
#ifdef LOCKING
	spin_unlock(&blink_spinlock);
//	mutex_unlock(&flash_blink_lock);
#endif
	pr_info("%s flash_blink unlock\n",__func__);

}

static void flash_start_blink_work_func(struct work_struct *work)
{
	pr_info("%s  [flashwake] flash_blink start work func\n",__func__);
#ifdef LOCKING
	spin_lock(&blink_spinlock);
//	mutex_lock(&flash_blink_lock);
#endif
	pr_info("%s flash_blink lock\n",__func__);

	interrupt_retime = 0;
	if (currently_blinking) {
		// already blinking, check if we should go back with blink num count to a faster pace...
		if (current_blink_num>8) {
			// if started blinking already over a lot of blinks, move back to the beginning, 
			// to shorter periodicity...
			current_blink_num = 5;
		} // otherwise if only a few blinks yet, don't reset count...
		if (in_no_flash_long_alarm_wake_time) { // if flashless long wait, start right now, so in case now it could flash, let it do right now
			// restart blinking with async work
			alarm_try_to_cancel(&flash_blink_rtc); // stop pending alarm...
			currently_blinking = 1;
			pr_info("%s blink queue work... #1\n",__func__);
			flash_start_queued = true;
			queue_work(flash_blink_workqueue, &flash_blink_work);
			in_no_flash_long_alarm_wake_time = false;
		}
#ifdef LOCKING
		spin_unlock(&blink_spinlock);
//		mutex_unlock(&flash_blink_lock);
#endif
		pr_info("%s flash_blink unlock\n",__func__);
	} else {
		currently_blinking = 1;
		current_blink_num = 0;
#ifdef LOCKING
		spin_unlock(&blink_spinlock);
//		mutex_unlock(&flash_blink_lock);
#endif
		pr_info("%s blink queue work... #2\n",__func__);
		queue_work(flash_blink_workqueue, &flash_blink_work);
//		do_flash_blink();
		pr_info("%s flash_blink unlock\n",__func__);
	}
}


static void flash_stop_blink_work_func(struct work_struct *work)
{
#ifdef LOCKING
	spin_lock(&blink_spinlock);
//	mutex_lock(&flash_blink_lock);
#endif

	if (!currently_blinking) goto exit;
	if (currently_torch_mode) goto exit;
	pr_info("%s [flashwake] flash_blink stop work func...\n",__func__);
	currently_blinking = 0;
	qpnp_torch_main(0,0);
	interrupt_retime = 1;
	alarm_cancel(&flash_blink_rtc); // stop pending alarm...
exit:
#ifdef LOCKING
	spin_unlock(&blink_spinlock);
//	mutex_unlock(&flash_blink_lock);
#endif
	flash_start_queued = false; // reset flag, no more stop work start needed for now...
}


void flash_blink(bool haptic) {
	pr_info("%s [flashwake] flash_blink\n",__func__);
	// is flash blink on?
	if (!smart_get_flash_blink_on()) return;
	// if not a haptic notificcation and haptic blink mode on, do not do blinking...
	if (!haptic && uci_get_flash_haptic_mode()) return;
	if (haptic && get_flash_ignore_vibration()) return;
	if (in_call) return;

	// if torch i on, don't blink
	if (currently_torch_mode) return;

	if (!init_done) return;

	pr_info("%s start blink queue work...\n",__func__);
	flash_start_queued = true;
	queue_work(flash_start_blink_workqueue, &flash_start_blink_work);
}
EXPORT_SYMBOL(flash_blink);

static void flash_blink_work_func(struct work_struct *work)
{
	pr_info("%s [flashwake] flash_blink work executing... calling do_flash_blink, set smp to -1...\n",__func__);
	smp_processor = -1; // signal that work started here, no need to wake idle cpus...
	flash_start_queued = true; // force settings this if async work is still
	do_flash_blink();
}

//extern void set_vibrate(int value);
extern void set_vibrate_boosted(int value);
static void vib_work_func(struct work_struct *vib_work_func_work)
{
	pr_info("%s set_vibrate boosted\n",__func__);
	set_vibrate_boosted(uci_get_vib_notification_length());
}
static DECLARE_WORK(vib_work_func_work, vib_work_func);

static enum alarmtimer_restart vib_rtc_callback(struct alarm *al, ktime_t now)
{
	pr_info("%s flash_blink\n",__func__);
	queue_work(vib_workqueue,&vib_work_func_work);
	return ALARMTIMER_NORESTART;
}

static enum alarmtimer_restart flash_blink_rtc_callback(struct alarm *al, ktime_t now)
{
	pr_info("%s [flashwake] flash_blink | interrupt_retime: %d\n",__func__,interrupt_retime);
	if (!interrupt_retime) {
		pr_info("%s [flashwake] blink queue work ALARM...\n",__func__);
		smp_processor = smp_processor_id();
		pr_info("%s [flashwake] flash_blink cpu %d\n",__func__, smp_processor);

		// queue actual flash work on current CPU for avoiding sleeping CPU...
		flash_start_queued = true;
		queue_work_on(smp_processor,flash_blink_workqueue, &flash_blink_work);

		alarm_cancel(&flash_blink_unidle_smp_cpu_rtc); // stop pending alarm...
		alarm_start_relative(&flash_blink_unidle_smp_cpu_rtc, ms_to_ktime(500)); // start new...
	}
	pr_info("%s flash_blink exit\n",__func__);
	return ALARMTIMER_NORESTART;
}

static enum alarmtimer_restart flash_blink_unidle_smp_cpu_rtc_callback(struct alarm *al, ktime_t now)
{
	pr_info("%s [flashwake] flash_blink cpu %d interrupt_retime %d \n",__func__, smp_processor, interrupt_retime);
	if (!interrupt_retime) {
		// make sure Queue execution is not stuck... would mean longer pauses between blinks than should...
		//wake_up_if_idle(smp_processor);
		if (smp_processor!=-1) {
			pr_info("%s [flashwake] work is still pending...wake all idle #1\n",__func__);
			wake_up_all_idle_cpus();
			mdelay(100);
			if (smp_processor!=-1) {
				pr_info("%s [flashwake] work is still pending...wake all idle #2\n",__func__);
				wake_up_all_idle_cpus();
			}
		}
	}
	return ALARMTIMER_NORESTART;
}


void flash_stop_blink(void) {
//	pr_info("%s flash_blink\n",__func__);
	if (!init_done) return;
	if (ntf_ringing) return; // screen on/user input shouldn't stop ringing triggered flashing!
	if (flash_start_queued) {
		pr_info("%s [flashwake] stop blink queue work...\n",__func__);
		queue_work(flash_stop_blink_workqueue, &flash_stop_blink_work);
	}
}
EXPORT_SYMBOL(flash_stop_blink);

#endif


#ifdef CONFIG_UCI_NOTIFICATIONS
static void ntf_listener(char* event, int num_param, char* str_param) {
	if (strcmp(event,NTF_EVENT_CHARGE_LEVEL) && strcmp(event, NTF_EVENT_INPUT)) {
		pr_info("%s blink ntf_timer listener event %s %d %s\n",__func__,event,num_param,str_param);
	}
	if (!strcmp(event,NTF_EVENT_NOTIFICATION)) {
		if (!!num_param) {
			// notif started
			if (!ntf_is_screen_on() || !ntf_wake_by_user())
			{
				if (str_param && !strcmp(str_param,NTF_EVENT_NOTIFICATION_ARG_HAPTIC)) {
					flash_blink(true); // haptic feedback based detection...
				} else {
					flash_blink(false);
				}
			}
		} else {
			// notif over
			flash_stop_blink();
		}
	}
	if (!strcmp(event,NTF_EVENT_WAKE_BY_USER)) { // SCREEN ON BY USER
		flash_stop_blink();
	}
	if (!strcmp(event,NTF_EVENT_LOCKED) && !num_param) { // UNLOCKED / faceunlock
		flash_stop_blink();
	}
	if (!strcmp(event,NTF_EVENT_INPUT)) { // INPUT
		if (ntf_wake_by_user()) {
			flash_stop_blink();
		}
	}
	if (!strcmp(event,NTF_EVENT_RINGING)) {
		if (num_param) {
			flash_blink(true);
		} else {
			flash_stop_blink();
		}
	}
	if (!strcmp(event,NTF_EVENT_IN_CALL)) { // call started
		in_call = !!num_param;
		if (in_call) {
			flash_stop_blink(); // stop flash/vib reminder
		}
	}

}
#endif

static int __init ntf_timer_init_module(void)
{
        int32_t rc = 0;
#if 1
        alarm_init(&flash_blink_rtc, ALARM_REALTIME,
                flash_blink_rtc_callback);
	alarm_init(&flash_blink_unidle_smp_cpu_rtc, ALARM_REALTIME,
		flash_blink_unidle_smp_cpu_rtc_callback);
        alarm_init(&vib_rtc, ALARM_REALTIME,
                vib_rtc_callback);
        flash_blink_workqueue = alloc_workqueue("flash_blink",
                                      WQ_HIGHPRI | WQ_MEM_RECLAIM, 0);

        flash_start_blink_workqueue = create_singlethread_workqueue("flash_start_blink");
        flash_stop_blink_workqueue = create_singlethread_workqueue("flash_stop_blink");
        vib_workqueue = create_singlethread_workqueue("flash_vib");
        INIT_WORK(&flash_blink_work, flash_blink_work_func);
        INIT_WORK(&flash_start_blink_work, flash_start_blink_work_func);
        INIT_WORK(&flash_stop_blink_work, flash_stop_blink_work_func);
#endif
#ifdef CONFIG_UCI_NOTIFICATIONS
	ntf_add_listener(ntf_listener);
#endif
	uci_add_user_listener(uci_user_listener);
	init_done = 1;
        return rc;
}


static void __exit ntf_timer_exit_module(void)
{
        return;
}

module_init(ntf_timer_init_module);
module_exit(ntf_timer_exit_module);
MODULE_DESCRIPTION("MSM FLASH");
MODULE_LICENSE("GPL v2");
