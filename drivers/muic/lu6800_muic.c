/*
 * Sniper MAX14526 & TS5USBA33402 multi MUIC driver
 *
 * Copyright (C) 2010 LGE, Inc.
 *
 * Author: Sookyoung Kim <sookyoung.kim@lge.com>
 *
 * Modified: 2010-11-17, Add MAX14526 control code <daewung.kim@lge.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>	// printk()
#include <linux/init.h>		// __init, __exit
#include <linux/uaccess.h>	// copy_from/to_user()
#include <linux/interrupt.h>	// request_irq()
#include <linux/irq.h>		// set_irq_type()
#include <linux/types.h>	// kernel data types
#include <asm/system.h>
// kernel/arch/arm/include/asm/gpio.h includes kernel/arch/arm/plat-omap/include/mach/gpio.h which,
// in turn, includes kernel/include/asm-generic/gpio.h.
// <mach/gpio.h> declares gpio_get|set_value(), gpio_to_irq().
// <asm-generic/gpio.h> declares struct gpio_chip, gpio_request(), gpio_free(), gpio_direction_input|output().
// The actual descriptions are in kernel/drivers/gpio/gpiolib.c and kernel/arch/arm/plat-omap/gpio.c.
#include <asm/gpio.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>	// INIT_WORK()
#include <linux/wakelock.h>

#include <linux/i2c/twl.h>	// chager_state_update_by_other()


//#include "hub_muic.h"
#include <linux/muic/muic.h>
#include <linux/switch_dp3t.h>
#include <linux/muic/muic_max14526deewp.h>
#include <linux/switch_usif.h>

//#include "hub_charging_ic.h"
#include "../hub/hub_charging_ic.h"		// temp kibum.lee@lge.com 20120502 MUIC re-work
#include "../mux.h"


#define TS5USBA33402	0x10
#define MAX14526	0x20


extern void android_USB_disconnect();// for usb disconnect event

/* LGE_CHANGE_S [kenneth.kang@lge.com] 2010-12-14, CP retain mode and 910K cable detect*/
#define CABLE_DETECT_910K
#define CP_RETAIN	//LGE_UPDATE [jaejoong.kim] disable CP_RETAIN (for other justin)
/* LGE_CHANGE_E [kenneth.kang@lge.com] 2010-12-14, CP retain mode and 910K cable detect*/

#define USB_RETAIN /* 2012.05.15, mannsik.chung@lge.com, USB retain mode. */
#ifdef USB_RETAIN
#undef CP_RETAIN
#endif

static const char name_muic_mode[MUIC_MODE_NO][30] = {
	"MUIC_UNKNOWN",		// 0
	"MUIC_NONE",   		// 1
	"MUIC_NA_TA",   	// 2
	"MUIC_LG_TA",   	// 3
	"MUIC_TA_1A", 	  	// 4
	"MUIC_INVALID_CHG",  	// 5
	"MUIC_AP_UART",   	// 6
	"MUIC_CP_UART",		// 7
	"MUIC_AP_USB", 		// 8
	"MUIC_CP_USB",		// 9
	"MUIC_TV_OUT_NO_LOAD",	// 10
	"MUIC_EARMIC",		// 11
	"MUIC_TV_OUT_LOAD",	// 12
	"MUIC_OTG",   		// 13
	"MUIC_MHL",			// 14
#if	defined(CONFIG_PRODUCT_LGE_LU6800) || defined(CONFIG_PRODUCT_LGE_KU5900)
	"MUIC_CP_DOWNLOAD",	// 15
	"MUIC_ILLEGAL_CHG",	//16
	"MUIC_RESERVE1",		// 17
	"MUIC_MODE_NO",		// 18	
#else
	"MUIC_RESERVE1",		// 15
	"MUIC_RESERVE2",		// 16
	"MUIC_RESERVE3",		// 17
	"MUIC_MODE_NO",		// 18
#endif	
};

static struct i2c_client *muic_client;
static struct delayed_work muic_wq;
void charger_state_update_by_other(void);

static u8 int_stat_val;
static u8 status_val;

#if 1
static u8 muic_device = TS5USBA33402;
#else
static u8 muic_device = MAX14526;
#endif

//TYPE_USIF_MODE usif_mode = USIF_AP;
//TYPE_DP3T_MODE dp3t_mode = DP3T_NC;
extern TYPE_USIF_MODE usif_mode;		// temp kibum.lee@lge.com 20120502 MUIC re-work
extern TYPE_DP3T_MODE dp3t_mode;	// temp kibum.lee@lge.com 20120502 MUIC re-work


// kibum.lee@lge.com 20120502 MUIC re-work
/* 
 * Add private device handle
 */
struct hub_muic_device {
	struct i2c_client *client;
};
// kibum.lee@lge.com 20120502 MUIC re-work

TYPE_UPON_IRQ  upon_irq = NOT_UPON_IRQ;
TYPE_MUIC_MODE muic_mode = MUIC_UNKNOWN;

extern s32 key_pressed;
extern s32 key_row;
extern s32 key_col;
static s32 key_was_pressed = 0;

// START sungchae.koo@lge.com 2011/03/03 LAB1_FW : WAKELOCK_CASE_AT_CP_USB : Ref from LU3000 {
extern	int reset_status;
// END sungchae.koo@lge.com 2011/03/03 LAB1_FW }

/* LGE_CHANGE_S [kenneth.kang@lge.com] 2010-12-14, CP retain mode */
#ifdef CP_RETAIN
static int is_cp_retained;
#endif

#ifdef USB_RETAIN /* 2012.05.15, mannsik.chung@lge.com, USB retain mode. */
enum usbmode_retained {
    NOT_RETAINED,
    CP_RETAINED,
    AP_RETAINED,
};
static int usb_retained = NOT_RETAINED;
#endif

static int hidden_menu_switching;
/* LGE_CHANGE_E [kenneth.kang@lge.com] 2010-12-14, CP retain mode */

/* LGE_CHANGE_START 2011-03-16 kenneth.kang@lge.com patch for Adb offline set and Mass Storage Driver detecting fail */    
static int muic_init_done=0;     // 20110113 ks.kwon@lge.com check muic driver init. state
/* LGE_CHANGE_END 2011-03-16 kenneth.kang@lge.com */

/* Wake lock for usb connection */
struct wlock {
	int wake_lock_on;
	struct wake_lock wake_lock;
};
static struct wlock the_wlock;

// START sungchae.koo@lge.com 2011/04/03 LAB1_FW : BLOCK_ENTERING_EARLYSUSPEND_STATE for factory test {
int usb_swiched_thru_factory_cmd = 0;
// END sungchae.koo@lge.com 2011/04/03 LAB1_FW }

// START injae02.lee@lge.com 2011/04/16 LAB1_FW : JUSTIN_FOTA_FIX {
int fota_ing = 0;
// END injae02.lee@lge.com 2011/04/16 LAB1_FW }

//void dp3t_switch_ctrl(TYPE_DP3T_MODE mode);
//void usif_switch_ctrl(TYPE_USIF_MODE mode);
extern void dp3t_switch_ctrl(TYPE_DP3T_MODE mode);	// temp kibum.lee@lge.com 20120502 MUIC re-work
extern void usif_switch_ctrl(TYPE_USIF_MODE mode);		// temp kibum.lee@lge.com 20120502 MUIC re-work
s32 muic_AP_UART_set(void);
s32 muic_CP_UART_set(void);
s32 muic_AP_USB_set(void);
s32 muic_CP_USB_set(void);

void check_usb_reg(void); /* 20111105, mschung@ubiquix.com, for stable USB connection. */

// LGE_UPDATE_S 20110412 [jaejoong.kim@lge.com] 
void muic_udelay(u32 microsec);

/* 20110727, mschung@ubiquix.com, Separate twl4030_bci_battery for justin. [UB_START] */
//2011-03-14 ntyeongon.moon@lge.com get muic INT_STAT  VBUS [START_LGE]
int is_muic_mvbus_on(void)
{
	return (int_stat_val&MVBUS); // MVBUS
}
EXPORT_SYMBOL(is_muic_mvbus_on);
//2011-03-14 ntyeongon.moon@lge.com get muic INT_STAT  VBUS [END_LGE]
/* 20110727, mschung@ubiquix.com, Separate twl4030_bci_battery for justin. [UB_END] */

#ifdef CONFIG_PROC_FS
/*--------------------------------------------------------------------
 * BEGINS: Proc file system related functions for MUIC.
 *--------------------------------------------------------------------
 */
#define	HUB_MUIC_PROC_FILE "driver/hmuic"
static struct proc_dir_entry *hub_muic_proc_file;

