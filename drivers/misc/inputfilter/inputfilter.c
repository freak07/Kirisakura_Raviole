#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/slab.h>


#define CONFIG_IFILTER_KCAL_ANYTIME

#include <linux/notifier.h>
#include <linux/fb.h>

#include <linux/alarmtimer.h>
#include <linux/notification/notification.h>
#include <linux/uci/uci.h>
#include <linux/inputfilter/inputfilter.h>

#define DRIVER_AUTHOR "illes pal <illespal@gmail.com>"
#define DRIVER_DESCRIPTION "inputfilter driver"
#define DRIVER_VERSION "3.0"

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");

#ifdef CONFIG_HZ_300
#define JIFFY_MUL 3
#elif CONFIG_HZ_250
#define JIFFY_MUL 2
#else
#define JIFFY_MUL 1
#endif

#define IFILTER_PWRKEY_DUR          20
#define FUNC_CYCLE_DUR          12 + JIFFY_MUL
#define VIB_STRENGTH		70

#define IFILTER_SWITCH_STOCK 0
#define IFILTER_SWITCH_HOME 1
#define IFILTER_SWITCH_DTAP 2
//#define IFILTER_SWITCH_DTAP_LDTAP 3 // for back fingerprint scanner, 
			// to act only as Home on doubletapping, 
			// and act as power off on a longer period doubletap
			// ...over a threshold don't do anything

#define IFILTER_SWITCH_DTAP_TTAP 3 // double tap home, triple tap screen off

#define IFILTER_KEY_HOME 0
#define IFILTER_KEY_APPSWITCH 1
#define IFILTER_KEY_NOTIFICATION 2

extern void set_vibrate(int value);
extern void set_vibrate_2(int value, int boost_level);
extern void set_vibrate_boosted(int value);

static const int ifilter_switch = IFILTER_SWITCH_STOCK;
static const int ifilter_key = 0;
static struct input_dev * ifilter_pwrdev;
static DEFINE_MUTEX(pwrkeyworklock);
static DEFINE_MUTEX(ifilteruncworklock);
static struct workqueue_struct *ifilter_input_wq;
static struct work_struct ifilter_input_work;

static struct workqueue_struct *ifilter_vib_wq;
static struct workqueue_struct *ifilter_pwr_wq;

static const int vib_strength = VIB_STRENGTH;
static const int unlock_vib_strength = VIB_STRENGTH;

// touchscreen input handler input work queue and work
static struct workqueue_struct *ts_input_wq;
static struct work_struct ts_input_work;
static struct input_dev *ts_device = NULL;

#if 0
static unsigned long last_screen_event_timestamp = 0;// zero to wait for first screen off
#endif
#if 1
static unsigned long last_screen_event_timestamp = 1; // start with 1 to let it work after boot. Pixel4
#endif

static unsigned int last_screen_off_seconds = 0;
static unsigned int last_screen_on_seconds = 0;

unsigned int get_global_seconds(void) {
	struct timespec64 ts;
	unsigned int ret = 0;
	ktime_get_real_ts64(&ts);
	ret = (unsigned int)(ts.tv_sec);
	return ret;
}

static struct workqueue_struct *kcal_listener_wq;

#define S_MIN_SECS 60

// --- smart notification settings --
// how many inactive minutes to start trimming some types of notifications' periodicity/length of timeout/repetitions
// should be set to 0 if smart trim is inactive
static const int smart_trim_inactive_seconds = 6 * S_MIN_SECS;// 6 mins
// notif settings for trim period
static const int smart_trim_kad = NOTIF_TRIM;
static const int smart_trim_flashlight = NOTIF_TRIM;
static const int smart_trim_vib_reminder = NOTIF_DEFAULT;
static const int smart_trim_notif_booster = NOTIF_DEFAULT;
static const int smart_trim_bln_light = NOTIF_DEFAULT;
static const int smart_trim_pulse_light = NOTIF_DEFAULT;

// how many inactive minutes to start stopping some types of notifications
// should be set to 0 if smart stop is inactive
static const int smart_stop_inactive_seconds = 60 * S_MIN_SECS; // 60 mins
// notif settings for stop period
static const int smart_stop_kad = NOTIF_STOP;
static const int smart_stop_flashlight = NOTIF_DIM;
static const int smart_stop_vib_reminder = NOTIF_TRIM;
static const int smart_stop_notif_booster = NOTIF_DEFAULT;
static const int smart_stop_bln_light = NOTIF_TRIM;
static const int smart_stop_pulse_light = NOTIF_DEFAULT;

// how many inactive minutes to start hibarnete (extended stop) some types of notifications
// should be set to 0 if smart stop is inactive
static const int smart_hibernate_inactive_seconds = 4 * 60 * S_MIN_SECS; // 4 * 60 mins
// notif settings for hibernate period
static const int smart_hibernate_kad = NOTIF_STOP;
static const int smart_hibernate_flashlight = NOTIF_STOP;
static const int smart_hibernate_vib_reminder = NOTIF_STOP;
static const int smart_hibernate_notif_booster = NOTIF_STOP;
static const int smart_hibernate_bln_light = NOTIF_DIM;
static const int smart_hibernate_pulse_light = NOTIF_DIM;

static const int phone_ring_in_silent_mode = 0;
static const int face_down_screen_off = 0;
static const int face_down_screen_off_vib = 0;

static const int smart_silent_mode_stop = 0;
static const int smart_silent_mode_hibernate = 0;

// --------------------------------------------------------------------------------
// uci configs --------------------------------------------------------------------
// --------------------------------------------------------------------------------
static int uci_fingerprint_mode = ifilter_switch;
static int uci_fingerprint_key = ifilter_key;
static int uci_fp_swipe_mode = 0;
static int uci_fp_vib_strength = vib_strength;
static int uci_fp_unlock_vib_strength = unlock_vib_strength;
static int uci_smart_trim_inactive_mins = smart_trim_inactive_seconds/60;
static int uci_smart_stop_inactive_mins = smart_stop_inactive_seconds/60;
static int uci_smart_hibernate_inactive_mins = smart_hibernate_inactive_seconds/60;
static int uci_smart_silent_mode_hibernate = smart_silent_mode_hibernate;
static int uci_smart_silent_mode_stop = smart_silent_mode_stop;
static int uci_phone_ring_in_silent_mode = phone_ring_in_silent_mode;
static int uci_face_down_screen_off = face_down_screen_off;
static int uci_face_down_screen_off_vib = face_down_screen_off_vib;


static void uci_user_listener(void) {
	uci_fingerprint_mode = uci_get_user_property_int_mm("fingerprint_mode", ifilter_switch, 0, 3);
	uci_fingerprint_key = uci_get_user_property_int_mm("fingerprint_key", ifilter_key, 0, 2);
	uci_fp_vib_strength = uci_get_user_property_int_mm("fp_vib_strength", vib_strength, 0, 90);
	uci_fp_swipe_mode = uci_get_user_property_int_mm("fp_swipe_mode", 0, 0, 1);
	uci_fp_unlock_vib_strength = uci_get_user_property_int_mm("fp_unlock_vib_strength", unlock_vib_strength, 0, 90);

	uci_smart_trim_inactive_mins = uci_get_user_property_int_mm("smart_trim_inactive_mins", smart_trim_inactive_seconds/60, 0, 2 * 24 * 60);
	uci_smart_stop_inactive_mins = uci_get_user_property_int_mm("smart_stop_inactive_mins", smart_stop_inactive_seconds/60, 0, 2 * 24 * 60);
	uci_smart_hibernate_inactive_mins = uci_get_user_property_int_mm("smart_hibernate_inactive_mins", smart_hibernate_inactive_seconds/60, 0, 2 * 24 * 60);
	uci_smart_silent_mode_hibernate = uci_get_user_property_int_mm("smart_silent_mode_hibernate", smart_silent_mode_hibernate, 0, 1);
	uci_smart_silent_mode_stop = uci_get_user_property_int_mm("smart_silent_mode_stop", smart_silent_mode_stop, 0, 1);

	uci_phone_ring_in_silent_mode = uci_get_user_property_int_mm("phone_ring_in_silent_mode", phone_ring_in_silent_mode, 0, 1);

	uci_face_down_screen_off = uci_get_user_property_int_mm("face_down_screen_off", face_down_screen_off, 0, 1);
	uci_face_down_screen_off_vib = uci_get_user_property_int_mm("face_down_screen_off_vib", face_down_screen_off_vib, 0, 1);
}

static int get_ifilter_switch(void) {
	return uci_fingerprint_mode;
}
static int get_ifilter_key(void) {
	int fp_key = uci_fingerprint_key;
	return fp_key==2?KEY_KPDOT:(fp_key==1?KEY_APPSELECT:KEY_HOME);
}
static int get_vib_strength(void) {
	return uci_fp_vib_strength;
}
static int get_unlock_vib_strength(void) {
	return uci_fp_unlock_vib_strength;
}

// early screen on flag
static int screen_on = 1;
static unsigned long last_screen_on_early_time = 0;
// full screen on flag
static int screen_on_full = 1;
static int screen_off_early = 0;

int input_is_screen_on(void) {
	return screen_on;
}
EXPORT_SYMBOL(input_is_screen_on);


/**
* this variable is 1 if KAD was blocked only by Proximity or not being yet Locked. On sys uci listener check this, and start kad if blocking is over.
*/
static int kad_should_start_on_uci_sys_change = 0;
void kernel_ambient_display(void);
void kernel_ambient_display_led_based(void);


int uci_get_smart_trim_inactive_seconds(void) {
	return uci_smart_trim_inactive_mins*60;
}
int uci_get_smart_stop_inactive_seconds(void) {
	return uci_smart_stop_inactive_mins*60;
}
int uci_get_smart_hibernate_inactive_seconds(void) {
	return uci_smart_hibernate_inactive_mins*60;
}
int uci_get_smart_silent_mode_hibernate(void) {
	return uci_smart_silent_mode_hibernate;
}
int uci_get_smart_silent_mode_stop(void) {
	return uci_smart_silent_mode_stop;
}

int ifilter_silent_mode = 0;
// KAD should run if in ringing mode... companion app channels the info
int ifilter_ringing = 0;
/**
* If an app that is waking screen from sleep like Alarm or Phone, this should be set higher than 0
* If that happens, KAD should STOP running and no new KAD screen should start till value is back to 0,
* meaning apps were closed/dismissed. Companion app channels this number.
*/
int ifilter_screen_waking_app = 0;

int silent_mode_hibernate(void) {
	if (uci_get_smart_silent_mode_hibernate()) {
		return ifilter_silent_mode;
	}
	return 0;
}
int silent_mode_stop(void) {
	if (uci_get_smart_silent_mode_stop()) {
		return ifilter_silent_mode;
	}
	return 0;
}

static int get_phone_ring_in_silent_mode(void) {
	return uci_phone_ring_in_silent_mode;
}

static struct alarm vibrate_rtc;
static enum alarmtimer_restart vibrate_rtc_callback(struct alarm *al, ktime_t now)
{
	pr_info("%s kad\n",__func__);
	set_vibrate_boosted(998);
	return ALARMTIMER_NORESTART;
}


static int get_face_down_screen_off(void) {
	return uci_face_down_screen_off;
}

// should vibrate when face down screen off gesture triggers..?
static int get_face_down_screen_off_vib(void) {
	return uci_face_down_screen_off_vib;
}

bool should_screen_off_face_down(int screen_timeout_sec, int face_down);
static void ifilter_pwrtrigger(int vibration, int delay, const char caller[]);

void stop_kernel_ambient_display(bool interrupt_ongoing);

int stored_lock_state = 0;
// register sys uci listener
// TODO move this to ntf event handling, together with new silent / face down event...
static int last_face_down = 0;
void ifilter_uci_sys_listener(void) {
	int locked = 0;
	pr_info("%s uci sys parse happened...\n",__func__);
	{
		int silent = uci_get_sys_property_int_mm("silent", 0, 0, 1);
		int ringing = uci_get_sys_property_int_mm("ringing", 0, 0, 1);

		int face_down = uci_get_sys_property_int_mm("face_down", 0, 0, 1);
		int screen_timeout_sec = uci_get_sys_property_int_mm("screen_timeout", 15, 0, 600);

		int screen_waking_app = uci_get_sys_property_int("screen_waking_apps", 0);
		locked = uci_get_sys_property_int_mm("locked", 0, 0, 1);
		if (screen_waking_app != -EINVAL) ifilter_screen_waking_app = screen_waking_app;

		pr_info("%s uci sys silent %d ringing %d face_down %d timeout %d last_face_down %d wake_by_user %d screen_waking_app %d \n",__func__,silent, ringing, face_down, screen_timeout_sec, last_face_down, ntf_wake_by_user(),ifilter_screen_waking_app);
		ifilter_silent_mode = silent;
		if (ifilter_silent_mode && ringing && (ringing!=ifilter_ringing) && get_phone_ring_in_silent_mode()) {
//			ktime_t wakeup_time;
//			ktime_t curr_time = ktime_get();
//			wakeup_time = ktime_add_us(curr_time,
//			    (2 * 1000LL * 1000LL)); // msec to usec
			alarm_cancel(&vibrate_rtc);
			alarm_start_relative(&vibrate_rtc, ms_to_ktime(2 * 1000)); // start new...
			set_vibrate_boosted(999);
		} else {
			if (!ringing) {
				alarm_cancel(&vibrate_rtc);
			}
		}
		ifilter_ringing = ringing;
		if (face_down && last_face_down!=face_down) {
			if (screen_on && !ringing && !ifilter_screen_waking_app) {
				if (should_screen_off_face_down(screen_timeout_sec, face_down)) {
					ifilter_pwrtrigger(!!get_face_down_screen_off_vib(),0,__func__);
				}
			}
		}
		last_face_down = face_down;
	}
	if (!locked&&stored_lock_state!=locked) {
		ntf_input_event(__func__,"");
		stop_kernel_ambient_display(true);
	} else
	if (ifilter_ringing || ifilter_screen_waking_app) {
		ntf_input_event(__func__,"");
		stop_kernel_ambient_display(true);
	}
	if (!screen_on && kad_should_start_on_uci_sys_change) {
		kernel_ambient_display();
	}
	stored_lock_state = locked;
}



static unsigned int smart_last_user_activity_time = 0;
void smart_set_last_user_activity_time(void) {
	smart_last_user_activity_time = get_global_seconds();
}
EXPORT_SYMBOL(smart_set_last_user_activity_time);

int smart_get_inactivity_time(void) {
	unsigned int diff = 0;
	int diff_in_sec = 0;
	if (smart_last_user_activity_time==0) smart_last_user_activity_time = get_global_seconds();
	diff = get_global_seconds() - smart_last_user_activity_time;
	diff_in_sec = diff / 1;
	pr_info("%s smart_notif - inactivity in sec: %d\n",__func__, diff_in_sec);
//	if (1) return 0;
	return diff_in_sec;
}


int smart_get_notification_level(int notif_type) {
	int diff_in_sec = smart_get_inactivity_time();
	int ret = NOTIF_DEFAULT;
	bool trim = uci_get_smart_trim_inactive_seconds() > 0 && diff_in_sec > uci_get_smart_trim_inactive_seconds();
	bool stop = uci_get_smart_stop_inactive_seconds() > 0 && diff_in_sec > uci_get_smart_stop_inactive_seconds();
	bool hibr = uci_get_smart_hibernate_inactive_seconds() > 0 && diff_in_sec > uci_get_smart_hibernate_inactive_seconds();
	if (silent_mode_hibernate()) hibr = true;
	if (silent_mode_stop()) stop = true;
	switch (notif_type) {
		case NOTIF_KAD:
			ret = hibr?smart_hibernate_kad:(stop?smart_stop_kad:(trim?smart_trim_kad:NOTIF_DEFAULT));
			break;
		case NOTIF_FLASHLIGHT:
			ret = hibr?smart_hibernate_flashlight:(stop?smart_stop_flashlight:(trim?smart_trim_flashlight:NOTIF_DEFAULT));
			break;
		case NOTIF_VIB_REMINDER:
			if ( (hibr?smart_hibernate_flashlight:(stop?smart_stop_flashlight:(trim?smart_trim_flashlight:NOTIF_DEFAULT) )) == NOTIF_STOP ) ret = NOTIF_STOP; else // without flashlight, no reminder possible
			ret = hibr?smart_hibernate_vib_reminder:(stop?smart_stop_vib_reminder:(trim?smart_trim_vib_reminder:NOTIF_DEFAULT));
			break;
		case NOTIF_VIB_BOOSTER:
			ret = hibr?smart_hibernate_notif_booster:(stop?smart_stop_notif_booster:(trim?smart_trim_notif_booster:NOTIF_DEFAULT));
			break;
		case NOTIF_BUTTON_LIGHT:
			ret = hibr?smart_hibernate_bln_light:(stop?smart_stop_bln_light:(trim?smart_trim_bln_light:NOTIF_DEFAULT));
			break;
		case NOTIF_PULSE_LIGHT:
			ret = hibr?smart_hibernate_pulse_light:(stop?smart_stop_pulse_light:(trim?smart_trim_pulse_light:NOTIF_DEFAULT));
			break;
	}
	pr_info("%s smart_notif - level for type %d is %d -- state trim %d stop %d hibr %d \n",__func__, notif_type, ret, trim,stop,hibr);
	return ret;
}
EXPORT_SYMBOL(smart_get_notification_level);

// /// smart notif ////

