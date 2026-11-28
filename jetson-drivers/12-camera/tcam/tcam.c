// SPDX-License-Identifier: GPL-2.0
/*
 * tcam：虚拟摄像头（V4L2 采集驱动 + videobuf2）。没有传感器，帧由软件生成：
 *   8 条彩条（白黄青绿品红蓝黑），中间一个灰色方块每帧右移 8 像素，看得出画面在动。
 *
 *   格式：YUYV、RGB24；分辨率：320x240、640x480（默认）、1280x720；帧率 30 fps
 *   控件：亮度（0～255，默认 128）、水平翻转
 *   /dev/videoN 支持 mmap 流式采集和 read()
 *
 * 真实的摄像头驱动在 V4L2 这一层的样子和它完全一样，区别只在「帧从哪来」：
 * 这里是 CPU 填，Jetson 上是 CSI 接收 → VI 写进内存（见 README 第 6 节）。
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#define TCAM_FPS	30

struct tcam_fmt {
	u32 fourcc;
	unsigned int bpp;		/* 每像素字节数 */
};

static const struct tcam_fmt tcam_formats[] = {
	{ V4L2_PIX_FMT_YUYV, 2 },
	{ V4L2_PIX_FMT_RGB24, 3 },
};

static const struct v4l2_frmsize_discrete tcam_sizes[] = {
	{ 320, 240 }, { 640, 480 }, { 1280, 720 },
};

/* 彩条颜色：RGB，以及对应的 YUV（BT.601 有限范围） */
static const u8 bar_rgb[8][3] = {
	{ 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
	{ 255, 0, 255 }, { 255, 0, 0 }, { 0, 0, 255 }, { 0, 0, 0 },
};
static const u8 bar_yuv[8][3] = {
	{ 235, 128, 128 }, { 210, 16, 146 }, { 170, 166, 16 }, { 145, 54, 34 },
	{ 106, 202, 222 }, { 81, 90, 240 }, { 41, 240, 110 }, { 16, 128, 128 },
};

struct tcam_buf {
	struct vb2_v4l2_buffer vb;	/* 必须放在第一个 */
	struct list_head list;
};

struct tcam {
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *brightness, *hflip;
	struct vb2_queue queue;
	struct mutex lock;		/* 串行化所有 ioctl */

	struct v4l2_pix_format pix;
	const struct tcam_fmt *fmt;

	spinlock_t qlock;		/* 保护 bufs：buf_queue 和出帧的 work 都会碰它 */
	struct list_head bufs;
	struct delayed_work work;
	u32 sequence;
	ktime_t next;			/* 下一帧应该出的时间 */
};

static const struct tcam_fmt *tcam_find_fmt(u32 fourcc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(tcam_formats); i++)
		if (tcam_formats[i].fourcc == fourcc)
			return &tcam_formats[i];
	return NULL;
}

/* ---------- 生成一帧 ---------- */

static inline u8 clamp_u8(int v)
{
	return clamp(v, 0, 255);
}

static void tcam_fill(struct tcam *c, u8 *dst, u32 seq)
{
	unsigned int w = c->pix.width, h = c->pix.height, x, y;
	int off = c->brightness->val - 128;
	bool flip = c->hflip->val;
	unsigned int box_x = (seq * 8) % w, box_y0 = h / 2 - 20, box_y1 = h / 2 + 20;
	bool yuyv = c->fmt->fourcc == V4L2_PIX_FMT_YUYV;

	for (y = 0; y < h; y++) {
		u8 *line = dst + y * c->pix.bytesperline;
		bool box_row = y >= box_y0 && y < box_y1;

		for (x = 0; x < w; x += yuyv ? 2 : 1) {
			unsigned int sx = flip ? w - 1 - x : x;
			int bar = sx * 8 / w;
			bool in_box = box_row && x >= box_x && x < box_x + 32;

			if (yuyv) {
				u8 yy = in_box ? 128 : clamp_u8(bar_yuv[bar][0] + off);
				u8 u = in_box ? 128 : bar_yuv[bar][1];
				u8 v = in_box ? 128 : bar_yuv[bar][2];

				*line++ = yy; *line++ = u; *line++ = yy; *line++ = v;
			} else {
				int k;

				for (k = 0; k < 3; k++)
					*line++ = in_box ? 128 : clamp_u8(bar_rgb[bar][k] + off);
			}
		}
	}
}

