/*
 *
 */ 
#include "veye_mvcam.h"
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

#define DRIVER_VERSION			KERNEL_VERSION(1, 0x01, 0x03) 


#define mvcam_NAME			"mvcam"
//reserved
/* Embedded metadata stream structure */
#define VEYE_MV_EMBEDDED_LINE_WIDTH 16384
#define VEYE_MV_NUM_EMBEDDED_LINES 1

//#define DEBUG_PRINTK

#ifndef DEBUG_PRINTK
static int debug = 0;
#define debug_printk(s , ... )
#define VEYE_TRACE 
#else
static int debug = 1;
#define debug_printk printk
#define VEYE_TRACE 
//#define VEYE_TRACE printk(KERN_INFO"%s %s %d \n",__FILE__,__FUNCTION__,__LINE__);
#endif

module_param(debug, int, 0644);

#define STARTUP_MIN_DELAY_US	500*1000//500ms
#define STARTUP_DELAY_RANGE_US	1000

struct reg_mv {
	u16 addr;
	u32 val;
};

struct mvcam_reg_list {
	unsigned int num_of_regs;
	const struct reg_mv *regs;
};

struct mvcam_format {
	u32 index;
	u32 mbus_code;//mbus format
	u32 data_type;//mv data format
};

struct mvcam_roi
{
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct mvcam_mode {
	u32 width;
	u32 height;
};

static const s64 link_freq_menu_items[] = {
	MVCAM_DEFAULT_LINK_FREQ,
};


struct mvcam {
	struct v4l2_subdev sd;
	struct media_pad pad;

    u32    model_id; 
	struct gpio_desc	*reset_gpio; //ADP-MV2 do not use this
	struct gpio_desc	*pwdn_gpio;

    struct i2c_client *client;
    //data format 
	struct mvcam_format *supported_formats;
	int num_supported_formats;
	int current_format_idx;
    u32 max_width;
    u32 max_height;
    u32 min_width;
    u32 min_height;
    struct v4l2_rect roi;//the same as roi
    //max fps @ current roi format
    u32 max_fps;
    u32 cur_fps;
    u32 h_flip;
    u32 v_flip;
    
	struct v4l2_ctrl_handler ctrl_handler;
    struct v4l2_ctrl *ctrls[MVCAM_MAX_CTRLS];
	/* V4L2 Controls */
    struct v4l2_ctrl *frmrate;
    
	/*
	 * Mutex for serialized access:
	 * Protect sensor module set pad format and start/stop streaming safely.
	 */
	struct mutex mutex;

	/* Streaming on/off */
	bool streaming;
    
    u32			    module_index;
	const char		*module_facing;
	const char		*module_name;
	const char		*len_name;
	u32 lane_data_num;
};


static inline struct mvcam *to_mvcam(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct mvcam, sd);
}

static int mvcam_readl_reg(struct i2c_client *client,
								   u16 addr, u32 *val)
{
    u16 buf = htons(addr);
    u32 data;
    struct i2c_msg msgs[2] = {
		{
			.addr = client->addr,
			.flags= 0,
			.len = 2,
			.buf = (u8 *)&buf,
		},
		{
			.addr = client->addr,
			.flags= I2C_M_RD,
			.len = 4,
			.buf = (u8 *)&data,
		},
	};

	if(i2c_transfer(client->adapter, msgs, 2) != 2){
		return -1;
	}

	*val = ntohl(data);

	return 0;
}

static int mvcam_writel_reg(struct i2c_client *client,
									u16 addr, u32 val)
{
	u8 data[6];
	struct i2c_msg msgs[2] = {
		{
			.addr = client->addr,
			.flags= 0,
			.len = 6,
			.buf = data,
		},
	};
    debug_printk("mvcam write 0x%x val 0x%x\n",addr,val);
	addr = htons(addr);
	val = htonl(val);
	memcpy(data, &addr, 2);
	memcpy(data + 2, &val, 4);    
	if(i2c_transfer(client->adapter, msgs, 1) != 1)
		return -1;

	return 0;
}

static int mvcam_read(struct i2c_client *client, u16 addr, u32 *value)
{
	int ret;
	int count = 0;
	while (count++ < I2C_READ_RETRY_COUNT) {
		ret = mvcam_readl_reg(client, addr, value);
		if(!ret) {
			//v4l2_dbg(1, debug, client, "%s: 0x%02x 0x%04x\n",
			//	__func__, addr, *value);
			return ret;
		}
	}
    
	v4l2_err(client, "%s: Reading register 0x%02x failed\n",
			 __func__, addr);
	return ret;
}

static int mvcam_write(struct i2c_client *client, u16 addr, u32 value)
{
	int ret;
	int count = 0;
	while (count++ < I2C_WRITE_RETRY_COUNT) {
		ret = mvcam_writel_reg(client, addr, value);
		if(!ret)
			return ret;
	}
	v4l2_err(client, "%s: Write 0x%04x to register 0x%02x failed\n",
			 __func__, value, addr);
	return ret;
}

/* Write a list of registers */
static int __maybe_unused  mvcam_write_regs(struct i2c_client *client,
			     const struct reg_mv *regs, u32 len)
{
	unsigned int i;
	int ret;

	for (i = 0; i < len; i++) {
		ret = mvcam_write(client, regs[i].addr,regs[i].val);
		if (ret) {
			dev_err_ratelimited(&client->dev,
					    "Failed to write reg 0x%4.4x. error = %d\n",
					    regs[i].addr, ret);

			return ret;
		}
	}
	return 0;
}

static u32 bit_count(u32 n)
{
    n = (n &0x55555555) + ((n >>1) &0x55555555) ;
    n = (n &0x33333333) + ((n >>2) &0x33333333) ;
    n = (n &0x0f0f0f0f) + ((n >>4) &0x0f0f0f0f) ;
    n = (n &0x00ff00ff) + ((n >>8) &0x00ff00ff) ;
    n = (n &0x0000ffff) + ((n >>16) &0x0000ffff) ;

    return n ;
}

static int mvcam_getroi(struct mvcam *mvcam)
{
  //  int ret;
    struct i2c_client *client = mvcam->client;
    mvcam_read(client, ROI_Offset_X,&mvcam->roi.left);
    mvcam_read(client, ROI_Offset_Y,&mvcam->roi.top);
    mvcam_read(client, ROI_Width,&mvcam->roi.width);
    mvcam_read(client, ROI_Height,&mvcam->roi.height);
    v4l2_dbg(1, debug, mvcam->client, "%s:get roi(%d,%d,%d,%d)\n",
			 __func__, mvcam->roi.left,mvcam->roi.top,mvcam->roi.width,mvcam->roi.height);
    return 0;
}