// kad
// -- KAD (Kernel Ambient Display --
#ifdef CONFIG_IFILTER_KAD
static int kad_on = 0; // is kad enabled?
#endif
static int kad_only_on_charger = 0; // do KAD only on charger?
static int kad_disable_touch_input = 1; // disable touch input while KAD?
static int kad_disable_fp_input = 1; // disable fingerprint input while KAD?
static int kad_kcal = 1; // do kcal coloring/grayscale?
static int kad_halfseconds = 11; // how long KAD should display
static int kad_repeat_times = 4; // how many times... 
static int kad_repeat_multiply_period = 1; // make periods between each longer?
static int kad_repeat_period_sec = 12; // period between each repeat
static int squeeze_peek_kcal = 0;
static int kad_two_finger_gesture = 0; // two finger gesture will wake KAD
static int kad_three_finger_gesture = 1; // three finger gesture will wake KAD
#ifdef CONFIG_IFILTER_KAD
static int kad_start_after_proximity_left = 1; // start pending kad if leaving proximity
#endif
static int kad_start_delay_halfseconds = 2; // how long KAD should wait before starting to avoid collision with notif sounds - min 1 max 4

static int kad_running_for_aod_gesture = 0; // state, if KAD is initiated for aod gesture, repetition shouldn't be done

static int kad_kcal_sat = 128;
static int kad_kcal_val = 135;
static int kad_kcal_cont = 255;
static int peek_kcal_sat = 128;
static int peek_kcal_val = 254;
static int peek_kcal_cont = 254;

static int kad_kcal_r = 254;
static int kad_kcal_g = 254;
static int kad_kcal_b = 254;
static int peek_kcal_r = 254;
static int peek_kcal_g = 254;
static int peek_kcal_b = 254;

#ifdef CONFIG_IFILTER_KAD
static int get_kad_start_after_proximity_left(void) {
	return uci_get_user_property_int_mm("kad_start_after_proximity_left", kad_start_after_proximity_left, 0, 1);
}
#endif
static int get_kad_pick_up_show(void) {
	return uci_get_user_property_int_mm("kad_pick_up_show", 0, 0, 1);
}
static int get_kad_pick_up_block_camera(void) {
	return uci_get_user_property_int_mm("kad_pick_up_block_camera", 0, 0, 1);
}

static int get_kad_kcal_sat(void) {
	return uci_get_user_property_int_mm("kad_kcal_sat", kad_kcal_sat, 128, 383);
}
static int get_kad_kcal_val(void) {
	return uci_get_user_property_int_mm("kad_kcal_val", kad_kcal_val, 128, 383);
}
static int get_kad_kcal_cont(void) {
	return uci_get_user_property_int_mm("kad_kcal_cont", kad_kcal_cont, 128, 383);
}
static int get_kad_kcal_r(void) {
	return uci_get_user_property_int_mm("kad_kcal_r", kad_kcal_r, 40, 256);
}
static int get_kad_kcal_g(void) {
	return uci_get_user_property_int_mm("kad_kcal_g", kad_kcal_g, 40, 256);
}
static int get_kad_kcal_b(void) {
	return uci_get_user_property_int_mm("kad_kcal_b", kad_kcal_b, 40, 256);
}

static int get_peek_kcal_sat(void) {
	return uci_get_user_property_int_mm("peek_kcal_sat", peek_kcal_sat, 128, 383);
}
static int get_peek_kcal_val(void) {
	return uci_get_user_property_int_mm("peek_kcal_val", peek_kcal_val, 128, 383);
}
static int get_peek_kcal_cont(void) {
	return uci_get_user_property_int_mm("peek_kcal_cont", peek_kcal_cont, 128, 383);
}
static int get_peek_kcal_r(void) {
	return uci_get_user_property_int_mm("peek_kcal_r", peek_kcal_r, 40, 256);
}
static int get_peek_kcal_g(void) {
	return uci_get_user_property_int_mm("peek_kcal_g", peek_kcal_g, 40, 256);
}
static int get_peek_kcal_b(void) {
	return uci_get_user_property_int_mm("peek_kcal_b", peek_kcal_b, 40, 256);
}

static int get_kad_kcal(void) {
	return uci_get_user_property_int_mm("kad_kcal", kad_kcal, 0, 1);
}
static int get_kad_repeat_multiply_period(void) {
	return uci_get_user_property_int_mm("kad_repeat_multiply_period", kad_repeat_multiply_period, 0, 1);
}
static int get_kad_disable_fp_input(void) {
	return uci_get_user_property_int_mm("kad_disable_fp_input", kad_disable_fp_input, 0, 1);
}
static int get_kad_disable_touch_input(void) {
	// block if set to disable touch input, or is in aod gesture mode
	return uci_get_user_property_int_mm("kad_disable_touch_input", kad_disable_touch_input, 0, 1) || kad_running_for_aod_gesture;
}
static int get_kad_three_finger_gesture(void) {
	return uci_get_user_property_int_mm("kad_three_finger_gesture", kad_three_finger_gesture, 0, 1);
}
static int get_kad_two_finger_gesture(void) {
	return uci_get_user_property_int_mm("kad_two_finger_gesture", kad_two_finger_gesture, 0, 1);
}
static int get_kad_start_delay_halfseconds(void) {
	return uci_get_user_property_int_mm("kad_start_delay_halfseconds", kad_start_delay_halfseconds, 1, 6);
}


static int smart_get_kad_halfseconds(void) {
	int level = smart_get_notification_level(NOTIF_KAD);
	int ret = uci_get_user_property_int_mm("kad_halfseconds", kad_halfseconds, 5, 20);
	if (level != NOTIF_DEFAULT) {
		ret = max(5,uci_get_user_property_int_mm("kad_halfseconds", kad_halfseconds, 5, 20)/2);
	}
	pr_info("%s smart_notif =========== level: %d  kad halfsec %d \n",__func__, level, ret);
	return ret;
}
static int smart_get_kad_repeat_times(void) {
	int level = smart_get_notification_level(NOTIF_KAD);
	if (level == NOTIF_DEFAULT) return uci_get_user_property_int_mm("kad_repeat_times", kad_repeat_times, 1, 10);
	return max(1,uci_get_user_property_int_mm("kad_repeat_times", kad_repeat_times, 1, 10)/2);
}
static int smart_get_kad_repeat_period_sec(void) {
	int level = smart_get_notification_level(NOTIF_KAD);
	if (level == NOTIF_DEFAULT) return uci_get_user_property_int_mm("kad_repeat_period_sec", kad_repeat_period_sec, 4, 20);
	return uci_get_user_property_int_mm("kad_repeat_period_sec", kad_repeat_period_sec, 4, 20)*2;
}




int is_kad_on(void) {
#ifdef CONFIG_IFILTER_KAD
	if (uci_get_user_property_int_mm("kad_on", kad_on, 0, 1)) {
		return 1;
	}
#endif
	return 0;
}

static int last_screen_lock_check_was_false = 0;


static bool is_screen_locked(void) {
	int lock_timeout_sec = uci_get_sys_property_int_mm("lock_timeout", 0, 0, 1900);
	int locked = uci_get_sys_property_int_mm("locked", 1, 0, 1);
	int time_passed = get_global_seconds() - last_screen_off_seconds;

	pr_info("%s ifilter locked; %d lock timeout: %d time passed after blank: %d \n",__func__,locked, lock_timeout_sec, time_passed);

	if (locked) return true;

	if (!last_screen_lock_check_was_false && time_passed>=lock_timeout_sec) {
		return true;
	}
	if (screen_on) {
		// screen was just turned but not enough time passed...
		// ...till next screen off lock_timeout shouldn't be checked, as with screen on, lock timeout obviously won't happen
		last_screen_lock_check_was_false = 1;
	}
	return false;
}

// if effectively kad starts from leaving proximity / lock this should be set true. use it to block AOD gesture overriding KAD feature...
static bool kad_started_leaving_proximity_or_locked = false;

/**
* determines if kad should start in current stage
* Will store kad_should_start_on_uci_sys_change = 1 if it's blocked by proximity/lock screen not on yet...
*/
static int should_kad_start(void) {
#ifdef CONFIG_IFILTER_KAD
	if (ifilter_ringing || ifilter_screen_waking_app) return 0;
	if (uci_get_user_property_int_mm("kad_on", kad_on, 0, 1)) {
		int level = smart_get_notification_level(NOTIF_KAD);
		if (level != NOTIF_STOP) {
			int proximity = uci_get_sys_property_int_mm("proximity", 0, 0, 1);
			int locked = is_screen_locked()?1:0;
			pr_info("%s kadproximity %d locked %d\n",__func__,proximity, locked);
				// TODO in companion app when screen is off, timeout and locking is not possible to detect... uci_get_sys_property_int_mm("locked", 1, 0, 1);
			if (proximity || !locked) {
				if (get_kad_start_after_proximity_left()) kad_should_start_on_uci_sys_change = 1;
				return 0;
			} else {
				if (kad_should_start_on_uci_sys_change) kad_started_leaving_proximity_or_locked = true;
				kad_should_start_on_uci_sys_change = 0;
				return 1;
			}
		}
	}
#endif
	return 0;
}

static bool store_at_unblank_is_squeeze_peek_kcal = false;
int is_squeeze_peek_kcal(bool unblank) {
	if (unblank) {
		store_at_unblank_is_squeeze_peek_kcal = uci_get_user_property_int_mm("squeeze_peek_kcal", squeeze_peek_kcal, 0, 1) && is_screen_locked();
	}
	return store_at_unblank_is_squeeze_peek_kcal;
}

// variables...
static int kad_running = 0; // state, if KAD is initiated and ongoing screen on...
static int kad_running_for_kcal_only = 0; // state, if KAD is initiated and ongoing screen on but for squeeze peek kcal
static int kad_repeat_counter = 0;
static int needs_kcal_restore_on_screen_on = 0;
static int init_done = 0;

static struct alarm kad_repeat_rtc;


/**
* tells if currently a facedown event from companion app UCI sys triggering, should at the same time do a screen off as well
*/
bool should_screen_off_face_down(int screen_timeout_sec, int face_down) {
	pr_info("%s face down screen off? kad_running %d screen_on %d \n",__func__, kad_running, screen_on);
	if (get_face_down_screen_off() && !kad_running && screen_on) {
		if (smart_get_inactivity_time()<(screen_timeout_sec-3) && face_down) {
			pr_info("%s yes, face down screen off! \n",__func__);
			return true;
		}
	}
	pr_info("%s no, no face down screen off... \n",__func__);
	return false;
}

static struct alarm register_input_rtc;
static enum alarmtimer_restart register_input_rtc_callback(struct alarm *al, ktime_t now)
{
	pr_info("%s kad\n",__func__);
	ntf_input_event(__func__,"");
	return ALARMTIMER_NORESTART;
}

extern int kcal_internal_override(int kcal_sat, int kcal_val, int kcal_cont, int r, int g, int b);
//int kcal_internal_override(int kcal_sat, int kcal_val, int kcal_cont, int r, int g, int b) { return 1; }
extern int kcal_internal_restore(bool force_kcal_update);
//int kcal_internal_restore(void) { return 1; }
extern void kcal_internal_backup(void);
//void kcal_internal_backup(void) { }

static int kad_kcal_overlay_on = 0;
static int kad_kcal_backed_up = 0;

/**
global variable that tells if a sleep should be done in kcal push listener before restoring colors or its immediate
*/
static bool kcal_sleep_before_restore = false;

DEFINE_MUTEX(kcal_read_write_lock);

static void kcal_restore_sync(bool force_kcal_update) {
	mutex_lock(&kcal_read_write_lock);
	if (!kad_running && needs_kcal_restore_on_screen_on && kad_kcal_backed_up && kad_kcal_overlay_on) {
		pr_info("%s kad\n",__func__);
		if (((is_kad_on() && kad_kcal) || is_squeeze_peek_kcal(false)) && screen_on) { 
			int retry_count = 2;
			pr_info("%s kad RRRRRRRRRRRR restore... screen %d kad %d overlay_on %d backed_up %d need_restore %d\n",__func__, screen_on, kad_running, kad_kcal_overlay_on, kad_kcal_backed_up, needs_kcal_restore_on_screen_on);
			while (retry_count-->0) {
				if (screen_on && kcal_internal_restore(force_kcal_update)) {
					needs_kcal_restore_on_screen_on = 0;
					kad_kcal_overlay_on = 0;
					kad_kcal_backed_up = 0; // changes to kcal may happen in user apps...don't take granted it's backed up (like night mode)
					break;
				}
				msleep(5);
			}
		}
	}
	mutex_unlock(&kcal_read_write_lock);
}

static void kcal_restore(struct work_struct * kcal_restore_work) 
{
	pr_info("%s kad ############ restore_backup     screen %d kad %d overlay_on %d backed_up %d need_restore %d\n",__func__, screen_on, kad_running, kad_kcal_overlay_on, kad_kcal_backed_up, needs_kcal_restore_on_screen_on);
	if (kcal_sleep_before_restore) { msleep(250); } // squeeze peek timed out, wait a bit till screen faded enough... otherwise instant restore
	kcal_restore_sync(!kcal_sleep_before_restore);
}
static DECLARE_WORK(kcal_restore_work, kcal_restore);

static int kcal_push_restore = 0;
static void kcal_listener(struct work_struct * kcal_listener_work)
{
	pr_info("%s kad ## kcal listener start   screen %d kad %d overlay_on %d backed_up %d need_restore %d\n",__func__, screen_on, kad_running, kad_kcal_overlay_on, kad_kcal_backed_up, needs_kcal_restore_on_screen_on);
	while (1) {
		if (kcal_push_restore) {
			pr_info("%s kad !! kcal listener restore  screen %d kad %d overlay_on %d backed_up %d need_restore %d\n",__func__, screen_on, kad_running, kad_kcal_overlay_on, kad_kcal_backed_up, needs_kcal_restore_on_screen_on);
			kcal_push_restore = 0;
			if (kcal_sleep_before_restore) { msleep(250); } // 230->250 (oreo screen off a bit longer) is ok, before a screen off happens fully...
			if (screen_on) kcal_restore_sync(!kcal_sleep_before_restore);
			break;
		}
		msleep(5);
//		pr_info("%s kad !! kcal listener running   screen %d kad %d overlay_on %d backed_up %d need_restore %d\n",__func__, screen_on, kad_running, kad_kcal_overlay_on, kad_kcal_backed_up, needs_kcal_restore_on_screen_on);
	}
}
static DECLARE_WORK(kcal_listener_work, kcal_listener);

static void kcal_set(struct work_struct * kcal_set_work)
{
	pr_info("%s kad ## !!!!!!!!!!!!!!!!!! set    screen %d kad %d overlay_on %d backed_up %d need_restore %d\n",__func__, screen_on, kad_running, kad_kcal_overlay_on, kad_kcal_backed_up, needs_kcal_restore_on_screen_on);
	mutex_lock(&kcal_read_write_lock);
	if (kad_running) {
		// store local value to make sure in the full logic there's no sideeffect of changing these settings, while setting up kcal greyscale...
		int local_kad_kcal = get_kad_kcal();
		int local_squeeze_kcal = is_squeeze_peek_kcal(true);
		pr_info("%s kad\n",__func__);
		if (((is_kad_on() && local_kad_kcal && !kad_running_for_kcal_only) || (local_squeeze_kcal && kad_running_for_kcal_only)) && !kad_kcal_overlay_on)
		{
			// make sure to start only after enough time passed since screen on, because with srgb profile colors get wrong if concurs
			unsigned int time_since_screen_on = 0;
			int max_try = 3999;
			while ((!screen_on) && max_try-->=0 ) {
				usleep_range(650,700);
			}
			usleep_range(750,800);
			max_try = 3999;
			time_since_screen_on = jiffies - last_screen_on_early_time;
			while ((time_since_screen_on < 8*JIFFY_MUL) && max_try-->=0 ) {
				usleep_range(650,700);
				time_since_screen_on = jiffies - last_screen_on_early_time;
			}
			// ---- wait for screen on end

			if ((local_kad_kcal || local_squeeze_kcal) && screen_on && !kad_kcal_overlay_on) {
				int retry_count = 2;
				pr_info("%s kad backup... BBBBBBBBBBBB   screen %d kad %d overlay_on %d backed_up %d need_restore %d\n",__func__, screen_on, kad_running, kad_kcal_overlay_on, kad_kcal_backed_up, needs_kcal_restore_on_screen_on);
				while (retry_count-->0) {
					if (screen_on) {
						kcal_internal_backup();
						kad_kcal_backed_up = 1;
						break;
					}
					msleep(5);
				}
			}
		}
		if (((is_kad_on() && local_kad_kcal && !kad_running_for_kcal_only) || (local_squeeze_kcal && kad_running_for_kcal_only)) && kad_kcal_backed_up && !kad_kcal_overlay_on) {
			int retry_count = 60;
			bool done = false;
			pr_info("%s kad override... SSSSSSSSSS   screen %d kad %d overlay_on %d backed_up %d need_restore %d\n",__func__, screen_on, kad_running, kad_kcal_overlay_on, kad_kcal_backed_up, needs_kcal_restore_on_screen_on);
			while (retry_count-->0) {
				if (!kad_running_for_kcal_only && !kad_running_for_aod_gesture) {
					if (screen_on && kcal_internal_override(get_kad_kcal_sat(),get_kad_kcal_val(),get_kad_kcal_cont(), get_kad_kcal_r(), get_kad_kcal_g(), get_kad_kcal_b())) {
						kad_kcal_overlay_on = 1;
						done = true;
						break;
					}
				} else {
					if (screen_on && kcal_internal_override(get_peek_kcal_sat(),get_peek_kcal_val(),get_peek_kcal_cont(), get_peek_kcal_r(), get_peek_kcal_g(), get_peek_kcal_b())) {
						kad_kcal_overlay_on = 1;
						done = true;
						break;
					}
				}
				msleep(10);
			}
			if (!done) pr_info("%s kad SSSS kcal DIDN'T HAPPEN\n",__func__);
		}
	}
	mutex_unlock(&kcal_read_write_lock);
}
static DECLARE_WORK(kcal_set_work, kcal_set);



// Signal int when squeeze2peek triggered set to 1, while waiting for time passing, before the automatic screen off.
// It is used also when a second short squeeze happens, which should interrupt the process by setting this to 0, 
// and don't let auto screen off happen in squeeze_peekmode function.
static int squeeze_peek_wait = 0;


