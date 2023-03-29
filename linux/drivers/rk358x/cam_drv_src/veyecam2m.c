// SPDX-License-Identifier: GPL-2.0
/*
 * veyecam2m driver
 */

#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/videodev2.h>
#include <linux/version.h>
#include <linux/rk-camera-module.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-image-sizes.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of_gpio.h>


#define DRIVER_VERSION			KERNEL_VERSION(0, 0x01, 0x01)
#define VEYECAM2M_MEDIA_BUS_FMT MEDIA_BUS_FMT_UYVY8_2X8 //MEDIA_BUS_FMT_YUYV8_2X8
//#define DEBUG_PRINTK
#ifndef DEBUG_PRINTK
#define debug_printk(s , ... )
#define VEYE_TRACE 
#else
#define debug_printk printk
#define VEYE_TRACE printk("%s %s %d \n",__FILE__,__FUNCTION__,__LINE__);
#endif

/* External clock frequency is 24.0M */
// we do not need it
#define VEYECAM2M_XCLK_FREQ		24000000

/* Pixel rate is fixed at 74.25M for all the modes */
#define VEYECAM2M_PIXEL_RATE		74250000
/*mipi clk is 297Mhz */ 
#define VEYECAM2M_DEFAULT_LINK_FREQ	297000000


#define VEYECAM2M_XCLR_MIN_DELAY_US	6000
#define VEYECAM2M_XCLR_DELAY_RANGE_US	1000

/* veyecam2m model register address */
#define VEYECAM2M_MODEL_ID_ADDR		0x0001
#define VEYECAM2M_DEVICE_ID 		0x06

#define SENSOR_TYPR_ADDR_L    0x20
#define SENSOR_TYPR_ADDR_H    0x21

#define BOARD_TYPR_ADDR    0x25

/* registers */
#define VEYECAM_STREAMING_ON    0x001D
#define VEYECAM_MODE_STANDBY		0x00
#define VEYECAM_MODE_STREAMING		0x01


static DEFINE_MUTEX(veyecam2m_power_mutex);

#define OF_CAMERA_PINCTRL_STATE_DEFAULT	"rockchip,camera_default"
#define OF_CAMERA_PINCTRL_STATE_SLEEP	"rockchip,camera_sleep"

#define veyecam2m_NAME			"veyecam2m"

#define veyecam2m_XVCLK_FREQ		24000000

static const s64 link_freq_menu_items[] = {
	VEYECAM2M_DEFAULT_LINK_FREQ,
};
static u32 clkout_enabled_index = 0;

static const char * const veyecam2m_supply_names[] = {
	"avdd",		/* Analog power */
	"dovdd",	/* Digital I/O power */
	"dvdd",		/* Digital core power */
};

#define veyecam2m_NUM_SUPPLIES ARRAY_SIZE(veyecam2m_supply_names)

struct veyecam2m_reg {
	u16 address;
	u8 val;
};

struct veyecam2m_reg_list {
	u32 num_of_regs;
	const struct veyecam2m_reg *regs;
};

/* Mode : resolution and related config&values */
struct veyecam2m_mode {
    u32 bus_fmt;
	/* Frame width */
	u32 width;
	/* Frame height */
	u32 height;
    /* max framerate */
	struct v4l2_fract max_fps;
	/* V-timing */
	//u32 vts_def;
	/* Default register values */
	struct veyecam2m_reg_list reg_list;
    u32 vc[PAD_MAX];
};

static const struct veyecam2m_reg mode_1920_1080_regs[] = {

};

/* Mode configs */
static const struct veyecam2m_mode supported_modes[] = {
	{
		/* 1080P 30fps  */
		.width = 1920,
		.height = 1080,
        .max_fps = {
				.numerator = 10000,
				.denominator = 300000,
		},
        .bus_fmt = VEYECAM2M_MEDIA_BUS_FMT,
		//.vts_def = veyecam2m_VTS_30FPS_1080P,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1920_1080_regs),
			.regs = mode_1920_1080_regs,
		},
        .vc[PAD0] = V4L2_MBUS_CSI2_CHANNEL_0,
	},
};

struct veyecam2m {
	struct i2c_client	*client;
	struct clk		*xvclk; //todo delete
	struct gpio_desc	*mipi_pwr_gpio;//todo delete
	struct gpio_desc	*reset_gpio; //todo delete
	struct gpio_desc	*pwdn_gpio;
	struct regulator_bulk_data supplies[veyecam2m_NUM_SUPPLIES];

	struct pinctrl		*pinctrl;
	struct pinctrl_state	*pins_default;
	struct pinctrl_state	*pins_sleep;

	struct v4l2_subdev	subdev;
	struct media_pad	pad;
    struct v4l2_mbus_framefmt fmt;
    
	struct v4l2_ctrl_handler ctrl_handler;
    struct v4l2_ctrl    *link_freq;
	struct v4l2_ctrl    *pixel_rate;
	struct v4l2_ctrl	*exposure;
	struct v4l2_ctrl	*anal_gain;
	struct v4l2_ctrl	*digi_gain;
	struct v4l2_ctrl	*hblank;
	struct v4l2_ctrl	*vblank;
	struct v4l2_ctrl	*test_pattern;//todo delete 
	struct mutex		mutex;
	bool			streaming;
	bool			power_on;
	