static int mvcam_setroi(struct mvcam *mvcam)
{
  //  int ret;
    u32 fps_reg;
    struct i2c_client *client = mvcam->client;
    v4l2_dbg(1, debug, mvcam->client, "%s:set roi(%d,%d,%d,%d)\n",
			 __func__, mvcam->roi.left,mvcam->roi.top,mvcam->roi.width,mvcam->roi.height);
    mvcam_write(client, ROI_Offset_X,mvcam->roi.left);
    msleep(1);
    mvcam_write(client, ROI_Offset_Y,mvcam->roi.top);
    msleep(1);
    mvcam_write(client, ROI_Width,mvcam->roi.width);
    msleep(1);
    mvcam_write(client, ROI_Height,mvcam->roi.height);
    msleep(8);
    //get sensor max framerate 
    mvcam_read(client, MaxFrame_Rate,&fps_reg);
    mvcam->max_fps = fps_reg/100;
    mvcam_read(client, Framerate,&fps_reg);
    mvcam->cur_fps = fps_reg/100;
    v4l2_ctrl_modify_range(mvcam->frmrate, 1, mvcam->max_fps, 1, mvcam->cur_fps);
    
//    dev_info(&client->dev,
//			 "max fps is %d,cur fps %d\n",
//			 mvcam->max_fps,mvcam->cur_fps);
    return 0;
}

static int mvcam_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	int ret;
    struct mvcam *mvcam = 
		container_of(ctrl->handler, struct mvcam, ctrl_handler);
    struct i2c_client *client = mvcam->client;
    
	switch (ctrl->id) {
	case V4L2_CID_VEYE_MV_TRIGGER_MODE:
        ret = mvcam_read(client, Trigger_Mode,&ctrl->val);
		break;
	case V4L2_CID_VEYE_MV_TRIGGER_SRC:
        ret = mvcam_read(client, Trigger_Source,&ctrl->val);
		break;
	case V4L2_CID_VEYE_MV_FRAME_RATE:
        ret = mvcam_read(client, Framerate,&ctrl->val);
        ctrl->val = ctrl->val/100;
        mvcam->cur_fps = ctrl->val;
		break;
	default:
		dev_info(&client->dev,
			 "mvcam_g_volatile_ctrl ctrl(id:0x%x,val:0x%x) is not handled\n",
			 ctrl->id, ctrl->val);
		ret = -EINVAL;
		break;
	}
    
    v4l2_dbg(1, debug, mvcam->client, "%s: cid = (0x%X), value = (%d).\n",
                     __func__, ctrl->id, ctrl->val);

	return ret;
}

