#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/of_fdt.h>
#include <asm/setup.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/genhd.h>
#include <linux/kernel.h>
#include <linux/utsname.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/file.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/kdev_t.h>
#include <linux/string.h>

#include "partition_rw.h"

static struct platform_device *proinfo_pdev;
static struct device *proinfo_root_dev;

//define type
typedef enum{
    WT_PROINFO_countrycode = 0,
}wt_proinfo_type;

//#define WT_PROINFO_COUNTRYCODE_LEN 512
#define WT_PROINFO_COUNTRYCODE_STRING_LEN 4
#define COUNTRYCODE_OEM_OFFSET 338// (32*10+18)//Need to modify
#define OEM_PAGE_SIZE 512
char countrycode_buf[WT_PROINFO_COUNTRYCODE_STRING_LEN+1];

static int wt_proinfo_countrycode_read(wt_proinfo_type type, char* buf);
static int wt_proinfo_countrycode_write(wt_proinfo_type type, const char* buf, int len);


#define WT_PROINFO_COUNTRYCODE_CREATE_ATTR(name) \
static ssize_t proinfo_countrycode_##name##_store(struct device * dev, struct device_attribute *attr, const char * buf,size_t count); \
static ssize_t proinfo_countrycode_##name##_show(struct device *dev, struct device_attribute *attr, char *buf); \
\
\
static ssize_t proinfo_countrycode_##name##_store(struct device * dev, struct device_attribute *attr, const char * buf,\
                                  size_t count)\
{\
        int ret = -1;\
        printk("entry  %s\n",__FUNCTION__);\
\
        ret = wt_proinfo_countrycode_write(WT_PROINFO_##name, buf, count);\
        \
        return ret;\
}\
\
static ssize_t proinfo_countrycode_##name##_show(struct device *dev, struct device_attribute *attr, char *buf)\
{\
        int ret = -1;\
        printk("entry  %s\n",__FUNCTION__);\
\
        ret = wt_proinfo_countrycode_read(WT_PROINFO_##name, buf);\
\
        return ret;\
}\
\
static DEVICE_ATTR(name, S_IWUSR|S_IRUSR|S_IROTH|S_IRGRP, proinfo_countrycode_##name##_show, proinfo_countrycode_##name##_store);

WT_PROINFO_COUNTRYCODE_CREATE_ATTR(countrycode)


static int wt_create_device_files(void)
{
        int rc = 0;

        rc = device_create_file(proinfo_root_dev, &dev_attr_countrycode);
        if (rc)
                return rc;
        return 0;
}

static int wt_proinfo_countrycode_read(wt_proinfo_type type, char* buf)
{
    char oem_buf[OEM_PAGE_SIZE] = {'\0'};
    if(type != WT_PROINFO_countrycode){
        printk("[TYPE] %s: open proinfo path error\n", __func__);
        return 0;
    }
    
    printk("%s read countrycode [%d] \n", __func__, type);

    if(ufs_partition_rw("oem", 0, 0, (void *)oem_buf, OEM_PAGE_SIZE) < 0) {
        return 0;
    }
    printk("%s buf=%s\n", __func__, oem_buf);
    memcpy(buf, oem_buf+COUNTRYCODE_OEM_OFFSET, WT_PROINFO_COUNTRYCODE_STRING_LEN);
    return WT_PROINFO_COUNTRYCODE_STRING_LEN;
}

static int wt_proinfo_countrycode_write(wt_proinfo_type type, const char* buf, int len)
{
        char * oem_buf;//[OEM_PAGE_SIZE];
        
        printk(KERN_ERR "%s,%s len %d\n",__func__,buf,len);
        if(type != WT_PROINFO_countrycode){
                printk("[TYPE] %s: open proinfo path error\n", __func__);
                return 0;
        }

        if(len > WT_PROINFO_COUNTRYCODE_STRING_LEN+1)
                return 0;
        else {
            oem_buf = kmalloc(OEM_PAGE_SIZE, GFP_KERNEL);
            if(oem_buf == NULL)
                return 0;
            
            if(ufs_partition_rw("oem", 0, 0, (void *)oem_buf, OEM_PAGE_SIZE) < 0) {
                kfree(oem_buf);
                return 0;
            }
            memcpy(&oem_buf[COUNTRYCODE_OEM_OFFSET], buf, WT_PROINFO_COUNTRYCODE_STRING_LEN);

            if(ufs_partition_rw("oem", 1, 0, (void *)oem_buf, OEM_PAGE_SIZE) < 0) {
                kfree(oem_buf);
                return 0;
             }
             kfree(oem_buf);
        }
        return WT_PROINFO_COUNTRYCODE_STRING_LEN;
}

static struct platform_driver proinfo_pdrv = {
        .driver = {
                .name   = "proinfo",
                .owner  = THIS_MODULE,
        //      .pm     = &proinfo_ops,
        },
};

static int __init
proinfo_init(void)
{
        int rc;

        rc = platform_driver_register(&proinfo_pdrv);
        if (rc)
                return rc;

        proinfo_pdev = platform_device_register_simple("proinfo", -1, NULL,
                                                        0);
        if (IS_ERR(proinfo_pdev)) {
                rc = PTR_ERR(proinfo_pdev);
                goto out_pdrv;
        }



        proinfo_root_dev = root_device_register("proinfo");
        if (IS_ERR(proinfo_root_dev)) {
                rc = PTR_ERR(proinfo_root_dev);
                goto out_pdev;
        }
        rc = wt_create_device_files();
        if (rc)
                goto out_root;

        return 0;

out_root:
        root_device_unregister(proinfo_root_dev);
out_pdev:
        platform_device_unregister(proinfo_pdev);
out_pdrv:
        platform_driver_unregister(&proinfo_pdrv);
        return rc;
}

/*
 * The init/exit functions.
 */
static void __exit
proinfo_exit(void)
{
        platform_device_unregister(proinfo_pdev);
        platform_driver_unregister(&proinfo_pdrv);
        root_device_unregister(proinfo_root_dev);
}

module_init(proinfo_init);
module_exit(proinfo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("wt_modem");