	bool			initial_status;  //Whether the isp has been initialized
	const struct veyecam2m_mode *cur_mode;
	u32			module_index;
	const char		*module_facing;
	const char		*module_name;
	const char		*len_name;
	u32 lane_data_num;
};

#define to_veyecam2m(sd) container_of(sd, struct veyecam2m, subdev)

static int veyecam2m_write_reg(struct veyecam2m *veyecam2m, u16 reg, u8 val)
{
	int ret;
	unsigned char data[3] = { reg >> 8, reg & 0xff, val};
	
    struct i2c_client *client = v4l2_get_subdevdata(&veyecam2m->subdev);
	ret = i2c_master_send(client, data, 3);
	/*
	 * Writing the wrong number of bytes also needs to be flagged as an
	 * error. Success needs to produce a 0 return code.
	 */
	if (ret == 3) {
		ret = 0;
	} else {
		dev_dbg(&client->dev, "%s: i2c write error, reg: %x\n",
				__func__, reg);
		if (ret >= 0)
			ret = -EINVAL;
	}

	return ret;
}

static int veyecam2m_read_reg(struct veyecam2m *veyecam2m, u16 reg, u8 *val)
{
	int ret;
	unsigned char data_w[2] = { reg >> 8, reg & 0xff };
	struct i2c_client *client = v4l2_get_subdevdata(&veyecam2m->subdev);

	ret = i2c_master_send(client, data_w, 2);
	/*
	 * A negative return code, or sending the wrong number of bytes, both
	 * count as an error.
	 */
	if (ret != 2) {
		dev_dbg(&client->dev, "%s: i2c write error, reg: %x\n",
			__func__, reg);
		if (ret >= 0)
			ret = -EINVAL;
		return ret;
	}

	ret = i2c_master_recv(client, val, 1);
	/*
	 * The only return value indicating success is 1. Anything else, even
	 * a non-negative value, indicates something went wrong.
	 */
	if (ret == 1) {
		ret = 0;
	} else {
		dev_dbg(&client->dev, "%s: i2c read error, reg: %x\n",
				__func__, reg);
		if (ret >= 0)
			ret = -EINVAL;
	}

	return ret;
}

/* Write a list of registers */
static int veyecam2m_write_regs(struct veyecam2m *veyecam2m,
			     const struct veyecam2m_reg *regs, u32 len)
{
	struct i2c_client *client = v4l2_get_subdevdata(&veyecam2m->subdev);
	unsigned int i;
	int ret;

	for (i = 0; i < len; i++) {
		ret = veyecam2m_write_reg(veyecam2m, regs[i].address, regs[i].val);
		if (ret) {
			dev_err_ratelimited(&client->dev,
					    "Failed to write reg 0x%4.4x. error = %d\n",
					    regs[i].address, ret);
			return ret;
		}
	}
	return 0;
}

static void veyecam2m_set_default_format(struct veyecam2m *veyecam2m)
{
	struct v4l2_mbus_framefmt *fmt;
    VEYE_TRACE
	fmt = &veyecam2m->fmt;
	fmt->code = supported_modes[0].bus_fmt;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
/*	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_MAP_QUANTIZATION_DEFAULT(true,
							  fmt->colorspace,
							  fmt->ycbcr_enc);
	fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);
*/
	fmt->width = supported_modes[0].width;
	fmt->height = supported_modes[0].height;
	fmt->field = V4L2_FIELD_NONE;
}


static int veyecam2m_set_pad_format(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct veyecam2m *veyecam2m = to_veyecam2m(sd);
    const struct veyecam2m_mode *new_mode;
    int ret = 0,mode,flag=0;
	const struct veyecam2m_reg_list *reg_list;
    VEYE_TRACE
	mutex_lock(&veyecam2m->mutex);
	
        for(mode=0;mode<ARRAY_SIZE(supported_modes);mode++) {
           if((fmt->format.width==supported_modes[mode].width)&&
                   (fmt->format.height==supported_modes[mode].height)){
                     new_mode = &supported_modes[mode];
                     flag=1;
                     break;
            }
         }
         if(flag==0){
           //debug_printk("veyecam2m_set_pad_format=======error");
           ret = -EINVAL;
           goto error;
         }

	fmt->format.code = new_mode->bus_fmt;
	fmt->format.width = new_mode->width;
	fmt->format.height = new_mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
    veyecam2m->cur_mode = new_mode;
	/* Apply default values of current mode */
	reg_list = &veyecam2m->cur_mode->reg_list;
	ret = veyecam2m_write_regs(veyecam2m, reg_list->regs, reg_list->num_of_regs);
	if (ret) {
		//dev_err(&client->dev, "%s failed to set mode\n", __func__);
		goto error;
	}

error:
	mutex_unlock(&veyecam2m->mutex);

	return ret;
}
static int __veyecam2m_get_pad_format(struct veyecam2m *veyecam2m,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_format *fmt)
{
    //struct veyecam2m *veyecam2m = to_veyecam2m(sd);
    const struct veyecam2m_mode *mode = veyecam2m->cur_mode;
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
        //debug_printk("__veyecam2m_get_pad_format====V4L2_SUBDEV_FORMAT_TRY===");
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
                fmt->format = *v4l2_subdev_get_try_format(&veyecam2m->subdev, cfg, fmt->pad);
                //debug_printk("=V4L2_SUBDEV_FORMAT_TRY==ok");
#else
               // debug_printk("=V4L2_SUBDEV_FORMAT_TRY==err");
                return -ENOTTY;
#endif
	} else {
		fmt->format.width = mode->width;
        fmt->format.height = mode->height;
        fmt->format.code = mode->bus_fmt;
		fmt->format.field = V4L2_FIELD_NONE;
	}
	return 0;
}
static int veyecam2m_get_pad_format(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *fmt)
{
	struct veyecam2m *veyecam2m = to_veyecam2m(sd);
	int ret;
    VEYE_TRACE
	mutex_lock(&veyecam2m->mutex);
	ret = __veyecam2m_get_pad_format(veyecam2m, cfg, fmt);
	mutex_unlock(&veyecam2m->mutex);
	return ret;
}