static int mvcam_s_ctrl(struct v4l2_ctrl *ctrl)
{
	int ret;
	struct mvcam *mvcam = 
		container_of(ctrl->handler, struct mvcam, ctrl_handler);
    struct i2c_client *client = mvcam->client;
    
	v4l2_dbg(1, debug, mvcam->client, "%s: cid = (0x%X), value = (%d).\n",
			 __func__, ctrl->id, ctrl->val);
	
    switch (ctrl->id) {
	case V4L2_CID_VEYE_MV_TRIGGER_MODE:
        ret = mvcam_write(client, Trigger_Mode,ctrl->val);
		break;
	case V4L2_CID_VEYE_MV_TRIGGER_SRC:
        ret = mvcam_write(client, Trigger_Source,ctrl->val);
		break;
	case V4L2_CID_VEYE_MV_SOFT_TRGONE:
        ret = mvcam_write(client, Trigger_Software,1);
		break;
	case V4L2_CID_VEYE_MV_FRAME_RATE:
        ret = mvcam_write(client, Framerate,ctrl->val*100);
        mvcam->cur_fps = ctrl->val;
		break;
    case V4L2_CID_VEYE_MV_ROI_X:
        mvcam->roi.left = rounddown(ctrl->val, MV_CAM_ROI_W_ALIGN);
        v4l2_dbg(1, debug, mvcam->client, "set roi_x %d round to %d.\n",
			 ctrl->val, mvcam->roi.left);
        ret = 0;
		break;
    case V4L2_CID_VEYE_MV_ROI_Y:
        mvcam->roi.top = rounddown(ctrl->val, MV_CAM_ROI_H_ALIGN);
        v4l2_dbg(1, debug, mvcam->client, "set roi_y %d round to %d.\n",
			 ctrl->val, mvcam->roi.top);
        ret = 0;
		break;
	default:
		dev_info(&client->dev,
			 "mvcam_s_ctrl ctrl(id:0x%x,val:0x%x) is not handled\n",
			 ctrl->id, ctrl->val);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static const struct v4l2_ctrl_ops mvcam_ctrl_ops = {
    .g_volatile_ctrl = mvcam_g_volatile_ctrl,
	.s_ctrl = mvcam_s_ctrl,
};

static struct v4l2_ctrl_config mvcam_v4l2_ctrls[] = {
    //standard v4l2_ctrls
    {
		.ops = NULL,
		.id = V4L2_CID_LINK_FREQ,
		.name = NULL,//kernel will fill it
		.type = V4L2_CTRL_TYPE_MENU ,
        .def = 0,
		.min = 0,
        .max = ARRAY_SIZE(link_freq_menu_items) - 1,
        .step = 0,
        .qmenu_int = link_freq_menu_items,
		.flags = V4L2_CTRL_FLAG_READ_ONLY,
	},
	{
		.ops = &mvcam_ctrl_ops,
		.id = V4L2_CID_PIXEL_RATE,
		.name = NULL,//kernel will fill it
		.type = V4L2_CTRL_TYPE_INTEGER,
		.def = MV_CAM_PIXEL_RATE,
		.min = MV_CAM_PIXEL_RATE,
		.max = MV_CAM_PIXEL_RATE,
		.step = 1,
		.flags = V4L2_CTRL_FLAG_READ_ONLY,
	},
	//custom v4l2-ctrls
	{
		.ops = &mvcam_ctrl_ops,
		.id = V4L2_CID_VEYE_MV_TRIGGER_MODE,
		.name = "trigger_mode",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.def = Image_Continues,
		.min = 0,
		.max = Image_trigger_mode_num-1,
		.step = 1,
		.flags = V4L2_CTRL_FLAG_VOLATILE|V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
	},
	{
		.ops = &mvcam_ctrl_ops,
		.id = V4L2_CID_VEYE_MV_TRIGGER_SRC,
		.name = "trigger_src",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.def = Trg_Hard,
		.min = 0,
		.max = Trg_Hard_src_num-1,
		.step = 1,
		.flags = V4L2_CTRL_FLAG_VOLATILE|V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
	},
	{
		.ops = &mvcam_ctrl_ops,
		.id = V4L2_CID_VEYE_MV_SOFT_TRGONE,
		.name = "soft_trgone",
		.type = V4L2_CTRL_TYPE_BUTTON,
		.def = 0,
		.min = 0,
		.max = 0,
		.step = 0,
	},
	{
		.ops = &mvcam_ctrl_ops,
		.id = V4L2_CID_VEYE_MV_FRAME_RATE,
		.name = "frame_rate",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.def = MV_CAM_DEF_FPS,
		.min = 0,
		.max = MV_CAM_DEF_FPS,
		.step = 1,
		.flags = V4L2_CTRL_FLAG_VOLATILE|V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
	},
	{
		.ops = &mvcam_ctrl_ops,
		.id = V4L2_CID_VEYE_MV_ROI_X,
		.name = "roi_x",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.def = 0,
		.min = 0,
		.max = 0,//to read from camera
		.step = MV_CAM_ROI_W_ALIGN,
		.flags = 0,
	},
	{
		.ops = &mvcam_ctrl_ops,
		.id = V4L2_CID_VEYE_MV_ROI_Y,
		.name = "roi_y",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.def = 0,
		.min = 0,
		.max = 0,//to read from camera
		.step = MV_CAM_ROI_H_ALIGN,
		.flags = 0,
	},
};
//grab some ctrls while streaming
static void mvcam_v4l2_ctrl_grab(struct mvcam *mvcam,bool grabbed)
{
    int i = 0;
    for (i = 0; i < ARRAY_SIZE(mvcam_v4l2_ctrls); ++i) {
		switch(mvcam->ctrls[i]->id)
        {
            case V4L2_CID_VEYE_MV_TRIGGER_MODE:
            case V4L2_CID_VEYE_MV_TRIGGER_SRC:
            case V4L2_CID_VEYE_MV_FRAME_RATE:
            case V4L2_CID_VEYE_MV_ROI_X:
            case V4L2_CID_VEYE_MV_ROI_Y:
                v4l2_ctrl_grab(mvcam->ctrls[i], grabbed);
            break;

            default:
            break;
        }
	}
}

static void mvcam_v4l2_ctrl_init(struct mvcam *mvcam)
{
    int i = 0;
    u32 value = 0;
    struct i2c_client *client = mvcam->client;
    for (i = 0; i < ARRAY_SIZE(mvcam_v4l2_ctrls); ++i) {
		switch(mvcam_v4l2_ctrls[i].id)
        {
            case V4L2_CID_VEYE_MV_TRIGGER_MODE:
                mvcam_read(client, Trigger_Mode,&value);
                mvcam_v4l2_ctrls[i].def = value;
                v4l2_dbg(1, debug, mvcam->client, "%s:default trigger mode %d\n", __func__, value);
            break;
            case V4L2_CID_VEYE_MV_TRIGGER_SRC:
                mvcam_read(client, Trigger_Source,&value);
                mvcam_v4l2_ctrls[i].def = value;
                v4l2_dbg(1, debug, mvcam->client, "%s:default trigger source %d\n", __func__, value);
            break;
            case V4L2_CID_VEYE_MV_FRAME_RATE:
                mvcam_read(client, Framerate,&value);
                mvcam_v4l2_ctrls[i].def = value/100;
                mvcam_read(client, MaxFrame_Rate,&value);
                mvcam_v4l2_ctrls[i].max = value/100;
                v4l2_dbg(1, debug, mvcam->client, "%s:default framerate %lld , max fps %lld \n", __func__, \
                    mvcam_v4l2_ctrls[i].def,mvcam_v4l2_ctrls[i].max);
            break;
            case V4L2_CID_VEYE_MV_ROI_X:
                //mvcam_read(client, ROI_Offset_X,value);
                //mvcam_v4l2_ctrls[i].def = value;
                mvcam_v4l2_ctrls[i].max = mvcam->max_width - mvcam->min_width;
            break;
            case V4L2_CID_VEYE_MV_ROI_Y:
                //mvcam_read(client, ROI_Offset_Y,value);
                //mvcam_v4l2_ctrls[i].def = value;
                mvcam_v4l2_ctrls[i].max = mvcam->max_height - mvcam->min_height;
            break;
            default:
            break;
        }
	}
}

static int mvcam_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad_id,
				struct v4l2_mbus_config *config)
{
	u32 val = 0;
    struct mvcam *mvcam = to_mvcam(sd);
	val = 1 << (mvcam->lane_data_num - 1) |
	      V4L2_MBUS_CSI2_CHANNEL_0 |
	      V4L2_MBUS_CSI2_CONTINUOUS_CLOCK;
	config->type = V4L2_MBUS_CSI2_DPHY;
	config->flags = val;
    VEYE_TRACE
	return 0;
}

static int mvcam_csi2_enum_mbus_code(
			struct v4l2_subdev *sd,
			struct v4l2_subdev_pad_config *cfg,
			struct v4l2_subdev_mbus_code_enum *code)
{
	struct mvcam *mvcam = to_mvcam(sd);
	struct mvcam_format *supported_formats = mvcam->supported_formats;
	int num_supported_formats = mvcam->num_supported_formats;
    VEYE_TRACE
	if (code->index != 0)
			return -EINVAL;
        
    if (code->index >= num_supported_formats)
        return -EINVAL;
    code->code = supported_formats[code->index].mbus_code;
    v4l2_dbg(1, debug, sd, "%s: index = (%d) mbus code (%x)\n", __func__, code->index,code->code);

	return 0;
}

static int mvcam_csi2_enum_framesizes(
			struct v4l2_subdev *sd,
			struct v4l2_subdev_pad_config *cfg,
			struct v4l2_subdev_frame_size_enum *fse)
{
	struct mvcam *mvcam = to_mvcam(sd);
VEYE_TRACE
	v4l2_dbg(1, debug, sd, "%s: code = (0x%X), index = (%d)\n",
			 __func__, fse->code, fse->index);
    if (fse->index != 0)
        return -EINVAL;
    fse->min_width = fse->max_width =
        mvcam->roi.width;
    fse->min_height = fse->max_height =
        mvcam->roi.height;
    return 0;
}

static int mvcam_g_frame_interval(struct v4l2_subdev *sd,
				    struct v4l2_subdev_frame_interval *fi)
{
    /* max framerate */
	struct v4l2_fract fract_fps;
	struct mvcam *mvcam = to_mvcam(sd);
    VEYE_TRACE
	mutex_lock(&mvcam->mutex);
    fract_fps.numerator = 100;
    fract_fps.denominator = mvcam->cur_fps*100;
	fi->interval = fract_fps;
	mutex_unlock(&mvcam->mutex);

	return 0;
}

static int mvcam_s_frame_interval(struct v4l2_subdev *sd,
				    struct v4l2_subdev_frame_interval *fi)
{
    
	struct mvcam *mvcam = to_mvcam(sd);
    VEYE_TRACE
    if(fi->interval.numerator == 0)
        return -EINVAL;
    
    v4l2_dbg(1, debug, sd, "%s: numerator %d, denominator %d\n",
    			__func__, fi->interval.numerator,fi->interval.denominator);

	mutex_lock(&mvcam->mutex);
    mvcam->cur_fps = fi->interval.denominator/fi->interval.numerator;
    mvcam_write(mvcam->client, Framerate,mvcam->cur_fps*100);
	mutex_unlock(&mvcam->mutex);

	return 0;
}


static int mvcam_csi2_get_fmt(struct v4l2_subdev *sd,
								struct v4l2_subdev_pad_config *cfg,
								struct v4l2_subdev_format *format)
{
	struct mvcam *mvcam = to_mvcam(sd);
	struct mvcam_format *current_format;
VEYE_TRACE
	mutex_lock(&mvcam->mutex);
    if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
        #ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
            format->format = *v4l2_subdev_get_try_format(&mvcam->sd, cfg, format->pad);
        #else
               debug_printk("=V4L2_SUBDEV_FORMAT_TRY==err");
                mutex_unlock(&mvcam->mutex);
                return -ENOTTY;
        #endif
        } else {
    		current_format = &mvcam->supported_formats[mvcam->current_format_idx];
    		format->format.width = mvcam->roi.width;
    		format->format.height = mvcam->roi.height;
            
    		format->format.code = current_format->mbus_code;
    		format->format.field = V4L2_FIELD_NONE;
            //for uyvy gstreamer 
    		//format->format.colorspace = V4L2_COLORSPACE_SRGB;//V4L2_COLORSPACE_REC709;
/*            format->format.ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(format->format.colorspace);
        	format->format.quantization = V4L2_MAP_QUANTIZATION_DEFAULT(true,
        							  format->format.colorspace,
        							  format->format.ycbcr_enc);
        	format->format.xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(format->format.colorspace);
*/            
    		v4l2_dbg(1, debug, sd, "%s: width: (%d) height: (%d) code: (0x%X)\n",
    			__func__, format->format.width,format->format.height,
    				format->format.code);
    	} 

	mutex_unlock(&mvcam->mutex);
	return 0;
}

