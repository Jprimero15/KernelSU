#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/memory.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>

#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/namei.h>
#include <linux/rcupdate.h>

#include <linux/delay.h> // msleep

#include "klog.h"
#include "selinux/selinux.h"

#define FILE_MAGIC 0x7f4b5355 // ' KSU', u32
#define FILE_FORMAT_VERSION 1 // u32

struct perm_data {
  struct list_head list;
  uid_t uid;
  bool allow;
};

static struct list_head allow_list;

#define KERNEL_SU_ALLOWLIST "/data/adb/.ksu_allowlist"

static struct workqueue_struct *ksu_workqueue;
static struct work_struct ksu_save_work;
static struct work_struct ksu_load_work;

bool persistent_allow_list(void);

struct file *permissive_filp_open(const char * path, int flags, umode_t mode) {
  struct file* fp;
  // fixme: u:r:kernel:s0 don't have permission to write /data/adb...
  bool enforcing = getenforce();
  if (enforcing) setenforce(false);
  fp = filp_open(path, flags, mode);
  if (enforcing) setenforce(true);
  return fp;
}

bool ksu_allow_uid(uid_t uid, bool allow) {

  // find the node first!
  struct perm_data *p = NULL;
  struct list_head *pos = NULL;
  bool result = false;
  list_for_each(pos, &allow_list) {
    p = list_entry(pos, struct perm_data, list);
    pr_info("ksu_allow_uid :%d, allow: %d\n", p->uid, p->allow);
    if (uid == p->uid) {
      p->allow = allow;
      result = true;
      goto exit;
    }
  }

  // not found, alloc a new node!
  p = (struct perm_data *)kmalloc(sizeof(struct perm_data), GFP_KERNEL);
  if (!p) {
    pr_err("alloc allow node failed.\n");
    return false;
  }
  p->uid = uid;
  p->allow = allow;

  list_add_tail(&p->list, &allow_list);
  result = true;

exit:

  persistent_allow_list();

  return result;
}

bool ksu_is_allow_uid(uid_t uid) {
  struct perm_data *p = NULL;
  struct list_head *pos = NULL;
  list_for_each(pos, &allow_list) {
    p = list_entry(pos, struct perm_data, list);
    pr_info("is_allow_uid uid :%d, allow: %d\n", p->uid, p->allow);
    if (uid == p->uid) {
      return p->allow;
    }
  }

  return false;
}

bool ksu_get_allow_list(int *array, int *length, bool allow) {
  struct perm_data *p = NULL;
  struct list_head *pos = NULL;
  int i = 0;
  list_for_each(pos, &allow_list) {
    p = list_entry(pos, struct perm_data, list);
    pr_info("get_allow_list uid: %d allow: %d\n", p->uid, p->allow);
    if (p->allow == allow) {
      array[i++] = p->uid;
    }
  }
  *length = i;

  return true;
}

void do_persistent_allow_list(struct work_struct *work) {
  u32 magic = FILE_MAGIC;
  u32 version = FILE_FORMAT_VERSION;
  struct perm_data *p = NULL;
  struct list_head *pos = NULL;
  loff_t off = 0;

  struct file *fp = permissive_filp_open(KERNEL_SU_ALLOWLIST, O_WRONLY | O_CREAT, 0644);

  if (IS_ERR(fp)) {
    pr_err("save_allow_list creat file failed: %d\n", PTR_ERR(fp));
    return;
  }

  // store magic and version
  if (kernel_write(fp, &magic, sizeof(magic), &off) != sizeof(magic)) {
    pr_err("save_allow_list write magic failed.\n");
    goto exit;
  }

  if (kernel_write(fp, &version, sizeof(version), &off) != sizeof(version)) {
    pr_err("save_allow_list write version failed.\n");
    goto exit;
  }

  list_for_each(pos, &allow_list) {
    p = list_entry(pos, struct perm_data, list);
    pr_info("save allow list uid :%d, allow: %d\n", p->uid, p->allow);
    kernel_write(fp, &p->uid, sizeof(p->uid), &off);
    kernel_write(fp, &p->allow, sizeof(p->allow), &off);
  }

exit:
  filp_close(fp, 0);
}

void do_load_allow_list(struct work_struct *work) {

  loff_t off = 0;
  ssize_t ret = 0;
  struct file *fp = NULL;
  int n = 0;
  u32 magic;
  u32 version;

  fp = filp_open("/data/adb/", O_RDONLY, 0);
  if (IS_ERR(fp)) {
    int errno = PTR_ERR(fp);
    pr_err("load_allow_list open '/data/adb' failed: %d\n", PTR_ERR(fp));
    if (errno == -ENOENT) {
      msleep(2000);
      queue_work(ksu_workqueue, &ksu_load_work);
      return;
    } else {
      pr_info("load_allow list dir exist now!");
    }
  } else {
    filp_close(fp, 0);
  }

#if 1
  // load allowlist now!
  fp = permissive_filp_open(KERNEL_SU_ALLOWLIST, O_RDONLY, 0);

  if (IS_ERR(fp)) {
    pr_err("load_allow_list open file failed: %d\n", PTR_ERR(fp));
    return;
  }

  // verify magic
  if (kernel_read(fp, &magic, sizeof(magic), &off) != sizeof(magic) || magic != FILE_MAGIC) {
    pr_err("allowlist file invalid: %d!\n", magic);
    goto exit;
  }

  if (kernel_read(fp, &version, sizeof(version), &off) != sizeof(version)) {
    pr_err("allowlist read version: %d failed\n", version);
    goto exit;
  }

  pr_info("allowlist version: %d\n", version);

  while (true) {
    u32 uid;
    bool allow = false;
    ret = kernel_read(fp, &uid, sizeof(uid), &off);
    if (ret <= 0) {
      pr_info("load_allow_list read err: %d\n", ret);
      break;
    }
    ret = kernel_read(fp, &allow, sizeof(allow), &off);

    pr_info("load_allow_uid: %d, allow: %d\n", uid, allow);

    ksu_allow_uid(uid, allow);
  }

exit:

  filp_close(fp, 0);
#endif
}

static int init_work(void) {
  ksu_workqueue = alloc_workqueue("kernelsu_work_queue", 0, 0);
  INIT_WORK(&ksu_save_work, do_persistent_allow_list);
  INIT_WORK(&ksu_load_work, do_load_allow_list);
  return 0;
}

// make sure allow list works cross boot
bool persistent_allow_list(void) {
  queue_work(ksu_workqueue, &ksu_save_work);
  return true;
}

bool ksu_allowlist_init(void) {

  INIT_LIST_HEAD(&allow_list);

  init_work();

  // start load allow list.
  queue_work(ksu_workqueue, &ksu_load_work);

  return true;
}

bool ksu_allowlist_exit(void) {

  destroy_workqueue(ksu_workqueue);

  return true;
}