static int veyecam2m_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_mbus_code_enum *code)
{
    VEYE_TRACE
	if (code->index != 0)
		return -EINVAL;
	code->code = VEYECAM2M_MEDIA_BUS_FMT;//MEDIA_BUS_FMT_UYVY8_2X8;
	return 0;
}

static int veyecam2m_enum_frame_sizes(struct v4l2_subdev *sd,
				    struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_frame_size_enum *fse)
{
    VEYE_TRACE
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != VEYECAM2M_MEDIA_BUS_FMT/*MEDIA_BUS_FMT_UYVY8_2X8*/)
		return -EINVAL;

	fse->min_width  = supported_modes[fse->index].width;
	fse->max_width  = supported_modes[fse->index].width;
	fse->max_height = supported_modes[fse->index].height;
	fse->min_height = supported_modes[fse->index].height;
	return 0;
}

static int veyecam2m_g_frame_interval(struct v4l2_subdev *sd,
				    struct v4l2_subdev_frame_interval *fi)
{
	struct veyecam2m *veyecam2m = to_veyecam2m(sd);
	const struct veyecam2m_mode *mode = veyecam2m->cur_mode;
    VEYE_TRACE
	mutex_lock(&veyecam2m->mutex);
	fi->interval = mode->max_fps;
	mutex_unlock(&veyecam2m->mutex);

	return 0;
}

//#define veyecam2m_LANES 2
/*static int veyecam2m_g_mbus_config(struct v4l2_subdev *sd,
                                 struct v4l2_mbus_config *config)
{
        u32 val = 0;
		u32 veyecam2m_lanes = 0;
		struct veyecam2m *veyecam2m = to_veyecam2m(sd);
        VEYE_TRACE
        //debug_printk("veye data lan num %d",veyecam2m->lane_data_num);
		veyecam2m_lanes = veyecam2m->lane_data_num;
        val = 1 << (veyecam2m_lanes - 1) |
        V4L2_MBUS_CSI2_CHANNEL_0 |
        V4L2_MBUS_CSI2_NONCONTINUOUS_CLOCK ;//discontinues mode

        config->type = V4L2_MBUS_CSI2;
        config->flags = val;

        return 0;
}
*/
static void veyecam2m_get_module_inf(struct veyecam2m *veyecam2m,
				   struct rkmodule_inf *inf)
{
    VEYE_TRACE
	memset(inf, 0, sizeof(*inf));
	strlcpy(inf->base.sensor, veyecam2m_NAME, sizeof(inf->base.sensor));
	strlcpy(inf->base.module, veyecam2m->module_name,
		sizeof(inf->base.module));
	strlcpy(inf->base.lens, veyecam2m->len_name, sizeof(inf->base.lens));
}

static int veyecam2m_get_channel_info(struct veyecam2m *veyecam2m, struct rkmodule_channel_info *ch_info)
{
       if (ch_info->index < PAD0 || ch_info->index >= PAD_MAX)
               return -EINVAL;
       ch_info->vc = veyecam2m->cur_mode->vc[ch_info->index];
       ch_info->width = veyecam2m->cur_mode->width;
       ch_info->height = veyecam2m->cur_mode->height;
       ch_info->bus_fmt = veyecam2m->cur_mode->bus_fmt;
       return 0;
}