static int mvcam_csi2_get_fmt_idx_by_code(struct mvcam *mvcam,
											u32 mbus_code)
{
	int i;
	struct mvcam_format *formats = mvcam->supported_formats;
	for (i = 0; i < mvcam->num_supported_formats; i++) {
		if (formats[i].mbus_code == mbus_code)
			return i; 
	}
	return -EINVAL;
}
/*
static int mvcam_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_selection *sel)
{
	struct mvcam *mvcam = to_mvcam(sd);
VEYE_TRACE
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP: {
    		//sel->r = *__mvcam_get_pad_crop(mvcam, cfg, sel->pad,
    		//				sel->which);
            sel->r = mvcam->roi;
    		break;
    	}

    //active area
        case V4L2_SEL_TGT_CROP_DEFAULT:
        case V4L2_SEL_TGT_NATIVE_SIZE:
        case V4L2_SEL_TGT_CROP_BOUNDS:
            sel->r.top = 0;
            sel->r.left = 0;
            sel->r.width = mvcam->max_width;
            sel->r.height = mvcam->max_height;
		break;
        default:
		return -EINVAL;
	}
    sel->flags = V4L2_SEL_FLAG_LE;
    v4l2_dbg(1, debug, sd, "%s: target %d\n", __func__,V4L2_SEL_TGT_CROP);
    return 0;
}*/
static int mvcam_set_selection(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_selection *sel)
{
   //     struct i2c_client *client = v4l2_get_subdevdata(sd);
        struct mvcam *mvcam = to_mvcam(sd);
    VEYE_TRACE
        switch (sel->target) {
        case V4L2_SEL_TGT_CROP:
            mvcam->roi.left  = clamp(rounddown(sel->r.left, MV_CAM_ROI_W_ALIGN), 0U, (mvcam->max_width-mvcam->min_width));
            mvcam->roi.top  = clamp(rounddown(sel->r.top, MV_CAM_ROI_H_ALIGN), 0U, (mvcam->max_height-mvcam->min_height));
            mvcam->roi.width = clamp(rounddown(sel->r.width, MV_CAM_ROI_W_ALIGN), mvcam->min_width, mvcam->max_width);
            mvcam->roi.height = clamp(rounddown(sel->r.height, MV_CAM_ROI_H_ALIGN), mvcam->min_height, mvcam->max_height);
            mvcam_setroi(mvcam);
            break;
        default:
            return -EINVAL;
        }
        v4l2_dbg(1, debug, sd, "%s: target %d\n", __func__,V4L2_SEL_TGT_CROP);
        return 0;
}
static int mvcam_frm_supported(int roi_x,int wmin, int wmax, int ws,
				int roi_y,int hmin, int hmax, int hs,
				int w, int h)
{
	if (
		(roi_x+w) > wmax || w < wmin ||
		(roi_y+h) > hmax || h < hmin ||
		(h) % hs != 0 ||
		(w) % ws != 0
	)
		return -EINVAL;

	return 0;
}

static int mvcam_csi2_try_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_format *format)
{
	struct mvcam *mvcam = to_mvcam(sd);
	int ret = 0;
VEYE_TRACE
	ret = mvcam_frm_supported(
			mvcam->roi.left,mvcam->min_width, mvcam->max_width, MV_CAM_ROI_W_ALIGN,
			mvcam->roi.top,mvcam->min_height, mvcam->max_height, MV_CAM_ROI_H_ALIGN,
			format->format.width, format->format.height);

	if (ret < 0) {
		v4l2_err(sd, "Not supported size!\n");
		return ret;
	}

	return 0;
}

static int mvcam_csi2_set_fmt(struct v4l2_subdev *sd,
								struct v4l2_subdev_pad_config *cfg,
								struct v4l2_subdev_format *format)
{
	int i;
	struct mvcam *mvcam = to_mvcam(sd);
    //struct v4l2_mbus_framefmt *framefmt;
    struct v4l2_subdev_selection sel;
    
    /*if ((format->format.width != mvcam->roi.width ||
     format->format.height != mvcam->roi.height))
    {
        v4l2_info(sd, "Changing the resolution is not supported with VIDIOC_S_FMT! \n Pls use VIDIOC_S_SELECTION.\n");
        v4l2_info(sd,"%d,%d,%d,%d\n",format->format.width,mvcam->roi.width,format->format.height,mvcam->roi.height);
        return -EINVAL;
    }*/

VEYE_TRACE
    //format->format.colorspace =  V4L2_COLORSPACE_SRGB;
    format->format.field = V4L2_FIELD_NONE;

