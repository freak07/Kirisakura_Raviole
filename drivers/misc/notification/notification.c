/*
 * Copyright (C) 2018 Pal Zoltan Illes
 *
 * Licensed under the GPL-2 or later.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/spinlock.h>


//file operation+
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/vmalloc.h>
#include <asm/uaccess.h>
#include <linux/buffer_head.h>
#include <linux/mm.h>
//file operation-


#define CONFIG_MSM_DRM_NOTIFY
//#undef CONFIG_MSM_DRM_NOTIFY
#undef CONFIG_FB

#include <linux/notifier.h>
#include <linux/fb.h>

#if defined(CONFIG_UCI_NOTIFICATIONS_SCREEN_CALLBACKS)
#elif defined(CONFIG_DRM)
#include <drm/drm_panel.h>
struct drm_panel *active_panel;
#elif defined(CONFIG_MSM_DRM_NOTIFY)
#include <drm/drm_panel.h>
#endif

#include <linux/alarmtimer.h>
#include <linux/uci/uci.h>
#include <linux/notification/notification.h>

#define DRIVER_AUTHOR "illes pal <illespal@gmail.com>"
#define DRIVER_DESCRIPTION "uci notifications driver"
#define DRIVER_VERSION "1.2"

//#define NTF_D_LOG

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");

#if defined(CONFIG_UCI_NOTIFICATIONS_SCREEN_CALLBACKS)
#elif defined(CONFIG_FB) || defined(CONFIG_DRM)
struct notifier_block *uci_ntf_fb_notifier;
#elif defined(CONFIG_MSM_DRM_NOTIFY)
struct notifier_block *uci_ntf_msm_drm_notif;
#endif

bool ntf_face_down = false;
EXPORT_SYMBOL(ntf_face_down);
bool ntf_proximity = false;
EXPORT_SYMBOL(ntf_proximity);
bool ntf_silent = false;
EXPORT_SYMBOL(ntf_silent);
bool ntf_ringing = false;
EXPORT_SYMBOL(ntf_ringing);


// helper functions

static long get_global_mseconds(void) {
        struct timespec64 ts;
        long ret = 0;
	ktime_get_real_ts64(&ts);
        ret = (ts.tv_sec*1000LL) + ((ts.tv_nsec)/(1000LL*1000LL));
        //I("%s time = %ld",__func__,ret);
        return ret;
}

// listeners

static void (*ntf_listeners[100])(char* event, int num_param, char* str_param);
static int ntf_listener_counter = 0;

static void ntf_notify_listeners(char* event, int num_param, char* str_param) {
	int i =0;
	for (;i<ntf_listener_counter;i++) {
		(*ntf_listeners[i])(event,num_param,str_param);
	}
}

void ntf_add_listener(void (*f)(char* event, int num_param, char* str_param)) {
	if (ntf_listener_counter<100) {
		ntf_listeners[ntf_listener_counter++] = f;
	} else {
		// error;
	}
}
EXPORT_SYMBOL(ntf_add_listener);

static bool screen_on = true, screen_on_early = false, screen_off_early = false;
static long last_input_event = 0;

// ======= SCREEN ON/OFF

bool ntf_is_screen_on(void) {
	return screen_on;
}
EXPORT_SYMBOL(ntf_is_screen_on);
bool ntf_is_screen_early_on(void) {
	return screen_on_early;
}
EXPORT_SYMBOL(ntf_is_screen_early_on);
bool ntf_is_screen_early_off(void) {
	return screen_off_early;
}
EXPORT_SYMBOL(ntf_is_screen_early_off);

// ======= phone state
static bool ntf_in_call = false;
bool ntf_is_in_call(void) {
	return ntf_in_call;
}
EXPORT_SYMBOL(ntf_is_in_call);

// ======= CHARGE

bool is_charging = false;
bool ntf_is_charging(void) {
	return is_charging;
}
EXPORT_SYMBOL(ntf_is_charging);

#define CHARGE_STATE_ASYNC
#define CHARGE_STATE_ASYNC_DELAY_MSEC 20

#ifdef CHARGE_STATE_ASYNC
static struct workqueue_struct *uci_charge_state_async_wq;

static bool charge_state_async = true;

static void uci_charge_state_async_func(struct work_struct * uci_charge_state_async_func_work)
{
	bool on = charge_state_async;
	pr_info("%s notify charge state async = %u\n",__func__,on);
	ntf_notify_listeners(NTF_EVENT_CHARGE_STATE, on, "");
}
static DECLARE_DELAYED_WORK(uci_charge_state_async_func_work, uci_charge_state_async_func);
#endif

bool charge_state_changed = true;
unsigned long last_charge_state_change_time = 0;
void ntf_set_charge_state(bool on) {
#ifdef NTF_D_LOG
	pr_info("%s [cleanslate] charge state = %d\n",__func__,on);
#endif
	if (on!=is_charging) {
		// change handle
#ifndef CHARGE_STATE_ASYNC
		ntf_notify_listeners(NTF_EVENT_CHARGE_STATE, on, "");
#else
		pr_info("%s schedule async charge state work...\n",__func__);
		charge_state_async = on;
		cancel_delayed_work(&uci_charge_state_async_func_work);
		queue_delayed_work(uci_charge_state_async_wq,&uci_charge_state_async_func_work,msecs_to_jiffies(CHARGE_STATE_ASYNC_DELAY_MSEC)); // Wait N msecs to make sure usb comm is done
#endif
		charge_state_changed = true;
	}
	is_charging = on;
	last_charge_state_change_time = jiffies;
}
EXPORT_SYMBOL(ntf_set_charge_state);
int charge_level = -1;
void ntf_set_charge_level(int level) {
#ifdef NTF_D_LOG
	pr_info("%s [cleanslate] level = %d\n",__func__,level);
#endif
//	if (level!=charge_level || (charge_state_changed && is_charging))
	{
// change handle
		ntf_notify_listeners(NTF_EVENT_CHARGE_LEVEL, level, "");
		charge_state_changed = false;
	}
	charge_level = level;
}
EXPORT_SYMBOL(ntf_set_charge_level);

// wake_by_user: used for AmbientDisplay detection:
// this if true, then device is waken by user. Otherwise no Input device was triggered, so we can deduce that it's an AmibentDisplay wake
// and therefore if this is 0, Flashlight notification can be triggered, and bln_on_screenoff also can be stored, so that BLN can be
// triggered later when screen is self BLANKing / screen off....
static bool wake_by_user = true;
static unsigned long screen_off_jiffies = 0;
static bool kad_wake = false;


#if defined(CONFIG_UCI_NOTIFICATIONS_SCREEN_CALLBACKS)
void ntf_screen_on(void) {
		long last_input_event_diff = (get_global_mseconds() - last_input_event);
		screen_on_early = true;
		ntf_notify_listeners(NTF_EVENT_WAKE_EARLY,1,"");
		pr_info("%s ntf uci screen on -early\n",__func__);
		if (!screen_on || screen_off_early) {
			wake_by_user = kad_wake?false:(last_input_event_diff < 1400); // TODO to identify wake by ambient display, this callback is not sufficient. for now setting wake by user all the time
			// ...as after motion launch or Always on there's no screen on event again when pressing an input... so this is not called at that time
			kad_wake = false;
			pr_info("[cleanslate] ntf uci screen on , wake_by_user = %d last input diff %d \n", wake_by_user, (int)last_input_event_diff);
			screen_on = true;
			screen_on_early = true;
			screen_off_early = false;
			if (wake_by_user) {
				ntf_notify_listeners(NTF_EVENT_WAKE_BY_USER,1,"");
			} else {
				ntf_notify_listeners(NTF_EVENT_WAKE_BY_FRAMEWORK,1,"");
			}
		}
}
EXPORT_SYMBOL(ntf_screen_on);

void ntf_screen_off(void) {
		screen_off_early = true;
		ntf_notify_listeners(NTF_EVENT_SLEEP_EARLY,1,"");
		pr_info("%s ntf uci screen off\n",__func__);
		{
			pr_info("ntf uci screen off\n");
			screen_on = false;
			screen_on_early = false;
			screen_off_early = true;
			wake_by_user = false;
			screen_off_jiffies = jiffies;
			ntf_notify_listeners(NTF_EVENT_SLEEP,1,"");
		}
}
EXPORT_SYMBOL(ntf_screen_off);

#elif defined(CONFIG_FB) || defined(CONFIG_DRM)
static int first_unblank = 1;

static int fb_notifier_callback(struct notifier_block *self,
                                 unsigned long event, void *data)
{
    struct fb_event *evdata = data;
    int *blank;
    long last_input_event_diff = (get_global_mseconds() - last_input_event);

    if (evdata && evdata->data && event == FB_EARLY_EVENT_BLANK ) {
        blank = evdata->data;
        switch (*blank) {
        case FB_BLANK_UNBLANK:
		screen_on_early = true;
		ntf_notify_listeners(NTF_EVENT_WAKE_EARLY,1,"");
		pr_info("ntf uci screen on -early\n");
            break;

        case FB_BLANK_POWERDOWN:
        case FB_BLANK_HSYNC_SUSPEND:
        case FB_BLANK_VSYNC_SUSPEND:
        case FB_BLANK_NORMAL:
		screen_off_early = true;
		ntf_notify_listeners(NTF_EVENT_SLEEP_EARLY,1,"");
		pr_info("ntf uci screen off -early\n");
            break;
        }
    }
    if (evdata && evdata->data && event == FB_EVENT_BLANK ) {
        blank = evdata->data;
        switch (*blank) {
        case FB_BLANK_UNBLANK:
		pr_info("ntf uci screen on\n");
                wake_by_user = kad_wake?false:true; //first_unblank || last_input_event_diff < 1400; // TODO to identify wake by ambient display, this callback is not sufficient. for now setting wake by user all the time
		// ...as after motion launch or Always on there's no screen on event again when pressing an input... so this is not called at that time
		pr_info("[cleanslate] ntf uci screen on , kad_wake = %d wake_by_user = %d last input diff = %d \n", kad_wake, wake_by_user, (int)last_input_event_diff);
		kad_wake = false;
		if (first_unblank) {
			first_unblank = 0;
		}
		screen_on = true;
		screen_on_early = true;
		screen_off_early = false;
		if (wake_by_user) {
			ntf_notify_listeners(NTF_EVENT_WAKE_BY_USER,1,"");
		} else {
			ntf_notify_listeners(NTF_EVENT_WAKE_BY_FRAMEWORK,1,"");
		}
            break;

        case FB_BLANK_POWERDOWN:
        case FB_BLANK_HSYNC_SUSPEND:
        case FB_BLANK_VSYNC_SUSPEND:
        case FB_BLANK_NORMAL:
		pr_info("ntf uci screen off\n");
		screen_off_jiffies = jiffies;
		screen_on = false;
		screen_on_early = false;
		screen_off_early = true;
		//wake_by_user = false; // TODO set this back uncommented if AOD detection is fine
		ntf_notify_listeners(NTF_EVENT_SLEEP,1,"");
            break;
        }
    }
    return 0;
}


#elif defined(CONFIG_MSM_DRM_NOTIFY)
static int first_unblank = 1;

static int fb_notifier_callback(
    struct notifier_block *nb, unsigned long val, void *data)
{
    struct msm_drm_notifier *evdata = data;
    unsigned int blank;
    long last_input_event_diff = (get_global_mseconds() - last_input_event);

    // allow in all plus new type REQUEST_EVENT_BLANK to catch AOD -> normal screen on events
    if (val != MSM_DRM_EARLY_EVENT_BLANK && val != MSM_DRM_EVENT_BLANK)
	return 0;

    if (evdata->id != MSM_DRM_PRIMARY_DISPLAY)
        return 0;

    pr_info("[info] %s go to the msm_drm_notifier_callback value = %d\n",
	    __func__, (int)val);

    if (evdata && evdata->data && val == MSM_DRM_EARLY_EVENT_BLANK) {
	blank = *(int *)(evdata->data);
	switch (blank) {
	case MSM_DRM_BLANK_POWERDOWN:
		screen_off_early = true;
		ntf_notify_listeners(NTF_EVENT_SLEEP_EARLY,1,"");
		pr_info("ntf uci screen off\n");
	    break;
	case MSM_DRM_BLANK_UNBLANK:
		screen_on_early = true;
		ntf_notify_listeners(NTF_EVENT_WAKE_EARLY,1,"");
		pr_info("ntf uci screen on\n");
	    break;
	default:
	    pr_info("%s defalut\n", __func__);
	    break;
	}
    }
    if (evdata && evdata->data && val == MSM_DRM_EVENT_BLANK) {
	blank = *(int *)(evdata->data);
	switch (blank) {
	case MSM_DRM_BLANK_POWERDOWN:
		{
			pr_info("ntf uci screen off\n");
			screen_on = false;
			screen_on_early = false;
			screen_off_early = true;
			wake_by_user = false;
			screen_off_jiffies = jiffies;
			ntf_notify_listeners(NTF_EVENT_SLEEP,1,"");
		}
	    break;
	case MSM_DRM_BLANK_UNBLANK:
		pr_info("ntf uci screen on\n");
		if (!screen_on || screen_off_early) {
			wake_by_user = kad_wake?false:true; //first_unblank || last_input_event_diff < 1400; // TODO to identify wake by ambient display, this callback is not sufficient. for now setting wake by user all the time
			// ...as after motion launch or Always on there's no screen on event again when pressing an input... so this is not called at that time
			kad_wake = false;
			if (first_unblank) {
				first_unblank = 0;
			}
			pr_info("[cleanslate] ntf uci screen on , wake_by_user = %d last input diff %d \n", wake_by_user, (int)last_input_event_diff);
			screen_on = true;
			screen_on_early = true;
			screen_off_early = false;
			if (wake_by_user) {
				ntf_notify_listeners(NTF_EVENT_WAKE_BY_USER,1,"");
			} else {
				ntf_notify_listeners(NTF_EVENT_WAKE_BY_FRAMEWORK,1,"");
			}
		}
	    break;
	default:
	    pr_info("%s default\n", __func__);
	    break;
	}
    }
    return NOTIFY_OK;
}
#endif

void ntf_screen_aod_on(void) {
//	wake_by_user = false;
	pr_info("fpf ntf uci AOD on\n");
	ntf_notify_listeners(NTF_EVENT_AOD_GESTURE,1,"on");
}
EXPORT_SYMBOL(ntf_screen_aod_on);

void ntf_screen_full_on(void) {
/*	pr_info("fpf ntf uci fullscreen on\n");
//	wake_by_user // TODO
	if (!screen_on) {
		screen_on = true;
		screen_on_early = true;
		screen_off_early = false;
		if (wake_by_user) {
			ntf_notify_listeners(NTF_EVENT_WAKE_BY_USER,1,"");
		} else {
			ntf_notify_listeners(NTF_EVENT_WAKE_BY_FRAMEWORK,1,"");
		}
	}*/
}
EXPORT_SYMBOL(ntf_screen_full_on);