static long veyecam2m_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct veyecam2m *veyecam2m = to_veyecam2m(sd);
	long ret = 0;
    struct rkmodule_channel_info *ch_info;
    VEYE_TRACE
	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		veyecam2m_get_module_inf(veyecam2m, (struct rkmodule_inf *)arg);
		break;
    case RKMODULE_GET_CHANNEL_INFO:
		ch_info = (struct rkmodule_channel_info *)arg;
		ret = veyecam2m_get_channel_info(veyecam2m, ch_info);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long veyecam2m_compat_ioctl32(struct v4l2_subdev *sd,
				   unsigned int cmd, unsigned long arg)
{
	void __user *up = compat_ptr(arg);
	struct rkmodule_inf *inf;
	struct rkmodule_channel_info *ch_info;
	long ret;
    VEYE_TRACE
	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		inf = kzalloc(sizeof(*inf), GFP_KERNEL);
		if (!inf) {
			ret = -ENOMEM;
			return ret;
		}
		ret = veyecam2m_ioctl(sd, cmd, inf);
		if (!ret)
			ret = copy_to_user(up, inf, sizeof(*inf));
        if (ret)
            ret = -EFAULT;
		kfree(inf);
		break;
	/*case RKMODULE_AWB_CFG:
		cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
		if (!cfg) {
			ret = -ENOMEM;
			return ret;
		}
		ret = copy_from_user(cfg, up, sizeof(*cfg));
		if (!ret)
			ret = veyecam2m_ioctl(sd, cmd, cfg);
		kfree(cfg);
		break;*/
    case RKMODULE_GET_CHANNEL_INFO:
		ch_info = kzalloc(sizeof(*ch_info), GFP_KERNEL);
		if (!ch_info) {
			ret = -ENOMEM;
			return ret;
		}
		ret = veyecam2m_ioctl(sd, cmd, ch_info);
		if (!ret) {
			ret = copy_to_user(up, ch_info, sizeof(*ch_info));
			if (ret)
				ret = -EFAULT;
		}
		kfree(ch_info);
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}
#endif

static int veyecam2m_start_streaming(struct veyecam2m *veyecam2m)
{
	struct i2c_client *client = v4l2_get_subdevdata(&veyecam2m->subdev);
	const struct veyecam2m_reg_list *reg_list;
	int ret;
    VEYE_TRACE
	/* Apply default values of current mode */
	reg_list = &veyecam2m->cur_mode->reg_list;
	ret = veyecam2m_write_regs(veyecam2m, reg_list->regs, reg_list->num_of_regs);
	if (ret) {
		dev_err(&client->dev, "%s failed to set mode\n", __func__);
		return ret;
	}
	/* In case these controls are set before streaming */
	/*mutex_unlock(&veyecam2m->mutex);
	ret = v4l2_ctrl_handler_setup(&veyecam2m->ctrl_handler);
	mutex_lock(&veyecam2m->mutex);
	if (ret){
        debug_printk("veyecam2m_start_streaming=====v4l2_ctrl_handler_setup failed %d",ret);
		return ret;
    }*/
//    debug_printk("veyecam2m_start_streaming=====ok");
	/* set stream on register */
	return veyecam2m_write_reg(veyecam2m, VEYECAM_STREAMING_ON, VEYECAM_MODE_STREAMING);
}

static void veyecam2m_stop_streaming(struct veyecam2m *veyecam2m)
{
	struct i2c_client *client = v4l2_get_subdevdata(&veyecam2m->subdev);
	int ret;
    VEYE_TRACE
	/* set stream off register */
	ret = veyecam2m_write_reg(veyecam2m, VEYECAM_STREAMING_ON, VEYECAM_MODE_STANDBY);
	if (ret)
		dev_err(&client->dev, "%s failed to set stream\n", __func__);
}

static int veyecam2m_s_stream(struct v4l2_subdev *sd, int on)
{
	struct veyecam2m *veyecam2m = to_veyecam2m(sd);
	//struct i2c_client *client = veyecam2m->client;
	int ret = 0;
    VEYE_TRACE
    	/* export gpio */
	if (!IS_ERR(veyecam2m->reset_gpio))
		gpiod_export(veyecam2m->reset_gpio, false);
	if (!IS_ERR(veyecam2m->pwdn_gpio))
		gpiod_export(veyecam2m->pwdn_gpio, false);
    
	mutex_lock(&veyecam2m->mutex);
	on = !!on;
	if (on == veyecam2m->streaming){
		goto unlock_and_return;
	}
	if (on) {
		/*ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}*/

		ret = veyecam2m_start_streaming(veyecam2m);
		if (ret) {
			v4l2_err(sd, "start stream failed while write regs\n");
			//pm_runtime_put(&client->dev);
			goto unlock_and_return;
		}
	} else {
		veyecam2m_stop_streaming(veyecam2m);
		//pm_runtime_put(&client->dev);
	}

	veyecam2m->streaming = on;

unlock_and_return:
	mutex_unlock(&veyecam2m->mutex);

	return ret;
}

static int veyecam2m_read_model(struct veyecam2m *veyecam2m)
{
    struct i2c_client *client = v4l2_get_subdevdata(&veyecam2m->subdev);
	int ret;
    u8 snr_l;
    u8 snr_h;
    u8 board_no;
    ret = veyecam2m_read_reg(veyecam2m, SENSOR_TYPR_ADDR_L, &snr_l);
	if (ret) {
		dev_err(&client->dev, "probe failed \n");
		return -ENODEV;
	}
    ret = veyecam2m_read_reg(veyecam2m, SENSOR_TYPR_ADDR_H, &snr_h);
	if (ret) {
		dev_err(&client->dev, "probe failed \n");
		return -ENODEV;
	}
    ret = veyecam2m_read_reg(veyecam2m, BOARD_TYPR_ADDR, &board_no);
	if (ret) {
		dev_err(&client->dev, "probe failed \n");
		return -ENODEV;
	}
    if(snr_l == 0x03 && snr_h == 0x27){
        dev_err(&client->dev, "sensor is IMX327\n");
    }
    else if(snr_l == 0x04 && snr_h == 0x62){
        dev_err(&client->dev, "sensor is IMX462\n");
    }
    else if(snr_l == 0x03 && snr_h == 0x85){
        dev_err(&client->dev, "sensor is IMX385\n");
    }
     if(board_no == 0x4C){
        dev_err(&client->dev, "board type is ONE board\n");
    }else{
        dev_err(&client->dev, "board type is TWO board\n");
    }
    return 0;
}