    v4l2_dbg(1, debug, sd, "%s: code: 0x%X",
            __func__, format->format.code);
    if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
                //framefmt = v4l2_subdev_get_try_format(sd, cfg,
                //                      format->pad);
               // *framefmt = format->format;
                return mvcam_csi2_try_fmt(sd, cfg, format);
     } else {
        i = mvcam_csi2_get_fmt_idx_by_code(mvcam, format->format.code);
        if (i < 0)
            return -EINVAL;
        mvcam->current_format_idx = i;
        mvcam_write(mvcam->client,Pixel_Format,mvcam->supported_formats[i].data_type);

        mvcam->roi.width = format->format.width;
        mvcam->roi.height = format->format.height;
        sel.target = V4L2_SEL_TGT_CROP;
        sel.r = mvcam->roi;
        mvcam_set_selection(sd, NULL, &sel);

        //format->format.width = mvcam->roi.width;
    }
    //update_controls(mvcam);
	return 0;
}



static void mvcam_get_module_inf(struct mvcam *mvcam,
				   struct rkmodule_inf *inf)
{
    VEYE_TRACE
	memset(inf, 0, sizeof(*inf));
	strlcpy(inf->base.sensor, mvcam_NAME, sizeof(inf->base.sensor));
	strlcpy(inf->base.module, mvcam->module_name,
		sizeof(inf->base.module));
	strlcpy(inf->base.lens, mvcam->len_name, sizeof(inf->base.lens));
}

static int mvcam_get_channel_info(struct mvcam *mvcam, struct rkmodule_channel_info *ch_info)
{
        struct mvcam_format *current_format;
       if (ch_info->index < PAD0 || ch_info->index >= PAD_MAX)
               return -EINVAL;
       VEYE_TRACE
       ch_info->vc = V4L2_MBUS_CSI2_CHANNEL_0;
       ch_info->width = mvcam->roi.width;
       ch_info->height = mvcam->roi.height;
       current_format = &mvcam->supported_formats[mvcam->current_format_idx];
       ch_info->bus_fmt = current_format->mbus_code;
       return 0;
}

static long mvcam_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct mvcam *mvcam = to_mvcam(sd);
	long ret = 0;
    struct rkmodule_channel_info *ch_info;
    VEYE_TRACE
	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		mvcam_get_module_inf(mvcam, (struct rkmodule_inf *)arg);
		break;
    case RKMODULE_GET_CHANNEL_INFO:
		ch_info = (struct rkmodule_channel_info *)arg;
		ret = mvcam_get_channel_info(mvcam, ch_info);
		break;
    case RKMODULE_GET_CSI_DSI_INFO:
		*(int *)arg = RKMODULE_CSI_INPUT;
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long mvcam_compat_ioctl32(struct v4l2_subdev *sd,
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
		ret = mvcam_ioctl(sd, cmd, inf);
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
			ret = mvcam_ioctl(sd, cmd, cfg);
		kfree(cfg);
		break;*/
    case RKMODULE_GET_CHANNEL_INFO:
		ch_info = kzalloc(sizeof(*ch_info), GFP_KERNEL);
		if (!ch_info) {
			ret = -ENOMEM;
			return ret;
		}
		ret = mvcam_ioctl(sd, cmd, ch_info);
		if (!ret) {
			ret = copy_to_user(up, ch_info, sizeof(*ch_info));
			if (ret)
				ret = -EFAULT;
		}
		kfree(ch_info);
        break;
    case RKMODULE_GET_CSI_DSI_INFO:
		*(int *)arg = RKMODULE_CSI_INPUT;
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}
#endif



static void mvcam_free_controls(struct mvcam *mvcam)
{
    VEYE_TRACE
	v4l2_ctrl_handler_free(mvcam->sd.ctrl_handler);
	//mutex_destroy(&mvcam->mutex);
}

static u32 mvdatatype_to_mbus_code(int data_type)
{
    VEYE_TRACE
   // debug_printk("%s: data type %d\n",
	//				__func__, data_type);
    switch(data_type) {
	case MV_DT_Mono8:
        return MEDIA_BUS_FMT_Y8_1X8;
	case MV_DT_Mono10:
		return MEDIA_BUS_FMT_Y10_1X10;
	case MV_DT_Mono12:
		return MEDIA_BUS_FMT_Y12_1X12;
    case MV_DT_Mono14:
		return MEDIA_BUS_FMT_Y14_1X14;
	case MV_DT_UYVY:
		return MEDIA_BUS_FMT_UYVY8_2X8;
	}
	return 0;
}

int get_fmt_index(struct mvcam *mvcam,u32 datatype)
{
    int i = 0;
    for(;i < mvcam->num_supported_formats;++i)
    {
        if((mvcam->supported_formats[i].data_type) == datatype)
            return i;
    }
    return -1;
}

static int mvcam_enum_pixformat(struct mvcam *mvcam)
{
	int ret = 0;
	u32 mbus_code = 0;
	int pixformat_type;
	int index = 0;
    int bitindex = 0;
	int num_pixformat = 0;
    u32 fmtcap = 0;
    u32 cur_fmt;
	struct i2c_client *client = mvcam->client;
VEYE_TRACE
    ret = mvcam_read(client, Format_cap, &fmtcap);
    if (ret < 0)
		goto err;
	num_pixformat = bit_count(fmtcap);
	if (num_pixformat < 0)
		goto err;
    
    v4l2_dbg(1, debug, mvcam->client, "%s: format count: %d; format cap 0x%x\n",
					__func__, num_pixformat,fmtcap);
    
	mvcam->supported_formats = devm_kzalloc(&client->dev,
		sizeof(*(mvcam->supported_formats)) * (num_pixformat+1), GFP_KERNEL);
	while(fmtcap){
        if(fmtcap&1){
            //which bit is set?
            pixformat_type = bitindex;
            fmtcap >>= 1;
            bitindex++;
        }
        else{
            fmtcap >>= 1;
            bitindex++;
            continue;
        }
        mbus_code = mvdatatype_to_mbus_code(pixformat_type);
		mvcam->supported_formats[index].index = index;
		mvcam->supported_formats[index].mbus_code = mbus_code;
		mvcam->supported_formats[index].data_type = pixformat_type;
        v4l2_dbg(1, debug, mvcam->client, "%s support format index %d mbuscode %d datatype: %d\n",
					__func__, index,mbus_code,pixformat_type);
        index++;
	}
	mvcam->num_supported_formats = num_pixformat;

    mvcam_read(client, Pixel_Format, &cur_fmt);
	mvcam->current_format_idx = get_fmt_index(mvcam,cur_fmt);
    v4l2_dbg(1, debug, mvcam->client, "%s: cur format: %d\n",
					__func__, cur_fmt);
	// mvcam_add_extension_pixformat(mvcam);
	return 0;
VEYE_TRACE
err:
	return -ENODEV;
}