//

bool ntf_wake_by_user(void) {
	return wake_by_user;
}
EXPORT_SYMBOL(ntf_wake_by_user);

void ntf_input_event(const char* caller, const char *param) {
	// input event happened, stop stuff, store timesamp, set wake_by_user
	//pr_info("%s called by %s",__func__,caller);
	last_input_event = get_global_mseconds();
	wake_by_user = true;
	smart_set_last_user_activity_time();
	ntf_notify_listeners(NTF_EVENT_INPUT,1,(char *)param);
}
EXPORT_SYMBOL(ntf_input_event);

// TODO move fpf part here?
//extern int register_fp_vibration(void);

void ntf_vibration(int length) {
	if (length>=MIN_TD_VALUE_NOTIFICATION) {
		unsigned int time_diff_charge = jiffies - last_charge_state_change_time;
#if 0
// op6
		if (length==MIN_TD_VALUE_OP6_FORCED_FP) return;
		if (length==MIN_TD_VALUE_OP6_SILENT_MODE) return;
#endif
		if (jiffies_to_msecs(time_diff_charge) > 2400) { // if charger attached too close in time, it will be a non notif vibration, don't trigger...
			ntf_notify_listeners(NTF_EVENT_NOTIFICATION, 1, NTF_EVENT_NOTIFICATION_ARG_HAPTIC);
		}
	}
#if 0
// htc u12
	if (length==TD_VALUE_HTC_U12_FINGERPRINT) register_fp_vibration();//ntf_input_event(__func__,"fp");
#endif
}
EXPORT_SYMBOL(ntf_vibration);