/* Verify chip ID */
static int veyecam2m_identify_module(struct veyecam2m *veyecam2m)
{
	struct i2c_client *client = v4l2_get_subdevdata(&veyecam2m->subdev);
	int ret;
	//u32 val;
    int err;
    u8 device_id;
    VEYE_TRACE
	ret = veyecam2m_read_reg(veyecam2m, VEYECAM2M_MODEL_ID_ADDR, &device_id);
	if (ret) {
		dev_err(&client->dev, "probe failed \n");
		return -ENODEV;
	}
    if (device_id == VEYECAM2M_DEVICE_ID) 
    {
        err = 0;
        dev_err(&client->dev, " camera id is veyecam2m\n");
    }
    else
    {
        err = -ENODEV;
		dev_err(&client->dev, "%s: invalid sensor model id: %d\n",
			__func__, device_id);
    }
    veyecam2m_read_model(veyecam2m);
	return err;
}

static int __veyecam2m_power_on(struct veyecam2m *veyecam2m);
static void __veyecam2m_power_off(struct veyecam2m *veyecam2m);
static int veyecam2m_s_power(struct v4l2_subdev *sd, int on)
{
	struct veyecam2m *veyecam2m = to_veyecam2m(sd);
	struct i2c_client *client = veyecam2m->client;
	struct device *dev = &veyecam2m->client->dev;
	int ret = 0;
    VEYE_TRACE
	mutex_lock(&veyecam2m->mutex);

	/* If the power state is not modified - no work to do. */
	if (veyecam2m->power_on == !!on)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		ret = __veyecam2m_power_on(veyecam2m);
		if(ret){
			dev_err(dev, "veyecam2m power on failed\n");
		}
		veyecam2m->power_on = true;

		/* export gpio */
		if (!IS_ERR(veyecam2m->reset_gpio))
			gpiod_export(veyecam2m->reset_gpio, false);
		if (!IS_ERR(veyecam2m->pwdn_gpio))
			gpiod_export(veyecam2m->pwdn_gpio, false);
	} else {
		pm_runtime_put(&client->dev);
		__veyecam2m_power_off(veyecam2m);
		veyecam2m->power_on = false;
		/* unexport gpio */
		if (!IS_ERR(veyecam2m->reset_gpio))
			gpiod_unexport(veyecam2m->reset_gpio);
		if (!IS_ERR(veyecam2m->pwdn_gpio))
			gpiod_unexport(veyecam2m->pwdn_gpio);
	}

unlock_and_return:

	mutex_unlock(&veyecam2m->mutex);

	return ret;
}


static int __veyecam2m_power_on(struct veyecam2m *veyecam2m)
{
	int ret;
	struct device *dev = &veyecam2m->client->dev;
    VEYE_TRACE
	if (!IS_ERR_OR_NULL(veyecam2m->pins_default)) {
		ret = pinctrl_select_state(veyecam2m->pinctrl,
					   veyecam2m->pins_default);
		if (ret < 0)
			dev_err(dev, "could not set pins\n");
	}

	if (!IS_ERR(veyecam2m->reset_gpio))
		gpiod_set_value_cansleep(veyecam2m->reset_gpio, 0);


	if (!IS_ERR(veyecam2m->pwdn_gpio))
		gpiod_set_value_cansleep(veyecam2m->pwdn_gpio, 0);

	msleep(4);

	if (clkout_enabled_index){
		ret = clk_prepare_enable(veyecam2m->xvclk);
		if (ret < 0) {
			dev_err(dev, "Failed to enable xvclk\n");
			return ret;
		}
	}

	ret = regulator_bulk_enable(veyecam2m_NUM_SUPPLIES, veyecam2m->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators\n");
		goto disable_clk;
	}

	if (!IS_ERR(veyecam2m->mipi_pwr_gpio))
		gpiod_set_value_cansleep(veyecam2m->mipi_pwr_gpio, 1);

	usleep_range(500, 1000);
	if (!IS_ERR(veyecam2m->reset_gpio))
		gpiod_set_value_cansleep(veyecam2m->reset_gpio, 1);

	usleep_range(500, 1000);
	if (!IS_ERR(veyecam2m->pwdn_gpio))
		gpiod_set_value_cansleep(veyecam2m->pwdn_gpio, 1);

    usleep_range(500, 1000);
    //do not output data when power on , because the mipi rx is not ready.
    veyecam2m_stop_streaming(veyecam2m);
	return 0;

disable_clk:
	if (clkout_enabled_index)
		clk_disable_unprepare(veyecam2m->xvclk);

	return ret;
}