/* Start streaming */
static int mvcam_start_streaming(struct mvcam *mvcam)
{
	struct i2c_client *client = mvcam->client;
	int ret;
    VEYE_TRACE
	/* Apply customized values from user */
 //   ret =  __v4l2_ctrl_handler_setup(mvcam->sd.ctrl_handler);
    debug_printk("mvcam_start_streaming \n");
	/* set stream on register */
    ret = mvcam_write(client, Image_Acquisition,1);
	if (ret)
		return ret;

	/* some v4l2 ctrls cannot change during streaming */
    mvcam_v4l2_ctrl_grab(mvcam,true);
	return ret;
}

/* Stop streaming */
static int mvcam_stop_streaming(struct mvcam *mvcam)
{
	struct i2c_client *client = mvcam->client;
	int ret;
VEYE_TRACE
	/* set stream off register */
    ret = mvcam_write(client, Image_Acquisition,0);
	if (ret)
		dev_err(&client->dev, "%s failed to set stream\n", __func__);
    debug_printk("mvcam_stop_streaming \n");
    
   	 mvcam_v4l2_ctrl_grab(mvcam,false);

	/*
	 * Return success even if it was an error, as there is nothing the
	 * caller can do about it.
	 */
	return 0;
}

static int mvcam_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct mvcam *mvcam = to_mvcam(sd);
    struct i2c_client *client = mvcam->client;
	int ret = 0;
	enable = !!enable;
	
	if (mvcam->streaming == enable) {
        dev_info(&client->dev, "%s already streamed!\n", __func__);
		return 0;
	}
VEYE_TRACE
	if (enable) {

		/*
		 * Apply default & customized values
		 * and then start streaming.
		 */
		ret = mvcam_start_streaming(mvcam);
		if (ret)
			goto end;
	} else {
		mvcam_stop_streaming(mvcam);
	}
	mvcam->streaming = enable;
	return ret;
end:
	return ret;
}

/* Power management functions */
static int mvcam_power_on(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct mvcam *mvcam = to_mvcam(sd);
VEYE_TRACE

    gpiod_set_value_cansleep(mvcam->pwdn_gpio, 1);
	usleep_range(STARTUP_MIN_DELAY_US,
		     STARTUP_MIN_DELAY_US + STARTUP_DELAY_RANGE_US);
    
	gpiod_set_value_cansleep(mvcam->reset_gpio, 1);
	usleep_range(STARTUP_MIN_DELAY_US,
		     STARTUP_MIN_DELAY_US + STARTUP_DELAY_RANGE_US);
    debug_printk("mvcam_power_on\n");
	return 0;
}

static int mvcam_power_off(struct device *dev)
{
    
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct mvcam *mvcam = to_mvcam(sd);
    VEYE_TRACE
    //do not really power off, because we might use i2c script at any time
    gpiod_set_value_cansleep(mvcam->pwdn_gpio, 1);//still use 1
	gpiod_set_value_cansleep(mvcam->reset_gpio, 0);

    debug_printk("mvcam_power_off, not really off\n");
	return 0;
}


static int mvcam_enum_frame_interval(struct v4l2_subdev *sd,
				       struct v4l2_subdev_pad_config *cfg,
				       struct v4l2_subdev_frame_interval_enum *fie)
{
    VEYE_TRACE
    /* max framerate */
	struct v4l2_fract fract_fps;
	struct mvcam *mvcam = to_mvcam(sd);
	mutex_lock(&mvcam->mutex);
    fie->width = mvcam->roi.width;
	fie->height = mvcam->roi.height;
    fract_fps.numerator = 100;
    fract_fps.denominator = mvcam->max_fps*100;
    fie->interval = fract_fps;
	mutex_unlock(&mvcam->mutex);
	return 0;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static int mvcam_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct mvcam *mvcam = to_mvcam(sd);
	struct v4l2_mbus_framefmt *try_fmt =
		v4l2_subdev_get_try_format(sd, fh->pad, 0);
    
//	struct v4l2_mbus_framefmt *try_fmt_meta =
//		v4l2_subdev_get_try_format(sd, fh->pad, METADATA_PAD);
    struct v4l2_rect *try_crop;
    VEYE_TRACE
    mutex_lock(&mvcam->mutex);
	/* Initialize try_fmt */
	try_fmt->width = mvcam->max_width;
	try_fmt->height = mvcam->max_height;
	try_fmt->code = mvcam->supported_formats[0].mbus_code;
	try_fmt->field = V4L2_FIELD_NONE;

	/* Initialize try_fmt for the embedded metadata pad */
/*	try_fmt_meta->width = MVCAM_EMBEDDED_LINE_WIDTH;
	try_fmt_meta->height = MVCAM_NUM_EMBEDDED_LINES;
	try_fmt_meta->code = MEDIA_BUS_FMT_SENSOR_DATA;
	try_fmt_meta->field = V4L2_FIELD_NONE;
*/
    /* Initialize try_crop rectangle. */
	try_crop = v4l2_subdev_get_try_crop(sd, fh->pad, 0);
	try_crop->top = 0;
	try_crop->left = 0;
	try_crop->width = mvcam->max_width;
	try_crop->height = mvcam->max_height;
    
    mutex_unlock(&mvcam->mutex);
    
	return 0;
}
#endif

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static const struct v4l2_subdev_internal_ops mvcam_internal_ops = {
	.open = mvcam_open,
};
#endif

static const struct v4l2_subdev_core_ops mvcam_core_ops = {
    .log_status = v4l2_ctrl_subdev_log_status,
    .subscribe_event = v4l2_ctrl_subdev_subscribe_event,
    .unsubscribe_event = v4l2_event_subdev_unsubscribe,
    
    //.s_power = mvcam_s_power,
    .ioctl = mvcam_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl32 = mvcam_compat_ioctl32,
#endif
};

static const struct v4l2_subdev_video_ops mvcam_video_ops = {
	.s_stream = mvcam_set_stream,
    .g_frame_interval = mvcam_g_frame_interval,
    .s_frame_interval = mvcam_s_frame_interval, 
};

static const struct v4l2_subdev_pad_ops mvcam_pad_ops = {
	.enum_mbus_code = mvcam_csi2_enum_mbus_code,
	.get_fmt = mvcam_csi2_get_fmt,
	.set_fmt = mvcam_csi2_set_fmt,
	.enum_frame_size = mvcam_csi2_enum_framesizes,
    
	//.get_selection = mvcam_get_selection,
	//.set_selection = mvcam_set_selection,
    .get_mbus_config = mvcam_g_mbus_config,
	.enum_frame_interval = mvcam_enum_frame_interval,
};

static const struct v4l2_subdev_ops mvcam_subdev_ops = {
	.core = &mvcam_core_ops,
	.video = &mvcam_video_ops,
	.pad = &mvcam_pad_ops,
};

static int __maybe_unused mvcam_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct mvcam *mvcam = to_mvcam(sd);
VEYE_TRACE
	if (mvcam->streaming)
		mvcam_stop_streaming(mvcam);

	return 0;
}