extern void set_notification_booster(int value);
extern int get_notification_booster(void);
extern void set_notification_boost_only_in_pocket(int value);
extern int get_notification_boost_only_in_pocket(void);

// value used to signal that HOME button release event should be synced as well in home button func work if it was not interrupted.
static int do_home_button_off_too_in_work_func = 0;

static int wait_for_squeeze_power = 0;
static unsigned long last_squeeze_power_registration_jiffies = 0;

/* PowerKey work func */
static void ifilter_presspwr(struct work_struct * ifilter_presspwr_work) {

	unsigned int squeeze_reg_diff = 0;
	if (!mutex_trylock(&pwrkeyworklock))
                return;
	// if wait for squeeze power is on (called from squeeze code)...
	if (wait_for_squeeze_power) {
		wait_for_squeeze_power = 0;
		// if screen is on...
		if (screen_on) {
			// ...wait a bit...
			msleep(30);
			// .. check if last power resgistration through threshold reg callback happened lately... if so no need to do screen off..
			squeeze_reg_diff = jiffies - last_squeeze_power_registration_jiffies;
			pr_info("%s squeeze_reg_diff %u\n",__func__,squeeze_reg_diff);
			if (squeeze_reg_diff< 4*JIFFY_MUL ) goto exit;
		}
	}
	pr_info("%s power press sync - screen_on: %d \n",__func__, screen_on);
	input_event(ifilter_pwrdev, EV_KEY, KEY_POWER, 1);
	input_event(ifilter_pwrdev, EV_SYN, 0, 0);
	msleep(IFILTER_PWRKEY_DUR);
	input_event(ifilter_pwrdev, EV_KEY, KEY_POWER, 0);
	input_event(ifilter_pwrdev, EV_SYN, 0, 0);
	msleep(IFILTER_PWRKEY_DUR/2);
	// resetting this do_home_button_off_too_in_work_func when powering down, as it causes the running HOME button func work 
	//	to trigger a HOME button release sync event to input device, resulting in an unwanted  screen on.
	do_home_button_off_too_in_work_func = 0;
	exit:
        mutex_unlock(&pwrkeyworklock);
	return;
}
static DECLARE_DELAYED_WORK(ifilter_presspwr_work, ifilter_presspwr);

static void ifilter_vib(void) {
	// avoid using squeeze vib length 15...
	set_vibrate_2(20,get_vib_strength());
}
static void ifilter_vib_work_func(struct work_struct * ifilter_vib_work) {
	ifilter_vib();
}
static DECLARE_WORK(ifilter_vib_work, ifilter_vib_work_func);


/* PowerKey trigger */
static void ifilter_pwrtrigger(int vibration, int delay, const char caller[]) {
	if (vibration) ifilter_vib();
	pr_info("%s power press - screen_on: %d caller %s\n",__func__, screen_on,caller);
	mod_delayed_work(ifilter_pwr_wq, &ifilter_presspwr_work, msecs_to_jiffies(delay));
        return;
}


static void ifilter_input_callback(struct work_struct *unused) {
	return;
}

static void ifilter_input_event(struct input_handle *handle, unsigned int type,
				unsigned int code, int value) {
}

static int ifilter_input_dev_filter(struct input_dev *dev) {
	pr_info("%s %s\n",__func__, dev->name);
	if (strstr(dev->name, "uinput-fpc") || strstr(dev->name, "fpc1020") || strstr(dev->name, "gf_input") || strstr(dev->name, "uinput-goodix")) {
		return 0;
	} else {
		return 1;
	}
}

static int ifilter_input_connect(struct input_handler *handler,
				struct input_dev *dev, const struct input_device_id *id) {
	struct input_handle *handle;
	int error;

	if (ifilter_input_dev_filter(dev))
		return -ENODEV;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "ifilter";

	error = input_register_handle(handle);

	error = input_open_device(handle);

	return 0;

}


// break home button func -> in the HOME button work func, where we count from a sync-surpressend first press of HOME button, this is used in external sources 
// 	to break counting of time passing. This way, HOME button press sync can be avoided, and double tap of HOME button can be translated into a POWER OFF even instead.
//	If this value is set to 1, counting will break, and work func will exit without calling INPUT device HOME sync.
//	meanwhile the other func_trigger call will still face LOCK locking, and based on the locking=true will start a Power off.
static int break_home_button_func_work = 1;

// time_count_done_in_home_button_func_work -> represents if the time counting in the HOME button work func is over, meaning that the next HOME button press in func_trigger code
//	shouldn't be interpreted as a double tap instead 
// 	just exit the func trigger call without Power off and leaving normal HOME button sync to work in the home_button work func after the time count
static int time_count_done_in_home_button_func_work = 0;

// job_done_in_home_button_func_work -> represents if we arrived inside the home button work func at the counting of time, without interruption (break_home_button still 0), thus it can be set to 1,
//	HOME button 1 sync will be done in work, and it's also signalling that when the FP device sends release event, in the filter code, HOME button 0 sync can be done.
//	The trigger func will set it to 0 always, so it shows the job was interrupted, which is important when the release of the button is done after
//		trigger job found that the LOCK is not holding anymore, and does nothing, in which case the filter call should send HOME - 0 sync to input device.
static int job_done_in_home_button_func_work = 0;

// signals if fingerprint PRESS was registered, so we can track that no multiple releases happen from FP device
static int fingerprint_pressed = 0;

// signals when the powering down of screen happens while FP is still being pressed, so filter won't turn screen on, when the button is released based on this value.
static int powering_down_with_fingerprint_still_pressed = 0;


// minimum doubletap wait latency will be: (BASE_VALUE + PERIOD) * FUNC_CYLCE_DUR -> minimum is right now (9+0) * 9 = 81msec
#define DT_WAIT_PERIOD_MAX 9
#define DT_WAIT_PERIOD_BASE_VALUE 12
#define DT_WAIT_PERIOD_DEFAULT 2
static int doubletap_wait_period = DT_WAIT_PERIOD_DEFAULT;
static int get_doubletap_wait_period(void) {
	return uci_get_user_property_int_mm("fp_doubletap_wait_period", doubletap_wait_period, 0, 9);
}

/* Home button work func 
	will start with trying to lock worklock
	then use vibrator to signal button press 'imitation'
	Will set break_home_button_func_work to 0, as we just started, and interruptions are signalled through this integer
	While break is not done, it will count the maximum time that is acceptable between two BUTTON presses whchi is interpreted as double press
	- If it's exiting due to Interruption (break_home_button_func_work called from another func_trigger call)
	    it won't do anything just release lock and return. The trigger call will then power down screen, as this counts as double tap
	- If it exited with counting done (break == 0) it will sync a HOME = 1 event to itself
	    - and it will signal job_done_in_home_button_func_work = 1, so when filter func receives Key released, it can Sync a HOME = 0 to input device,
		or set do_home_buttons_too -> 1, so the hom button func work job will itself send the HOME = 0 synced before exiting
*/
static void ifilter_home_button_func(struct work_struct * ifilter_presspwr_work) {
	int count_cycles = 0;
	if (!mutex_trylock(&ifilteruncworklock)) {
		return;
	}
	break_home_button_func_work = 0;
	time_count_done_in_home_button_func_work = 0;
	ifilter_vib();
	while (!break_home_button_func_work) {
		count_cycles++;
		if (count_cycles > (DT_WAIT_PERIOD_BASE_VALUE + get_doubletap_wait_period())) {
			break;
		}
		msleep(FUNC_CYCLE_DUR);
		pr_debug("ifilter %s counting in cycle before KEY_HOME 1 synced: %d / %d cycles \n",__func__, count_cycles, DT_WAIT_PERIOD_BASE_VALUE+get_doubletap_wait_period());
	}
	time_count_done_in_home_button_func_work = 1;
	if (break_home_button_func_work == 0) {
		job_done_in_home_button_func_work = 1;
		pr_info("ifilter %s home 1 \n",__func__);
		if (get_ifilter_key()!=KEY_KPDOT) {
			input_event(ifilter_pwrdev, EV_KEY, get_ifilter_key(), 1);
			input_event(ifilter_pwrdev, EV_SYN, 0, 0);
			msleep(1);
			if (do_home_button_off_too_in_work_func) {
				pr_info("ifilter %s home 0 \n",__func__);
				input_event(ifilter_pwrdev, EV_KEY, get_ifilter_key(), 0);
				input_event(ifilter_pwrdev, EV_SYN, 0, 0);
				do_home_button_off_too_in_work_func = 0;
				msleep(1);
	//			ifilter_vib();
			}
		} else {
			if (do_home_button_off_too_in_work_func) {
				write_uci_out("fp_touch");
			}
		}
	} 
	mutex_unlock(&ifilteruncworklock);
	pr_info("ifilter %s mutex unlocked \n",__func__);
	return;
}
static DECLARE_WORK(ifilter_home_button_func_work, ifilter_home_button_func);


/* ifilter home button func trigger */
static void ifilter_home_button_func_trigger(void) {
	pr_info("ifilter %s time_count_done_in_home_button_func_work %d job_done_in_home_button_func_work %d\n",__func__, time_count_done_in_home_button_func_work, job_done_in_home_button_func_work);
	job_done_in_home_button_func_work = 0;
	break_home_button_func_work = 1;
	if (mutex_is_locked(&ifilteruncworklock)) {
		// mutex in hold, this means the HOME button was pressed again in a short time...
		pr_info("ifilter %s is locked, checkin %d time_count_done_in_home_button_func_work...", __func__, time_count_done_in_home_button_func_work);
		if (!time_count_done_in_home_button_func_work) { // and we still counting the cycles in the job, so double tap poweroff can be done...
			// double home:
			pr_info("ifilter double tap home, power off\n");
			if (fingerprint_pressed == 1) { // there was no release of the fingerprint button, so go screen off with signalling that here...
				powering_down_with_fingerprint_still_pressed = 1;
			} else { 
				powering_down_with_fingerprint_still_pressed = 0; 
			}
			queue_work(ifilter_vib_wq, &ifilter_vib_work);
			mdelay(50); // delay a bit, so finger up can trigger input event in goofix driver before screen off suspend...causes issues with fp input events after screen wake otherwise
			ifilter_pwrtrigger(0,0,__func__);
			do_home_button_off_too_in_work_func = 0;
		}
                return;
	}
	schedule_work(&ifilter_home_button_func_work);
        return;
}

DEFINE_MUTEX(stop_kad_mutex);

void ntf_block_camera(bool state) {
}
EXPORT_SYMBOL(ntf_block_camera);

// kad stop
static void stop_kad_running(bool instant_sat_restore, const char* caller)
{
	if (instant_sat_restore) {
		squeeze_peek_wait = 0; // interrupt peek wait, touchscreen was interacted, don't turn screen off after peek time over...
	}
	if (!mutex_trylock(&stop_kad_mutex)) {
		return;
	}
	pr_info("%s %s ----------- stop kad running ---------\n",__func__,caller);
	ntf_block_camera(false);
	kad_should_start_on_uci_sys_change = 0;
	kad_started_leaving_proximity_or_locked = false;
	if (kad_running) {
		kad_running = 0;
		if (instant_sat_restore) {
			kcal_sleep_before_restore = false;
			needs_kcal_restore_on_screen_on = 1;
			kcal_push_restore = 1;
		} else {
			kcal_sleep_before_restore = true; // wait a bit as screen will turn off...so it's not visible when saturation back
			needs_kcal_restore_on_screen_on = 1;
			kcal_push_restore = 1;
		}
	}
	kad_running_for_kcal_only = 0;
	kad_running_for_aod_gesture = 0;
	mutex_unlock(&stop_kad_mutex);
}

static void ts_poke(void);

void register_fp_wake(void) {
	pr_info("%s kad ifilter fp wake registered\n",__func__);
	if (screen_on_full && !screen_off_early && (!get_kad_disable_fp_input() || !kad_running || kad_running_for_kcal_only)) {
		bool poke = kad_kcal_overlay_on;
		if (init_done) {
			alarm_cancel(&kad_repeat_rtc);
		}
		stop_kad_running(true,__func__);
		if (poke) {
			ts_poke();
		}
	}

// pixel 5
	ntf_input_event(__func__,"");

// fp tee driver does not call ever the register_fp_irq part, so the vibration based detection will handle register input event part instead. Leave this commented, to avoid "pocket-touches" cancelling out notifications
/*	if (init_done && (!kad_running || !get_kad_disable_fp_input()) && screen_on_full) {
		ktime_t wakeup_time;
		ktime_t curr_time = { .tv64 = 0 };
		wakeup_time = ktime_add_us(curr_time,
		    (1 * 500LL)); // msec to usec
		alarm_cancel(&register_input_rtc);
		alarm_start_relative(&register_input_rtc, wakeup_time); // start new...
	}*/
}
EXPORT_SYMBOL(register_fp_wake);
void register_fp_irq(void) {
	pr_info("%s kad ifilter fp tap irq registered\n",__func__);
	if (screen_on_full && !screen_off_early && (!get_kad_disable_fp_input() || !kad_running || kad_running_for_kcal_only)) {
		bool poke = kad_kcal_overlay_on;
		if (init_done) {
			alarm_cancel(&kad_repeat_rtc);
		}
		stop_kad_running(true,__func__);
		if (poke) {
			ts_poke();
		}
	}
	if (init_done && screen_on_full && !screen_off_early) { 
	// only register user input when screen is on, cause FP Wake is not enabled for FP, meaning it shouldn't count as a user input while screen is still off
//		ktime_t wakeup_time;
//		ktime_t curr_time = ktime_get();
//		wakeup_time = ktime_add_us(curr_time,
//		    (1 * 500LL)); // msec to usec
		alarm_cancel(&register_input_rtc);
		alarm_start_relative(&register_input_rtc, ms_to_ktime(1)); // start new...
	}
}
EXPORT_SYMBOL(register_fp_irq);


static unsigned long last_fp_down = 0;
static unsigned long last_fp_short_touch = 0;

static bool triple_tap_wait = false;

static struct alarm triple_tap_rtc;
static enum alarmtimer_restart triple_tap_rtc_callback(struct alarm *al, ktime_t now)
{
	triple_tap_wait = false;
// home button simulation
	if (get_ifilter_key()!=KEY_KPDOT) {
	input_report_key(ifilter_pwrdev, get_ifilter_key(), 1);
	input_sync(ifilter_pwrdev);
	input_report_key(ifilter_pwrdev, get_ifilter_key(), 0);
	input_sync(ifilter_pwrdev);
	} else write_uci_out("fp_touch");
	return ALARMTIMER_NORESTART;
}