static void __veyecam2m_power_off(struct veyecam2m *veyecam2m)
{
	int ret;
	struct device *dev = &veyecam2m->client->dev;
	veyecam2m->initial_status = false;
    VEYE_TRACE
	if (!IS_ERR(veyecam2m->reset_gpio))
		gpiod_set_value_cansleep(veyecam2m->reset_gpio, 1);
	if (!IS_ERR(veyecam2m->pwdn_gpio))
		gpiod_set_value_cansleep(veyecam2m->pwdn_gpio,0);
	if (!IS_ERR(veyecam2m->mipi_pwr_gpio))
		gpiod_set_value_cansleep(veyecam2m->mipi_pwr_gpio,1);
	if (clkout_enabled_index)
		clk_disable_unprepare(veyecam2m->xvclk);
	if (!IS_ERR_OR_NULL(veyecam2m->pins_sleep)) {
		ret = pinctrl_select_state(veyecam2m->pinctrl,
					   veyecam2m->pins_sleep);
		if (ret < 0)
			dev_dbg(dev, "could not set pins\n");
	}
	regulator_bulk_disable(veyecam2m_NUM_SUPPLIES, veyecam2m->supplies);

}
static int veyecam2m_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct veyecam2m *veyecam2m = to_veyecam2m(sd);
    VEYE_TRACE
	if(veyecam2m->power_on == false)
		return __veyecam2m_power_on(veyecam2m);
	else
		printk("veyecam2m is power on, nothing to do\n");

	return 0;
}

static int veyecam2m_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct veyecam2m *veyecam2m = to_veyecam2m(sd);
    VEYE_TRACE
	if(veyecam2m->power_on == true){
		__veyecam2m_power_off(veyecam2m);
		veyecam2m->power_on = false;
	}

	return 0;
}

static int veyecam2m_enum_frame_interval(struct v4l2_subdev *sd,
				       struct v4l2_subdev_pad_config *cfg,
				       struct v4l2_subdev_frame_interval_enum *fie)
{
    VEYE_TRACE
	if (fie->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fie->code != VEYECAM2M_MEDIA_BUS_FMT)
		return -EINVAL;

	fie->width = supported_modes[fie->index].width;
	fie->height = supported_modes[fie->index].height;
	fie->interval = supported_modes[fie->index].max_fps;
	return 0;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static int veyecam2m_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct veyecam2m *veyecam2m = to_veyecam2m(sd);
	struct v4l2_mbus_framefmt *try_fmt =
				v4l2_subdev_get_try_format(sd, fh->pad, 0);
    VEYE_TRACE
	mutex_lock(&veyecam2m->mutex);
	/* Initialize try_fmt */
	try_fmt->width = supported_modes[0].width;
	try_fmt->height = supported_modes[0].height;
	try_fmt->code = supported_modes[0].bus_fmt;
	try_fmt->field = V4L2_FIELD_NONE;
    
	mutex_unlock(&veyecam2m->mutex);
	/* No crop or compose */

	return 0;
}
#endif

static const struct dev_pm_ops veyecam2m_pm_ops = {
	SET_RUNTIME_PM_OPS(veyecam2m_runtime_suspend,
			   veyecam2m_runtime_resume, NULL)
};

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static const struct v4l2_subdev_internal_ops veyecam2m_internal_ops = {
	.open = veyecam2m_open,
};
#endif

static const struct v4l2_subdev_core_ops veyecam2m_core_ops = {
        .log_status = v4l2_ctrl_subdev_log_status,
        .subscribe_event = v4l2_ctrl_subdev_subscribe_event,
        .unsubscribe_event = v4l2_event_subdev_unsubscribe,
        .s_power = veyecam2m_s_power,
        .ioctl = veyecam2m_ioctl,
#ifdef CONFIG_COMPAT
        .compat_ioctl32 = veyecam2m_compat_ioctl32,
#endif
};

static const struct v4l2_subdev_video_ops veyecam2m_video_ops = {
	.s_stream = veyecam2m_s_stream,
	.g_frame_interval = veyecam2m_g_frame_interval,
	//.g_mbus_config = veyecam2m_g_mbus_config,
};

static const struct v4l2_subdev_pad_ops veyecam2m_pad_ops = {
	.enum_mbus_code = veyecam2m_enum_mbus_code,
	.enum_frame_size = veyecam2m_enum_frame_sizes,
	.enum_frame_interval = veyecam2m_enum_frame_interval,
	.get_fmt = veyecam2m_get_pad_format,
	.set_fmt = veyecam2m_set_pad_format,
};

static const struct v4l2_subdev_ops veyecam2m_subdev_ops = {
	.core	= &veyecam2m_core_ops,
	.video	= &veyecam2m_video_ops,
	.pad	= &veyecam2m_pad_ops,
};
#if 0
static int veyecam2m_set_ctrl(struct v4l2_ctrl *ctrl)
{
    VEYE_TRACE
    return 0;
    #if 0
	struct veyecam2m *veyecam2m = container_of(ctrl->handler,
					     struct veyecam2m, ctrl_handler);
	struct i2c_client *client = veyecam2m->client;
	s64 max;

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/* Update max exposure while meeting expected vblanking */
		max = veyecam2m->cur_mode->height + ctrl->val - 4;
		__v4l2_ctrl_modify_range(veyecam2m->exposure,
					 veyecam2m->exposure->minimum, max,
					 veyecam2m->exposure->step,
					 veyecam2m->exposure->default_value);
		break;
	}
	if (pm_runtime_get(&client->dev) <= 0)
		return 0;

	pm_runtime_put(&client->dev);
	return 0;
    #endif
}

