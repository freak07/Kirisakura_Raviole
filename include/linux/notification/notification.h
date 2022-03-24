#ifndef __NOTIFICATION_H__
#define __NOTIFICATION_H__

enum notif_type {
        NOTIF_KAD,
        NOTIF_FLASHLIGHT,
        NOTIF_VIB_REMINDER,
        NOTIF_VIB_BOOSTER,
        NOTIF_BUTTON_LIGHT,
        NOTIF_PULSE_LIGHT
};

enum notif_smart_level_type {
        NOTIF_DEFAULT, // keep as is
        NOTIF_TRIM, // trim, make less often, shorter, weaker
        NOTIF_DIM, // dim the light
        NOTIF_STOP // stop overall
};

enum notif_led_type {
	NTF_LED_RED = 0,
	NTF_LED_GREEN,
	NTF_LED_BLUE
};

#define NTF_EVENT_NOTIFICATION "notification"
#define NTF_EVENT_RINGING "ringing"
#define NTF_EVENT_CHARGE_STATE "charge_state"
#define NTF_EVENT_CHARGE_LEVEL "charge_level"
#define NTF_EVENT_INPUT "input"
#define NTF_EVENT_WAKE_BY_USER "wake_by_user"
#define NTF_EVENT_WAKE_BY_FRAMEWORK "wake_by_framework"
#define NTF_EVENT_AOD_GESTURE "aod_gesture"
#define NTF_EVENT_WAKE_EARLY "wake_early"
#define NTF_EVENT_SLEEP_EARLY "sleep_early"
#define NTF_EVENT_SLEEP "sleep"
#define NTF_EVENT_PROXIMITY "proximity"
#define NTF_EVENT_LOCKED "locked"
#define NTF_EVENT_CAMERA_ON "camera"
#define NTF_EVENT_IN_CALL "in_call"

#define NTF_EVENT_NOTIFICATION_ARG_HAPTIC "haptic"

#define MIN_TD_VALUE_NOTIFICATION 100
// sense framework based values, 1000 for call, 500 for alarm
#define MIN_TD_VALUE_NOTIFICATION_CALL 1000
#define MIN_TD_VALUE_NOTIFICATION_ALARM 500
// op6
#define MIN_TD_VALUE_OP6_SILENT_MODE 300
#define MIN_TD_VALUE_OP6_FORCED_FP 250
// u12
#define TD_VALUE_HTC_U12_FINGERPRINT 40

extern void smart_set_last_user_activity_time(void);
extern int smart_get_notification_level(int notif_type);

// screen state queries
extern bool ntf_is_screen_on(void);
extern bool ntf_is_screen_early_on(void);
extern bool ntf_is_screen_early_off(void);
// query camera state
extern bool ntf_is_camera_on(void);

// charge callbacks to notify ntf - call it from battery/policy drivers
extern void ntf_set_charge_state(bool on);
extern void ntf_set_charge_level(int level);
extern bool ntf_is_charging(void);
extern bool ntf_is_in_call(void);

// flashlight
extern void ntf_set_cam_flashlight(bool on);

// was the screen wake by user input...
extern bool ntf_wake_by_user(void);
// signal a user input (use it in touchscreen, input drivers..)
extern void ntf_input_event(const char* caller, const char *param);
// vibration events
extern void ntf_vibration(int val);
// led blink events
extern void ntf_led_blink(enum notif_led_type led, bool on);
// led off - possibly notification over...
extern void ntf_led_off(void);
// call this signal when kad is before waking screen.
extern void ntf_kad_wake(void);
// camera on/off, to avoid display of KAD or other ntf services
extern void ntf_camera_started(void);
extern void ntf_camera_stopped(void);
extern void ntf_block_camera(bool val);

// AOD
extern void ntf_screen_aod_on(void);
extern void ntf_screen_full_on(void);

#ifdef CONFIG_UCI_NOTIFICATIONS_SCREEN_CALLBACKS
// screen on/off callbacks - insert in driver code like backlight...
extern void ntf_screen_on(void);
extern void ntf_screen_off(void);
#endif

/** add change listener */
extern void ntf_add_listener(void (*f)(char* event, int num_param, char* str_param));

#endif /* __NOTIFICATION_H__ */
