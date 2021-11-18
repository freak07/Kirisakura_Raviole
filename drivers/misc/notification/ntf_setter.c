/*
 * Copyright (C) 2020 Pal Zoltan Illes
 * illespal@gmail.com
 *
 * Licensed under the GPL-2 or later.
 */

/**
    central module to set peripherals
    upon notification events, or charging...etc
*/

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/spinlock.h>

#include <linux/alarmtimer.h>
#include <linux/notification/notification.h>
#include <linux/notification/notification_set.h>
#include <linux/uci/uci.h>

#ifdef LOCKING
static DEFINE_SPINLOCK(blink_spinlock);
#endif

static int init_done = 0;

static bool charging = false;
static int last_charge_level = 0;
static bool blinking = false;


bool rgb_pulse = false;
int rgb_pulse_pattern = 0;
bool rgb_batt_colored = false;
bool rgb_batt_colored_discrete = false;
int rgb_batt_colored_lvl0 = 0;
int rgb_batt_colored_lvl1 = 30;
int rgb_batt_colored_lvl2 = 70;
bool rgb_pulse_blink_on_charger = false;
int rgb_pulse_blink_on_charger_red_limit = 70;

extern void ntf_led_front_set_charge_colors(int r, int g, int b, bool warp, bool blink);
extern ntf_led_front_release_charge(void);

extern void ntf_led_back_set_charge_colors(int r, int g, int b, bool warp, bool blink);
extern ntf_led_back_release_charge(void);

int notification_booster_overdrive_perc = 0;
bool boost_only_in_pocket = false;
bool in_pocket = false;

void set_led_charge_colors(int level, bool blink) {
	if (rgb_batt_colored) {

        static int last_level = 0;
        int level_div = level / 5;
        int level_round = level_div * 5; // rounding by 5;
        int us_level = (level_round * 235)/100;
        int red_coeff = 255 - (us_level); // red be a bit more always, except on FULL charge ( 255 - 220 -> Min = 25, except on full where it is 1 )
        int green_coeff = 235 - red_coeff; // green be a bit less always, except on FULL charge ( Green max is 220, min 1 - except on full where its 255)

        pr_info(" %s level %d last_level %d charging %d \n",__func__, level, last_level, charging);

        // store new values into last_ variables
        last_level = level;

        if (!rgb_batt_colored_discrete) {
                if (green_coeff < 1) green_coeff = 10;

                if (level<5) { // under 5, always full RED but low light for red
                        red_coeff = 80;
                        green_coeff = 1;
                } else
                if (level<15) { // under 15, always full RED but lower light for red
                        red_coeff = 160;
                        green_coeff = 3;
                } else
                if (level<20) { // under 20, always full RED full light for red
                        red_coeff = 255;
                        green_coeff = 7;
                }
        } else {
                if (level < rgb_batt_colored_lvl0) {
                        green_coeff = 0;
                        red_coeff = 40;
                } else
                if (level < rgb_batt_colored_lvl1) {
                        green_coeff = 40;
                        red_coeff = 160;
                } else
                if (level < rgb_batt_colored_lvl2) {
                        green_coeff = 120;
                        red_coeff = 90;
                } else {
                        green_coeff = 235;
                        red_coeff = 20;
                }
        }

        if (level == 100) { // at 100, always full GREEN, (except when blinking, then set coeff to 20). 
			// Minimum is 1, to have red LED on for warping
                red_coeff = blinking?20:1;
                green_coeff = 255;
                pr_info("%s color transition at full strength: red %d green %d \n",__func__, red_coeff, green_coeff);
        }

	ntf_led_front_set_charge_colors(red_coeff, green_coeff, 0, level==100, blinking);
// WARP TEST:	ntf_led_front_set_charge_colors(1, 255, 0, true, blinking);
	}
}

void set_led_blink(bool b) {
	if (rgb_pulse_blink_on_charger || !b) {
		blinking = b;
	}
	if (rgb_pulse_blink_on_charger) {
		if (charging) {
			set_led_charge_colors(last_charge_level,blinking);
		}
	}
}

#ifdef CONFIG_UCI_NOTIFICATIONS