static const struct v4l2_ctrl_ops veyecam2m_ctrl_ops = {
	.s_ctrl = veyecam2m_set_ctrl,
};
#endif
static int veyecam2m_initialize_controls(struct veyecam2m *veyecam2m)
{
	const struct veyecam2m_mode *mode;
	struct v4l2_ctrl_handler *handler;
	//struct v4l2_ctrl *ctrl;
	int ret;
    VEYE_TRACE
	handler = &veyecam2m->ctrl_handler;
	mode = veyecam2m->cur_mode;
	ret = v4l2_ctrl_handler_init(handler, 2);
	if (ret)
		return ret;
	handler->lock = &veyecam2m->mutex;
    
    veyecam2m->link_freq = v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ,
				      ARRAY_SIZE(link_freq_menu_items) - 1, 0, link_freq_menu_items);

    if (veyecam2m->link_freq)
    veyecam2m->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* By default, PIXEL_RATE is read only */
	veyecam2m->pixel_rate = v4l2_ctrl_new_std(handler, NULL,
					       V4L2_CID_PIXEL_RATE,
					       VEYECAM2M_PIXEL_RATE,
					       VEYECAM2M_PIXEL_RATE, 1,
					       VEYECAM2M_PIXEL_RATE);

	if (handler->error) {
		ret = handler->error;
		dev_err(&veyecam2m->client->dev,
			"Failed to init controls(%d)\n", ret);
		goto err_free_handler;
	}

	veyecam2m->subdev.ctrl_handler = handler;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

static int veyecam2m_configure_regulators(struct veyecam2m *veyecam2m)
{
	unsigned int i;
    VEYE_TRACE
	for (i = 0; i < veyecam2m_NUM_SUPPLIES; i++)
		veyecam2m->supplies[i].supply = veyecam2m_supply_names[i];

	return devm_regulator_bulk_get(&veyecam2m->client->dev,
				       veyecam2m_NUM_SUPPLIES,
				       veyecam2m->supplies);
}

static void free_gpio(struct veyecam2m *veyecam2m)
{
	if (!IS_ERR(veyecam2m->pwdn_gpio))
		gpio_free(desc_to_gpio(veyecam2m->pwdn_gpio));
        if (!IS_ERR(veyecam2m->reset_gpio))
                gpio_free(desc_to_gpio(veyecam2m->reset_gpio));
        if (!IS_ERR(veyecam2m->mipi_pwr_gpio))
		gpio_free(desc_to_gpio(veyecam2m->mipi_pwr_gpio));
}