static int __maybe_unused mvcam_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct mvcam *mvcam = to_mvcam(sd);
	int ret;

	if (mvcam->streaming) {
		ret = mvcam_start_streaming(mvcam);
		if (ret)
			goto error;
	}
    VEYE_TRACE
	return 0;

error:
	mvcam_stop_streaming(mvcam);
	mvcam->streaming = 0;
	return ret;
}

static int mvcam_enum_controls(struct mvcam *mvcam)
{
    struct i2c_client *client = mvcam->client;
    struct v4l2_ctrl_handler *ctrl_hdlr;
    struct v4l2_fwnode_device_properties props;
    int ret;
    int i;
    struct v4l2_ctrl *ctrl;
    ctrl_hdlr = &mvcam->ctrl_handler;
    ret = v4l2_ctrl_handler_init(ctrl_hdlr, ARRAY_SIZE(mvcam_v4l2_ctrls));
    if (ret)
        return ret;
VEYE_TRACE
   // mutex_init(&mvcam->mutex);
    ctrl_hdlr->lock = &mvcam->mutex;
    
    for (i = 0; i < ARRAY_SIZE(mvcam_v4l2_ctrls); ++i) {
		ctrl = v4l2_ctrl_new_custom(
			ctrl_hdlr,
			&mvcam_v4l2_ctrls[i],
			NULL);
		if (ctrl == NULL) {
			dev_err(&client->dev, "Failed to init %d ctrl\n",i);
			continue;
		}
		mvcam->ctrls[i] = ctrl;
        if(mvcam->ctrls[i]->id == V4L2_CID_VEYE_MV_FRAME_RATE){
            mvcam->frmrate = mvcam->ctrls[i];
        }
        dev_dbg(&client->dev, "init control %s success\n",mvcam_v4l2_ctrls[i].name);
	}
    
	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s control init failed (%d)\n",
			__func__, ret);
		goto error;
	}

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto error;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &mvcam_ctrl_ops,
					      &props);
	if (ret)
		goto error;

	mvcam->sd.ctrl_handler = ctrl_hdlr;
    v4l2_ctrl_handler_setup(ctrl_hdlr);
VEYE_TRACE
    dev_info(&client->dev, "mvcam_enum_controls success\n");
	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);
	//mutex_destroy(&mvcam->mutex);

	return ret;

}

/* Verify chip ID */
static int mvcam_identify_module(struct mvcam * mvcam)
{
	int ret;
    u32 device_id;
	u32 firmware_version;
    struct i2c_client *client = v4l2_get_subdevdata(&mvcam->sd);
    
    ret = mvcam_read(client, Model_Name, &device_id);
    if (ret ) {
        dev_err(&client->dev, "failed to read chip id\n");
        ret = -ENODEV;
        return ret;
    }
    switch (device_id)
    {
        case MV_MIPI_IMX178M:
            mvcam->model_id = device_id;
            dev_info(&client->dev, "camera is: MV-MIPI-IMX178M\n");
            break; 
        case MV_MIPI_IMX296M:
            mvcam->model_id = device_id;
            dev_info(&client->dev, "camera is：MV-MIPI-IMX296M\n");
            break; 
        case MV_MIPI_SC130M:
            mvcam->model_id = device_id;
            dev_info(&client->dev, "camera is: MV-MIPI-SC130M\n");
            break; 
        case MV_MIPI_IMX265M:
            mvcam->model_id = device_id;
            dev_info(&client->dev, "camera is: MV-MIPI-IMX265M\n");
            break; 
        case MV_MIPI_IMX264M:
            mvcam->model_id = device_id;
            dev_info(&client->dev, "camera is: MV-MIPI-IMX264M\n");
            break; 
        case RAW_MIPI_SC132M:
            mvcam->model_id = device_id;
            dev_info(&client->dev, "camera is: RAW-MIPI-SC132M\n");
            break;
        case MV_MIPI_IMX287M:
            mvcam->model_id = device_id;
            dev_info(&client->dev, "camera is: MV_MIPI_IMX287M\n");
            break;
        default:
            dev_err(&client->dev, "camera id do not support: %x \n",device_id);
		return -EIO;
    }
    
    ret = mvcam_read(client, Device_Version, &firmware_version);
    if (ret) {
        dev_err(&client->dev, "read firmware version failed\n");
    }
    dev_info(&client->dev, "firmware version: 0x%04X\n", firmware_version);
	return 0;
}