static ssize_t muic_proc_read(struct file *filp, char *buf, size_t len, loff_t *offset)
{
	int ret;
    char val[2]={0,};

	s32 i;
	u32 reg_val;

    static int read_done = 0;

    if( read_done )
    {
        read_done = 0;
        return 0;
    }

	for(i=0; i<=5; i++) {
		reg_val = i2c_smbus_read_byte_data(muic_client, i);
		printk(KERN_INFO "[MUIC] Reg 0x%X, Val=0x%X\n", i, reg_val);
	}

	printk(KERN_INFO "[MUIC] muic_proc_read(): muic_mode = %s (%d)\n", name_muic_mode[(s32)muic_mode], muic_mode);
	/* fill the buffer, return the buffer size */
    memset(val, 0, sizeof(val));
    val[0] = (char)muic_mode+'0';
    ret = sprintf( buf, "%s", val);

    read_done = 1;
	return ret;
}


static ssize_t muic_proc_write(struct file *filp, const char *buf, size_t len, loff_t *off){
	char messages[12];
	u32 val;
	u32 reg;
	s32 err;
	char cmd;

	if(len > 12)
		len = 12;

	if(copy_from_user(messages, buf, len))
		return -EFAULT;

	sscanf(buf, "%c 0x%x 0x%x", &cmd, &reg, &val);

	printk(KERN_INFO "[MUIC] LGE: MUIC_proc_write \n");

	switch(cmd){

// START sungchae.koo@lge.com 2011/04/03 LAB1_FW : BLOCK_ENTERING_EARLYSUSPEND_STATE for factory test {
		/* AP_USB mode for factory command */
		case 'f':
			//muic_AP_USB_set();
                    muic_set_mode(MUIC_AP_USB);
			usb_swiched_thru_factory_cmd = 1;
			break;
// END sungchae.koo@lge.com 2011/04/03 LAB1_FW }

// LGE_UPDATE 20110311 [jaejoong.kim@lge.com] add CP Image Downloadfor Hidden Menu - AP CP USB Swithcing
		/* CP Image Download */
		case '3':
                        //muic_CP_USB_set();
                        muic_set_mode(MUIC_CP_USB);	// kibum.lee@lge.com
                        gpio_set_value(26, 0);
			mdelay(100);
                        gpio_set_value(26, 1);
                        break;
// LGE_UPDATE 20110311 [jaejoong.kim@lge.com] add CP Image Downloadfor Hidden Menu - AP CP USB Swithcing

		/* AP_UART mode*/
		case '6':
			//muic_AP_UART_set();
			muic_set_mode(MUIC_AP_UART);	// kibum.lee@lge.com
			break;
/* LGE_CHANGE_S [kenneth.kang@lge.com] 2011-01-06, CP retain mode and Hidden Menu AP CP Switching Retain */
		/* CP_UART mode*/
		case '7':
			//muic_CP_UART_set();
			muic_set_mode(MUIC_CP_UART);	// kibum.lee@lge.com
			hidden_menu_switching = 7;
			break;

		/* AP_USB mode*/
		case '8':
			//muic_AP_USB_set();
			muic_set_mode(MUIC_AP_USB);	// kibum.lee@lge.com
			hidden_menu_switching = 8;
			break;

		/* CP_USB mode*/
		case '9':
			//muic_CP_USB_set();
			muic_set_mode(MUIC_CP_USB);	// kibum.lee@lge.com
			hidden_menu_switching = 9;
			break;

		case 'w':
			err = i2c_smbus_write_byte_data(muic_client, reg, val);
			printk(KERN_INFO "[MUIC] LGE: Hub MUIC Write Reg. = 0x%X, Value 0x%X\n", reg, val);
			break;

		default :
			printk(KERN_INFO "[MUIC] LGE: Hub MUIC invalid command\n");
		        printk(KERN_INFO "[MUIC] 6: AP_UART, 7: CP_UART, 8: AP_USB, 9: CP_USB\n");
		        printk(KERN_INFO "[MUIC] or \"w address value\"\n");
			break;
	}
	return len;
}

static struct file_operations hub_muic_proc_ops = {
	.read = muic_proc_read,
	.write = muic_proc_write,
};

static void create_hub_muic_proc_file(void){
	hub_muic_proc_file = create_proc_entry(HUB_MUIC_PROC_FILE, 0777, NULL);
	if(hub_muic_proc_file) {
		//hub_muic_proc_file->owner = THIS_MODULE; // 20100617 kernel API changed
		hub_muic_proc_file->proc_fops = &hub_muic_proc_ops;
	} else
		printk(KERN_INFO "[MUIC] LGE: Hub MUIC proc file create failed!\n");
}

static void remove_hub_muic_proc_file(void){
	remove_proc_entry(HUB_MUIC_PROC_FILE, NULL);
}

/*--------------------------------------------------------------------
 * ENDS: Proc file system related functions for MUIC.
 *--------------------------------------------------------------------
 */
#endif //CONFIG_PROC_FS

TYPE_MUIC_MODE get_muic_mode(void){
//kibum.lee@lge.com 20120502 MUIC re-work
#if defined(CONFIG_MUIC)
	return muic_get_mode();
#else
	return muic_mode;
//kibum.lee@lge.com 20120502 MUIC re-work
#endif
}
EXPORT_SYMBOL(get_muic_mode);

/*
 * Function: Read the MUIC register whose internal address is addr
 *           and save the u8 content into value.
 * Return value: Negative errno upon failure, 0 upon success.
 */
static s32 muic_i2c_read_byte_local(u8 addr, u8 *value){
	*value = i2c_smbus_read_byte_data(muic_client, (u8)addr);
	if(*value < 0){
		printk(KERN_INFO "[MUIC] muic_i2c_read_byte_local failed.\n");
		return *value;
	}

	return 0;
}

/*
 * Function: Write u8 value to the MUIC register whose internal address is addr.
 * Return value: Negative errno upon failure, 0 upon success.
 */
static s32 muic_i2c_write_byte_local(u8 addr, u8 value){
	s32 ret;
	ret = i2c_smbus_write_byte_data(muic_client, (u8)addr, (u8)value);
	if(ret < 0) printk(KERN_INFO "[MUIC] muic_i2c_write_byte_local failed.\n");
	return ret;
}

static void set_wakelock(u32 set) {

	if (set > 0) {
		if(!the_wlock.wake_lock_on)
			wake_lock(&the_wlock.wake_lock);
			the_wlock.wake_lock_on=1;
	} 
// START injae02.lee@lge.com 2011/04/16 LAB1_FW : JUSTIN_FOTA_FIX {
	else if( fota_ing )
	{
		// do noting.
		printk(KERN_WARNING "[MUIC] set_wakelock : do nothing!\n");
	}
// END injae02.lee@lge.com 2011/04/16 LAB1_FW }

	else 
	{
		if(the_wlock.wake_lock_on)
			wake_unlock(&the_wlock.wake_lock);
	}
	the_wlock.wake_lock_on = set;

	printk(KERN_WARNING "[MUIC] wake_lock : %s\n", (set ? "on" : "off"));
}

//kibum.lee@lge.com 20120502 MUIC re-work
#if 0
void usif_switch_ctrl(TYPE_USIF_MODE mode){

/* LGE_CHANGE_START 2011-03-16 kenneth.kang@lge.com patch for Adb offline set and Mass Storage Driver detecting fail */    
	/* 20110113 ks.kwon@lge.com check muic driver init. state [START] */
	if(!muic_init_done){

		printk(KERN_WARNING "[MUIC] MUIC has not been initialized! Nothing will be done!!!.\n");
		return ;
	}	
    /* 20110113 ks.kwon@lge.com check muic driver init. state [END] */
/* LGE_CHANGE_END 2011-03-16 kenneth.kang@lge.com */    	

    /* gpio output mode setting */
    gpio_direction_output(USIF_IN_1_GPIO, 0);

    /* gpio data setting */
	if(mode == USIF_AP){
		gpio_set_value(USIF_IN_1_GPIO, 0);
// START injae02.lee@lge.com 2011/04/16 LAB1_FW : JUSTIN_FOTA_FIX {
		printk(KERN_WARNING "[MUIC] usif_switch_ctrl( USIF_AP )\n");
		dp3t_switch_ctrl(DP3T_NC);
		set_wakelock(1);
		fota_ing = 1;
// END injae02.lee@lge.com 2011/04/16 LAB1_FW }
	}
// START injae02.lee@lge.com 2011/04/16 LAB1_FW : JUSTIN_FOTA_FIX {
	else if( fota_ing )
	{
		// do nothing.
		printk(KERN_WARNING "[MUIC] usif_switch_ctrl : do nothing!\n");
	}
// END injae02.lee@lge.com 2011/04/16 LAB1_FW }

	else if(mode == USIF_DP3T){
// START injae02.lee@lge.com 2011/04/16 LAB1_FW : JUSTIN_FOTA_FIX {
		printk(KERN_WARNING "[MUIC] usif_switch_ctrl( USIF_DP3T )\n");
		if( !fota_ing )
		{
			printk(KERN_WARNING "[MUIC] usif_switch_ctrl() : fota_ing != 1\n");
// END injae02.lee@lge.com 2011/04/16 LAB1_FW }
			gpio_set_value(USIF_IN_1_GPIO, 1);
		}
	}
	else{
		/* Just keep the current path */
	}
	usif_mode = mode;
}
EXPORT_SYMBOL(usif_switch_ctrl);