void ntf_led_blink(enum notif_led_type led, bool on) {
	// low battery blink RED, don't do a thing...
	if (on && led == NTF_LED_RED && charge_level <= 15) return;
	if (on) {
#if 1
// op6 - if blink starts too close to screen off, don't trigger notification event
		unsigned int diff_screen_off = jiffies - screen_off_jiffies;
		if (diff_screen_off <= 50) return;
#endif
		ntf_notify_listeners(NTF_EVENT_NOTIFICATION,1,"");
	}
}
EXPORT_SYMBOL(ntf_led_blink);

void ntf_kad_wake(void) {
	kad_wake = true;
}
EXPORT_SYMBOL(ntf_kad_wake);

void ntf_led_off(void) {
	ntf_notify_listeners(NTF_EVENT_NOTIFICATION,0,"off");
}

static bool camera_on = false;
void ntf_camera_started(void) {
	ntf_notify_listeners(NTF_EVENT_CAMERA_ON,1,"on");
	camera_on = true;
}
EXPORT_SYMBOL(ntf_camera_started);

void ntf_camera_stopped(void) {
	ntf_notify_listeners(NTF_EVENT_CAMERA_ON,0,"off");
	camera_on = false;
}
EXPORT_SYMBOL(ntf_camera_stopped);

bool ntf_is_camera_on(void) {
	return camera_on;
}
EXPORT_SYMBOL(ntf_is_camera_on);