/*
    filter will work on FP card events.
    if screen is not on it will work on powering it on when needed (except when Button released start (button press) was started while screen was still on: powering_down_with_fingerprint_still_pressed = 1)
    Otherwise if
	- pressed (value > 0)
		it will call home button trigger job, to handle single fp button presses or double taps.
	- if released:
		if home button work job is done already, finish with syncing HOME release to input device
		if home button work job is still running, set 'do_home_buttons_off_too' to 1, so job will sync HOME release as well
*/
static bool ifilter_input_filter(struct input_handle *handle,
                                    unsigned int type, unsigned int code,
                                    int value)
{
	pr_info("%s event t:%d c:%d v:%d\n",__func__,type,code,value);
	if (type != EV_KEY)
		return false;

	ntf_input_event(__func__,"");
	if (screen_on_full && !screen_off_early) {
		squeeze_peek_wait = 0; // interrupt peek wait, touchscreen was interacted, don't turn screen off after peek time over...
		if (kad_running || kad_running_for_kcal_only) { 
			stop_kad_running(true,__func__);
			ts_poke();
		}
	}


	// if it's not on, don't filter anything...
	if (get_ifilter_switch() == 0) {
#if 1
// op6 specific
		if (code == KEY_HOME) return true; // do not let KEY HOME through for OP6...
#endif
		return false;
	}

	if (code != KEY_HOME && code != KEY_WAKEUP && code != KEY_UP && code != KEY_DOWN && code!=BTN_GAMEPAD) // avoid ?? KEY_EAST ? 305 // OP6
		return false; // do not react on this code...

	if (uci_fp_swipe_mode) {
		if (code==KEY_UP || code==KEY_DOWN) {
			pr_info("%s letting thru key up/down on fp : %d\n",__func__,code);
			return false;
		}
	}


	if (code == KEY_WAKEUP) {
		pr_debug("ifilter - wakeup %d %d \n",code,value);
	}


	if (get_ifilter_switch() == IFILTER_SWITCH_DTAP_TTAP) {
		if (value > 0) {
		// finger touching sensor
			if (!screen_on) {
				return false; // don't filter so pin appears
			} else {
				// screen is on, first touch on fp
				fingerprint_pressed = 1;
				last_fp_down = jiffies;
				ifilter_vib();
			}
			if (triple_tap_wait) {
				// third tap touching, cancel job, to let user rethink, and hold tap to do nothing instead of home
				// this is fine to cancel here, because we do not set triple_tap_wait to false -> so short fp tap lift can still mean Screen off
				alarm_cancel(&triple_tap_rtc);
			}
		} else {
		// finger leaving sensor
			if (fingerprint_pressed) {
				if (!screen_on) {
					return false;
				} else {
					unsigned int fp_down_up_diff = jiffies - last_fp_down;
					fingerprint_pressed = 0;
					// release
					if (fp_down_up_diff < 20 * JIFFY_MUL) {
						// this was an intentional short tap on the fp, let's see doubletapping times..
						unsigned int last_short_tap_diff = jiffies - last_fp_short_touch;
						last_fp_short_touch = jiffies;
						if (last_short_tap_diff> (DT_WAIT_PERIOD_BASE_VALUE + 9 + get_doubletap_wait_period()*2) * JIFFY_MUL) {
							// to big difference between the two taps
							return false;
						} else {
							if (triple_tap_wait) { // triple tap happened, power off
								alarm_cancel(&triple_tap_rtc);
								triple_tap_wait = false;
								ifilter_pwrtrigger(0,0,__func__);
							} else {
//								ktime_t wakeup_time;
//								ktime_t curr_time = ktime_get();
								triple_tap_wait = true;
//								wakeup_time = ktime_add_us(curr_time,
//									    (((DT_WAIT_PERIOD_BASE_VALUE + 9 + get_doubletap_wait_period()*2)*10LL + 5LL) * 1000LL)); // msec to usec 30+ jiffies (300msec)
								alarm_cancel(&triple_tap_rtc);
								alarm_start_relative(&triple_tap_rtc, ms_to_ktime(
										(((DT_WAIT_PERIOD_BASE_VALUE + 9 + get_doubletap_wait_period()*2)*10LL + 5LL))
										)
									); // start new...
							}
						}
					}
				}
			}
		}
#if 0
	} else if (get_ifilter_switch() == IFILTER_SWITCH_DTAP_LDTAP) {
	// phone back fingerprint sensor working : home button dtap, longer dtap sleep
		if (value > 0) {
		// finger touching sensor
			if (!screen_on) {
				return false; // don't filter so pin appears
			} else {
				// screen is on, first touch on fp
				fingerprint_pressed = 1;
				last_fp_down = jiffies;
				ifilter_vib();
			}
		} else {
		// finger leaving sensor
			if (fingerprint_pressed) {
				if (!screen_on) {
					return false;
				} else {
					unsigned int fp_down_up_diff = jiffies - last_fp_down;
					fingerprint_pressed = 0;
					// release
					if (fp_down_up_diff < 20 * JIFFY_MUL) {
						// this was an intentional short tap on the fp, let's see doubletapping times..
						unsigned int last_short_tap_diff = jiffies - last_fp_short_touch;
						last_fp_short_touch = jiffies;
						if (last_short_tap_diff> 60 * JIFFY_MUL) {
							return false;
						} else {
							if (last_short_tap_diff > (DT_WAIT_PERIOD_BASE_VALUE + 9 + get_doubletap_wait_period()*2) * JIFFY_MUL) { // long doubletap
								ifilter_pwrtrigger(0,0,__func__);
							} else { // short doubletap
								if (get_ifilter_key()!=KEY_KPDOT) {
								// home button simulation
								input_report_key(ifilter_pwrdev, get_ifilter_key(), 1);
								input_sync(ifilter_pwrdev);
								input_report_key(ifilter_pwrdev, get_ifilter_key(), 0);
								input_sync(ifilter_pwrdev);
								} else write_uci_out("fp_touch");
							}
						}
					}
				}
			}
		}
#endif
	} else if (get_ifilter_switch() == IFILTER_SWITCH_DTAP) {
	//standalone kernel mode. double tap means switch off
	if (value > 0) {
		if (!screen_on) {
			return false; // don't filter so pin appears
		} else {
			fingerprint_pressed = 1;
			pr_info("ifilter %s starting trigger \n",__func__);
			ifilter_home_button_func_trigger();
		}
		return true;
	} else {
		if (fingerprint_pressed) {
			if (!screen_on) {
				if (!powering_down_with_fingerprint_still_pressed) {
					return false; // don't filter so pin appears
				} else {
					// fingerprint release happens after a screen off that started AFTER the fingerprint was pressed. So do not wake the screen
					powering_down_with_fingerprint_still_pressed = 0;
					return false;
				}
			} else {
				// screen is on...
				// release the fingerprint_pressed variable...
				fingerprint_pressed = 0;
				// if job was all finished inside the work func, we need to call the HOME = 0 release event here, as we couldn't signal to the work to do it on it's own
				if (job_done_in_home_button_func_work) {
						if (get_ifilter_key()!=KEY_KPDOT) {
						pr_info("ifilter %s do key_home 0 sync as job was done, but without the possible signalling for HOME 0\n",__func__);
						input_report_key(ifilter_pwrdev, get_ifilter_key(), 0);
						input_sync(ifilter_pwrdev); 
						} else write_uci_out("fp_touch");
				} else {
				// job is not yet finished in home button func work, let's signal it, to do the home button = 0 sync as well
					if (screen_on) {
						do_home_button_off_too_in_work_func = 1;
					} else {
						return false;
					}
				}
			}
			return true;
		} else 
		{ // let event flow through
			return false;
		}
	}
	}
	if (get_ifilter_switch() == 1) {
		// simple home button mode, user space handles behavior
		if (!screen_on) {
			return false;
		}
		if (value > 0) {
			ifilter_vib();
			input_report_key(ifilter_pwrdev, KEY_HOME, 1);
			input_sync(ifilter_pwrdev);
		} else {
			input_report_key(ifilter_pwrdev, KEY_HOME, 0);
			input_sync(ifilter_pwrdev);
		}
	}
	return true;
}



// ---------------- SQUEEZE TO WAKE SLEEP: 
// wakelock method code
static int squeeze_wake = 0;
static int squeeze_sleep = 0;
static int squeeze_sleep_on_long = 0;
static int squeeze_peek = 0;
static int squeeze_peek_halfseconds = 4;

static int get_squeeze_wake(void) {
	return uci_get_user_property_int_mm("squeeze_wake", squeeze_wake, 0, 1);
}
static int get_squeeze_sleep(void) {
	return uci_get_user_property_int_mm("squeeze_sleep", squeeze_sleep, 0, 1);
}
static int get_squeeze_sleep_on_long(void) {
	return uci_get_user_property_int_mm("squeeze_sleep_on_long", squeeze_sleep_on_long, 0, 1);
}
static int get_squeeze_peek(void) {
	return uci_get_user_property_int_mm("squeeze_peek", squeeze_peek, 0, 1);
}
static int get_squeeze_peek_halfseconds(void) {
	return uci_get_user_property_int_mm("squeeze_peek_halfseconds", squeeze_peek_halfseconds, 2, 12);
}


DEFINE_MUTEX(start_kad_mutex);
#define KAD_FOR_SQUEEZE 1
#define KAD_FOR_AOD 2
#define KAD_FOR_KAD 0
static void start_kad_running(int origination) {
	if (!mutex_trylock(&start_kad_mutex)) {
		return;
	}
	pr_info("%s === ----------- start kad running --------- ==\n", __func__);
	kad_running = 1;
	kad_running_for_kcal_only = origination==KAD_FOR_SQUEEZE;
	kad_running_for_aod_gesture = origination==KAD_FOR_AOD;
	if (origination==KAD_FOR_SQUEEZE) { // squeeze peek should always block camera
		ntf_block_camera(true);
	}
	if (origination==KAD_FOR_AOD) {
		if (get_kad_pick_up_block_camera()) { // block face unlock camera?
			ntf_block_camera(true);
		}
	}
	pr_info("%s kad - origination: %d \n",__func__, origination);
	if (is_screen_locked()) {
		if ((is_kad_on()&&get_kad_kcal())||(kad_running_for_kcal_only&&is_squeeze_peek_kcal(true))) {
			schedule_work(&kcal_set_work);
			kcal_push_restore = 0;
			queue_work(kcal_listener_wq, &kcal_listener_work);
		}
	}
	mutex_unlock(&start_kad_mutex);
}


#if 0
// defines what maximum level of user setting for minimum squeeze power set on sense ui
// will set kernel-side squeeze-to-sleep/wake active. This way user can set below this
// level the squeeze power on Sense UI, and kernel-squeeze handling will turn on
static int squeeze_power_kernel_max_threshold = 1;// 0 - 9
// 112, 132, 152, 172, 192, 212, 232, 252, 272, 292
// 100-120 - 121-130...- 280-300
static int get_squeeze_power_kernel_max_threshold(void) {
	return uci_get_user_property_int_mm("squeeze_power_kernel_max_threshold", squeeze_power_kernel_max_threshold, 0,9);
}
#endif

// int that signals if kernel should handle squeezes for squeeze to sleep/wake
static int squeeze_kernel_handled = 1;

void register_squeeze_power_threshold_change(int power) {
#if 0
	int new_level = (power - 101) / 20;
	pr_info("%s squeeze call new_level power %d max level %d power %d \n",__func__,new_level,get_squeeze_power_kernel_max_threshold(),power);
	if (new_level <= get_squeeze_power_kernel_max_threshold() && power>=100) { // at least raw squeeze power -> 100 it should be, below that first notch is not registered...
		squeeze_kernel_handled = 1;
	} else {
		squeeze_kernel_handled = 0;
	}
#endif
	last_squeeze_power_registration_jiffies = jiffies;
	squeeze_kernel_handled = 1;
}
EXPORT_SYMBOL(register_squeeze_power_threshold_change);

static void squeeze_vib(void) {
	set_vibrate_2(15,5); // a bit weaker click on pixel4
}


// ===========
// Swipe
// ===========

// sysfs parameters
static int squeeze_swipe = 0;
static int squeeze_swipe_vibration = 1;

static int get_squeeze_swipe(void) {
	return uci_get_user_property_int_mm("squeeze_swipe", squeeze_swipe, 0, 1);
}
static int get_squeeze_swipe_vibration(void) {
	return uci_get_user_property_int_mm("squeeze_swipe_vibration", squeeze_swipe_vibration, 0, 1);
}
static int get_squeeze_long_vibration(void) {
	return uci_get_user_property_int_mm("squeeze_long_vibration", 0, 0, 1);
}

// members...
static int squeeze_swipe_dir = 1;
int last_mt_slot = 0;
int last_emulated_mt_slot = 0;
int highest_mt_slot = 0;
int pseudo_rnd = 0;

int swipe_step_wait_time_mul = 100; // tune this to find optimal slowness of swipe for all kinds of apps

unsigned long last_scroll_emulate_timestamp = 0;
static DEFINE_MUTEX(squeeze_swipe_lock);

#define SWIPE_ACCELERATED_TIME_LIMIT 150 * JIFFY_MUL
int interrupt_swipe_longcount = 0;
int swipe_longcount_finished = 1;
unsigned long swipe_longcount_start = 0;
static void swipe_longcount(struct work_struct * swipe_longcount_work) {
	while (1) {
		if (interrupt_swipe_longcount) {
			interrupt_swipe_longcount = 0;
			return;
			pr_info("%s ######## squeeze call || swipe_longcount interrupted\n",__func__);
		}
		if (jiffies - swipe_longcount_start > SWIPE_ACCELERATED_TIME_LIMIT) {
			pr_info("%s ######## squeeze call || swipe_longcount VIBRATION !! \n",__func__);
			swipe_longcount_finished = 1;
			if (get_squeeze_swipe_vibration() && screen_on && get_squeeze_sleep()) {
				//set_vibrate(20);
				set_vibrate(25); // clickety vibration special value on u12+
			}
			return;
		}
		msleep(1);
	}
}
static DECLARE_WORK(swipe_longcount_work, swipe_longcount);
static void swipe_longcount_trigger(void) {
	swipe_longcount_finished = 0;
	interrupt_swipe_longcount = 0;
	schedule_work(&swipe_longcount_work);
}


#if 1
#define TS_MAP_SIZE 1000
static int ts_current_type[TS_MAP_SIZE], ts_current_code[TS_MAP_SIZE], ts_current_value[TS_MAP_SIZE];
static int ts_current_count = 0;
static int ts_emulated_events_in_progress = 0;
// global filter, ts finger touching counter...
static int finger_counter = 0;

static int ts_track_type[TS_MAP_SIZE], ts_track_code[TS_MAP_SIZE], ts_track_value[TS_MAP_SIZE];
static int ts_track_size = 0;
static int ts_track_intercepted = 0;
static int ts_track_mistmatch = 0;
static int ts_track_47_count = 0;
static void ts_track_event_clear(bool clear_mismatch) {
	pr_info("%s\n",__func__);
	ts_track_size = 0;
	ts_track_intercepted = 0;
	if (clear_mismatch) ts_track_mistmatch = 0;
}
static void ts_track_event_gather(int type, int code, int value) {
	ts_track_type[ts_track_size] = type;
	ts_track_code[ts_track_size] = code;
	ts_track_value[ts_track_size] = value;
	ts_track_size++;
	pr_info("%s ---- add Input: %d %d %d Size: %d\n",__func__,type,code,value,ts_track_size);
}
static void ts_track_event_run(void) {
	int i;
	for (i=0;i<ts_track_size;i++) {
		input_event(ts_device,ts_track_type[i],ts_track_code[i],ts_track_value[i]);
	}
}
DEFINE_MUTEX(track_check_lock);
static int ts_track_event_check(int type, int code, int value) {
	//mutex_lock(&track_check_lock);
	{
	int i = ts_track_intercepted;
	pr_info("%s #### checking Input: %d %d %d Against: %d %d %d | size %d | found %d \n",__func__,type,code,value, ts_track_type[i], ts_track_code[i], ts_track_value[i], ts_track_size, ts_track_intercepted);
	if (ts_track_type[i] == type &&
		ts_track_code[i] == code &&
		ts_track_value[i] == value) {
		pr_info("%s ++++ intercepted Input: %d %d %d Against: %d %d %d \n",__func__,type,code,value, ts_track_type[i], ts_track_code[i], ts_track_value[i]);
		if (ts_track_47_count>0) { ts_track_47_count--; }
		ts_track_intercepted++;
		//mutex_unlock(&track_check_lock);
		return 1;
	}
	if (type == EV_ABS && code == 47) {
		ts_track_47_count++;
	} else {
		if (ts_track_47_count>0) { ts_track_47_count--; ts_track_intercepted++; }
	}
	pr_info("%s ---- mismatch Input: %d %d %d Against: %d %d %d \n",__func__,type,code,value, ts_track_type[i], ts_track_code[i], ts_track_value[i]);
	ts_track_mistmatch++;
	}
	//mutex_unlock(&track_check_lock);
	return 0;
}
static int dump_count = 0;
static int ts_track_event_complete(void) {
	pr_info("%s ???? checking | size %d | found %d \n",__func__, ts_track_size, ts_track_intercepted);
	if (dump_count++ % 20 && ts_track_size <4) {
		int i = 0;
		for (i = ts_track_intercepted; i<ts_track_size;i++) 
		{
			pr_info("%s ----# Input left [%d]: %d %d %d \n",__func__, i, ts_track_type[i], ts_track_code[i], ts_track_value[i]);
		}
		dump_count = 0;
	}
	return ts_track_intercepted == ts_track_size;
}


static int longcount_squeeze_swipe_dir_change = 0;

static int last_swipe_very_quick = 0;



int is_real_ts_input_filtered(void) {
	return (mutex_is_locked(&squeeze_swipe_lock));
}
EXPORT_SYMBOL(is_real_ts_input_filtered);