void dp3t_switch_ctrl(TYPE_DP3T_MODE mode){
    /* gpio output mode setting */
    gpio_direction_output(IFX_VBUS_EN, 0);
    gpio_direction_output(DP3T_IN_1_GPIO, 0);
    gpio_direction_output(DP3T_IN_2_GPIO, 0);

// START injae02.lee@lge.com 2011/04/16 LAB1_FW : JUSTIN_FOTA_FIX {
	if( fota_ing ) {
		printk(KERN_WARNING "[MUIC] dp3t_switch_ctrl() fota_ing == 1, returns\n");
		dp3t_mode = mode;
		return;
		//mode = DP3T_NC;
	}
// END injae02.lee@lge.com 2011/04/16 LAB1_FW }

	if(mode == DP3T_AP_UART){
		gpio_set_value(IFX_VBUS_EN, 0);
		gpio_set_value(DP3T_IN_1_GPIO, 1);
		gpio_set_value(DP3T_IN_2_GPIO, 0);
	}
	else if(mode == DP3T_CP_UART){
		gpio_set_value(IFX_VBUS_EN, 0);
		gpio_set_value(DP3T_IN_1_GPIO, 0);
		gpio_set_value(DP3T_IN_2_GPIO, 1);
	}
	else if(mode == DP3T_CP_USB){
		gpio_set_value(IFX_VBUS_EN, 1);
		gpio_set_value(DP3T_IN_1_GPIO, 1);
		gpio_set_value(DP3T_IN_2_GPIO, 1);
	}
	else if(mode == DP3T_NC){
		gpio_set_value(IFX_VBUS_EN, 0);
		gpio_set_value(DP3T_IN_1_GPIO, 0);
		gpio_set_value(DP3T_IN_2_GPIO, 0);
	}
	else{
		/* Just keep the current path */
	}
	dp3t_mode = mode;
}
#endif
//kibum.lee@lge.com 20120502 MUIC re-work

s32 muic_AP_UART_set(void){

	s32 ret;
/* LGE_CHANGE_START 2011-03-16 kenneth.kang@lge.com patch for Adb offline set and Mass Storage Driver detecting fail */    
	/* 20110113 ks.kwon@lge.com check muic driver init. state [START] */
	if(!muic_init_done){

		printk(KERN_WARNING "[MUIC] MUIC has not been initialized! Nothing will be done!!!.\n");
		return 0;
	}	
    /* 20110113 ks.kwon@lge.com check muic driver init. state [END] */
/* LGE_CHANGE_END 2011-03-16 kenneth.kang@lge.com */

	/* Connect AP UART to MUIC UART */
	//dp3t_switch_ctrl(DP3T_AP_UART); // BJ

	if (muic_device == MAX14526) {
		/* ID_200, VLDO 2.6V, ADC Enable, Charge Pump.*/
		ret = muic_i2c_write_byte_local(CONTROL_1, MID_200 | MADC_EN | MCP_EN);
	}
	else {
		/* ID_200, VLDO 2.6V, SEMREN on. ADC is auto.*/
		ret = muic_i2c_write_byte_local(CONTROL_1, MID_200 | MSEMREN);
	}

	/* Connect DP, DM to UART_TX, UART_RX*/
	ret = muic_i2c_write_byte_local(SW_CONTROL, DP_UART | DM_UART);

	/* Turn on charger IC with FACTORY mode */
	charging_ic_set_factory_mode();

	muic_mode = MUIC_AP_UART;
	printk(KERN_WARNING "[MUIC] muic_distinguish_vbus_accessory(): AP_UART\n");

	set_wakelock(0);

	return ret;
}

s32 muic_AP_USB_set(void){

	s32 ret;
/* LGE_CHANGE_START 2011-03-16 kenneth.kang@lge.com patch for Adb offline set and Mass Storage Driver detecting fail */    
	/* 20110113 ks.kwon@lge.com check muic driver init. state [START] */
	if(!muic_init_done){

		printk(KERN_WARNING "[MUIC] MUIC has not been initialized! Nothing will be done!!!.\n");
		return 0;
	}

    /* 20110113 ks.kwon@lge.com check muic driver init. state [END] */
/* LGE_CHANGE_END 2011-03-16 kenneth.kang@lge.com */

	if(hidden_menu_switching == 8)	// if USB mode change AP_USB through Proc filesystem, force D+/D- open
	{
	        ret = muic_i2c_write_byte_local(SW_CONTROL, DP_OPEN | DM_OPEN);
	        muic_udelay(100000);

	        if(ret < 0)
	                printk(KERN_WARNING "[MUIC] i2c write error for D+/D- open in muic_CP_USB_SET\n");
	}

	/* Justin?�서 AP_UART�??�정??AP_USB ?�작 ?�함 */
    //dp3t_switch_ctrl(DP3T_NC);

	if (muic_device == MAX14526) {
		/* ID_200, VLDO 2.6V, ADC Enable,
		 * USB 2.0 also needs the charge pump (CP_EN) on.*/
		ret = muic_i2c_write_byte_local(CONTROL_1, MID_200 | MADC_EN | MCP_EN);
	}
	else {


// START sungchae.koo@lge.com 2011/03/10 LAB1_FW {
		if( MUIC_AP_USB != muic_mode ) 
		{
			ret = muic_i2c_write_byte_local(SW_CONTROL, DP_OPEN | DM_OPEN);
			/* ID_200, VLDO 2.6V, SEMREN on. ADC is auto.
			 * USB 2.0 also needs the charge pump (CP_EN) on.*/
			ret = muic_i2c_write_byte_local(CONTROL_1, MID_200 | MSEMREN | MCP_EN);
		}
// END sungchae.koo@lge.com 2011/03/10 LAB1_FW }
	}

	/* Connect DP, DM to USB_DP, USB_DM */
	ret = muic_i2c_write_byte_local(SW_CONTROL, DP_USB | DM_USB);

	/* Turn on charger IC with TA mode */
	charging_ic_set_usb_mode();

	muic_mode = MUIC_AP_USB;
	printk(KERN_WARNING "[MUIC] muic_distinguish_vbus_accessory(): AP_USB\n");

	/* wake lock for usb connection */
	set_wakelock(1);

//	check_usb_reg(); /* 20111105, mschung@ubiquix.com, for stable USB connection. */

	return ret;
}

s32 muic_CP_UART_set(void){

	s32 ret;

/* LGE_CHANGE_START 2011-03-16 kenneth.kang@lge.com patch for Adb offline set and Mass Storage Driver detecting fail */    
	/* 20110113 ks.kwon@lge.com check muic driver init. state [START] */
	if(!muic_init_done){

		printk(KERN_WARNING "[MUIC] MUIC has not been initialized! Nothing will be done!!!.\n");
		return 0;
	}	
    /* 20110113 ks.kwon@lge.com check muic driver init. state [END] */
/* LGE_CHANGE_END 2011-03-16 kenneth.kang@lge.com */

	/* Connect CP UART signals to DP3T */
	//usif_switch_ctrl(USIF_DP3T);
	/* Connect CP UART to MUIC UART */
	//dp3t_switch_ctrl(DP3T_CP_UART);				// kibum.lee@lge.com

	if (muic_device == MAX14526) {
		/* ID_200, VLDO 2.6V, ADC is auto, Charge Pump.*/
		ret = muic_i2c_write_byte_local(CONTROL_1, MID_200 | MADC_EN | MCP_EN);
	}
	else {
		/* ID_200, VLDO 2.6V, SEMREN on. ADC is auto.*/
		ret = muic_i2c_write_byte_local(CONTROL_1, MID_200 | MSEMREN);
	}

	/* Connect DP, DM to UART_TX, UART_RX*/
	ret = muic_i2c_write_byte_local(SW_CONTROL, DP_UART | DM_UART);

	/* Turn on charger IC with FACTORY mode */
	charging_ic_set_factory_mode();

	muic_mode = MUIC_CP_UART;
	printk(KERN_WARNING "[MUIC] muic_distinguish_vbus_accessory(): CP_UART\n");

	/* wake lock for the factory mode */
/* LGE_CHANG_START 2011-03-30 kenneth.kang@lge.com wakelock on when AT command sequence by CP UART */
	set_wakelock(1);
/* LGE_CHANGE_END 2011-03-30 kenneth.kang@lge.com */

	return ret;
}