static int last_notification_number = 0;
static bool ntf_locked = true;
// registered sys uci listener
static void uci_sys_listener(void) {
        pr_info("%s [CLEANSLATE] sys listener... \n",__func__);
        {
                bool ringing_new = !!uci_get_sys_property_int_mm("ringing", 0, 0, 1);
                bool proximity_new = !!uci_get_sys_property_int_mm("proximity", 0, 0, 1);
                bool locked_new = !!uci_get_sys_property_int_mm("locked", 0, 0, 1);
                bool in_call = !!uci_get_sys_property_int_mm("in_call", 0, 0, 1);
                ntf_face_down = !!uci_get_sys_property_int_mm("face_down", 0, 0, 1);
                ntf_silent = !!uci_get_sys_property_int_mm("silent", 0, 0, 1);

		if (in_call != ntf_in_call) {
			ntf_in_call = in_call;
			ntf_notify_listeners(NTF_EVENT_IN_CALL, ntf_in_call?1:0, "");
		}
		if (proximity_new != ntf_proximity) {
			ntf_proximity = proximity_new;
			ntf_notify_listeners(NTF_EVENT_PROXIMITY, ntf_proximity?1:0, "");
		}
		if (locked_new != ntf_locked) {
			ntf_locked = locked_new;
			ntf_notify_listeners(NTF_EVENT_LOCKED, ntf_locked?1:0, "");
		}

                if (ringing_new && !ntf_ringing) {
			ntf_notify_listeners(NTF_EVENT_RINGING, 1, "");
                }
                if (!ringing_new && ntf_ringing) {
                        ntf_ringing = false;
			ntf_notify_listeners(NTF_EVENT_RINGING, 0, "");
                }
                ntf_ringing = ringing_new;

                pr_info("%s uci sys face_down %d\n",__func__,ntf_face_down);
                pr_info("%s uci sys proximity %d\n",__func__,ntf_proximity);
                pr_info("%s uci sys silent %d\n",__func__,ntf_silent);
                pr_info("%s uci sys ringing %d\n",__func__,ntf_ringing);
        }
	{
    		int ringing = uci_get_sys_property_int_mm("ringing", 0, 0, 1);
    		pr_info("%s uci sys ringing %d\n",__func__,ringing);
    		if (ringing) {
                        ntf_input_event(__func__,NULL);
    		}
	}
	{
    		int notifications = uci_get_sys_property_int("notifications",0);
    		if (notifications != -EINVAL) {
    			if (notifications>last_notification_number) {
				// send notification event
				ntf_notify_listeners(NTF_EVENT_NOTIFICATION, 1, "");
			}
			last_notification_number = notifications;
		}
	}
}