static void ts_poke_emulate(struct work_struct * ts_poke_emulate_work) {
	int i;
	int local_slot = last_mt_slot;
	pr_info("%s ts_input checking finger counter over 0, then don't simulate %d\n",__func__, finger_counter);
	pr_info("%s ts_input ######### squeeze try_lock #########\n",__func__);
	if (!mutex_trylock(&squeeze_swipe_lock)) {
		return;
	}
	for (i=0; i<TS_MAP_SIZE; i++) {
		ts_current_type[i]=100;
	}
	ts_emulated_events_in_progress = 0;
	ts_current_type[0] = 3;	ts_current_code[0] = 47;ts_current_value[0] = 31;
	ts_current_count = 1;
	{
		int y_diff = 1100;
		int y_delta = -6;
		int y_steps = 5;
		int pseudo_rnd = 0;
		swipe_step_wait_time_mul = 200;
		{
			int empty_check_count = 0;
			int first_steps = 1;
			int second_step_done = 0;
			unsigned long start_time = jiffies;
			unsigned int diff_time = 0;

			ts_track_event_clear(true);
			while (y_steps-->0) {
				if (first_steps) {
//#ifndef CONFIG_IFILTER_TS_PRESSURE
					ts_track_event_gather(EV_KEY, BTN_TOUCH, 1);
					ts_track_event_gather(EV_KEY, BTN_TOOL_FINGER, 1);
//#endif
					ts_track_event_gather(EV_ABS, ABS_MT_SLOT, ++local_slot);
					ts_track_event_gather(EV_ABS, ABS_MT_TRACKING_ID, local_slot);
					first_steps = 0;
				} else {
					if (!second_step_done) {
						ts_track_event_clear(true); // clear out "47" mismatch at start
						second_step_done = 1;
					}
					ts_track_event_clear(false);
				}
				ts_track_event_gather(EV_ABS, ABS_MT_POSITION_X, 700+ (pseudo_rnd++)%2);
				ts_track_event_gather(EV_ABS, ABS_MT_POSITION_Y, 1000+y_diff);
				y_diff += y_delta;
//#ifdef CONFIG_IFILTER_TS_PRESSURE
				ts_track_event_gather(EV_ABS, ABS_MT_PRESSURE, 70+ (pseudo_rnd%2));
//#else
				ts_track_event_gather(EV_ABS, ABS_MT_TOUCH_MAJOR, 3+ (pseudo_rnd%2));
				ts_track_event_gather(EV_ABS, ABS_MT_TOUCH_MINOR, 3+ (pseudo_rnd%2));
//#endif
				ts_track_event_gather(EV_SYN, 0, 0);
				ts_track_event_run();
				udelay(5 * swipe_step_wait_time_mul);
				if (y_steps%10==0) {
					pr_info("%s ts_input squeeze emulation step = %d POS_Y = %d \n",__func__,y_steps, 1000+y_diff);
				}
				while(!ts_track_event_complete()) {
					diff_time = jiffies - start_time;
					if (diff_time>4*JIFFY_MUL) {
						pr_info("%s breaking incomplete check cycle ts_check\n",__func__);
						break;
					}
					msleep(1);
				}
			}
			pr_info("ifilter %s ts DOWN 0 \n",__func__);
			{
				ts_track_event_clear(true);
			}
//#ifndef CONFIG_IFILTER_TS_PRESSURE
			ts_track_event_gather(EV_KEY, BTN_TOUCH, 0);
			ts_track_event_gather(EV_KEY, BTN_TOOL_FINGER, 0);
//#endif
			ts_track_event_gather(EV_ABS, ABS_MT_TRACKING_ID, -1);
			ts_track_event_gather(EV_SYN, 0, 0);
			ts_track_event_run();
			msleep(1);

			while(!ts_track_event_complete()) {
				msleep(1);
				empty_check_count++;
				if (empty_check_count%100==30) {
					pr_info("%s ts_check || fallback\n",__func__);
					input_event(ts_device,EV_ABS,ABS_MT_SLOT,0);
					input_event(ts_device,EV_ABS,ABS_MT_TRACKING_ID,-1);
					input_event(ts_device,EV_SYN,0,0);
					msleep(5);

					ts_track_event_clear(true);
					ts_track_event_gather(EV_ABS, ABS_MT_SLOT,31);
					ts_track_event_gather(EV_ABS, ABS_MT_TRACKING_ID, highest_mt_slot+1);
					ts_track_event_gather(EV_ABS, ABS_MT_POSITION_X, 0);
					ts_track_event_gather(EV_ABS, ABS_MT_POSITION_Y, 0);
//#ifdef CONFIG_IFILTER_TS_PRESSURE
					ts_track_event_gather(EV_ABS, ABS_MT_PRESSURE, 40);
//#endif
					ts_track_event_gather(EV_SYN, 0, 0);
					ts_track_event_run();
					while(!ts_track_event_complete()) {
						msleep(1);
						pr_info("%s ts_check || fallback wait 1\n",__func__);
						diff_time = jiffies - start_time;
						if (diff_time>30*JIFFY_MUL) break;
					}
					ts_track_event_clear(true);
					ts_track_event_gather(EV_ABS, ABS_MT_TRACKING_ID, -1);
					ts_track_event_gather(EV_SYN, 0, 0);
					ts_track_event_run();
					while(!ts_track_event_complete()) {
						msleep(1);
						pr_info("%s ts_check || fallback wait 2\n",__func__);
						diff_time = jiffies - start_time;
						if (diff_time>30*JIFFY_MUL) break;
					}
					msleep(10);

					ts_track_event_clear(true);
					ts_track_event_gather(EV_ABS, ABS_MT_SLOT,30);
					ts_track_event_gather(EV_ABS, ABS_MT_TRACKING_ID, highest_mt_slot);
					ts_track_event_gather(EV_ABS, ABS_MT_POSITION_X, 1);
					ts_track_event_gather(EV_ABS, ABS_MT_POSITION_Y, 1);
//#ifdef CONFIG_IFILTER_TS_PRESSURE
					ts_track_event_gather(EV_ABS, ABS_MT_PRESSURE, 41);
//#endif
					ts_track_event_gather(EV_SYN, 0, 0);
					ts_track_event_run();
					while(!ts_track_event_complete()) {
						msleep(1);
						pr_info("%s ts_check || fallback wait 1\n",__func__);
						diff_time = jiffies - start_time;
						if (diff_time>30*JIFFY_MUL) break;
					}
					ts_track_event_clear(true);
					ts_track_event_gather(EV_ABS, ABS_MT_TRACKING_ID, -1);
					ts_track_event_gather(EV_SYN, 0, 0);
					ts_track_event_run();
					while(!ts_track_event_complete()) {
						msleep(1);
						pr_info("%s ts_check || fallback wait 2\n",__func__);
						diff_time = jiffies - start_time;
						if (diff_time>30*JIFFY_MUL) break;
					}
					msleep(10);
				}
				diff_time = jiffies - start_time;
				if (diff_time>30*JIFFY_MUL) break;
			}
		}
	}
	while (ts_emulated_events_in_progress>10) {
		msleep(1);
	}
	msleep(20);
	mutex_unlock(&squeeze_swipe_lock);
	pr_info("%s ts_input ######### squeeze unlock #########\n",__func__);
}
DECLARE_WORK(ts_poke_emulate_work, ts_poke_emulate);

static struct alarm ts_poke_rtc;
static enum alarmtimer_restart ts_poke_rtc_callback(struct alarm *al, ktime_t now)
{
	pr_info("%s kad\n",__func__);
	schedule_work(&ts_poke_emulate_work);
	return ALARMTIMER_NORESTART;
}
#define CONFIG_IFILTER_POKE
static void ts_poke(void) {
#ifndef CONFIG_IFILTER_POKE
	return;
#else
	ktime_t wakeup_time;
	ktime_t curr_time = ktime_get();
	wakeup_time = ktime_add_us(curr_time,
		(100LL)); // msec to usec
	alarm_cancel(&ts_poke_rtc);
	alarm_start(&ts_poke_rtc,wakeup_time);
#endif
}

/*
START
type: 3 EV_ABS
code: 57
v: 325-324 : touching screen

LOOP:

type: 3 EV_ABS
code: 53 -> X coordinate 800
type: 3 EV_ABS
code: 54 -> Y value : 1300 to 1000 --> 20 steps
type: 3 EV_ABS
code: 58 -> ? strength? 70

type: 0 code: 0 v:0 	sync
END LOOP

type: 3 EV_ABS
code: 57
v: -1 : leaving screen

type: 0 code: 0 v:0 	sync
END ALL
*/
static void ts_scroll_emulate(int down, int full) {

	int local_slot = last_emulated_mt_slot;
	int y_diff;
	int y_delta;
	int y_steps; // tune this for optimal page size of scrolling
	int y_pressure;
	int rounds = 1;
	int i = 0;
	int allow_speedup_next = full?1:0;
	unsigned int last_scroll_time_diff = jiffies - last_scroll_emulate_timestamp;
	int double_swipe = 0;


	pr_info("%s ts_input ######### squeeze try_lock #########\n",__func__);
	if (!mutex_trylock(&squeeze_swipe_lock)) {
		return;
	}
	if (last_emulated_mt_slot>1) {
		last_emulated_mt_slot--;
	} else {
		last_emulated_mt_slot = last_mt_slot - 1;
	}

	ts_emulated_events_in_progress = 0;

	// if last scroll close enough, double round of swipe, if it's intended to be a full swipe...
	if (last_scroll_time_diff <= SWIPE_ACCELERATED_TIME_LIMIT && !swipe_longcount_finished && full) {
		pr_info("%s ts_input ###### double speed swipe ####### diff %u swipe longcount finished %d\n",__func__, last_scroll_time_diff, swipe_longcount_finished);
//		rounds = 2;
		double_swipe = 1;
	}

	swipe_longcount_start = jiffies;
	swipe_longcount_trigger();

	// reset filtering store used by ts input filtering...
	ts_current_count = 0;
	for (i=0; i<TS_MAP_SIZE; i++) {
		ts_current_type[i]=100;
	}

	if (last_scroll_time_diff > 5000 * JIFFY_MUL) { // a higher value...passed?
		if (full == 1) {
			if (longcount_squeeze_swipe_dir_change == 0) {
				// if last direction change was not done by middle long squeeze, then...
				full = -1; // long time passed, scroll only a bit to demo the direction...
			} else {
				longcount_squeeze_swipe_dir_change = 0;
			}
		}
	}
	while (--rounds>=0) {
		y_diff = down?300:0;
#if 0
		y_delta = down?-3:3;
		y_steps = full>0?70:(full==0?50:50);
#endif
#if 1
//		y_delta = down?-7:7;
		y_delta = down?-6:6;
		if (full<=0) y_delta = y_delta / 2;
		y_steps = full>0?18:(full==0?18:18);
		y_pressure = 110;
#endif
		pr_info("%s ts_input ######### squeeze emulation started ######### rounds %d \n",__func__, rounds);

		// speedy swipe for doubled rounds...
		if (double_swipe) {
#if 0
			y_delta = down?-5:5; // bigger delta for speed
			y_steps = 160; // fewer steps, to not run out of screen
			swipe_step_wait_time_mul = 300 - ( (( (SWIPE_ACCELERATED_TIME_LIMIT/JIFFY_MUL) - (last_scroll_time_diff/JIFFY_MUL))*2)/1 ); // 300 - 0
#endif
#if 1
			y_delta = down?-11:11; // bigger delta for speed
			y_steps = 12; // fewer steps, to not run out of screen

			if (last_scroll_time_diff > SWIPE_ACCELERATED_TIME_LIMIT / 2) last_scroll_time_diff = (last_scroll_time_diff * 5) / 3;
			if (last_scroll_time_diff > (SWIPE_ACCELERATED_TIME_LIMIT *4) / 3) last_scroll_time_diff = (last_scroll_time_diff * 8) / 4;

			swipe_step_wait_time_mul = 200 - ( (( (SWIPE_ACCELERATED_TIME_LIMIT/JIFFY_MUL) - (last_scroll_time_diff/JIFFY_MUL)))/1 ); // 150 - 0
			y_pressure = 90 + (20 * last_scroll_time_diff) / SWIPE_ACCELERATED_TIME_LIMIT;
#endif
			if (swipe_step_wait_time_mul > 85) last_swipe_very_quick = 0;
			if (!last_swipe_very_quick && swipe_step_wait_time_mul < 85) last_swipe_very_quick = 1;
#if 0
			if (last_swipe_very_quick && swipe_step_wait_time_mul < 85) swipe_step_wait_time_mul = (swipe_step_wait_time_mul*2)/3; // speed up on the extreme of fast value multiplier < 80, divide it
#endif
#if 1
			if (last_swipe_very_quick && swipe_step_wait_time_mul < 85) swipe_step_wait_time_mul = (swipe_step_wait_time_mul*4)/5; // speed up on the extreme of fast value multiplier < 80, divide it
			if (last_swipe_very_quick && swipe_step_wait_time_mul < 85) y_pressure = (y_pressure *2)/3;
#endif
			pr_info("%s ts_input ######### squeeze emulation - double swipe - SPEED %d \n",__func__, swipe_step_wait_time_mul);
			if (swipe_step_wait_time_mul > 300) swipe_step_wait_time_mul = 300; // to avoid concurrency problem with last_scroll_time_diff
			if (swipe_step_wait_time_mul < 0) swipe_step_wait_time_mul = 0;


		} else {
			if (full>0) {
				swipe_step_wait_time_mul = 210;
			} else {
				if (full==0) {
					swipe_step_wait_time_mul = 200;
				} else {
					swipe_step_wait_time_mul = 250;
				}
			}
		}

		// update last scroll ts
		last_scroll_emulate_timestamp = allow_speedup_next?jiffies:0; // if small turning swipe, avoid double speed swipe on the next occasion, setting here timestamp to 0

		// TODO how to determine portrait/landscape mode? currently only portrait

#if 0
		// to avoid skips, make X times larger steps... 
			//-> use this with higher resolution, not for 1080p screens
		y_steps = y_steps / 2;
		y_delta = y_delta * 2;
		swipe_step_wait_time_mul = swipe_step_wait_time_mul * 2;
#endif

		if (screen_on) {
			int empty_check_count = 0;
			int first_steps = 1;
			int second_step_done = 0;
			unsigned long start_time = jiffies;
			unsigned int diff_time = 0;
			int step_count = 0;
			pr_info("ifilter %s ts DOWN 1 \n",__func__);
			ts_track_event_clear(true);
			while (y_steps-->0) {
				if (first_steps) {
//#ifndef CONFIG_IFILTER_TS_PRESSURE
					ts_track_event_gather(EV_KEY, BTN_TOUCH, 1);
#if 0
// rog3 not
					ts_track_event_gather(EV_KEY, BTN_TOOL_FINGER, 1);
#endif
//#endif
					ts_track_event_gather(EV_ABS, ABS_MT_SLOT, --local_slot);
					ts_track_event_gather(EV_ABS, ABS_MT_TRACKING_ID, local_slot);
//#ifdef CONFIG_IFILTER_TS_PRESSURE
					ts_track_event_gather(EV_ABS, ABS_MT_PRESSURE, y_pressure+ (pseudo_rnd%2));
//#else
					ts_track_event_gather(EV_ABS, ABS_MT_TOUCH_MAJOR, y_pressure + (pseudo_rnd%2));
#if 0
// rog3 not
					ts_track_event_gather(EV_ABS, ABS_MT_TOUCH_MINOR, 3+ (pseudo_rnd%2));
#endif
//#endif
					first_steps = 0;
				} else {
					if (!second_step_done) {
						ts_track_event_clear(true); // clear out "47" mismatch at start
						second_step_done = 1;
					}
					ts_track_event_clear(false);
				}
#if 0
				ts_track_event_gather(EV_ABS, ABS_MT_POSITION_X, 800+ (pseudo_rnd++)%6);
#endif
#if 1
// 1080p
				ts_track_event_gather(EV_ABS, ABS_MT_POSITION_X, 540+ (pseudo_rnd++)%6);
#endif
				ts_track_event_gather(EV_ABS, ABS_MT_POSITION_Y, 1000+y_diff);
				y_diff += y_delta;
				if (step_count++<2) {
					y_diff -= 1;
				}

				ts_track_event_gather(EV_SYN, 0, 0);
				ts_track_event_run();

				if (step_count<2) {
					usleep_range(2 * swipe_step_wait_time_mul , (2 * swipe_step_wait_time_mul) + 1);
				} else {
					usleep_range(5 * swipe_step_wait_time_mul , (5 * swipe_step_wait_time_mul) + 1);
				}

				if (y_steps%10==0) {
					pr_info("%s ts_input squeeze emulation step = %d POS_Y = %d \n",__func__,y_steps, 1000+y_diff);
				}
				while(!ts_track_event_complete()) {
					diff_time = jiffies - start_time;
					if (diff_time>4*JIFFY_MUL) {
						pr_info("%s breaking incomplete check cycle ts_check\n",__func__);
						break;
					}
					msleep(1);
				}
			}
			pr_info("ifilter %s ts DOWN 0 \n",__func__);
			{
				ts_track_event_clear(true);
			}
//#ifndef CONFIG_IFILTER_TS_PRESSURE
			ts_track_event_gather(EV_KEY, BTN_TOUCH, 0);
#if 0
// rog3 not
			ts_track_event_gather(EV_KEY, BTN_TOOL_FINGER, 0);
#endif
//#endif
			ts_track_event_gather(EV_ABS, ABS_MT_TRACKING_ID, -1);
			ts_track_event_gather(EV_SYN, 0, 0);
			ts_track_event_run();
			msleep(1);

			while(!ts_track_event_complete()) {
				msleep(1);
				empty_check_count++;
				if (empty_check_count%100==30) {
					pr_info("%s ts_check || fallback\n",__func__);
					input_event(ts_device,EV_ABS,ABS_MT_SLOT,0);
					input_event(ts_device,EV_ABS,ABS_MT_TRACKING_ID,-1);
					input_event(ts_device,EV_SYN,0,0);
					msleep(5);

					ts_track_event_clear(true);
					ts_track_event_gather(EV_ABS, ABS_MT_SLOT,31);
					ts_track_event_gather(EV_ABS, ABS_MT_TRACKING_ID, highest_mt_slot+1);
					ts_track_event_gather(EV_ABS, ABS_MT_POSITION_X, 0);
					ts_track_event_gather(EV_ABS, ABS_MT_POSITION_Y, 0);
//#ifdef CONFIG_IFILTER_TS_PRESSURE
					ts_track_event_gather(EV_ABS, ABS_MT_PRESSURE, 40);
//#endif
					ts_track_event_gather(EV_SYN, 0, 0);
					ts_track_event_run();
					while(!ts_track_event_complete()) {
						msleep(1);
						pr_info("%s ts_check || fallback wait 1\n",__func__);
						diff_time = jiffies - start_time;
						if (diff_time>30*JIFFY_MUL) break;
					}
					ts_track_event_clear(true);
					ts_track_event_gather(EV_ABS, ABS_MT_TRACKING_ID, -1);
					ts_track_event_gather(EV_SYN, 0, 0);
					ts_track_event_run();
					while(!ts_track_event_complete()) {
						msleep(1);
						pr_info("%s ts_check || fallback wait 2\n",__func__);
						diff_time = jiffies - start_time;
						if (diff_time>30*JIFFY_MUL) break;
					}
					msleep(10);

					ts_track_event_clear(true);
					ts_track_event_gather(EV_ABS, ABS_MT_SLOT,30);
					ts_track_event_gather(EV_ABS, ABS_MT_TRACKING_ID, highest_mt_slot);
					ts_track_event_gather(EV_ABS, ABS_MT_POSITION_X, 1);
					ts_track_event_gather(EV_ABS, ABS_MT_POSITION_Y, 1);
//#ifdef CONFIG_IFILTER_TS_PRESSURE
					ts_track_event_gather(EV_ABS, ABS_MT_PRESSURE, 41);
//#endif
					ts_track_event_gather(EV_SYN, 0, 0);
					ts_track_event_run();
					while(!ts_track_event_complete()) {
						msleep(1);
						pr_info("%s ts_check || fallback wait 1\n",__func__);
						diff_time = jiffies - start_time;
						if (diff_time>30*JIFFY_MUL) break;
					}
					ts_track_event_clear(true);
					ts_track_event_gather(EV_ABS, ABS_MT_TRACKING_ID, -1);
					ts_track_event_gather(EV_SYN, 0, 0);
					ts_track_event_run();
					while(!ts_track_event_complete()) {
						msleep(1);
						pr_info("%s ts_check || fallback wait 2\n",__func__);
						diff_time = jiffies - start_time;
						if (diff_time>30*JIFFY_MUL) break;
					}
					msleep(10);
				}
				diff_time = jiffies - start_time;
				if (diff_time>30*JIFFY_MUL) break;
			}
		}
		pr_info("%s ts_input ######### squeeze emulation ended #########\n",__func__);
	}
	if (pseudo_rnd>4) pseudo_rnd = 0;
	msleep(100);
	while (ts_emulated_events_in_progress>10) {
		pr_info("%s ts_input ######### squeeze emulation left events %d  -- finger count %d \n",__func__,ts_emulated_events_in_progress, finger_counter);
		msleep(1);
	}
	msleep(20);
	mutex_unlock(&squeeze_swipe_lock);
	pr_info("%s ts_input ######### squeeze unlock #########\n",__func__);
}
#endif