/* 出帧：取队列里第一个空缓冲区，填好，交还给用户。按绝对时间排下一次，帧率不会越跑越慢 */
static void tcam_work(struct work_struct *work)
{
	struct tcam *c = container_of(to_delayed_work(work), struct tcam, work);
	struct tcam_buf *b = NULL;
	ktime_t now;
	long delay;

	spin_lock_irq(&c->qlock);
	if (!list_empty(&c->bufs)) {
		b = list_first_entry(&c->bufs, struct tcam_buf, list);
		list_del(&b->list);
	}
	spin_unlock_irq(&c->qlock);

	if (b) {
		tcam_fill(c, vb2_plane_vaddr(&b->vb.vb2_buf, 0), c->sequence);
		vb2_set_plane_payload(&b->vb.vb2_buf, 0, c->pix.sizeimage);
		b->vb.vb2_buf.timestamp = ktime_get_ns();
		b->vb.sequence = c->sequence;
		b->vb.field = V4L2_FIELD_NONE;
		vb2_buffer_done(&b->vb.vb2_buf, VB2_BUF_STATE_DONE);
	}
	/* 用户来不及还缓冲区时，这一帧就丢了，但帧号照样加 1：用户从帧号的跳变就能知道丢了几帧 */
	c->sequence++;

	c->next = ktime_add_ns(c->next, NSEC_PER_SEC / TCAM_FPS);
	now = ktime_get();
	delay = ktime_after(c->next, now) ? nsecs_to_jiffies(ktime_to_ns(ktime_sub(c->next, now))) : 0;
	schedule_delayed_work(&c->work, delay);
}

/* ---------- videobuf2 ---------- */

static int tcam_queue_setup(struct vb2_queue *q, unsigned int *nbuffers, unsigned int *nplanes,
			    unsigned int sizes[], struct device *alloc_devs[])
{
	struct tcam *c = vb2_get_drv_priv(q);

	if (*nplanes)
		return sizes[0] < c->pix.sizeimage ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = c->pix.sizeimage;
	return 0;
}

static int tcam_buf_prepare(struct vb2_buffer *vb)
{
	struct tcam *c = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < c->pix.sizeimage)
		return -EINVAL;
	return 0;
}

static void tcam_buf_queue(struct vb2_buffer *vb)
{
	struct tcam *c = vb2_get_drv_priv(vb->vb2_queue);
	struct tcam_buf *b = container_of(to_vb2_v4l2_buffer(vb), struct tcam_buf, vb);
	unsigned long flags;

	spin_lock_irqsave(&c->qlock, flags);
	list_add_tail(&b->list, &c->bufs);
	spin_unlock_irqrestore(&c->qlock, flags);
}

static void tcam_return_all(struct tcam *c, enum vb2_buffer_state state)
{
	struct tcam_buf *b, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&c->qlock, flags);
	list_for_each_entry_safe(b, tmp, &c->bufs, list) {
		list_del(&b->list);
		vb2_buffer_done(&b->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&c->qlock, flags);
}

static int tcam_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct tcam *c = vb2_get_drv_priv(q);

	c->sequence = 0;
	c->next = ktime_get();
	schedule_delayed_work(&c->work, 0);
	return 0;
}

