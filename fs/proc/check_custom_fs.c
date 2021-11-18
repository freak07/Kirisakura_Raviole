/**
    License GPL-2.0
    (c) by Pal Zoltan Illes 2017 illespal@gmail.com

    Checks different files in filesystem to diagnose how customizations
    shuld/could work.
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
#include <linux/uci/uci.h>
//file operation-

static void m_fclose(struct file* file) {
    filp_close(file, NULL);
}

static bool m_fopen_check(const char* path, int flags, int rights) {
    struct file* filp = NULL;
    int err = 0;

    filp = filp_open(path, flags, rights);

    if(IS_ERR(filp)) {
        err = PTR_ERR(filp);
        pr_err("[chk_magisk]File Open Error:%s %d\n",path, err);
	if (err==-2) {
		pr_err("[chk_magisk] verity File doesn't exist in root! magisk system\n");
		return true; // file doesnt exist, magisk
	}
        return false; // permission issue, exists, not magisk
    }
    if(!filp->f_op){
	pr_err("[chk_magisk]File Operation Method Error! non-magisk system\n");
        return false;
    }
    m_fclose(filp);
    pr_err("[chk_magisk] success..verity file found - non magisk system\n");
    return false; // successfuly opened, it's not magisk
}

static char *file_name="/verity_key";

bool finished = false;
bool magisk = false;
bool kadaway = true; // always return true for now, check not possible continuously outside system/system

// work func...
static void check_async(struct work_struct * check_async_work)
{
	if (finished) return;
	magisk = m_fopen_check(file_name, O_RDONLY, 0);
	//kadaway = !m_fopen_check(UCI_HOSTS_FILE, O_RDONLY, 0);
	pr_info("%s kadaway %d\n",__func__,kadaway);
	finished = true;
}
static DECLARE_WORK(check_async_work, check_async);

// work struct
static struct workqueue_struct *cfs_work_queue = NULL;

// sync call for is_magisk. Don't call it from atomic context!
bool is_magisk_sync(void) {
	return m_fopen_check(file_name, O_RDONLY, 0);
}
EXPORT_SYMBOL(is_magisk_sync);

void do_check(void) {
	if (cfs_work_queue) {
		if (!finished) {
			queue_work(cfs_work_queue, &check_async_work);
			while (!finished) {
				mdelay(1);
			}
			pr_info("%s kadaway %d\n",__func__,kadaway);
		}
	}
}

// async might_sleep part moved to work, delay wait for result.
// call this at initramfs mounted, where /init and /verity_key are yet in the root
// like when cmdline_show is shown first
bool is_magisk(void) {
	do_check();
	return magisk;
}
EXPORT_SYMBOL(is_magisk);

int uci_kadaway = 1;
static void uci_user_listener(void) {
	uci_kadaway = uci_get_user_property_int_mm("kadaway", 1, 0, 1);
	pr_info("%s uci_kadaway %d\n",__func__,uci_kadaway);
}
bool is_kadaway(void) {
	return kadaway && uci_kadaway;
}
EXPORT_SYMBOL(is_kadaway);

static bool uci_user_listener_added = false;
// call this from a non atomic contet, like init
void init_custom_fs(void) {
#if 0
	if (cfs_work_queue == NULL) {
		cfs_work_queue = create_singlethread_workqueue("customfs");
	}
#endif
	if (!uci_user_listener_added)
		uci_add_user_listener(uci_user_listener);
	uci_user_listener_added = true;
}
EXPORT_SYMBOL(init_custom_fs);