static void squeeze_swipe_func(struct work_struct * squeeze_swipe_work) {
	ts_scroll_emulate(squeeze_swipe_dir, 1);
}
static DECLARE_WORK(squeeze_swipe_work, squeeze_swipe_func);
static void squeeze_swipe_trigger(void) {
	pr_info("%s ts_input squeeze swipe trigger is_locked...\n",__func__);
	if (mutex_is_locked(&squeeze_swipe_lock)) {
		return;
	}
	interrupt_swipe_longcount = 1;
	pr_info("%s ts_input squeeze swipe trigger is_locked NOT..scheduling work...\n",__func__);
	schedule_work(&squeeze_swipe_work);
}


static void squeeze_swipe_short_func(struct work_struct * squeeze_swipe_short_work) {
	ts_scroll_emulate(squeeze_swipe_dir, 0);
}
static DECLARE_WORK(squeeze_swipe_short_work, squeeze_swipe_short_func);
static void squeeze_swipe_short_trigger(void) {
	pr_info("%s ts_input squeeze swipe trigger is_locked...\n",__func__);
	if (mutex_is_locked(&squeeze_swipe_lock)) {
		return;
	}
	interrupt_swipe_longcount = 1;
	pr_info("%s ts_input squeeze swipe trigger is_locked NOT..scheduling work...\n",__func__);
	schedule_work(&squeeze_swipe_short_work);
}



//#define MAX_SQUEEZE_TIME 35 * JIFFY_MUL // U12 change 35->38
#define MAX_SQUEEZE_TIME 39 * JIFFY_MUL // pixel 4
//#define MAX_SQUEEZE_TIME_LONG 69 * JIFFY_MUL
#define MAX_SQUEEZE_TIME_LONG 70 * JIFFY_MUL // rog3 70
#define MAX_NANOHUB_EVENT_TIME 4 * JIFFY_MUL
static unsigned long longcount_start = 0;
static int interrupt_longcount = 0;
static int longcount_finished = 0;
static void squeeze_longcount(struct work_struct * squeeze_longcount_work) {
	while (1) {
		if (interrupt_longcount) {
			pr_info("%s squeeze call || longcount interrupted\n",__func__);
			return;
		}
		if (jiffies - longcount_start > MAX_SQUEEZE_TIME) {
			pr_info("%s squeeze call || longcount VIBRATION !! \n",__func__);
			longcount_finished = 1;
			if (get_squeeze_long_vibration()) {// && screen_on) {
				squeeze_vib();
			}
			return;
		}
		msleep(7);
	}
}
static DECLARE_WORK(squeeze_longcount_work, squeeze_longcount);
static void squeeze_longcount_trigger(void) {
	longcount_finished = 0;
	interrupt_longcount = 0;
	schedule_work(&squeeze_longcount_work);
}

// last time when screen was switched off
unsigned long last_kad_screen_off_time = 0;
#define KAD_SCREEN_OFF_NEAR_TIME_MAX 320
bool is_near_kad_screen_off_time(void) {
	unsigned int diff = jiffies - last_kad_screen_off_time;
	pr_info("%s difference since last screen_off %u < %d\n",__func__,diff, KAD_SCREEN_OFF_NEAR_TIME_MAX * JIFFY_MUL);
	if (diff < KAD_SCREEN_OFF_NEAR_TIME_MAX * JIFFY_MUL) {
		return true;
	}
	return false;
}

// last time when screen was switched off by KAD/peek ending uninterrupted
unsigned long last_peek_timeout_screen_off_time = 0;
#define PEEK_TIMEOUT_SCREEN_OFF_NEAR_TIME_MAX 80
bool is_near_peek_timeout_screen_off_time(void) {
	unsigned int diff = jiffies - last_peek_timeout_screen_off_time;
	pr_info("%s difference since last kad_screen_off %u < %d\n",__func__,diff, PEEK_TIMEOUT_SCREEN_OFF_NEAR_TIME_MAX * JIFFY_MUL);
	if (diff < PEEK_TIMEOUT_SCREEN_OFF_NEAR_TIME_MAX * JIFFY_MUL) {
		return true;
	}
	return false;
}

// through this peekmode wait (while kad is on) can be interrupted for earlier kad ending and powerdown
static int interrupt_kad_peekmode_wait = 0;
static void squeeze_peekmode(struct work_struct * squeeze_peekmode_work) {
	unsigned int squeeze_reg_diff = 0;
	interrupt_kad_peekmode_wait = 0;
	squeeze_peek_wait = 1;
	if (wait_for_squeeze_power) {
		// wait_for_squeeze_power = 0; not reset "wait_for..." here.. it will be done in ifilter_pwrtrigger part!
		// if screen is on...
		if (screen_on) {
			// ...wait a bit...
			msleep(30);
			// .. check if last power resgistration through threshold reg callback happened lately... if so no need to do screen off..
			squeeze_reg_diff = jiffies - last_squeeze_power_registration_jiffies;
			pr_info("%s squeeze_reg_diff %u\n",__func__,squeeze_reg_diff);
			if (squeeze_reg_diff<4*JIFFY_MUL) return;
		}
	}
	if (kad_running && !kad_running_for_kcal_only && !kad_running_for_aod_gesture) {
		int count = smart_get_kad_halfseconds() * 4;
		while (!interrupt_kad_peekmode_wait && !(count--<=0)) {
			msleep(125);
		}
	} else {
		int count = get_squeeze_peek_halfseconds() * 4;
		while (!interrupt_kad_peekmode_wait && !(count--<=0)) {
			msleep(125);
		}
	}
	// screen still on and sqeueeze peek wait was not interrupted...
	pr_info("%s screen_on %d squeeze_peek_wait %d interrupt_kad_peekmode_wait %d\n",__func__,screen_on,squeeze_peek_wait,interrupt_kad_peekmode_wait);
	if (screen_on && squeeze_peek_wait) {
		last_kad_screen_off_time = jiffies;
		last_peek_timeout_screen_off_time = jiffies;
		ifilter_pwrtrigger(0,0,__func__);
		if (kad_running && !kad_running_for_kcal_only && !kad_running_for_aod_gesture && !interrupt_kad_peekmode_wait) { // not interrupted, and kad mode peek.. see if re-schedule is needed...
			kad_repeat_counter++;
			if (should_kad_start() && kad_repeat_counter<smart_get_kad_repeat_times()) { // only reschedule if kad should still smart start...and counter is below times limit...
				// alarm timer
//				ktime_t wakeup_time;
//				ktime_t curr_time = ktime_get();
//				wakeup_time = ktime_add_us(curr_time,
//					(smart_get_kad_repeat_period_sec() * (get_kad_repeat_multiply_period()?kad_repeat_counter:1) * 1000LL * 1000LL)); // msec to usec
				alarm_cancel(&kad_repeat_rtc);
				alarm_start_relative(&kad_repeat_rtc, ms_to_ktime(
						(smart_get_kad_repeat_period_sec() * (get_kad_repeat_multiply_period()?kad_repeat_counter:1) * 1000LL)
					)); // start new...
			}
		}
	} else {
		// interrupted peek wait, should cancel kad work to stop waking screen...
		// cancel alarm timer!
		kad_repeat_counter = 0;
	}
	stop_kad_running(!squeeze_peek_wait,__func__); // based on interruption, immediate kcal restore (and no screen off happening), or for screen off : sleep a bit and then do it..
	squeeze_peek_wait = 0;
}
static DECLARE_WORK(squeeze_peekmode_work, squeeze_peekmode);
static void squeeze_peekmode_trigger(void) {
	schedule_work(&squeeze_peekmode_work);
}

static struct alarm check_single_fp_vib_rtc;
int check_single_fp_running = 0;
static enum alarmtimer_restart check_single_fp_vib_rtc_callback(struct alarm *al, ktime_t now)
{
	// FP single vibration: unlock device event...
	pr_info("%s kad double fp vibration detection: single vib detected Stop KAD!\n",__func__);
	stop_kad_running(true,__func__);
	if (init_done) {
		alarm_cancel(&kad_repeat_rtc);
	}
	ntf_input_event(__func__,""); // this is unlocking screen, register it as intentional input event...
	check_single_fp_running = 0;
	return ALARMTIMER_NORESTART;
}

// this callback allows registration of FP vibration, in which case peek timeout auto screen off should be canceled...
int register_fp_vibration(void) {
	// fp scanner pressed, cancel peek timeout, but only do that automatically if not in kad mode (otherwise a double fp vibration check is due)
	pr_info("%s kad_kcal_overlay_on %d kad_running %d kad_running_for_kcal_only %d\n",__func__,kad_kcal_overlay_on,kad_running,kad_running_for_kcal_only);
	if ((!kad_running && screen_on) || kad_running_for_kcal_only) {
		bool poke = kad_kcal_overlay_on || kad_running_for_kcal_only;
		stop_kad_running(true,__func__);
		if (poke) {
			ts_poke();
		}
		ntf_input_event(__func__,"");
	} else {
		if (check_single_fp_running) {
			if (((!kad_running || !get_kad_disable_fp_input()) && screen_on) || (kad_running_for_kcal_only && screen_on)) {
				stop_kad_running(true,__func__);
				ntf_input_event(__func__,""); // KAD is not running or shouldnt block fp input, (for KAD a double FP vib means no stopping if kad fp input disabled, 
				// ...so might be pocket touch, do not register!)
				// ...so it's either for a screen on state, or for Squeeze peek KCAL only, so registering user activity to cancel smart timing is ok.
			}
			pr_info("%s kad double fp vibration detected, should not stop KAD!\n",__func__);
			alarm_cancel(&check_single_fp_vib_rtc);
			check_single_fp_running = 0;
		} else {
//			ktime_t wakeup_time;
//			ktime_t curr_time = ktime_get();
			pr_info("%s kad double fp vibration detection start!\n",__func__);
			check_single_fp_running = 1;
//			wakeup_time = ktime_add_us(curr_time,
//				(160LL * 1000LL)); // msec to usec
			alarm_start_relative(&check_single_fp_vib_rtc, ms_to_ktime(160));
		}
	}
	return get_unlock_vib_strength();
}
EXPORT_SYMBOL(register_fp_vibration);

static unsigned long last_squeeze_timestamp = 0;
static unsigned long last_nanohub_spurious_squeeze_timestamp = 0;

#define STAGE_INIT 0
#define STAGE_FIRST_WL 1
#define STAGE_VIB 2
static int stage = STAGE_INIT;

static unsigned long ts_panel_finger_up_time = 0;
static int ts_panel_finger_down = 0;

bool recent_touch(void) {
	unsigned int diff = jiffies - ts_panel_finger_up_time;
	if (ts_panel_finger_down>0) return true;
	if (diff<70) return true;
	return false;
}

void if_report_squeeze_event(unsigned long timestamp, bool vibration, int num_param) {
	// time passing since screen on/off state changed - to avoid collision of detections
	unsigned int diff = jiffies - last_screen_event_timestamp;
	// time passed since last nanohub driver based spurious squeeze wake event detection
	unsigned int nanohub_diff = jiffies - last_nanohub_spurious_squeeze_timestamp;

	pr_info("%s squeeze call ts %u diff %u nh_diff %u vibration %d num_param %d \n", __func__, (unsigned int)timestamp,diff,nanohub_diff,vibration, num_param);
	if (!squeeze_kernel_handled) return;

	if (!get_squeeze_wake() && !get_squeeze_sleep() && !get_squeeze_swipe() && !get_squeeze_peek()) return;
	if (!get_squeeze_wake() && !get_squeeze_peek() && !screen_on) return;
	if (!get_squeeze_sleep() && !get_squeeze_swipe() && (!get_squeeze_peek() || (get_squeeze_peek() && !squeeze_peek_wait)) && screen_on) return;

	if (!last_screen_event_timestamp) return;
	if ((!screen_on && diff < 3 * JIFFY_MUL) || (screen_on && diff < 30 * JIFFY_MUL)) return;

	pr_info("%s squeeze call ++ START STAGE : %d\n",__func__,stage);
	if (stage == STAGE_INIT) {
#if 1 
		if (vibration) {
			pr_info("%s squeeze call -- vibration in INIT phase, skipping to next stage, setting last squeeze timestamp... : %d\n",__func__,stage);
			// skip to wakelock stage right now
			stage = STAGE_FIRST_WL;
			last_squeeze_timestamp = jiffies;
		} else
		{
#endif
		pr_info("%s squeeze call -- END STAGE : %d\n",__func__,stage);
		return;
#if 1
		}
#endif
	}
	diff = jiffies - last_squeeze_timestamp;
	pr_info("%s squeeze call ++ squeeze diff : %u\n",__func__,diff);

	if (stage == STAGE_FIRST_WL) {
		if (vibration && diff <= 15 * JIFFY_MUL) { // changing 5 -> 15 on u12+, wake can be slower before vibration is actually done after wakelock...

			stage = STAGE_VIB;
			// start longcount trigger
//			longcount_start = last_squeeze_timestamp;
			longcount_start = jiffies;  // start fresh timestamp instead of using the Wakelock/Nanohub based detection stage - vibration triggering is the real good timing source...

			last_squeeze_timestamp = jiffies; // reset counting of time again at this point to be more precise after haptic feedback

			if (get_squeeze_swipe() && !swipe_longcount_finished) {
				// in swipe mode first vibration should stop swipe longcount, because the start of the squeeze should stop 
				// so that while a middle long gesture goes on, it won't finish with swipe_longcount_finished == 1 output, that would prevent direction change, and go
				// into screen off, while the swipe longcount vibration was not present at starting the next squeeze...
				interrupt_swipe_longcount = 1;
			}
			squeeze_longcount_trigger();
			pr_info("%s squeeze call -- END STAGE : %d\n",__func__,stage);
			return; 
		} else {
			if (vibration) { // vibration but too late...back to init state..
				stage = STAGE_INIT; 
			} else {
				// wakelock registered -> start time counting in FIRST_WL stage again...
				last_squeeze_timestamp = jiffies;
			}
			pr_info("%s squeeze call -- END STAGE : %d\n",__func__,stage);
			return;
		}
	}
	if (stage == STAGE_VIB) {
		stage = STAGE_INIT;
		// interrupt longcount
		interrupt_longcount = 1;
		if (vibration) {
			pr_info("%s squeeze call -- exiting because vibration endstage: %d\n",__func__,stage);
			return;
		} else if ( (diff<=MAX_SQUEEZE_TIME) || (screen_on && !longcount_finished) ) {
			pr_info("%s squeeze call -- power onoff endstage: %d\n",__func__,stage);
			wait_for_squeeze_power = 1; // pwr trigger should be canceled if right after squeeze happens a power setting
			// ..that would mean user is on the settings screen and calibrating.
			// also...
			ntf_input_event(__func__,"");
			// if peek mode is on, and between long squeeze and short squeeze, peek
			if (!screen_on && get_squeeze_peek()) {
				pr_info("%s squeeze call -- power onoff - PEEK MODE - PEEK wake: %d\n",__func__,stage);
				last_screen_event_timestamp = jiffies;
				start_kad_running(KAD_FOR_SQUEEZE);
				squeeze_peekmode_trigger();
			}
			if (screen_on && squeeze_peek_wait) { // screen on and squeeze peek going on?
				bool poke = kad_kcal_overlay_on;
				last_screen_event_timestamp = jiffies;
				stop_kad_running(true,__func__);
				if (poke) {
					ts_poke();
				}
			} else {
				if (screen_on && get_squeeze_swipe()) {
					squeeze_swipe_trigger();
				} else {
					last_screen_event_timestamp = jiffies;
					if (!screen_on || !get_squeeze_sleep_on_long()) {
						// POWER OFF / ON
						ifilter_pwrtrigger(0,0,__func__); // SCREEN ON if not already or OFF if screen is on and NOT squeeze sleep on long only...
					}
				}
			}
		} else if (!screen_on && diff>MAX_SQUEEZE_TIME && diff<=MAX_SQUEEZE_TIME_LONG && get_squeeze_peek()) {
			pr_info("%s squeeze call -- power onoff endstage PEEK MODE - full wake! %d\n",__func__,stage);
			last_screen_event_timestamp = jiffies;
			wait_for_squeeze_power = 1; // pwr trigger should be canceled if right after squeeze happens a power setting
			// ..that would mean user is on the settings screen and calibrating.
			ntf_input_event(__func__,"");
			ifilter_pwrtrigger(0,0,__func__); // POWER ON FULLY, NON PEEK
			stop_kad_running(true,__func__);
		} else if (screen_on && diff>MAX_SQUEEZE_TIME && diff<=MAX_SQUEEZE_TIME_LONG && (get_squeeze_swipe()||get_squeeze_sleep_on_long())) {
			if (get_squeeze_sleep()) {
				//unsigned int last_scroll_time_diff = jiffies - last_scroll_emulate_timestamp;
				wait_for_squeeze_power = 1; // pwr trigger should be canceled if right after squeeze happens a power setting
				// ..that would mean user is on the settings screen and calibrating.
				if (!swipe_longcount_finished && get_squeeze_swipe()) {
				// if quickly squeezing before last squeeze swipe ended, turn direction, instead of power off
					// turn direction as NO squeeze sleep is set on
					longcount_squeeze_swipe_dir_change = 1;
					squeeze_swipe_dir = !squeeze_swipe_dir;
					// call a bit of scrolling to show which direction it will go (full param = 0)
					squeeze_swipe_short_trigger();
					pr_info("%s squeeze TURN SWIPE DIRECTION -- END STAGE : %d\n",__func__,stage);
					ntf_input_event(__func__,"");
					return; // exit with turning...
				}
				// if swipe mode is on or long squeeze sleep, and between long squeeze max time and short squeeze, power off...
				if (get_squeeze_swipe()||get_squeeze_sleep_on_long()) {
					pr_info("%s squeeze call (after swipe or sleep_on_long -- power onoff endstage SWIPE - full sleep - swipe mode middle long gesture! %d\n",__func__,stage);
					last_screen_event_timestamp = jiffies;
					ifilter_pwrtrigger(0,0,__func__); // POWER OFF
					stop_kad_running(true,__func__);
				}
				return;
			} else if (get_squeeze_swipe()) {
				ntf_input_event(__func__,"");
				// turn direction as NO squeeze sleep is set on
				longcount_squeeze_swipe_dir_change = 1;
				squeeze_swipe_dir = !squeeze_swipe_dir;
				// call a bit of scrolling to show which direction it will go (full param = 0)
				squeeze_swipe_short_trigger();
				pr_info("%s squeeze TURN SWIPE DIRECTION -- END STAGE : %d\n",__func__,stage);
				return;
			}
		} else if (!screen_on || diff>75 * JIFFY_MUL) { // time passed way over a normal wakelock cycle... start with second phase instead!
			stage = STAGE_FIRST_WL;
			last_squeeze_timestamp = jiffies;
		}
		pr_info("%s squeeze call -- END STAGE : %d\n",__func__,stage);
	}

}
EXPORT_SYMBOL(if_report_squeeze_event);