s32 muic_CP_USB_set(void){

	s32 ret;

/* LGE_CHANGE_START 2011-03-16 kenneth.kang@lge.com patch for Adb offline set and Mass Storage Driver detecting fail */    	
	/* 20110113 ks.kwon@lge.com check muic driver init. state [START] */
	if(!muic_init_done){

		printk(KERN_WARNING "[MUIC] MUIC has not been initialized! Nothing will be done!!!.\n");
		return 0;
	}
	/* 20110113 ks.kwon@lge.com check muic driver init. state [END] */
/* LGE_CHANGE_END 2011-03-16 kenneth.kang@lge.com */

// LGE_UPDATE_S 20110411 [jaejoong.kim@lge.com] force D+/D- open in CP side
	ret = muic_i2c_write_byte_local(SW_CONTROL, DP_OPEN | DM_OPEN);
	muic_udelay(100000);

	if(ret < 0)
		printk(KERN_WARNING "[MUIC] i2c write error for D+/D- open in muic_CP_USB_SET\n");
// LGE_UPDATE_E 20110411 [jaejoong.kim@lge.com] force D+/D- open in CP side

	/* Connect CP UART signals to AP */
   	/* CP <-> USIF_AP ?�결??CP ?�운로드 Fail 발생 */
   // usif_switch_ctrl(USIF_DP3T);
	/* Connect CP USB to MUIC UART */
	//dp3t_switch_ctrl(DP3T_CP_USB);

	if (muic_device == MAX14526) {
		/* ID_200, VLDO 2.6V, ADC is auto.
		 * USB 2.0 also needs the charge pump (CP_EN) on.*/
		ret = muic_i2c_write_byte_local(CONTROL_1, MID_200 | MADC_EN | MCP_EN);
	}
	else {
		if( MUIC_CP_USB != muic_mode ) {
			/* ID_200, VLDO 2.6V, SEMREN on. ADC is auto.
			 * USB 2.0 also needs the charge pump (CP_EN) on.*/
			ret = muic_i2c_write_byte_local(CONTROL_1, MID_200 | MSEMREN | MCP_EN);
		}
	}

	ret = muic_i2c_write_byte_local(SW_CONTROL, DP_USB | DM_USB);

	// S ntkyucheul.park@lge.com ; add for charging mode when CP USB is slected
	int check_battery_present();
	if(check_battery_present()==1)
		charging_ic_set_usb_mode();
	else
		charging_ic_set_factory_mode();
	// E ntkyucheul.park@lge.com ; add for charging mode when CP USB is slected

	muic_mode = MUIC_CP_USB;
	printk(KERN_ERR "[MUIC] muic_distinguish_vbus_accessory(): CP_USB\n");

// START sungchae.koo@lge.com 2011/03/03 LAB1_FW : WAKELOCK_CASE_AT_CP_USB : Ref from LU3000 {
	if ( reset_status == 2 /* RESET_GLOBAL_SW_RESET */ ) {
        // If muic path is CP_USB, Reset cause is SW_RESET then WakeLock AP
		// wake lock for the factory mode
		if(0==the_wlock.wake_lock_on){
			wake_lock(&the_wlock.wake_lock);
			the_wlock.wake_lock_on=1;
			printk(KERN_WARNING "[MUIC] ====^^==== wake_lock_on=1 (locked)\n");
		}
	}
    else {
		/* wake lock off for the factory mode */
		set_wakelock(1);
    }
// END sungchae.koo@lge.com 2011/03/03 LAB1_FW }

	return ret;
}


/*
 * Linux bug: If a constant value larger than 20000,
 * compiler issues a __bad_udelay error.
 */
void muic_udelay(u32 microsec)
{
	u32 microseconds;
	microseconds = microsec;
	udelay(microseconds);
}

/* Initialize MUIC, i.e., the CONTROL_1,2 and SW_CONTROL registers.
 * 1) Prepare to sense INT_STAT and STATUS bits.
 * 2) Open MUIC paths. -> To keep the path from uboot setting, remove this stage.
 */
void muic_initialize(TYPE_RESET reset){

	s32 ret;

	printk(KERN_WARNING "[MUIC] muic_initialize()\n");

	if (reset) {
		ret = muic_i2c_write_byte_local(SW_CONTROL, DP_OPEN | DM_OPEN); // 20110915, mschung@ubiquix.com, Debug USB recognition at reboot.

		if (muic_i2c_read_byte_local(DEVICE_ID, &muic_device) < 0) {
			printk(KERN_ERR "[MUIC] Device detection falied! Set default device.\n");
			muic_device = TS5USBA33402;
		}
		muic_device &= 0xf0;
		printk(KERN_INFO "[MUIC] Device : %s\n", (muic_device == MAX14526) ? "MAX14526" : "TSUSBA33402");
	}
	else {
		// Clear Default Switch Position (0x03=0x24)
		ret = muic_i2c_write_byte_local(SW_CONTROL, DP_OPEN | DM_OPEN);
	}

	if (muic_device == MAX14526) {
		ret = muic_i2c_write_byte_local(CONTROL_1, MID_200 | MADC_EN);
		ret = muic_i2c_write_byte_local(CONTROL_2, MINT_EN);
	}
	else {
		/* Reset MUIC - Disable all */
		if(reset){
			ret = muic_i2c_write_byte_local(CONTROL_1, 0x00);
			ret = muic_i2c_write_byte_local(CONTROL_2, MUSB_DET_DIS);// USB_DET_DIS is neg enabled
		}

		/* comments from the case of HUB
		* Initialize MUIC - Default setting.
		*
		* CONTROL_1:
		* 
		* 	ID_2P2 	= 0. Enable to distinguish MUIC_EARMIC from MUIC_TV_OUT_LOAD and MUIC_OTG.
		* 		     Enable for MUIC_EARMIC operation.
		*	ID_620 	= 0. Enable only to distinguish MUIC_TV_OUT_LOAD from MUIC_OTG.
		*	ID_200 	= 1.
		*	VLDO 	= 0. Enable to apply 2.3V for MUIC_EARMIC operation.
		*	SEMREN 	= 1.
		*	ADC_EN 	= 0. Because it is automatically enabled upon any change in ID resistor.
		*	CP_EN 	= 0. Enalbe for USB 2.0 (MUIC_AP_USB, MUIC_CP_USB, and MUIC_OTG).
		*		     Enable for Audio charge pump (MUIC_EARMIC, MUIC_TV_OUT_LOAD).
		* 
		* CONTROL_2: 
		*
		* 	INTPOL 	= 0.
		* 	INT_EN	= 1.
		* 	MIC_LP	= 0.
		* 	CP_AUD 	= 1. Disable for Audio operation (MUIC_EARMIC, MUIC_TV_OUT_LOAD).
		* 	CHG_TYP	= 1.
		* 	USB_DET_DIS = 0. Negative enabled.
		*
		* SW_CONTROL: 
		*
		* 	MIC_ON	= 0. Enable for MUIC_EARMIC and MUIC_TV_OUT_LOAD.
		* 	DP	= 111 (open).
		* 	DM	= 111 (open).
		*/
		ret = muic_i2c_write_byte_local(CONTROL_1, MID_200 | MSEMREN);
		ret = muic_i2c_write_byte_local(CONTROL_2, MCHG_TYP);

		/* comments from the case of HUB
		* The initialization time for the facility to set STATUS register's
		* DCPORT and CHPORT = 250msec since CHG_TYP on.
		* The initialization times for the facilities to set INT_STAT register bits
		* since CONTROL_1, 2 settings are much shorter (< 70msec).
		* The settle down time for INT_STAT and STATUS register bits
		* since an interrupt occurs = 70msec.
		* 
		* Thus,
		* we need to wait 250msec if we enable CHG_TYP from its inactive state.
		* And, we need to wait just 70msec for all other cases including
		* interrupt and CONTROL register settings.
		*/
		if(reset)
			msleep(250);
		else
			msleep(70);

		/* Enable interrupt (INT_EN = 1). Keep other bits the same */
		ret = muic_i2c_write_byte_local(CONTROL_2, MINT_EN | MCHG_TYP);
	}

	/* Default setting : CP_UART to DP3T Switch. */
	//usif_switch_ctrl(USIF_DP3T);		// kibum.lee@lge.com

	set_wakelock(0);
	return;
}