/*
static int mvcam_init_mode(struct v4l2_subdev *sd)
{
    struct mvcam *mvcam = to_mvcam(sd);
    struct i2c_client *client = mvcam->client;

    struct v4l2_subdev_selection sel;
    //stop acquitsition
    mvcam_write(client, Image_Acquisition,0);
    //set to video stream mode
	//mvcam_write(client, Trigger_Mode,0);
    //set roi
	//todo : read current roi from camera and set to media node.
	//because RK3588's VICAP open the crop capbility by default, it will intercept the roi setting.
    mvcam->roi.left = 0;
    mvcam->roi.top = 0;
    mvcam->roi.width = mvcam->max_width;
    mvcam->roi.height = mvcam->max_height;
    sel.target = V4L2_SEL_TGT_CROP;
	sel.r = mvcam->roi;
    mvcam_set_selection(sd, NULL, &sel);
    return 0;
}
*/
static void free_gpio(struct mvcam *mvcam)
{
	if (!IS_ERR(mvcam->pwdn_gpio))
		gpio_free(desc_to_gpio(mvcam->pwdn_gpio));
    if (!IS_ERR(mvcam->reset_gpio))
        gpio_free(desc_to_gpio(mvcam->reset_gpio));
    //    if (!IS_ERR(mvcam->mipi_pwr_gpio))
	//	gpio_free(desc_to_gpio(mvcam->mipi_pwr_gpio));
}
static int mvcam_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	
	struct device_node *node = dev->of_node;
	struct device_node *endpoint_node = NULL;
	struct v4l2_fwnode_endpoint vep = {0};
	struct mvcam *mvcam;
	char facing[2];
	int ret;
    
	dev_info(dev, "veye mv series camera driver version: %02x.%02x.%02x\n",
		DRIVER_VERSION >> 16,
		(DRIVER_VERSION & 0xff00) >> 8,
		DRIVER_VERSION & 0x00ff);    
	mvcam = devm_kzalloc(&client->dev, sizeof(struct mvcam), GFP_KERNEL);
	if (!mvcam)
		return -ENOMEM;
    
    ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
				   &mvcam->module_index);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
				       &mvcam->module_facing);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
				       &mvcam->module_name);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
				       &mvcam->len_name);
	if (ret) {
		dev_err(dev, "could not get module information!\n");
		return -EINVAL;
	}
    
	/* Initialize subdev */
	v4l2_i2c_subdev_init(&mvcam->sd, client, &mvcam_subdev_ops);
	mvcam->client = client;
    
    mvcam->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(mvcam->reset_gpio)) {
	   dev_info(dev, "Failed to get reset-gpios, maybe no use\n");
	}

	mvcam->pwdn_gpio = devm_gpiod_get(dev, "pwdn", GPIOD_OUT_HIGH);
	if (IS_ERR(mvcam->pwdn_gpio)) {
	  dev_info(dev, "Failed to get pwdn-gpios, maybe no use\n");
	}

	endpoint_node = of_find_node_by_name(node,"endpoint");
	if(endpoint_node != NULL){
		//printk("mvcam get endpoint node success\n");
		ret=v4l2_fwnode_endpoint_parse(&endpoint_node->fwnode, &vep);
		if(ret){
			dev_info(dev, "Failed to get mvcam endpoint data lanes, set a default value\n");
			mvcam->lane_data_num = 2;
		}else{
			dev_info(dev, "Success to get mvcam endpoint data lanes, dts uses %d lanes\n", vep.bus.mipi_csi2.num_data_lanes);
			mvcam->lane_data_num = vep.bus.mipi_csi2.num_data_lanes;
		}
	}else{
		dev_info(dev,"mvcam get endpoint node failed\n");
		return -ENOENT;
	}
    mutex_init(&mvcam->mutex);
	
	ret = mvcam_power_on(dev);
	if (ret)
		goto err_destroy_mutex;
    
    ret = mvcam_identify_module(mvcam);
	if (ret){
        goto error_power_off;
    }

	if (mvcam_enum_pixformat(mvcam)) {
		dev_err(dev, "enum pixformat failed.\n");
		ret = -ENODEV;
		goto error_power_off;
	}

    mvcam_read(client, Sensor_Width, &mvcam->max_width);
    mvcam_read(client, Sensor_Height, &mvcam->max_height);
    if(mvcam->model_id == MV_MIPI_IMX178M){
        mvcam->min_width = MV_IMX178M_ROI_W_MIN;
        mvcam->min_height = MV_IMX178M_ROI_H_MIN;
    }else if(mvcam->model_id == MV_MIPI_SC130M){
        mvcam->min_width = MV_SC130M_ROI_W_MIN;
        mvcam->min_height = MV_SC130M_ROI_H_MIN;
    }else if(mvcam->model_id == MV_MIPI_IMX296M){
        mvcam->min_width = MV_IMX296M_ROI_W_MIN;
        mvcam->min_height = MV_IMX296M_ROI_H_MIN;
    }else if(mvcam->model_id == MV_MIPI_IMX265M){
        mvcam->min_width = MV_IMX265M_ROI_W_MIN;
        mvcam->min_height = MV_IMX265M_ROI_H_MIN;
    }else if(mvcam->model_id == MV_MIPI_IMX264M){
        mvcam->min_width = MV_IMX264M_ROI_W_MIN;
        mvcam->min_height = MV_IMX264M_ROI_H_MIN;
    }else if(mvcam->model_id == RAW_MIPI_SC132M){
        mvcam->min_width = RAW_SC132M_ROI_W_MIN;
        mvcam->min_height = RAW_SC132M_ROI_H_MIN;
    }else if(mvcam->model_id == MV_MIPI_IMX287M){
        mvcam->min_width = MV_IMX287M_ROI_W_MIN;
        mvcam->min_height = MV_IMX287M_ROI_H_MIN;
    }
    v4l2_dbg(1, debug, mvcam->client, "%s: max width %d; max height %d\n",
					__func__, mvcam->max_width,mvcam->max_height);
    //read roi
    mvcam_getroi(mvcam);
    
    mvcam_v4l2_ctrl_init(mvcam);
    
	if (mvcam_enum_controls(mvcam)) {
		dev_err(dev, "enum controls failed.\n");
		ret = -ENODEV;
		goto error_power_off;
	}
	
    //mvcam_init_mode(&mvcam->sd);
    //stop acquitsition
    mvcam_write(client, Image_Acquisition,0);
    
    
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	mvcam->sd.internal_ops = &mvcam_internal_ops;
	mvcam->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
#endif
#if defined(CONFIG_MEDIA_CONTROLLER)
	mvcam->pad.flags = MEDIA_PAD_FL_SOURCE;
	mvcam->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&mvcam->sd.entity, 1, &mvcam->pad);
	if (ret < 0){
		dev_err(dev, "media_entity_pads_init failed\n");
		goto error_power_off;
		}

#endif
	memset(facing, 0, sizeof(facing));
	if (strcmp(mvcam->module_facing, "back") == 0)
		facing[0] = 'b';
	else
		facing[0] = 'f';

	snprintf(mvcam->sd.name, sizeof(mvcam->sd.name), "m%02d_%s_%s %s",
		 mvcam->module_index, facing,
		 mvcam_NAME, dev_name(mvcam->sd.dev));

	ret = v4l2_async_register_subdev_sensor_common(&mvcam->sd);
	if (ret){
		dev_err(dev, "v4l2 async register subdev failed\n");
		goto error_media_entity;
	}
VEYE_TRACE
	return 0;

error_media_entity:
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&mvcam->sd.entity);
#endif
//error_handler_free:
error_power_off:
	mvcam_power_off(dev);
	mvcam_free_controls(mvcam);
    free_gpio(mvcam);
err_destroy_mutex:
	mutex_destroy(&mvcam->mutex);
	
	return ret;
}

static int mvcam_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct mvcam *mvcam = to_mvcam(sd);
VEYE_TRACE
	v4l2_async_unregister_subdev(sd);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
    mvcam_free_controls(mvcam);
    
	mutex_destroy(&mvcam->mutex);
    
	return 0;
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id mvcam_of_match[] = {
	{ .compatible = "veye,mvcam" },
	{},
};
MODULE_DEVICE_TABLE(of, mvcam_of_match);
#endif

static const struct i2c_device_id mvcam_match_id[] = {
    { "veye,mvcam", 0 },
	{ },
};

static struct i2c_driver veyemv_cam_i2c_driver = {
	.driver = {
		.name = "mvcam",
        //.pm = &mvcam_pm_ops,
		.of_match_table	= of_match_ptr(mvcam_of_match),
	},
	.probe = &mvcam_probe,
	.remove = &mvcam_remove,
    .id_table	= mvcam_match_id,
};

static int __init sensor_mod_init(void)
{
	return i2c_add_driver(&veyemv_cam_i2c_driver);
}

static void __exit sensor_mod_exit(void)
{
	i2c_del_driver(&veyemv_cam_i2c_driver);
}

device_initcall_sync(sensor_mod_init);
module_exit(sensor_mod_exit);

MODULE_AUTHOR("xumm <www.veye.cc>");
MODULE_DESCRIPTION("VEYE MV series mipi camera v4l2 driver");
MODULE_LICENSE("GPL v2");