static unsigned long kad_first_one_finger_touch_time = 0;
static unsigned long kad_first_one_finger_done = 0;

// -- KAD (Kernel Ambient Display --
void do_kernel_ambient_display(void) {
	pr_info("%s kad -- screen_on %d kad_running %d \n",__func__,screen_on, kad_running);

	if (uci_get_user_property_int_mm("kad_only_on_charger", kad_only_on_charger, 0, 1) && !ntf_is_charging()) return;

	if (!screen_on && !kad_running) {
		start_kad_running(KAD_FOR_KAD);
		pr_info("%s kad -- power onoff - PEEK MODE - PEEK wake: %d\n",__func__,stage);
		last_screen_event_timestamp = jiffies;
		kad_first_one_finger_touch_time = 0;
		kad_first_one_finger_done = 0;
		squeeze_peekmode_trigger();
		// KAD waking screen
		ntf_kad_wake(); // signal kad wake, so screen on will be intercepted as not wake by user
		ifilter_pwrtrigger(0,0,__func__);
	}
}

static enum alarmtimer_restart kad_repeat_rtc_callback(struct alarm *al, ktime_t now)
{
	pr_info("%s kad\n",__func__);
	if (should_kad_start()) {
		do_kernel_ambient_display();
	}
	return ALARMTIMER_NORESTART;
}

// KAD KernelAmbientDisplay in-kernel...
// this method is to initialize peek screen on aka "in-kernel AmbientDisplay" feature
static void kernel_ambient_display_internal(bool led_intercepted) {

	if (!should_kad_start()) return;
	pr_info("%s kad -- ||||||| +++++++++++++ KAD +++++++++++++ ////// screen_on %d kad_running %d \n",__func__,screen_on, kad_running);
	if (!led_intercepted || !is_near_kad_screen_off_time()) {
		kad_repeat_counter = 0;
	}
	//do_kernel_ambient_display();
	if (!screen_on && !kad_running && (!led_intercepted || !is_near_kad_screen_off_time())) // not screen on, not already running, and not too much close to previous KAD stop (LED rom store call can false positive trigger KAD if set only to LED when screen is off!)
	{
		// alarm timer
		// ...to wait a bit with starting the first instance, because of phone calls/alarms can turn screen on in the meantime
//		ktime_t wakeup_time;
//		ktime_t curr_time = ktime_get();
//		wakeup_time = ktime_add_us(curr_time,
//			( (get_kad_start_delay_halfseconds() * 500LL) + 100LL) * 1000LL); // config to avoid collision of notif sound and pwr button interrupt
		alarm_cancel(&kad_repeat_rtc);
		alarm_start_relative(&kad_repeat_rtc, ms_to_ktime(
				( (get_kad_start_delay_halfseconds() * 500LL) + 100LL) // config to avoid collision of notif sound and pwr button interrupt
			)); // start new...
	}
}
void kernel_ambient_display(void) {
	ntf_block_camera(true);
	kernel_ambient_display_internal(false);
}
EXPORT_SYMBOL(kernel_ambient_display);
void kernel_ambient_display_led_based(void) {
	ntf_block_camera(true);
	kernel_ambient_display_internal(true);
}
EXPORT_SYMBOL(kernel_ambient_display_led_based);

void stop_kernel_ambient_display(bool interrupt_ongoing) {
	if (init_done) {
		alarm_cancel(&kad_repeat_rtc);
	}
	if (interrupt_ongoing) {
		stop_kad_running(true,__func__);
	}
}
EXPORT_SYMBOL(stop_kernel_ambient_display);
int is_kernel_ambient_display(void) {
	return should_kad_start() && (!uci_get_user_property_int_mm("kad_only_on_charger", kad_only_on_charger, 0, 1) || ntf_is_charging());
}
EXPORT_SYMBOL(is_kernel_ambient_display);

// ----------------- nanohub callback methods

static unsigned long last_timestamp = 0;
#define SQUEEZE_EVENT_TYPE_NANOHUB  0
#define SQUEEZE_EVENT_TYPE_NANOHUB_INIT  1
#define SQUEEZE_EVENT_TYPE_VIBRATOR  2

#define MAX_NANOHUB_DIFF_INIT_END 7 * JIFFY_MUL
#define MIN_NANOHUB_DIFF_END_END 100 * JIFFY_MUL

static int last_event = 0;

void if_report_squeeze_wake_event(int nanohub_flag, int vibrator_flag, unsigned long timestamp, int init_event_flag)
{
	unsigned int diff = timestamp - last_timestamp;
	int event = nanohub_flag?(init_event_flag?SQUEEZE_EVENT_TYPE_NANOHUB_INIT:SQUEEZE_EVENT_TYPE_NANOHUB):SQUEEZE_EVENT_TYPE_VIBRATOR;

	pr_info("%s squeeze wake call, nano %d vib %d ts %u diff %u init flag %d event %d last_event %d\n", __func__, nanohub_flag,vibrator_flag,(unsigned int)timestamp,diff,init_event_flag, event, last_event);
	last_timestamp = timestamp;

	if (
		!screen_on && nanohub_flag && 
		(
		    ( diff < MAX_NANOHUB_DIFF_INIT_END && event!=last_event ) || 
		    ( event == last_event && event == SQUEEZE_EVENT_TYPE_NANOHUB && diff > MIN_NANOHUB_DIFF_END_END )
		)
	) {
		// helper case, where Wakelock event miss, this nanohub detection can help
		// we store the timestamp of the case when a full squeeze was detected from
		// nanohub init event and release event with a very little timediff 
		// (diff calculated when this method was called twice, and is a small enough value).
		//
		// ... if event is a Nanohub release event following another release vent after a long period passing, it usually means
		// ... nanohub driver missed the INIT event, in that case enter this branch too.
		pr_info("%s spurious squeeze nanohub detection triggered: diff %u\n",__func__, diff);
		last_nanohub_spurious_squeeze_timestamp = timestamp;

		if (stage == STAGE_VIB) {
			pr_info("%s spurious squeeze nanohub detection triggered: STAGE_VIB - calling register_squeeze right now.\n",__func__);
			// if process is already after detecting VIB (stage_vib), call directly in,
			// in some cases this is necessary, as userspace WL can delay too much,
			// while this nanohub call happening earlier...
			last_nanohub_spurious_squeeze_timestamp = 0;
			if_report_squeeze_event(timestamp,false,0);
		}

	}

#if 0
// this part, if nanohub would be reliable enough, could be used again.
// Currently it's losing some events, thus this part is not used at the moment.
	if (screen_on && diff < 45 * JIFFY_MUL && event!=last_event) {
		if (!get_squeeze_sleep()) return;
		pr_info("%s screen on and latest event diff small enough: pwr on\n",__func__);
		last_timestamp = 0;
		ifilter_pwrtrigger(0,0,__func__);
		return;
	}
#endif
	last_event = event;
	pr_info("%s latest nanohub/vib event processed. diff: %u\n",__func__,diff);
}
EXPORT_SYMBOL(if_report_squeeze_wake_event);


// ==================================
// ---------------fingerprint handler
// ==================================

static void ifilter_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}


static const struct input_device_id ifilter_ids[] = {
	{ .driver_info = 1 },
	{ },
};

static struct input_handler ifilter_input_handler = {
	.filter		= ifilter_input_filter,
	.event		= ifilter_input_event,
	.connect	= ifilter_input_connect,
	.disconnect	= ifilter_input_disconnect,
	.name		= "ifilter_inputreq",
	.id_table	= ifilter_ids,
};


/* check stored map of ts_current_X maps for matching values */
static bool check_ts_current_map(int type, int code, int value) {
	int i = 0;
	for (i=0; i<TS_MAP_SIZE; i++) {
		if (ts_current_type[i]==type && ts_current_code[i]==code && ts_current_value[i]==value) {
			// remove the event already happened, by setting invalid type 100
			ts_current_type[i]=100;
			ts_emulated_events_in_progress--;
			return true;
		}
	}
	return false;
}

// ==================================
// ------------- touch screen handler
// ==================================

static int last_x = 0, last_y = 0;
static int c_x = 0, c_y = 0;
static unsigned long last_ts_timestamp = 0;

static unsigned long last_vol_key_1_timestamp = 0;
static unsigned long last_vol_key_2_timestamp = 0;
static unsigned long last_vol_keys_start = 0;

static bool filtered_ts_event = false;
static unsigned long filtering_ts_event_last_event = 0;
static int kad_finger_counter = 0;

static int block_power_key_in_pocket = 0;
static int proximity = 0;
int get_block_power_key_in_pocket(void) {
	return proximity && uci_get_user_property_int_mm("block_power_key_in_pocket", block_power_key_in_pocket, 0, 1);
}
int get_block_volume_key_in_pocket(void) {
	return proximity && uci_get_user_property_int_mm("block_volume_key_in_pocket", 0, 0, 1);
}

//#define LOG_INPUT_EVENTS

static bool ts_is_touchscreen_key_event(int type, int code) {
	if (type != EV_KEY) return false;
	return (
		code==330 || code==325 ||
		code == 158 || code == 580);
}


static bool filter_next_power_key_up = false;
static bool ts_input_filter(struct input_handle *handle,
                                    unsigned int type, unsigned int code,
                                    int value)
{
#if 1
	bool filter_event = false;
	bool finger_touch_event = false;
	if (kad_running && !kad_running_for_kcal_only && get_kad_disable_touch_input() && (type!=EV_KEY || ts_is_touchscreen_key_event(type,code))) {
		// do nothing, don't stop stuff in led driver like flashlight etc...
	} else {
#if 0
		pr_info("%s event: %s t %d c %d v %d\n",__func__, handle->dev->name, type,code,value);
#endif
		if ( (strcmp("fts",handle->dev->name) 
				&& strcmp("sec_touchscreen",handle->dev->name)
				&& strcmp("synaptics_dsx",handle->dev->name)
				&& strcmp("touchpanel",handle->dev->name)
				&& strcmp("goodix_ts",handle->dev->name)
			)
			|| (type != EV_SYN && type != EV_MSC)) {
			// from touchscreen SYNC and MSC events are not necessarily user inputs! only signal input if not such..
			ntf_input_event(__func__,"");
		}
	}

	//pr_info("%s ts input filter called t %d c %d v %d\n",__func__, type,code,value);
	if (type == EV_KEY && code == BTN_TOUCH && value == 1) {
		// touch down
		ts_panel_finger_down++;
	}
	if (type == EV_KEY && code == BTN_TOUCH && value == 0) {
		// touch up
		ts_panel_finger_up_time = jiffies;
		ts_panel_finger_down--;
	}


	if (type == EV_KEY) {
#ifdef LOG_INPUT_EVENTS
		pr_info("%s _____ ts_input key %d %d %d\n",__func__,type,code,value);
#endif
		if (code == 116) {
			if (value==0 && filter_next_power_key_up) return true;

			if (value==1 && !screen_on && (get_block_power_key_in_pocket() || is_near_peek_timeout_screen_off_time())) {
				pr_info("%s proximity or near_peak_timeout ts_input power key filter (near peek timeout screenoff: %d)\n",__func__,is_near_peek_timeout_screen_off_time());
				filter_next_power_key_up = true;
				return true;
			} else {
				if (value==1 && screen_on && kad_running && (!kad_running_for_kcal_only)) {
					// in KAD / AOD KAD, filter power button, to avoid screen off... but stop KAD
					filter_next_power_key_up = true;
					last_screen_event_timestamp = jiffies;
					stop_kad_running(true,__func__);
					ntf_input_event(__func__,"");
					ts_poke();
					return true;
				} else {
					filter_next_power_key_up = false;
				}
			}
		}
		if ((code == 115 || code == 114) && !screen_on && get_block_volume_key_in_pocket() && !ntf_is_in_call()) { // do not filter in call
			pr_info("%s proximity ts_input volume key filter\n",__func__);
			return true; // filter vol up/dn in proximity...
		}
	}
#ifdef LOG_INPUT_EVENTS
	if (type == EV_ABS) {
		pr_info("%s _____ ts_input log_abs %d %d %d\n",__func__,type,code,value);
	}
	if (type == EV_SYN) {
		pr_info("%s _____ ts_input log_syn %d %d %d\n",__func__,type,code,value);
	}
#endif

	if (type == EV_KEY && code == KEY_VOLUMEUP && value == 1) {
		last_vol_keys_start = jiffies;
		goto skip_ts;
	}
	if (type == EV_KEY && code == KEY_VOLUMEDOWN && value == 1) {
		last_vol_keys_start = jiffies;
		goto skip_ts;
	}


	if (type == EV_KEY && code == KEY_VOLUMEUP && value == 0) {
		last_vol_key_1_timestamp = jiffies;
		if (last_vol_key_1_timestamp - last_vol_key_2_timestamp < 7 * JIFFY_MUL) {
//			unsigned int start_diff = jiffies - last_vol_keys_start;
//			register_double_volume_key_press( (start_diff > 50 * JIFFY_MUL) ? ((start_diff > 100 * JIFFY_MUL)?2:1):0 );
		}
		goto skip_ts;
	}
	if (type == EV_KEY && code == KEY_VOLUMEDOWN && value == 0) {
		last_vol_key_2_timestamp = jiffies;
		if (last_vol_key_2_timestamp - last_vol_key_1_timestamp < 7 * JIFFY_MUL) {
//			unsigned int start_diff = jiffies - last_vol_keys_start;
//			register_double_volume_key_press(start_diff > 50 * JIFFY_MUL ? ((start_diff > 100 * JIFFY_MUL)?2:1):0 );
		}
		goto skip_ts;
	}

	if (type == EV_ABS && code == 57 && value>=0) { // touch, still count more...
		finger_counter++;
		finger_touch_event = true;
	}
	if (type == EV_ABS && code == 57 && value<0) { // detouch
		finger_counter--;
		finger_touch_event = true;
	}
	if (type == EV_ABS && code == 47) { // number id
		finger_touch_event = true;
	}

	if (type == EV_ABS && code == ABS_MT_TRACKING_ID && value!=-1) {
		// store highest multitouch slot...
		if (highest_mt_slot<value) highest_mt_slot = value;
	}

	// if in track mode, only let through events emulated...
	if (mutex_is_locked(&squeeze_swipe_lock)) {
		if (!ts_track_event_complete()) {
			return !ts_track_event_check(type,code,value);
		}
	}

	if (mutex_is_locked(&squeeze_swipe_lock)) {
		// in emulated swipe...block event that is not the event matching emulation event values... ??? always let through finger touch/release true...
		if (!check_ts_current_map(type,code,value) && !finger_touch_event) {
			pr_info("%s ts_input filtering ts input while emulated scroll! %d %d %d\n",__func__,type,code,value);
			return true;
		} else {
			//pr_info("%s ts_input LETTING THROUGH ts input while emulated scroll! %d %d %d -- finger_counter %d -- ts_emulated_events yet: %d \n",__func__,type,code,value,finger_counter, ts_emulated_events_in_progress);
		}
	} else 
	{
		// only overwrite last_mt_slot when not in emulation! otherwise order will be confused on userspace
		if (type == EV_ABS && code == ABS_MT_TRACKING_ID && value!=-1) {
			last_mt_slot = value;
			last_emulated_mt_slot = value;
		}
		check_ts_current_map(type,code,value); // we still need to continue emptying the map, to keep blocking other events, meanwhile squeeze scrolling...

		if (code == ABS_MT_POSITION_X) {
			c_x = value;
		}
		if (code == ABS_MT_POSITION_Y) {
			c_y = value;
		}
		if (type == EV_SYN) {
			unsigned int ts_ts_diff = jiffies - last_ts_timestamp;
			if (ts_ts_diff < 2 * JIFFY_MUL) {
				if (abs(last_x-c_x)>abs(last_y-c_y)) {
					// X direction TODO
				} else {
					// Y direction
					if (c_x<110 || c_x>1300) {//too much on the edge, accidental touchscreen... 
					} else {
						if (last_y>c_y) { // swiping up
							if (squeeze_swipe_dir == 0) {
								last_scroll_emulate_timestamp = 0; // direction change, make the first scroll slow by putting this timestamp 0
								squeeze_swipe_dir = 1; // SCROLL DOWN
								pr_info("%s ts_input filtering TURNING DIRECTION ON INPUT FILTER c_x %d c_y %d \n",__func__,c_x,c_y);
							}
						} else if (last_y < c_y) { // swiping down
							if (squeeze_swipe_dir == 1) {
								last_scroll_emulate_timestamp = 0; // direction change, make the first scroll slow by putting this timestamp 0
								squeeze_swipe_dir = 0; // SCROLL UP
								pr_info("%s ts_input filtering TURNING DIRECTION ON INPUT FILTER c_x %d c_y %d \n",__func__,c_x,c_y);
							}
						}
					}
				}
			}
			last_ts_timestamp = jiffies;
			last_x = c_x;
			last_y = c_y;
		}
	}

#endif
skip_ts:
	if (screen_on_full && !screen_off_early) {
		//pr_info("%s touchscreen checks... kad running %d kad_running_for_kcal_only %d disable_touch_input %d ts_is_ts_key_event %d\n",__func__, kad_running, kad_running_for_kcal_only, get_kad_disable_touch_input(), ts_is_touchscreen_key_event(type,code));
		if (!kad_running || kad_running_for_kcal_only || !get_kad_disable_touch_input() || (type==EV_KEY && !ts_is_touchscreen_key_event(type,code))) { // if not in KAD display mode, or not touchscreen input
			//pr_info("%s squeeze_peek_wait = 0\n",__func__);
			squeeze_peek_wait = 0; // interrupt peek wait, touchscreen was interacted, don't turn screen off after peek time over...
			if (kad_running || kad_running_for_kcal_only) { 
				stop_kad_running(true,__func__);
			}
		} else if (kad_running && !kad_running_for_kcal_only && get_kad_disable_touch_input() && (type!=EV_KEY || ts_is_touchscreen_key_event(type,code))) {
			// if kad running and filtering on... filter it...
			if (code != 158 && code !=580) { // non virtual key (BACK/APPSWITCH), but real panel event: 
				//!ts_is_touchscreen_key_event(type,code)) { // do not filter all key events of TS, those are not virtual keys in every case
				filtering_ts_event_last_event = jiffies;
				filtered_ts_event = true;
				filter_event = true;
			}
			pr_info("%s filter branch... kad_first_one_finger_done %d kad_finger_counter %d filter_event %d \n",__func__, (kad_first_one_finger_done>0), kad_finger_counter, filter_event);
			if (type == EV_ABS) {
				pr_info("%s filter branch _____ ts_input log_abs %d %d %d\n",__func__,type,code,value);
			}
			if (type == EV_SYN) {
				pr_info("%s filter branch _____ ts_input log_syn %d %d %d\n",__func__,type,code,value);
			}
			if (kad_finger_counter == 0) { //
				if ((code==47) || (code==57 && value == -1) || (type==0 && code==0)) {
					// if no touch on screen yet, but events coming related to fingers leaving, then that must be remnants of a previous screen on state with fingers on screen while it turned off...
					// let thos events unfiltered and unchecked...
					filter_event = false;
					pr_info("%s filter branch _____ remnant event detected... do NOT filter EVENT: %d %d %d\n",__func__,type,code,value);
					/* Example...untouching fingers upon screen on...remnant events:
					[  440.638015] ts_input_filter filter branch _____ ts_input log_abs 3 47 0
					...
					[  440.638099] ts_input_filter filter branch _____ ts_input log_abs 3 57 -1
					[  440.638136] ts_input_filter filter branch _____ ts_input log_syn 0 0 0
					*/
				}
			}

		    if (filter_event) {
		    // if not the remnant events (filter_event==true) of a previous touches
		    // interrupted with screen off then we can continue to check what type of events are these...
		    // otherwise we shouldn't look for interrupting KAD filtering...
			if (code == 57 && value>0) {
				unsigned int time_diff = jiffies - kad_first_one_finger_touch_time;
				if (time_diff > 50*JIFFY_MUL) kad_first_one_finger_done = 0; // reset here if other touches already happened, 
						// ...but screen left untouched for a second touch for a longer time already...
				kad_finger_counter++;
				if (kad_finger_counter>1) {
					// over one finger, reset kad_first_one_finger_done...
					pr_info("%s kad first_one done = 0 (1) \n",__func__);
					kad_first_one_finger_done = 0;
				}
			}
			if (code == 57 && value<0) {
				if (kad_finger_counter == 1) {
					// exactly one finger leaving the screen...
					if (!kad_first_one_finger_done) {
						// first time...
						pr_info("%s kad first_one done = 1\n",__func__);
						kad_first_one_finger_touch_time = jiffies;
						kad_first_one_finger_done = 1;
					} else {
						unsigned int time_diff = jiffies - kad_first_one_finger_touch_time;
						pr_info("%s kad first_one done == 1 check time_diff %u \n",__func__,time_diff);
						kad_first_one_finger_touch_time = 0;
						kad_first_one_finger_done = 0;
						if (time_diff < 50*JIFFY_MUL) { // double tap single finger happened, stop kad without waking...
//							if (!kad_running_for_aod_gesture) 
							{
								// make timeout for kad
								pr_info("%s kad first_one done == 1 DOUBLE TAP, interrupt kad and vibrate \n",__func__);
								interrupt_kad_peekmode_wait = 1; // signal interruption for kad squeeze_peekmode work...
								ntf_input_event(__func__,""); // stop flashlight...
								set_vibrate(20); 
							} 
#if 0
							else {
								last_screen_event_timestamp = jiffies;
								stop_kad_running(true,__func__);
								ntf_input_event(__func__,"");
								ts_poke();
							}
#endif
						}
					}
				} else {
					pr_info("%s kad first_one done = 0 (2) \n",__func__);
					kad_first_one_finger_touch_time = 0;
					kad_first_one_finger_done = 0;
				}
				kad_finger_counter--;
				if (kad_finger_counter<0) kad_finger_counter = 0;
			}

			if (get_kad_two_finger_gesture() && kad_finger_counter==2) {
				squeeze_peek_wait = 0; // interrupt peek wait, touchscreen was interacted, don't turn screen off after peek time over...
				if (kad_running) {
					pr_info("%s ##### two finger -- stop kad running #######\n",__func__);
					stop_kad_running(true,__func__);
				}
			}
			if (get_kad_three_finger_gesture() && kad_finger_counter==3) {
				squeeze_peek_wait = 0; // interrupt peek wait, touchscreen was interacted, don't turn screen off after peek time over...
				if (kad_running) {
					pr_info("%s ##### three finger -- stop kad running #######\n",__func__);
					stop_kad_running(true,__func__);
				}
			}
		    }

		}
	}
	if (!filter_event && type!=EV_KEY && kad_finger_counter > 0) {
		// prevent touch events until user lifts all fingers that touched screen while filtering...
		if (code == 57 && value>0) { // touch, still count more...
			kad_finger_counter++;
		}
		if (code == 57 && value<0) { // detouch
			kad_finger_counter--;
			if (kad_finger_counter<0) kad_finger_counter = 0;
		}
		if (kad_finger_counter>0) filter_event = true;
		if (kad_finger_counter==0) {
			// start emulated gesture
			ts_poke();
		}
	}

	if (filter_event) {
		pr_info("%s ts_input filtering ts input while kad_control! %d %d %d\n",__func__,type,code,value);
		return true;
	}
	return false;
}

