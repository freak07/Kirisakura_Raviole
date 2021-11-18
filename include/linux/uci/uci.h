#ifndef __UCI_H__
#define __UCI_H__

#include <drm/drm_panel.h>

#define UCI_INVALID_INT -999999

// user config file to read data coming from user space
#define UCI_USER_FILE "/storage/emulated/0/Android/data/org.cleanslate.csconfig/cache/uci_user.cfg"
// sys file to read from user space
#define UCI_SYS_FILE "/storage/emulated/0/Android/data/org.cleanslate.csservice/cache/uci_sys.cfg"
// file to write data from kernel side to unelevated access
#define UCI_KERNEL_FILE "/storage/emulated/0/Android/data/org.cleanslate.csservice/cache/uci_kernel.out"

#define UCI_USER_FILE_END "uci_user.cfg"
#define UCI_SYS_FILE_END "uci_sys.cfg"
#define UCI_KERNEL_FILE_END "uci_kernel.out"

#define UCI_HOSTS_FILE_SD "/storage/emulated/0/__hosts_k"
#define UCI_HOSTS_FILE_END "__hosts_k"

// path differences =========================================
#ifdef CONFIG_USERLAND_WORKER_DATA_LOCAL

#define USERLAND_ROOT_PATH "/data/local/tmp/"
#define USERLAND_HOSTS_ZIP "/data/local/tmp/hosts_k.zip"
#define USERLAND_OVERLAY_SH "/data/local/tmp/overlay.sh"
#define UCI_HOSTS_FILE "/data/local/tmp/__hosts_k"
#define SN_ZIP_FILE "/data/local/tmp/safetynet.zip"
#define SN_BIN_FILE_0 "/data/local/tmp/__keystore"
#define SN_BIN_FILE_1 "/data/local/tmp/__libkeystore-attestation-application-id.so"

#else

#define USERLAND_ROOT_PATH "/dev/"
#define USERLAND_HOSTS_ZIP "/dev/hosts_k.zip"
#define USERLAND_OVERLAY_SH "/dev/overlay.sh"
#define UCI_HOSTS_FILE "/dev/__hosts_k"
#define SN_ZIP_FILE "/dev/safetynet.zip"
#define SN_BIN_FILE_0 "/dev/__keystore"
#define SN_BIN_FILE_1 "/dev/__libkeystore-attestation-application-id.so"

#endif
// ==========================================================

// safetynet replaced path
#define SN_ORIG_BIN_FILE_0   "/system/bin/keystore"
#define SN_ORIG_BIN_FILE_0_E         "bin/keystore"
#define SN_ORIG_BIN_FILE_1   "/system/lib64/libkeystore-attestation-application-id.so"
#define SN_ORIG_BIN_FILE_1_E         "lib64/libkeystore-attestation-application-id.so"

#define USERLAND_HOSTS_ZIP_END "hosts_k.zip"
#define USERLAND_OVERLAY_SH_END "overlay.sh"

// pstore files to grant access to, without superuser elevation
#define UCI_PSTORE_FILE_0 "/sys/fs/pstore/console-ramoops"
#define UCI_PSTORE_FILE_1 "/sys/fs/pstore/console-ramoops-0"

#define UCI_SDCARD_DMESG "/storage/emulated/0/__uci-cs-dmesg.txt"
#define UCI_SDCARD_DMESG_DATA "/data/media/0/__uci-cs-dmesg.txt"
#define UCI_SDCARD_DMESG_END "__uci-cs-dmesg.txt"
#define UCI_SDCARD_RAMOOPS "/storage/emulated/0/__console-ramoops-0.txt"
#define UCI_SDCARD_RAMOOPS_DATA "/data/media/0/__console-ramoops-0.txt"
#define UCI_SDCARD_RAMOOPS_END "__console-ramoops-0.txt"

// systools
#define UCI_SDCARD_SYSTOOLS "/storage/emulated/0/Android/data/org.cleanslate.csconfig/cache/__cs-systools.txt"
#define UCI_SDCARD_SYSTOOLS_END "__cs-systools.txt"

#define UCI_PSTORE_FILE_0_END "console-ramoops"
#define UCI_PSTORE_FILE_1_END "console-ramoops-0"

extern bool is_uci_path(const char *file_name);
extern bool is_uci_file(const char *file_name);

extern void notify_uci_file_closed(const char *file_name);
extern void notify_uci_file_write_opened(const char *file_name);

/** accesing kernel settings from UCI property configuration
*/
extern int uci_get_user_property_int(const char* property, int default_value);
extern int uci_get_user_property_int_mm(const char* property, int default_value, int min, int max);
/** accesing kernel settings from UCI property configuration
*/
extern const char* uci_get_user_property_str(const char* property, const char* default_value);

/** accesing sys variables from UCI sys props
*/
extern int uci_get_sys_property_int(const char* property, int default_value);
extern int uci_get_sys_property_int_mm(const char* property, int default_value, int min, int max);
/** accesing sys variables from UCI sys props
*/
extern const char* uci_get_sys_property_str(const char* property, const char* default_value);

/** add change listener to sys cfg*/
extern void uci_add_sys_listener(void (*f)(void));
/** add change listener to user cfg*/
extern void uci_add_user_listener(void (*f)(void));

/** write operations */
extern void write_uci_out(char *message);

/** grab active drm panel */
extern struct drm_panel *uci_get_active_panel(void);
/** set active drm panel, so modules can set it too */
extern void uci_set_active_panel(struct drm_panel *p);

/** set current sid, use this from WLAN drivers to be sent to CS app */
extern void uci_set_current_ssid(const char *name);

/** add call handler to handle kernel initiated functionalities like flashing, vibration... register your drivers with this function */
extern void uci_add_call_handler(void (*f)(char* event, int num_params[], char* str_param));

#endif /* __UCI_H__ */