static int veyecam2m_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct device_node *endpoint_node = NULL;
	struct v4l2_fwnode_endpoint vep = {0};
	struct veyecam2m *veyecam2m;
	struct v4l2_subdev *sd;
	char facing[2];
	int ret;
	//u32 val = 0;

	dev_info(dev, "veye camera driver version: %02x.%02x.%02x",
		DRIVER_VERSION >> 16,
		(DRIVER_VERSION & 0xff00) >> 8,
		DRIVER_VERSION & 0x00ff);

	veyecam2m = devm_kzalloc(dev, sizeof(*veyecam2m), GFP_KERNEL);
	if (!veyecam2m)
		return -ENOMEM;

	ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
				   &veyecam2m->module_index);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
				       &veyecam2m->module_facing);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
				       &veyecam2m->module_name);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
				       &veyecam2m->len_name);
	if (ret) {
		dev_err(dev, "could not get module information!\n");
		return -EINVAL;
	}

	veyecam2m->client = client;
	veyecam2m->cur_mode = &supported_modes[0];

	if (clkout_enabled_index){
		veyecam2m->xvclk = devm_clk_get(dev, "xvclk");
		if (IS_ERR(veyecam2m->xvclk)) {
			dev_err(dev, "Failed to get xvclk\n");
			return -EINVAL;
		}
		ret = clk_set_rate(veyecam2m->xvclk, veyecam2m_XVCLK_FREQ);
		if (ret < 0) {
			dev_err(dev, "Failed to set xvclk rate (24MHz)\n");
			return ret;
		}
		if (clk_get_rate(veyecam2m->xvclk) != veyecam2m_XVCLK_FREQ)
			dev_warn(dev, "xvclk mismatched, modes are based on 24MHz\n");
	}

	veyecam2m->mipi_pwr_gpio = devm_gpiod_get(dev, "power", GPIOD_OUT_LOW);
	if (IS_ERR(veyecam2m->mipi_pwr_gpio))
		dev_warn(dev, "Failed to get power-gpios, maybe no use\n");

	veyecam2m->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(veyecam2m->reset_gpio)) {
	   dev_info(dev, "Failed to get reset-gpios, maybe no use\n");
	}

	veyecam2m->pwdn_gpio = devm_gpiod_get(dev, "pwdn", GPIOD_OUT_LOW);
	if (IS_ERR(veyecam2m->pwdn_gpio)) {
	  dev_info(dev, "Failed to get pwdn-gpios, maybe no use\n");
	}

	ret = veyecam2m_configure_regulators(veyecam2m);
	if (ret) {
		dev_err(dev, "Failed to get power regulators\n");
		return ret;
	}

	veyecam2m->pinctrl = devm_pinctrl_get(dev);
	if (!IS_ERR(veyecam2m->pinctrl)) {
		veyecam2m->pins_default =
			pinctrl_lookup_state(veyecam2m->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_DEFAULT);
		if (IS_ERR(veyecam2m->pins_default))
			dev_err(dev, "could not get default pinstate\n");

		veyecam2m->pins_sleep =
			pinctrl_lookup_state(veyecam2m->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_SLEEP);
		if (IS_ERR(veyecam2m->pins_sleep))
			dev_err(dev, "could not get sleep pinstate\n");
	}

	endpoint_node = of_find_node_by_name(node,"endpoint");
	if(endpoint_node != NULL){
		//printk("veyecam2m get endpoint node success\n");
		ret=v4l2_fwnode_endpoint_parse(&endpoint_node->fwnode, &vep);
		if(ret){
			dev_info(dev, "Failed to get veyecam2m endpoint data lanes, set a default value\n");
			veyecam2m->lane_data_num = 2;
		}else{
			dev_info(dev, "Success to get veyecam2m endpoint data lanes, dts uses %d lanes\n", vep.bus.mipi_csi2.num_data_lanes);
			veyecam2m->lane_data_num = vep.bus.mipi_csi2.num_data_lanes;
		}
	}else{
		dev_info(dev,"veyecam2m get endpoint node failed\n");
		return -ENOENT;
	}
	//dev_info(dev,"veyecam2m num data lanes is %d\n", veyecam2m->lane_data_num);

	mutex_init(&veyecam2m->mutex);

	sd = &veyecam2m->subdev;
	v4l2_i2c_subdev_init(sd, client, &veyecam2m_subdev_ops);
	ret = veyecam2m_initialize_controls(veyecam2m);
	if (ret)
		goto err_destroy_mutex;

	ret = __veyecam2m_power_on(veyecam2m);
	if (ret) {
		dev_err(dev, "__veyecam2m_power_on failed\n");
		goto err_power_off;
	}
    
    msleep(100);
    
    ret = veyecam2m_identify_module(veyecam2m);
	if (ret)
		goto err_power_off;
    
    //clk discontinues mode
    veyecam2m_write_reg(veyecam2m,0x000b, 0xfe);
    veyecam2m_stop_streaming(veyecam2m);
    
    /* Initialize default format */
	veyecam2m_set_default_format(veyecam2m);
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	sd->internal_ops = &veyecam2m_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
#endif
#if defined(CONFIG_MEDIA_CONTROLLER)
	veyecam2m->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, 1, &veyecam2m->pad);
	if (ret < 0)
		goto err_power_off;

#endif

	memset(facing, 0, sizeof(facing));
	if (strcmp(veyecam2m->module_facing, "back") == 0)
		facing[0] = 'b';
	else
		facing[0] = 'f';

	snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
		 veyecam2m->module_index, facing,
		 veyecam2m_NAME, dev_name(sd->dev));
	ret = v4l2_async_register_subdev_sensor_common(sd);
	if (ret) {
		dev_err(dev, "v4l2 async register subdev failed\n");
		goto err_clean_entity;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	return 0;

err_clean_entity:
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
err_power_off:
	__veyecam2m_power_off(veyecam2m);
	free_gpio(veyecam2m);
//err_free_handler:
	v4l2_ctrl_handler_free(&veyecam2m->ctrl_handler);
err_destroy_mutex:
	mutex_destroy(&veyecam2m->mutex);

	return ret;
}

static int veyecam2m_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct veyecam2m *veyecam2m = to_veyecam2m(sd);

	v4l2_async_unregister_subdev(sd);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
	v4l2_ctrl_handler_free(&veyecam2m->ctrl_handler);
	mutex_destroy(&veyecam2m->mutex);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		__veyecam2m_power_off(veyecam2m);
	pm_runtime_set_suspended(&client->dev);

	return 0;
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id veyecam2m_of_match[] = {
	{ .compatible = "veye,veyecam2m" },
	{},
};
MODULE_DEVICE_TABLE(of, veyecam2m_of_match);
#endif

static const struct i2c_device_id veyecam2m_match_id[] = {
	{ "veye,veyecam2m", 0 },
	{ },
};

static struct i2c_driver veyecam2m_i2c_driver = {
	.driver = {
		.name = "veyecam2m",
		.pm = &veyecam2m_pm_ops,
		.of_match_table = of_match_ptr(veyecam2m_of_match),
	},
	.probe		= &veyecam2m_probe,
	.remove		= &veyecam2m_remove,
	.id_table	= veyecam2m_match_id,
};

static int __init sensor_mod_init(void)
{
	return i2c_add_driver(&veyecam2m_i2c_driver);
}

static void __exit sensor_mod_exit(void)
{
	i2c_del_driver(&veyecam2m_i2c_driver);
}

device_initcall_sync(sensor_mod_init);
module_exit(sensor_mod_exit);

MODULE_AUTHOR("xumm <www.veye.cc>");
MODULE_DESCRIPTION("veyecam2m sensor v4l2 driver");
MODULE_LICENSE("GPL v2");