/* Distinguish charger type.
 * This function is called *ONLY IF* INT_STAT's CHGDET == 1, i.e., a charger is detected.
 *
 * Chargers becomes to use the default MUIC setting
 * because the detection always happens only after the MUIC_NONE mode
 * which leads the default MUIC setting.
 * Thus, we don't need to explictly set MUIC registers here.
 */
s32 muic_distinguish_charger(void){

	s32 ret = 0;

	/* Enable charger IC in TA mode */
	charging_ic_set_ta_mode();

	/* Connect AP UART to MUIC UART */
	//dp3t_switch_ctrl(DP3T_AP_UART);

	/* 
	 *	LGE_UPDATE 20110425 [jaejoong.kim@lge.com] change IDNO for detecting LG_TA
	 *
	 *	IDNO == 0x05(0101) 180 KOhm. LG TA : Standard usb cable
	 *	IDNO == 0x06(0110) 240 KOhm. LG TA : Docomo accessaary usb cable
	 *	IDNO == 0x0b(1011) OPEN. TA charger
	 *	There is no ADC value of MUIC table for 220KOhm
	 * 	so, we need to double check 180 KOhm and 220 KOhm
	 */
	if ((int_stat_val & MIDNO) == 0x05 || (int_stat_val & MIDNO) == 0x06 || (int_stat_val & MIDNO) == 0x0b) {
		muic_mode = MUIC_LG_TA;
		muic_set_mode(MUIC_LG_TA);		// kibum.lee@lge.com 20120502 MUIC re-work
		printk(KERN_WARNING "[MUIC] muic_distinguish_charger(): MUIC_LG_TA\n");
		set_wakelock(0);
		return ret;
	}

	if (muic_device == MAX14526) {
		/* Prepare for reading charger type */
		ret = muic_i2c_write_byte_local(CONTROL_2, MINT_EN | MCHG_TYP);
		/* LGE_UPDATE_S [daewung.kim@lge.com] 2010-12-01, interrupt clear in TA detection */
		msleep(70);
		ret = muic_i2c_read_byte_local(INT_STAT, &status_val);
		/* LGE_UPDATE_E [daewung.kim@lge.com] 2010-12-01, interrupt clear in TA detection */
	}

	/* Read STATUS */
	ret = muic_i2c_read_byte_local(STATUS, &status_val);
	if (ret < 0) {
		printk(KERN_INFO "[MUIC] STATUS reading failed\n");
		muic_mode = MUIC_UNKNOWN;
		muic_set_mode(MUIC_UNKNOWN);		// kibum.lee@lge.com 20120502 MUIC re-work
		key_was_pressed = 0; // from HUB 
		set_wakelock(0);
		return ret;
	}

	printk(KERN_INFO "[MUIC] STATUS = %x\n", status_val);

	/* DCPORT == 1. Dedicated Charger */
	if ((status_val & MDCPORT) != 0) {
		muic_mode = MUIC_NA_TA;
		muic_set_mode(MUIC_NA_TA);		// kibum.lee@lge.com 20120502 MUIC re-work
		printk(KERN_WARNING "[MUIC] muic_distinguish_charger(): MUIC_NA_TA\n");
	}
	/* CHPORT == 1. HCHH */
	else if ((status_val & MCHPORT) != 0) {
		muic_mode = MUIC_TA_1A;
		muic_set_mode(MUIC_TA_1A);		// kibum.lee@lge.com 20120502 MUIC re-work
		printk(KERN_WARNING "[MUIC] muic_distinguish_charger(): MUIC_TA_1A\n");
	}
	/* DCPORT == 0 && CHPORT == 0. Definitely INVALID_CHG */
	else {
		muic_mode = MUIC_INVALID_CHG;
		muic_set_mode(MUIC_INVALID_CHG);		// kibum.lee@lge.com 20120502 MUIC re-work
		printk(KERN_WARNING "[MUIC] muic_distinguish_charger(): MUIC_INVALID_CHG\n");
	}

	set_wakelock(0);
	return ret;
}

/* LGE_UPDATE_S [daewung.kim@lge.com] 2010-12-01, modify detection scheme */
static void muic_device_none_detect(void)
{
	u8 reg_value;
	u8 status_value;
	s32 ret;

    if ((key_col == 1) && (key_row == 1)) // Volume up
        key_was_pressed = 1;
    else if ((key_col == 1) && (key_row == 2)) // Volume down
        key_was_pressed = 2;

	printk(KERN_WARNING "[MUIC] Device_None_Detect int_stat_val = 0x%x\n",int_stat_val);

/* LGE_CHANGE_S [kenneth.kang@lge.com] 2010-12-14, CP retain mode */
#ifdef CP_RETAIN	
	if (is_cp_retained)
	{
		//muic_CP_USB_set();
		muic_set_mode(MUIC_CP_USB);
	}
	else 
/* LGE_CHANGE_E [kenneth.kang@lge.com] 2011-01-06, CP retain mode */
#elif defined(USB_RETAIN) /* 2012.05.15, mannsik.chung@lge.com, USB retain mode. */
	ret = muic_i2c_read_byte_local(INT_STAT, &status_value);
	if(ret < 0)
		printk("[MUIC] [%s-%d] muic_i2c_read_byte_local Fail [ret = %d]\n", __func__, __LINE__, ret);
	else
		printk("[MUIC] [%s-%d] INT_STAT Value is 0x%x / MVBUS bit is %d\n", __func__, __LINE__, 
																		status_value, ((status_value & MVBUS) >> 4));

	if (usb_retained == CP_RETAINED)
	{
		//muic_CP_USB_set();
		if((status_value & MVBUS)) {
			printk("[MUIC] [%s-%d] muic_set_mode(MUIC_CP_USB)\n", __func__, __LINE__);
			muic_set_mode(MUIC_CP_USB);
		}
	}
	else if (usb_retained == AP_RETAINED)
	{
		//muic_AP_USB_set();
		if((status_value & MVBUS)) {
			printk("[MUIC] [%s-%d] muic_set_mode(MUIC_AP_USB)\n", __func__, __LINE__);
			muic_set_mode(MUIC_AP_USB);
		}
	}
    else 
#endif
	// IDNO=0100? 130Kohm :: CP UART MODE
	if(((int_stat_val & MIDNO) == 0x04)
			// LGE_CHANGE_S [sungchul.park@lge.com] 2011-02-08 for 130K and 100K and 300K ?�??? CP UART�??�정 {
			|| ((int_stat_val & MIDNO) == 0x03)
			|| ((int_stat_val & MIDNO) == 0x07)
			// LGE_CHANGE_E [sungchul.park@lge.com] 2011-02-08 }
			|| (hidden_menu_switching == 7)) {

		//muic_CP_UART_set();
		muic_set_mode(MUIC_CP_UART);	// kibum.lee@lge.com
	}
	// IDNO=0010? 56Kohm  :: CP USB MODE
	else if (((int_stat_val & MIDNO ) == 0x02) || (hidden_menu_switching == 9)){
		if (key_was_pressed == 2)
			//muic_AP_UART_set();
			muic_set_mode(MUIC_AP_UART);	// kibum.lee@lge.com
		else
		{
			// START sungchae.koo@lge.com 2011/04/03 LAB1_FW : STOP_ENTERING_EARLYSUSPEND_STATE for factory test {
			if( usb_swiched_thru_factory_cmd ) {
				printk(KERN_INFO "[MUIC] AP_USB because usb_swiched_thru_factory_cmd==1\n");
				//muic_AP_USB_set();
                            muic_set_mode(MUIC_AP_USB);
			} 
			else 
			// END sungchae.koo@lge.com 2011/04/03 LAB1_FW }
			{
				//muic_CP_USB_set();
				muic_set_mode(MUIC_CP_USB);
			}
		}
	}
	// LGE_UPDATE_S [kenneth.kang@lge.com] 2010-12-12, for 910K factory download
	// IDNO=1010? 910Kohm :: CP USB MODE
#ifdef 	CABLE_DETECT_910K	
	else if ((int_stat_val & MIDNO ) == 0x0a) {
		//muic_CP_USB_set();
		muic_set_mode(MUIC_CP_USB);	// kibum.lee@lge.com
	}
	else if ((int_stat_val & MIDNO ) == 0x09) {
		//muic_CP_USB_set();
		muic_set_mode(MUIC_CP_USB);	// kibum.lee@lge.com
	}
#endif	
	// CHGDET=1?  :: HIGH CURRENT USB or TA?
#if 0 //[120712-start] hrgo@ubiquix.com when is high current input usb, for usb connected. 
	else if (int_stat_val & MCHGDET) {
		muic_distinguish_charger();
	}
#endif //[120712-end] hrgo@ubiquix.com when is high current input usb, for usb connected. 

	// VBUS=1?  :: TA or AP USB?
	else if (int_stat_val & MVBUS) {
		if (muic_device == MAX14526) {
			// COMP2 to H-Z / COMN1 to C1COMP (0x03=0x23)
			muic_i2c_write_byte_local(SW_CONTROL, 0x23);

			msleep(3);

			// Read STATUS_REG (0x05)
			muic_i2c_read_byte_local(STATUS, &reg_value);

			if (reg_value & 0x01 ) {
				// Dedicated Charger(TA) Detected
				// COMP2 to H-Z / COMN1 to H-Z (0x03=0x24)
				muic_i2c_write_byte_local(SW_CONTROL, DP_OPEN | DM_OPEN);

				charging_ic_set_ta_mode();

				muic_mode = MUIC_NA_TA;
				muic_set_mode(MUIC_NA_TA);		// kibum.lee@lge.com 20120502 MUIC re-work
				printk(KERN_WARNING "[MUIC] Charger detected\n");
			}
			else {
				// USB Detected
				if (key_was_pressed == 2)
					//muic_CP_USB_set();
					muic_set_mode(MUIC_CP_USB);	// kibum.lee@lge.com
				else
					//muic_AP_USB_set();
					muic_set_mode(MUIC_AP_USB);	// kibum.lee@lge.com
			}
		}
		else {
//[120712-start] hrgo@ubiquix.com when is high current input usb, for usb connected. 
			// Read STATUS_REG (0x05)
			muic_i2c_read_byte_local(STATUS, &reg_value);

			printk(KERN_WARNING "[MUIC] STATUS == 0x%x\n", reg_value);

			if (reg_value & 0x80 ) {
				// Dedicated Charger(TA) Detected
				muic_i2c_write_byte_local(SW_CONTROL, DP_OPEN | DM_OPEN);

				charging_ic_set_ta_mode();

				muic_mode = MUIC_NA_TA;
				muic_set_mode(MUIC_NA_TA);
				printk(KERN_WARNING "[MUIC] NA_TA Charger detected\n");
			}
			else if (reg_value & 0x40 ) {
				// Dedicated Charger(TA) Detected
				muic_i2c_write_byte_local(SW_CONTROL, DP_OPEN | DM_OPEN);

				charging_ic_set_ta_mode();

				muic_mode = MUIC_LG_TA;
				muic_set_mode(MUIC_LG_TA);
				printk(KERN_WARNING "[MUIC] LG_TA Charger detected\n");
			}
			else
			{
				// USB Detected
				if (key_was_pressed == 2)
						//muic_CP_USB_set();
						muic_set_mode(MUIC_CP_USB);	// kibum.lee@lge.com
				else {
						//muic_AP_USB_set();
						muic_set_mode(MUIC_AP_USB);	// kibum.lee@lge.com
				}
			}
//[120712-end] hrgo@ubiquix.com when is high current input usb, for usb connected. 
		}
	}
	else {
		// Accessory Not Supported
		muic_mode = MUIC_NONE;
	}

	key_was_pressed = 0;
}