// registered user uci listener
static void uci_user_listener(void) {
        pr_info("%s [CLEANSLATE] user listener... \n",__func__);
}


static int __init ntf_init(void)
{
	int rc = 0;
	pr_info("uci ntf - init\n");
#if defined(CONFIG_UCI_NOTIFICATIONS_SCREEN_CALLBACKS)
	// nothing to do here...
#elif defined(CONFIG_DRM)
        uci_ntf_notifier = kzalloc(sizeof(struct notifier_block), GFP_KERNEL);;
        uci_ntf_notifier->notifier_call = fb_notifier_callback;
        if (active_panel) {
                drm_panel_notifier_register(active_panel,
                    uci_ntf_notifier);
        }
#elif defined(CONFIG_FB)
	uci_ntf_fb_notifier = kzalloc(sizeof(struct notifier_block), GFP_KERNEL);;
	uci_ntf_fb_notifier->notifier_call = fb_notifier_callback;
	fb_register_client(uci_ntf_fb_notifier);
#elif defined(CONFIG_MSM_DRM_NOTIFY)
        uci_ntf_msm_drm_notif = kzalloc(sizeof(struct notifier_block), GFP_KERNEL);;
        uci_ntf_msm_drm_notif->notifier_call = fb_notifier_callback;
        rc = msm_drm_register_client(uci_ntf_msm_drm_notif);
        if (rc)
                pr_err("Unable to register msm_drm_notifier: %d\n", rc);
#endif
#ifdef CHARGE_STATE_ASYNC
	uci_charge_state_async_wq = alloc_workqueue("uci_charge_state_async_wq",
		WQ_HIGHPRI | WQ_MEM_RECLAIM, 1);
#endif

	uci_add_sys_listener(uci_sys_listener);
	uci_add_user_listener(uci_user_listener);

	return rc;
}

static void __exit ntf_exit(void)
{
	pr_info("uci ntf - exit\n");
}

late_initcall(ntf_init);
module_exit(ntf_exit);

