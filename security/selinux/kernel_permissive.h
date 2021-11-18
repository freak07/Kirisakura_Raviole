/** License GPL-v2
    Copyright 2020 by Pal Zoltan Illes
*/

static int kernel_permissive_needed_count = 0;
static bool kernel_permissive = false;

#include <linux/spinlock.h>
static DEFINE_SPINLOCK(kp_set_lock);

void set_kernel_permissive(bool on) {
	spin_lock(&kp_set_lock);

	if (on)
		kernel_permissive_needed_count++;
	else
		kernel_permissive_needed_count--;

        kernel_permissive = kernel_permissive_needed_count!=0;
	pr_info("%s --- setting to: %d,  kp usage count: %d \n",__func__,kernel_permissive, kernel_permissive_needed_count);
	spin_unlock(&kp_set_lock);
}
EXPORT_SYMBOL(set_kernel_permissive);

// set this if only userspace should have permissivity,
// ...and in-kernel decisions should be still denied.
static bool full_permissive_kernel_suppressed = false;
void set_full_permissive_kernel_suppressed(bool on) {
        full_permissive_kernel_suppressed = on;
}
EXPORT_SYMBOL(set_full_permissive_kernel_suppressed);

// source class
#define KERNEL_SOURCE "u:r:kernel:s0"

#define TARGETS_LENGTH 30
// target class list
static char targets[TARGETS_LENGTH][255] = {
                "u:object_r:toolbox_exec:s0",
                "u:object_r:shell_exec:s0",
                "u:r:kernel:s0",
                "u:object_r:fuse:s0",
                "u:object_r:shell_data_file:s0",
                "u:object_r:property_data_file:s0",
                "u:object_r:property_socket:s0",
                "u:r:init:s0",
                "u:object_r:exported2_default_prop:s0",
                "u:object_r:vendor_radio_prop:s0",
                "u:object_r:default_prop:s0",
                "u:object_r:system_file:s0",
                "u:object_r:device:s0",
                "u:object_r:kmsg_device:s0", // needed for dmesg
                "u:object_r:properties_serial:s0", // needed for setprop
                "u:object_r:pstorefs:s0", // console ramoops
                "u:object_r:ctl_start_prop:s0",
//              "u:object_r:sdcardfs:s0", // sdcard copy -> do not add this, keep it safer, use uci.h/uci.c
                "u:object_r:mnt_user_file:s0", // for sdcardfs -> do not add this, instead use FS open/write per path check, keep it secure
                "u:r:vendor_init:s0",
                "u:r:ueventd:s0",
                "u:r:servicemanager:s0",
                "u:r:hwservicemanager:s0",
                "u:r:vndservicemanager:s0",
                "u:r:surfaceflinger:s0",
                "u:object_r:build_prop:s0",
                "u:object_r:bootloader_prop:s0",
                "u:object_r:property_service_version_prop:s0",
                "u:object_r:fingerprint_prop:s0",
                "u:object_r:build_odm_prop:s0",
                "u:object_r:build_vendor_prop:s0"

        };