s32 muic_device_detection(s32 upon_irq)
{
/* LGE_CHANGE_START 2011-03-16 kenneth.kang@lge.com patch for Adb offline set and Mass Storage Driver detecting fail */    
	/* 20110113 ks.kwon@lge.com check muic driver init. state [START] */
	if(!muic_init_done){

		printk(KERN_WARNING "[MUIC] MUIC has not been initialized! Nothing will be done!!!.\n");
		return 0;
	}	
    /* 20110113 ks.kwon@lge.com check muic driver init. state [END] */
/* LGE_CHANGE_END 2011-03-16 kenneth.kang@lge.com */
	// Read INT_STAT_REG (0x04)
	muic_i2c_read_byte_local(INT_STAT, &int_stat_val);
	printk(KERN_INFO "[MUIC] INT_STAT = %x\n", int_stat_val);

	switch (muic_mode)
	{
	case MUIC_NONE:
	case MUIC_UNKNOWN:
		muic_device_none_detect();
		break;

	// CP UART Mode
	case MUIC_CP_UART:
	case MUIC_AP_UART:
		android_USB_disconnect(); // for usb disconnect event

		if ((int_stat_val & MIDNO) == 0x0b) {
			muic_mode = MUIC_NONE;
		}
		else {
			muic_device_none_detect();
		}
		break;

	// TA Mode
	case MUIC_NA_TA:
	case MUIC_LG_TA:
	case MUIC_TA_1A:
	case MUIC_INVALID_CHG:
		if ((int_stat_val & MVBUS) == 0) {
			// Exit Charger Mode
			muic_mode = MUIC_NONE;
		}
		else {
			// Bug fix: charger detect interrupt 2 times
			set_wakelock(0);
		}
		break;

	// USB Mode
	case MUIC_AP_USB:
	case MUIC_CP_USB:
		android_USB_disconnect(); // for usb disconnect event

		if ((int_stat_val & MVBUS) == 0){
			// Exit USB Mode
			muic_mode = MUIC_NONE;
		}
		else {
			muic_device_none_detect();
		}
		break;

	default:
		muic_mode = MUIC_NONE;
		break;
	}

	if (muic_mode == MUIC_NONE) {
		muic_initialize(DEFAULT);
		printk(KERN_INFO "[MUIC] muic none\n");
		charging_ic_deactive();
		printk(KERN_INFO "[MUIC] charging_ic_deactive()\n");
		muic_set_mode(MUIC_NONE);		// kibum.lee@lge.com 20120502 MUIC re-work
	}

	charger_state_update_by_other();

	return 0;
}
EXPORT_SYMBOL(muic_device_detection);
/* LGE_UPDATE_E [daewung.kim@lge.com] 2010-12-01, modify detection scheme */

static void muic_wq_func(struct work_struct *muic_wq){
	s32 ret = 0;
	u32 retry_no;
	printk(KERN_INFO "[MUIC] muic_wq_func()\n");
	ret = muic_device_detection(UPON_IRQ);
}

static irqreturn_t muic_interrupt_handler(s32 irq, void *data){

	printk(KERN_WARNING "[MUIC] muic_interrupt_handler(): muic_mode = %s\n",
		name_muic_mode[(s32)muic_mode]);

printk("[MUIC] muic_interrupt_handler(): muic_mode = %s\n",
        name_muic_mode[(s32)muic_mode]);


	set_wakelock(1);
//[START] hrgo@ubiquix.com , 120727, When TA cable connected. not appear slow-charging Icon.
    cancel_delayed_work(&muic_wq);
//[END] hrgo@ubiquix.com , 120727, When TA cable connected. not appear slow-charging Icon.
	if (muic_device == TS5USBA33402)
		schedule_delayed_work(&muic_wq, msecs_to_jiffies(70));
	else
		schedule_delayed_work(&muic_wq, msecs_to_jiffies(250));//HZ / 10);

	return IRQ_HANDLED;
}

/* B-Prj Power off charging [kyungyoon.kim@lge.com] 2010-12-15 */
ssize_t muic_store_wake_lock(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf,
			  size_t count)
{
	int value = 0;

    	printk(KERN_INFO "[MUIC] muic_store_wake_lock \n");

	if (!count) {
		return -EINVAL;
	}

	value = simple_strtoul(buf, NULL, 10);
    	if (value >2 )
		return -EINVAL;

	printk(KERN_INFO "[MUIC]wake_lock=%d\n",value);
	set_wakelock(value);
	return count;
}

/* Shutdown issue at the case of USB booting [kyungyoon.kim@lge.com] 2010-12-25 */
ssize_t muic_store_charging_mode(struct device *dev, 
			  struct device_attribute *attr, 
			  const char *buf, 
			  size_t count)
{
	int value = 0;

    	printk(KERN_INFO "[MUIC] muic_store_charging_mode \n");
	
	if (!count) {
		return -EINVAL;
	}

	value = simple_strtoul(buf, NULL, 10);

    	if (value >2 )
		return -EINVAL;

	printk(KERN_INFO "[MUIC]charging_mode=%d\n",value);

	if (value==1)
		charging_ic_set_usb_mode_from_ta_mode();

	return count;
}