static void tcam_stop_streaming(struct vb2_queue *q)
{
	struct tcam *c = vb2_get_drv_priv(q);

	cancel_delayed_work_sync(&c->work);
	/* 规则：停止时，驱动手里的每个缓冲区都必须还给 vb2，否则 vb2 会报 WARN */
	tcam_return_all(c, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops tcam_vb2_ops = {
	.queue_setup = tcam_queue_setup,
	.buf_prepare = tcam_buf_prepare,
	.buf_queue = tcam_buf_queue,
	.start_streaming = tcam_start_streaming,
	.stop_streaming = tcam_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

/* ---------- ioctl ---------- */

static int tcam_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	strscpy(cap->driver, "tcam", sizeof(cap->driver));
	strscpy(cap->card, "jetson-drivers virtual camera", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:tutorial-cam", sizeof(cap->bus_info));
	return 0;
}

static int tcam_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(tcam_formats))
		return -EINVAL;
	f->pixelformat = tcam_formats[f->index].fourcc;
	return 0;
}

/* 把用户要的格式「修正」成我们支持的最接近的格式。TRY_FMT 只修正不生效，S_FMT 修正后生效 */
static void tcam_adjust(struct v4l2_pix_format *p)
{
	const struct tcam_fmt *fmt = tcam_find_fmt(p->pixelformat);
	const struct v4l2_frmsize_discrete *sz;

	if (!fmt)
		fmt = &tcam_formats[0];
	sz = v4l2_find_nearest_size(tcam_sizes, ARRAY_SIZE(tcam_sizes), width, height,
				    p->width, p->height);
	p->pixelformat = fmt->fourcc;
	p->width = sz->width;
	p->height = sz->height;
	p->field = V4L2_FIELD_NONE;
	p->bytesperline = p->width * fmt->bpp;
	p->sizeimage = p->bytesperline * p->height;
	p->colorspace = fmt->fourcc == V4L2_PIX_FMT_YUYV ? V4L2_COLORSPACE_SMPTE170M : V4L2_COLORSPACE_SRGB;
	p->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	p->quantization = V4L2_QUANTIZATION_DEFAULT;
	p->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static int tcam_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct tcam *c = video_drvdata(file);

	f->fmt.pix = c->pix;
	return 0;
}

static int tcam_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	tcam_adjust(&f->fmt.pix);
	return 0;
}

static int tcam_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct tcam *c = video_drvdata(file);

	if (vb2_is_busy(&c->queue))	/* 已经申请了缓冲区就不能改格式，缓冲区大小对不上 */
		return -EBUSY;
	tcam_adjust(&f->fmt.pix);
	c->pix = f->fmt.pix;
	c->fmt = tcam_find_fmt(c->pix.pixelformat);
	return 0;
}

static int tcam_enum_framesizes(struct file *file, void *fh, struct v4l2_frmsizeenum *fs)
{
	if (!tcam_find_fmt(fs->pixel_format) || fs->index >= ARRAY_SIZE(tcam_sizes))
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fs->discrete = tcam_sizes[fs->index];
	return 0;
}

static int tcam_enum_frameintervals(struct file *file, void *fh, struct v4l2_frmivalenum *fi)
{
	int i;

	if (fi->index || !tcam_find_fmt(fi->pixel_format))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(tcam_sizes); i++)
		if (tcam_sizes[i].width == fi->width && tcam_sizes[i].height == fi->height)
			break;
	if (i == ARRAY_SIZE(tcam_sizes))
		return -EINVAL;
	fi->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fi->discrete.numerator = 1;
	fi->discrete.denominator = TCAM_FPS;
	return 0;
}

static int tcam_g_parm(struct file *file, void *fh, struct v4l2_streamparm *a)
{
	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.capture.timeperframe.numerator = 1;
	a->parm.capture.timeperframe.denominator = TCAM_FPS;
	a->parm.capture.readbuffers = 2;
	return 0;
}

static int tcam_enum_input(struct file *file, void *fh, struct v4l2_input *inp)
{
	if (inp->index)
		return -EINVAL;
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	strscpy(inp->name, "Test pattern", sizeof(inp->name));
	return 0;
}