static void ntf_listener(char* event, int num_param, char* str_param) {
	if (strcmp(event,NTF_EVENT_CHARGE_LEVEL) && strcmp(event, NTF_EVENT_INPUT)) {
		pr_info("%s blink ntf_setter listener event %s %d %s\n",__func__,event,num_param,str_param);
	}

	if (!strcmp(event,NTF_EVENT_CHARGE_STATE)) {
		bool new_charging = !!num_param;
		if (new_charging != charging) {
			if (new_charging) {
				// set leds
				blinking = false; // reset blinking upon connection, can mix up things if not
				charging = new_charging;
				set_led_charge_colors(last_charge_level,blinking);
			} else {
				blinking = false;
				ntf_led_front_release_charge();
				ntf_led_back_release_charge();
			}
		}
		charging = new_charging;
	}
	if (!strcmp(event,NTF_EVENT_CHARGE_LEVEL)) {
		last_charge_level = num_param;
		if (charging) {
			// calculate and set RG(b)
			set_led_charge_colors(last_charge_level,blinking);
		} else {
			blinking = false;
			ntf_led_front_release_charge();
			ntf_led_back_release_charge();
		}
	}

	if (!strcmp(event,NTF_EVENT_NOTIFICATION)) {
		if (!!num_param) {
			// notif started
			if (!ntf_is_screen_on() || !ntf_wake_by_user())
			{
				set_led_blink(true);
			}
		} else {
			// notif over
			set_led_blink(false);
		}
	}
	if (!strcmp(event,NTF_EVENT_WAKE_BY_USER)) { // SCREEN ON BY USER
		set_led_blink(false);
	}
	if (!strcmp(event,NTF_EVENT_LOCKED) && !num_param) { // UNLOCKED / faceunlock
		set_led_blink(false);
	}
	if (!strcmp(event,NTF_EVENT_INPUT)) { // INPUT
		if (ntf_wake_by_user()) {
			if (blinking) {
				set_led_blink(false);
			}
		}
	}
	if (!strcmp(event,NTF_EVENT_SLEEP)) {
		ntf_vibration_set_in_pocket( (!ntf_is_screen_on() && boost_only_in_pocket)?notification_booster_overdrive_perc:0, boost_only_in_pocket?in_pocket:false);
	}
	if (!strcmp(event,NTF_EVENT_PROXIMITY)) { // proximity
		if (!!num_param) {
			in_pocket = true;
		} else{
			in_pocket = false;
		}
		ntf_vibration_set_in_pocket( (!ntf_is_screen_on() && boost_only_in_pocket)?notification_booster_overdrive_perc:0, (boost_only_in_pocket&&!ntf_is_screen_on())?in_pocket:false);
	}
}
#endif

static void uci_user_listener(void) {
	int vibration_power_percentage = uci_get_user_property_int_mm("vibration_power_percentage", 10, 0, 100);
	bool vibration_power_set = !!uci_get_user_property_int_mm("vibration_power_set", 0, 0, 1);
	ntf_vibration_set_haptic(vibration_power_set?vibration_power_percentage:0);

	rgb_pulse = !!uci_get_user_property_int_mm("bln_rgb_pulse", 0, 0, 1);
	rgb_pulse_pattern = uci_get_user_property_int_mm("bln_rgb_pulse_pattern", 0, 0, 4);
	rgb_batt_colored = !!uci_get_user_property_int_mm("bln_rgb_batt_colored", 0, 0, 1);
	rgb_batt_colored_discrete = !!uci_get_user_property_int_mm("bln_rgb_batt_colored_discrete", 0, 0, 1);
        rgb_batt_colored_lvl0 = uci_get_user_property_int_mm("bln_rgb_batt_colored_lvl_0", 0, 0, 99);
        rgb_batt_colored_lvl1 = uci_get_user_property_int_mm("bln_rgb_batt_colored_lvl_1", 30, 0, 99);
        rgb_batt_colored_lvl2 = uci_get_user_property_int_mm("bln_rgb_batt_colored_lvl_2", 70, 0, 99);
        rgb_pulse_blink_on_charger = !!uci_get_user_property_int_mm("bln_rgb_pulse_blink_on_charger", 0, 0, 1);
        rgb_pulse_blink_on_charger_red_limit = uci_get_user_property_int_mm("bln_rgb_pulse_blink_on_charger_red_limit", 70, 0, 100);

	notification_booster_overdrive_perc = uci_get_user_property_int_mm("notification_booster_overdrive_perc", 10, 0, 100);
	boost_only_in_pocket = !!uci_get_user_property_int_mm("boost_only_in_pocket", 0, 0, 1);
	ntf_vibration_set_in_pocket( (!ntf_is_screen_on() && boost_only_in_pocket)?notification_booster_overdrive_perc:0, (boost_only_in_pocket&&!ntf_is_screen_on())?in_pocket:false);


}

static int __init ntf_setter_init_module(void)
{
        int32_t rc = 0;

#ifdef CONFIG_UCI_NOTIFICATIONS
	ntf_add_listener(ntf_listener);
#endif
        uci_add_user_listener(uci_user_listener);

	init_done = 1;
        return rc;
}


static void __exit ntf_setter_exit_module(void)
{
        return;
}

module_init(ntf_setter_init_module);
module_exit(ntf_setter_exit_module);
MODULE_DESCRIPTION("NTF SETTER");
MODULE_LICENSE("GPL v2");