/* LGE_CHANGE_START 2011-03-16 kenneth.kang@lge.com patch for Adb offline set and Mass Storage Driver detecting fail */    
void vbus_irq_muic_handler(int state)
{
    if(muic_init_done){
        printk("[MUIC] %s vbus_state = %d \n",__func__,state);
        if (state == 2)
        {
            if (muic_mode == MUIC_NONE)
            {
//[START] hrgo@ubiquix.com , 120727, When TA cable connected. not appear slow-charging Icon.
//				schedule_work(&muic_wq);;
				schedule_delayed_work(&muic_wq, msecs_to_jiffies(1000));
//[END] hrgo@ubiquix.com , 120727, When TA cable connected. not appear slow-charging Icon.
			}
        }
        else
        {
//[START] hrgo@ubiquix.com , 120727, When TA cable connected. not appear slow-charging Icon.
            /*if (muic_mode != MUIC_NONE)*/
//				schedule_work(&muic_wq);;
				schedule_delayed_work(&muic_wq, msecs_to_jiffies(1000));
//[END] hrgo@ubiquix.com , 120727, When TA cable connected. not appear slow-charging Icon.
        }
    }
    printk("[MUIC] %s completed \n",__func__);
}
EXPORT_SYMBOL(vbus_irq_muic_handler);
/* LGE_CHANGE_END 2011-03-16 kenneth.kang@lge.com */

DEVICE_ATTR(charging_mode, 0644, NULL, muic_store_charging_mode);
/* Shutdown issue at the case of USB booting [kyungyoon.kim@lge.com] 2010-12-25 */

DEVICE_ATTR(wake_lock, 0644, NULL, muic_store_wake_lock);
/* B-Prj Power off charging [kyungyoon.kim@lge.com] 2010-12-15 */
/*
 * muic_probe() is called in the middle of booting sequence due to '__init'.
 * '__init' causes muic_probe() to be released after the booting.
 */

//kibum.lee@lge.com 20120502 MUIC re-work	start

int muic_set_ap_uart_mode(struct muic_client_device *mcdev)
{
	printk(KERN_INFO "[MUIC] hub_muic muic_set_ap_uart_mode() \n");
	return muic_AP_UART_set();
}


int muic_set_ap_usb_mode(struct muic_client_device *mcdev)
{
	printk(KERN_INFO "[MUIC] hub_muic muic_set_ap_usb_mode() \n");
	return muic_AP_USB_set();
}

int muic_set_cp_uart_mode(struct muic_client_device *mcdev)
{
	printk(KERN_INFO "[MUIC] hub_muic muic_set_cp_uart_mode() \n");
	return muic_CP_UART_set();

}

int muic_set_cp_usb_mode(struct muic_client_device *mcdev)
{
	printk(KERN_INFO "[MUIC] hub_muic muic_set_cp_usb_mode() \n");
	return muic_CP_USB_set();

}

static struct muic_device muic_dev = {
	.name = "hub_i2c_muic",
};

static struct muic_client_ops hub_muic_ops = {
	.on_ap_uart = muic_set_ap_uart_mode,
	.on_ap_usb 	= muic_set_ap_usb_mode,
	.on_cp_uart = muic_set_cp_uart_mode,
	.on_cp_usb 	= muic_set_cp_usb_mode,
};
//kibum.lee@lge.com 20120502 MUIC re-work	end

// kibum.lee@lge.com section mismatch error fix
//static s32 __init muic_probe(struct i2c_client *client, const struct i2c_device_id *id){
static s32 muic_probe(struct i2c_client *client, const struct i2c_device_id *id){

	s32 ret = 0;
	u32 retry_no = 0;
/* B-Prj Power off charging [kyungyoon.kim@lge.com] 2010-12-15 */
	int err = 0;
/* B-Prj Power off charging [kyungyoon.kim@lge.com] 2010-12-15 */

// kibum.lee@lge.com 20120502 MUIC re-work
	struct hub_muic_device *dev = NULL;
	struct muic_platform_data *pdata = client->dev.platform_data;

	dev_info(&client->dev, "muic: %s()\n", __func__);

	if (!pdata) {
		dev_err(&client->dev, "muic: %s: no platform data\n", __func__);
		return -EINVAL;
	}

	dev = kzalloc(sizeof(struct hub_muic_device), GFP_KERNEL);
	if (!dev) {
		dev_err(&client->dev, "muic: %s: no memory\n", __func__);
		return -ENOMEM;
	}

	dev->client = client;
// kibum.lee@lge.com 20120502 MUIC re-work

	muic_client = client;

	/* wake lock for usb connection */
	wake_lock_init(&the_wlock.wake_lock, WAKE_LOCK_SUSPEND, "usb_connection");
	the_wlock.wake_lock_on=0;

	/* GPIO initialization */
	omap_mux_init_gpio(MUIC_INT_GPIO, OMAP_PIN_INPUT_PULLUP | OMAP_PIN_OFF_WAKEUPENABLE);
	omap_mux_init_gpio(GPIO_DP3T_IN_1, OMAP_PIN_OUTPUT);
	omap_mux_init_gpio(GPIO_DP3T_IN_2, OMAP_PIN_OUTPUT);
	omap_mux_init_gpio(GPIO_USIF_IN_1, OMAP_PIN_OUTPUT);		// USIF_IN_1_GPIO -> GPIO_USIF_IN_1
	omap_mux_init_gpio(GPIO_IFX_USB_VBUS_EN, OMAP_PIN_OUTPUT);

//kibum.lee@lge.com 20120502 MUIC re-work	start
	ret = muic_device_register(&muic_dev, NULL);
	if (ret < 0) {
		dev_err(&client->dev, "muic: %s: muic_device_register failed\n",
				__func__);
		goto err_gpio_request;
	}

	i2c_set_clientdata(client, dev);
	
	printk("%s, registering ops\n", __func__);
	muic_client_dev_register(dev->client->name, dev, &hub_muic_ops);
//kibum.lee@lge.com 20120502 MUIC re-work end

	/* Initialize GPIO direction before use.
	 * Check if other driver already occupied it.
	 */
//kibum.lee@lge.com 20120502 MUIC re-work	start
	//if (gpio_request(GPIO_USIF_IN_1, "USIF switch control GPIO") < 0) {
	//	printk(KERN_INFO "[MUIC] GPIO %d USIF1_SW is already occupied by other driver!\n", GPIO_USIF_IN_1);
	//	/* We know board_hub.c:ifx_n721_configure_gpio() performs a gpio_request on this pin first.
	//	 * Because the prior gpio_request is also for the analog switch control, this is not a confliction.*/
	//	ret = -ENOSYS;
	//	goto err_gpio_request;
	//}

	//if (gpio_request(GPIO_DP3T_IN_1, "DP3T switch control 1 GPIO") < 0) {
	//	printk(KERN_INFO "[MUIC] GPIO %d GPIO_DP3T_IN_1 is already occupied by other driver!\n", GPIO_DP3T_IN_1);
	//	ret = -ENOSYS;
	//	goto err_gpio_request;
	//}

	//if (gpio_request(GPIO_DP3T_IN_2, "DP3T switch control 2 GPIO") < 0) {
	//	printk(KERN_INFO "[MUIC] GPIO %d GPIO_DP3T_IN_2 is already occupied by other driver!\n", GPIO_DP3T_IN_2);
	//	ret = -ENOSYS;
	//	goto err_gpio_request;
	//}

	//if (gpio_request(GPIO_IFX_USB_VBUS_EN, "IFX VBUS EN") < 0) {
	//	printk(KERN_INFO "[MUIC] GPIO %d GPIO_IFX_USB_VBUS_EN is already occupied by other driver!\n", GPIO_IFX_USB_VBUS_EN);
	//	ret = -ENOSYS;
	//	goto err_gpio_request;
	//}
//kibum.lee@lge.com 20120502 MUIC re-work end

	/* Initialize GPIO 40 (MUIC_INT_N).
	 * Check if other driver already occupied it.
	 */
	if (gpio_request(MUIC_INT_GPIO, "MUIC IRQ GPIO") < 0) {
		printk(KERN_INFO "[MUIC] GPIO %d MUIC_INT_N is already occupied by other driver!\n", MUIC_INT_GPIO);
		ret = -ENOSYS;
		goto err_gpio_request;
	}

	/* Initialize GPIO direction before use or IRQ setting */
	if (gpio_direction_input(MUIC_INT_GPIO) < 0) {
		printk(KERN_INFO "[MUIC] GPIO %d MUIC_INT_N direction initialization failed!\n", MUIC_INT_GPIO);
		ret = -ENOSYS;
		goto err_gpio_request;
	}

	/* Register MUIC work queue function */
	INIT_DELAYED_WORK(&muic_wq, muic_wq_func);

	/* Set up an IRQ line and enable the involved interrupt handler.
	 * From this point, a MUIC_INT_N can invoke muic_interrupt_handler().
	 * muic_interrupt_handler merely calls schedule_work() with muic_wq_func().
	 * muic_wq_func() actually performs the accessory detection.
	 */
	if (request_irq(gpio_to_irq(MUIC_INT_GPIO),
			  muic_interrupt_handler,
			  IRQF_TRIGGER_FALLING,
			  "muic_irq",
			  &client->dev) < 0) {
		printk(KERN_INFO "[MUIC] GPIO %d MUIC_INT_N IRQ line set up failed!\n", MUIC_INT_GPIO);
		free_irq(gpio_to_irq(MUIC_INT_GPIO), &client->dev);
		ret = -ENOSYS;
		goto err_gpio_request;		
	}

	/* Make the interrupt on MUIC INT wake up OMAP which is in suspend mode */
	if (enable_irq_wake(gpio_to_irq(MUIC_INT_GPIO)) < 0) {
		printk(KERN_INFO "[MUIC] GPIO %d MUIC_INT_N wake up source setting failed!\n", MUIC_INT_GPIO);
		disable_irq_wake(gpio_to_irq(MUIC_INT_GPIO));
		ret = -ENOSYS;
		goto err_gpio_request;
	}


	/* Prepare a human accessible method to control MUIC */
	create_hub_muic_proc_file();

/* B-Prj Power off charging [kyungyoon.kim@lge.com] 2010-12-15 */
	err = device_create_file(&client->dev, &dev_attr_wake_lock);
	err = device_create_file(&client->dev, &dev_attr_charging_mode);
/* B-Prj Power off charging [kyungyoon.kim@lge.com] 2010-12-15 */

	
	/* Initialize MUIC - Finally MUIC INT becomes enabled */

/* LGE_CHANGE_START 2011-03-16 kenneth.kang@lge.com patch for Adb offline set and Mass Storage Driver detecting fail */    
	 muic_init_done = 1; //20110113 ks.kwon@lge.com check muic driver init. state
/* LGE_CHANGE_END 2011-03-16 kenneth.kang@lge.com */
	muic_initialize(RESET);

	mdelay(70);
	ret = muic_device_detection(NOT_UPON_IRQ);

	/* If an erronous situation occurs, try again */
	while(ret < 0 && retry_no < 3){
		printk(KERN_INFO "[MUIC] muic_probe(): muic_device_detection() failed %d times\n", ++retry_no);
		ret = muic_device_detection(NOT_UPON_IRQ);
	}

    // LGE_CHANGE_S [sungchul.park@lge.com] 2011-02-08 to enable usb phy power {
//    twl4030_usb_irq_exported(); // LGE TI Prakash
    // LGE_CHANGE_E [sungchul.park@lge.com] 2011-02-08 }

	printk(KERN_INFO "[MUIC] muic_probe(): done!\n");

	return ret;
	
err_muic_device_register:
	free_irq(gpio_to_irq(MUIC_INT_GPIO), dev);
err_gpio_request:
	kfree(dev);

	return ret;
}