static int tcam_g_input(struct file *file, void *fh, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int tcam_s_input(struct file *file, void *fh, unsigned int i)
{
	return i ? -EINVAL : 0;
}

static const struct v4l2_ioctl_ops tcam_ioctl_ops = {
	.vidioc_querycap = tcam_querycap,
	.vidioc_enum_fmt_vid_cap = tcam_enum_fmt,
	.vidioc_g_fmt_vid_cap = tcam_g_fmt,
	.vidioc_try_fmt_vid_cap = tcam_try_fmt,
	.vidioc_s_fmt_vid_cap = tcam_s_fmt,
	.vidioc_enum_framesizes = tcam_enum_framesizes,
	.vidioc_enum_frameintervals = tcam_enum_frameintervals,
	.vidioc_g_parm = tcam_g_parm,
	.vidioc_s_parm = tcam_g_parm,	/* 帧率固定，设置时原样返回 30 fps */
	.vidioc_enum_input = tcam_enum_input,
	.vidioc_g_input = tcam_g_input,
	.vidioc_s_input = tcam_s_input,
	/* 缓冲区管理全部交给 vb2 */
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_log_status = v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations tcam_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

/* ---------- probe ---------- */

static int tcam_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vb2_queue *q;
	struct tcam *c;
	int ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	mutex_init(&c->lock);
	spin_lock_init(&c->qlock);
	INIT_LIST_HEAD(&c->bufs);
	INIT_DELAYED_WORK(&c->work, tcam_work);
	c->pix.pixelformat = V4L2_PIX_FMT_YUYV;
	c->pix.width = 640;
	c->pix.height = 480;
	tcam_adjust(&c->pix);
	c->fmt = tcam_find_fmt(c->pix.pixelformat);

	ret = v4l2_device_register(dev, &c->v4l2_dev);
	if (ret)
		return ret;

	v4l2_ctrl_handler_init(&c->ctrls, 2);
	c->brightness = v4l2_ctrl_new_std(&c->ctrls, NULL, V4L2_CID_BRIGHTNESS, 0, 255, 1, 128);
	c->hflip = v4l2_ctrl_new_std(&c->ctrls, NULL, V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (c->ctrls.error) {
		ret = c->ctrls.error;
		goto err_ctrls;
	}
	c->v4l2_dev.ctrl_handler = &c->ctrls;

	q = &c->queue;
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	q->drv_priv = c;
	q->buf_struct_size = sizeof(struct tcam_buf);
	q->ops = &tcam_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;	/* 缓冲区用 vmalloc：CPU 填帧不需要物理连续 */
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = 2;	/* 主线 6.8 里叫 min_buffers_needed，这个内核带了 6.9 的改名 */
	q->lock = &c->lock;
	ret = vb2_queue_init(q);
	if (ret)
		goto err_ctrls;

	c->vdev.v4l2_dev = &c->v4l2_dev;
	c->vdev.fops = &tcam_fops;
	c->vdev.ioctl_ops = &tcam_ioctl_ops;
	c->vdev.release = video_device_release_empty;
	c->vdev.lock = &c->lock;
	c->vdev.queue = q;
	c->vdev.vfl_dir = VFL_DIR_RX;
	c->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
	strscpy(c->vdev.name, "tutorial-cam", sizeof(c->vdev.name));
	video_set_drvdata(&c->vdev, c);
	ret = video_register_device(&c->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_ctrls;

	platform_set_drvdata(pdev, c);
	dev_info(dev, "registered %s\n", video_device_node_name(&c->vdev));
	return 0;

err_ctrls:
	v4l2_ctrl_handler_free(&c->ctrls);
	v4l2_device_unregister(&c->v4l2_dev);
	return ret;
}

static void tcam_remove(struct platform_device *pdev)
{
	struct tcam *c = platform_get_drvdata(pdev);

	video_unregister_device(&c->vdev);
	v4l2_ctrl_handler_free(&c->ctrls);
	v4l2_device_unregister(&c->v4l2_dev);
}

static struct platform_driver tcam_driver = {
	.probe = tcam_probe,
	.remove_new = tcam_remove,
	.driver.name = "tutorial-cam",
};

/* 没有真实硬件，就在加载模块时自己注册一个平台设备，让驱动有东西可 probe */
static struct platform_device *tcam_pdev;

static int __init tcam_init(void)
{
	int ret = platform_driver_register(&tcam_driver);

	if (ret)
		return ret;
	tcam_pdev = platform_device_register_simple("tutorial-cam", -1, NULL, 0);
	if (IS_ERR(tcam_pdev)) {
		platform_driver_unregister(&tcam_driver);
		return PTR_ERR(tcam_pdev);
	}
	return 0;
}

static void __exit tcam_exit(void)
{
	platform_device_unregister(tcam_pdev);
	platform_driver_unregister(&tcam_driver);
}

module_init(tcam_init);
module_exit(tcam_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("jetson-drivers tutorial: virtual V4L2 camera");