static void ts_input_callback(struct work_struct *unused) {
	return;
}

static void ts_input_event(struct input_handle *handle, unsigned int type,
				unsigned int code, int value) {
}

static int ts_input_dev_filter(struct input_dev *dev) {
	pr_info("%s %s\n",__func__, dev->name);
	if (
		strstr(dev->name, "touchpanel") ||
		strstr(dev->name, "fts") ||
		strstr(dev->name, "sec_touchscreen") ||
		strstr(dev->name, "himax-touchscreen") ||
		strstr(dev->name, "synaptics_dsx") ||
		strstr(dev->name, "synaptics,s3320") ||
		strstr(dev->name, "max1187x_touchscreen_0") ||
		strstr(dev->name, "nvt_touchscreen") ||
		strstr(dev->name, "cyttsp") ||
		strstr(dev->name, "qpnp_pon") ||
		strcmp(dev->name, "goodix_ts")==0 ||
		strstr(dev->name, "gpio")
	    ) {
		// storing static ts_device for using outside this handle context as well

		// Pixel4XL
		if (strstr(dev->name, "fts")) ts_device = dev;
		// Pixel4
		if (strstr(dev->name, "sec_touchscreen")) ts_device = dev;
		// U11
		if (strstr(dev->name, "cyttsp")) ts_device = dev;
		// U11Life
		if (strstr(dev->name, "nvt_touchscreen")) ts_device = dev;
		// U11+
		if (strstr(dev->name, "synaptics_dsx")) ts_device = dev;
		// m10
		if (strstr(dev->name, "max1187x_touchscreen_0")) ts_device = dev;
		// op6
		if (strstr(dev->name, "synaptics,s3320")) ts_device = dev;
		// op8/pro
		if (strstr(dev->name, "touchpanel")) ts_device = dev;
		// asus
		if (strcmp(dev->name, "goodix_ts")==0) ts_device = dev;

		return 0;
	} else {
		return 1;
	}
}


static int ts_input_connect(struct input_handler *handler,
				struct input_dev *dev, const struct input_device_id *id) {
	struct input_handle *handle;
	int error;

	if (ts_input_dev_filter(dev))
		return -ENODEV;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "ifilter_ts";


	error = input_register_handle(handle);

	error = input_open_device(handle);

	return 0;

}

static void ts_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}


static const struct input_device_id ts_ids[] = {
	{ .driver_info = 1 },
	{ },
};

static struct input_handler ts_input_handler = {
	.filter		= ts_input_filter,
	.event		= ts_input_event,
	.connect	= ts_input_connect,
	.disconnect	= ts_input_disconnect,
	.name		= "ts_inputreq",
	.id_table	= ts_ids,
};


static struct kobject *ifilter_kobj;


#ifdef CONFIG_UCI_NOTIFICATIONS
bool charging = true;
static void ntf_listener(char* event, int num_param, char* str_param) {
        if (strcmp(event,NTF_EVENT_CHARGE_LEVEL) && strcmp(event, NTF_EVENT_INPUT)) {
                pr_info("%s ifilter ntf listener event %s %d %s\n",__func__,event,num_param,str_param);
        }

	if (!strcmp(event,NTF_EVENT_AOD_GESTURE)) {
		if (!screen_on && !proximity && get_kad_pick_up_show()) {
			if (!kad_should_start_on_uci_sys_change && !kad_started_leaving_proximity_or_locked) { 
			// only start AOD gesture peek, if not already waiting for proximity being left for a pending KAD, or not already started from that scenario...
				last_screen_event_timestamp = jiffies;
				start_kad_running(KAD_FOR_AOD);
				squeeze_peekmode_trigger();
				ifilter_pwrtrigger(0,0,__func__); // SCREEN ON
			}
		}
	} else
	if (!strcmp(event,NTF_EVENT_CAMERA_ON)) {
		if (!!num_param) {
			// camera on.. if KAD running, stop it, display is camera app!
			if (screen_on && kad_running) {
				if (kad_running_for_kcal_only) {
					stop_kad_running(true,__func__);
				} else {
					interrupt_kad_peekmode_wait = 1; // interrupt KAD
				}
			}
		}
	} else
	if (!strcmp(event,NTF_EVENT_WAKE_EARLY)) {
		screen_on = 1;
		screen_off_early = 0;
		last_screen_on_seconds = get_global_seconds();
		last_screen_on_early_time = jiffies;
		pr_info("ifilter kad screen on -early\n");
	} else

	if (!strcmp(event,NTF_EVENT_WAKE_BY_USER) || !strcmp(event,NTF_EVENT_WAKE_BY_FRAMEWORK)) {
		screen_on = 1;
		screen_on_full = 1;
		screen_off_early = 0;
		last_screen_event_timestamp = jiffies;
		pr_info("%s kad screen on\n",__func__);
		kcal_sleep_before_restore = true;
		schedule_work(&kcal_restore_work);
		pr_info("ifilter screen on\n");
	} else

	if (!strcmp(event,NTF_EVENT_SLEEP_EARLY)) {
		screen_on = 0;
		screen_off_early = 1;
		//screen_on_full = 0;
		last_kad_screen_off_time = jiffies;
		pr_info("ifilter kad screen off -early\n");
	} else

	if (!strcmp(event,NTF_EVENT_SLEEP)) {
		screen_on = 0;
		screen_on_full = 0;
		kad_finger_counter = 0;
		last_kad_screen_off_time = jiffies;
		last_screen_event_timestamp = jiffies;
		last_screen_off_seconds = get_global_seconds();
		last_screen_lock_check_was_false = 0;
		last_scroll_emulate_timestamp = 0;
		pr_info("ifilter kad screen off\n");
	} else

	if (!strcmp(event,NTF_EVENT_RINGING)) {
//		stop_kernel_ambient_display(true); // TODO move to ifilter
	} else
	if (!strcmp(event,NTF_EVENT_NOTIFICATION)) {
		if (!!num_param) {
		}
	} else
        if (!strcmp(event,NTF_EVENT_CHARGE_LEVEL)) {
        } else
        if (!strcmp(event,NTF_EVENT_INPUT)) {
#ifdef CONFIG_IFILTER_KCAL_ANYTIME
		if (!kad_running && !kad_running_for_kcal_only) {
			kcal_internal_restore(true);
		}
#endif
		if (!kad_running && !kad_running_for_kcal_only) {
			ntf_block_camera(false);
		}
        } else
        if (!strcmp(event,NTF_EVENT_PROXIMITY)) {
		proximity = !!num_param;
        } else
        if (!strcmp(event,NTF_EVENT_LOCKED)) {
		if (!num_param) { // unlocked... probably fingerprint...
			if (screen_on) {
				pr_info("%s kad unlocked: Stop KAD!\n",__func__);
				stop_kad_running(true,__func__);
				if (init_done) {
					alarm_cancel(&kad_repeat_rtc);
				}
				ntf_input_event(__func__,""); // this is unlocking screen, register it as intentional input event... to stop other stuff in other drivers like flashlight
			//	register_fp_vibration();
			}
		}
        } else
        if (!strcmp(event,NTF_EVENT_CHARGE_STATE)) {
		bool input_event = false;
		if (!!num_param != charging) {
			input_event = true;
		}
                charging = !!num_param;
		if (!charging) {
		}
		if (input_event) {
//			stop_kernel_ambient_display(true);
		}
        }
}
#endif


static int __init ifilter_init(void)
{
	int rc = 0;
	pr_info("ifilter - init\n");

	ifilter_pwrdev = input_allocate_device();
	if (!ifilter_pwrdev) {
		pr_err("Failed to allocate ifilter_pwrdev\n");
		goto err_alloc_dev;
	}

	input_set_capability(ifilter_pwrdev, EV_KEY, KEY_POWER);
	input_set_capability(ifilter_pwrdev, EV_KEY, KEY_HOME);
	input_set_capability(ifilter_pwrdev, EV_KEY, KEY_APPSELECT);
	
	set_bit(EV_KEY, ifilter_pwrdev->evbit);
	set_bit(KEY_HOME, ifilter_pwrdev->keybit);

	ifilter_pwrdev->name = "qwerty";
	ifilter_pwrdev->phys = "qwerty/input0";

	rc = input_register_device(ifilter_pwrdev);
	if (rc) {
		pr_err("%s: input_register_device err=%d\n", __func__, rc);
		goto err_input_dev;
	}

	// ifilter handler
	kcal_listener_wq = alloc_workqueue("kcal_list", WQ_HIGHPRI, 1);
	ifilter_input_wq = alloc_workqueue("ifilter_iwq", WQ_HIGHPRI, 1);;
	ifilter_pwr_wq = alloc_workqueue("ifilter_pwq", WQ_HIGHPRI, 1);;
	ifilter_vib_wq = alloc_workqueue("ifilter_vwq", WQ_HIGHPRI, 1);;
	if (!ifilter_input_wq) {
		pr_err("%s: Failed to create workqueue\n", __func__);
		return -EFAULT;
	}
	INIT_WORK(&ifilter_input_work, ifilter_input_callback);

	rc = input_register_handler(&ifilter_input_handler);
	if (rc)
		pr_err("%s: Failed to register ifilter_input_handler\n", __func__);
	else
		pr_info("%s: ifilter - input handler registered\n",__func__);

	// ts handler
	ts_input_wq = create_workqueue("ts_iwq");
	if (!ts_input_wq) {
		pr_err("%s: Failed to create workqueue\n", __func__);
		return -EFAULT;
	}
	INIT_WORK(&ts_input_work, ts_input_callback);

	rc = input_register_handler(&ts_input_handler);
	if (rc)
		pr_err("%s: Failed to register ts_input_handler\n", __func__);

	alarm_init(&register_input_rtc, ALARM_REALTIME,
		register_input_rtc_callback);
	alarm_init(&kad_repeat_rtc, ALARM_REALTIME,
		kad_repeat_rtc_callback);
	alarm_init(&check_single_fp_vib_rtc, ALARM_REALTIME,
		check_single_fp_vib_rtc_callback);
	alarm_init(&ts_poke_rtc, ALARM_REALTIME,
		ts_poke_rtc_callback);
	alarm_init(&vibrate_rtc, ALARM_REALTIME,
		vibrate_rtc_callback);
	alarm_init(&triple_tap_rtc, ALARM_REALTIME,
		triple_tap_rtc_callback);

	uci_add_user_listener(uci_user_listener);
	uci_add_sys_listener(ifilter_uci_sys_listener);
	ntf_add_listener(ntf_listener);

	init_done = 1;
	smart_last_user_activity_time = get_global_seconds();
err_input_dev:
//	input_free_device(ifilter_pwrdev);

err_alloc_dev:
	pr_info("%s ifilter done\n", __func__);

	return 0;
}

static void __exit ifilter_exit(void)
{
	kobject_del(ifilter_kobj);
	input_unregister_handler(&ifilter_input_handler);
	destroy_workqueue(ifilter_input_wq);
	destroy_workqueue(ifilter_pwr_wq);
	destroy_workqueue(ifilter_vib_wq);
	destroy_workqueue(kcal_listener_wq);
	input_unregister_device(ifilter_pwrdev);
	input_free_device(ifilter_pwrdev);

	return;
}

late_initcall(ifilter_init);
module_exit(ifilter_exit);
