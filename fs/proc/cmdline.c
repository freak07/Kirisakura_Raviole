// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <asm/setup.h>

#if 1
#include <linux/slab.h>

//#define REPLACE_SN

#ifdef CONFIG_UCI
#include <linux/spinlock.h>

static bool done = false;
static DEFINE_SPINLOCK(show_lock);
static bool magisk = true;
#endif

extern bool is_magisk(void);
extern bool is_magisk_sync(void);
extern void init_custom_fs(void);

#endif

static char new_command_line[COMMAND_LINE_SIZE];

static int cmdline_proc_show(struct seq_file *m, void *v)
{
#ifdef CONFIG_UCI
	static int count = 0;
	spin_lock(&show_lock);
	if (done) {
	} else {
		magisk = is_magisk();
		done = true;
	}
	spin_unlock(&show_lock);
	pr_debug("%s cleanslate cmdline debugging. call number # %d\n",__func__,count++);
#endif
	seq_puts(m, new_command_line);
	seq_putc(m, '\n');
	return 0;
}

#ifdef REMOVE_SN
static void remove_flag(char *cmd, const char *flag)
{
	char *start_addr, *end_addr;

	/* Ensure all instances of a flag are removed */
	while ((start_addr = strstr(cmd, flag))) {
		end_addr = strchr(start_addr, ' ');
		if (end_addr)
			memmove(start_addr, end_addr + 1, strlen(end_addr));
		else
			*(start_addr - 1) = '\0';
	}
}

static void remove_safetynet_flags(char *cmd)
{
	remove_flag(cmd, "androidboot.veritymode=");
}
#endif


#ifdef REPLACE_SN
static char *padding = "                ";

static void replace_flag(char *cmd, const char *flag, const char *flag_new)
{
	char *start_addr, *end_addr;

	/* Ensure all instances of a flag are replaced */
	while ((start_addr = strstr(cmd, flag))) {
		end_addr = strchr(start_addr, ' ');
		if (end_addr) {
			if (strlen(flag)<strlen(flag_new)) {
				// xx yy=a zz
				//    ^   ^
				// xx yy=bb zz
				int length_to_copy = strlen( start_addr + (strlen(flag) ) ) + 1; // +1 to copy trailing '/0'
				int length_diff = strlen(flag_new)-strlen(flag);
				memcpy(start_addr+(strlen(flag)+length_diff), start_addr+(strlen(flag)), length_to_copy);
				memcpy(start_addr+(strlen(flag)), padding, length_diff);
			}
			memcpy(start_addr, flag_new, strlen(flag_new));
		}
		else
			*(start_addr - 1) = '\0';
	}
}

static void replace_safetynet_flags(char *cmd)
{
	// WARNING: be aware that you can't replace shorter string with longer ones in the function called here...
	replace_flag(cmd, "androidboot.vbmeta.device_state=unlocked",
			  "androidboot.vbmeta.device_state=locked  ");
	replace_flag(cmd, "androidboot.enable_dm_verity=0",
			  "androidboot.enable_dm_verity=1");
	replace_flag(cmd, "androidboot.secboot=disabled",
			  "androidboot.secboot=enabled ");
	replace_flag(cmd, "androidboot.verifiedbootstate=orange",
			  "androidboot.verifiedbootstate=green ");
	replace_flag(cmd, "androidboot.veritymode=logging",
			  "androidboot.veritymode=enforcing");
	replace_flag(cmd, "androidboot.veritymode=eio",
			  "androidboot.veritymode=enforcing");

}
#endif

static int __init proc_cmdline_init(void)
{
#ifdef CONFIG_UCI
	init_custom_fs();
#endif
	strcpy(new_command_line, saved_command_line);

	/*
	 * Remove/replace various flags from command line seen by userspace in order to
	 * pass SafetyNet CTS check.
	 */
#ifdef REPLACE_SN
	replace_safetynet_flags(new_command_line);
#endif
#ifdef REMOVE_SN
	remove_safetynet_flags(new_command_line);
#endif

	proc_create_single("cmdline", 0, NULL, cmdline_proc_show);
	return 0;
}
fs_initcall(proc_cmdline_init);