static s32 muic_remove(struct i2c_client *client){
	free_irq(gpio_to_irq(MUIC_INT_GPIO), &client->dev);
	gpio_free(MUIC_INT_GPIO);
	//gpio_free(GPIO_USIF_IN_1);
	//gpio_free(GPIO_DP3T_IN_1);
	//gpio_free(GPIO_DP3T_IN_2);
	//gpio_free(GPIO_IFX_USB_VBUS_EN);

	if (muic_device == MAX14526) {
		// CTRL2_REG(0x02=0x01)
		muic_i2c_write_byte_local(CONTROL_2, MUSB_DET_DIS);
		// CTRL1_REG(0x01=0x00)
		muic_i2c_write_byte_local(CONTROL_1, 0x00);
		// SW_CTRL_REG(0x03=0x24)
		muic_i2c_write_byte_local(SW_CONTROL, DP_OPEN | DM_OPEN);
	}

/* B-Prj Power off charging [kyungyoon.kim@lge.com] 2010-12-15 */
	device_remove_file(&client->dev, &dev_attr_wake_lock);
	device_remove_file(&client->dev, &dev_attr_charging_mode);
/* B-Prj Power off charging [kyungyoon.kim@lge.com] 2010-12-15 */

	i2c_set_clientdata(client, NULL);	// For what?
	remove_hub_muic_proc_file();

	/* wake lock for usb connection */
	wake_lock_destroy(&the_wlock.wake_lock);

	return 0;
}

static s32 muic_suspend(struct i2c_client *client, pm_message_t state){
	client->dev.power.power_state = state;
/* S[, 2012.08.18, mannsik.chung@lge.com, MUIC should not ignore disconnection of USB cable. */
#if 0
	cancel_delayed_work(&muic_wq);
#else
	flush_delayed_work(&muic_wq);
#endif
/* E], 2012.08.18, mannsik.chung@lge.com, MUIC should not ignore disconnection of USB cable. */
	printk(KERN_INFO "[MUIC] muic_suspend \n");
	
	return 0;
}

static s32 muic_resume(struct i2c_client *client){
	client->dev.power.power_state = PMSG_ON;
	printk(KERN_INFO "[MUIC] muic_resume \n");

	return 0;
}

static const struct i2c_device_id muic_ids[] = {
	{"hub_i2c_muic", 0},
	{/* end of list */},
};

/* Allow user space tools to figure out which devices this driver can control.
 * The first parameter should be 'i2c' for i2c client chip drivers.
 */
//MODULE_MUIC_TABLE(i2c, muic_ids);

static struct i2c_driver muic_driver = {
	.probe      = muic_probe,
	.remove	    = muic_remove,
	.suspend    = muic_suspend,
	.resume     = muic_resume,
	.id_table   = muic_ids,
	.driver     = {
	.name       = "hub_i2c_muic",
	.owner      = THIS_MODULE,
	},
};

static s32 __init muic_init(void){
	printk(KERN_WARNING "[MUIC] muic_init()\n");
	return i2c_add_driver(&muic_driver);
}

static void __exit muic_exit(void){
	i2c_del_driver(&muic_driver);
}

//--[[ LGE_UBIQUIX_MODIFIED_START : scchoi@ubiquix.com [2011.06.29] - Workaround - to support UART debug.
//#if 0
/* LGE_CHANGE_S [kenneth.kang@lge.com] 2010-12-14, CP retain mode */
#ifdef CP_RETAIN
static s32 __init muic_state(char *str)
{
	s32 muic_value = simple_strtol(str, NULL, 0);
	is_cp_retained = muic_value;
	printk(KERN_INFO "[MUIC] CP Retain : %d\n", muic_value);
	return 1;
}
__setup("muic_state=", muic_state);
#elif defined(USB_RETAIN) /* 2012.05.15, mannsik.chung@lge.com, USB retain mode. */
static s32 __init muic_state(char *str)
{
    s32 muic_value = simple_strtol(str, NULL, 0);
    usb_retained = muic_value;

	/* S[, 2012.05.17, mannsik.chung@lge.com, In retain mode, phone should not start charger animation. */
	{
		extern int reset_status;
		if (usb_retained)
			reset_status = 0; /* 0 == RESET_NORMAL */
	}
	/* E], 2012.05.17, mannsik.chung@lge.com, In retain mode, phone should not start charger animation. */

    printk(KERN_INFO "[MUIC] USB Retain as : %d\n", muic_value);
    return 1;
}
__setup("muic_state=", muic_state);

#endif
/* LGE_CHANGE_E [kenneth.kang@lge.com] 2010-12-14, CP retain mode */

/*
 MUIC must initialize more than TWL4030-usb module init. When boot with usb plug in,
 Omap3630 maybe miss a USB interrupt if twl4030-usb module's init faster than MUIC init.

 LGE_UPDATE  20110419 [jaejoong.kim@lge.com]
 */
// arch_initcall(muic_init);
// #else
module_init(muic_init);
// #endif
//--]]GE_UBIQUIX_MODIFIED_END scchoi@ubiquix.com [2011.06.29] - Workaround - to support UART debug.

module_exit(muic_exit);

MODULE_AUTHOR("LG Electronics");
MODULE_DESCRIPTION("Hub MUIC Driver");
MODULE_LICENSE("GPL");
