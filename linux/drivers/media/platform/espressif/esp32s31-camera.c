// SPDX-License-Identifier: GPL-2.0-only
/* ESP32-S31 eight-bit DVP capture into scatter-gather PSRAM buffers. */

#include <linux/clk.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-sg.h>

#define CAM_CTRL		0x04
#define CAM_CTRL1		0x08
#define CAM_RGB_YUV		0x0c
#define CAM_UPDATE		BIT(4)
#define CAM_VSYNC_EOF		BIT(8)
#define CAM_PCLK_INV		BIT(22)
#define CAM_DE_INV		BIT(25)
#define CAM_VSYNC_INV		BIT(27)
#define CAM_START		BIT(29)
#define CAM_RESET		BIT(30)
#define CAM_FIFO_RESET		BIT(31)
#define CAM_WIDTH		640
#define CAM_HEIGHT		480
#define CAM_FRAME_SIZE		(CAM_WIDTH * CAM_HEIGHT * 2)
/* Isolate samples at the VSYNC boundary beyond the visible image. */
#define CAM_BUFFER_SIZE		PAGE_ALIGN(CAM_FRAME_SIZE + 64)
#define CAM_TIMEOUT		msecs_to_jiffies(2000)

struct esp32s31_camera_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct esp32s31_camera {
	struct device *dev;
	void __iomem *regs;
	struct dma_chan *dma;
	struct clk_bulk_data clocks[4];
	struct v4l2_device v4l2;
	struct video_device video;
	struct media_device media;
	struct media_pad pad;
	struct v4l2_async_notifier notifier;
	struct v4l2_subdev *sensor;
	struct vb2_queue queue;
	struct mutex lock;
	spinlock_t qlock;
	struct list_head buffers;
	struct esp32s31_camera_buffer *active;
	struct delayed_work watchdog;
	unsigned int sequence;
	unsigned long deadline;
	u32 polarity;
	bool streaming;
	bool removing;
	bool dma_faulted;
};

static void esp32s31_camera_complete(void *data,
				     const struct dmaengine_result *result);

/* qlock protects receiver state and the buffer list, including DMA callbacks. */
static int esp32s31_camera_next(struct esp32s31_camera *cam)
{
	struct dma_async_tx_descriptor *desc;
	struct esp32s31_camera_buffer *buffer;
	struct sg_table *sgt;
	dma_cookie_t cookie;

	if (!cam->streaming || cam->active || list_empty(&cam->buffers))
		return 0;
	buffer = list_first_entry(&cam->buffers,
				 struct esp32s31_camera_buffer, list);
	sgt = vb2_dma_sg_plane_desc(&buffer->vb.vb2_buf, 0);
	desc = dmaengine_prep_slave_sg(cam->dma, sgt->sgl, sgt->nents,
				       DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT |
				       DMA_CTRL_ACK);
	if (!desc)
		return -ENOMEM;
	desc->callback_result = esp32s31_camera_complete;
	desc->callback_param = cam;
	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie))
		return cookie;
	list_del(&buffer->list);
	cam->active = buffer;
	/* ESP-IDF's SC101IOT fix discards the preceding frame's partial sample. */
	writel(cam->polarity | CAM_FIFO_RESET, cam->regs + CAM_CTRL1);
	dma_async_issue_pending(cam->dma);
	writel(CAM_VSYNC_EOF | CAM_UPDATE, cam->regs + CAM_CTRL);
	writel(cam->polarity | CAM_START, cam->regs + CAM_CTRL1);
	cam->deadline = jiffies + CAM_TIMEOUT;
	mod_delayed_work(system_wq, &cam->watchdog, CAM_TIMEOUT);
	return 0;
}

static void esp32s31_camera_complete(void *data,
				     const struct dmaengine_result *result)
{
	struct esp32s31_camera *cam = data;
	struct esp32s31_camera_buffer *buffer;
	enum vb2_buffer_state state = VB2_BUF_STATE_ERROR;
	unsigned long flags;

	spin_lock_irqsave(&cam->qlock, flags);
	writel(cam->polarity, cam->regs + CAM_CTRL1);
	writel(CAM_VSYNC_EOF | CAM_UPDATE, cam->regs + CAM_CTRL);
	buffer = cam->active;
	cam->active = NULL;
	if (buffer) {
		if (result && result->result == DMA_TRANS_NOERROR &&
		    result->residue <= CAM_BUFFER_SIZE - CAM_FRAME_SIZE)
			state = VB2_BUF_STATE_DONE;
		buffer->vb.sequence = cam->sequence++;
		buffer->vb.field = V4L2_FIELD_NONE;
		buffer->vb.vb2_buf.timestamp = ktime_get_ns();
		vb2_set_plane_payload(&buffer->vb.vb2_buf, 0,
				      state == VB2_BUF_STATE_DONE ? CAM_FRAME_SIZE : 0);
		vb2_buffer_done(&buffer->vb.vb2_buf, state);
	}
	if (esp32s31_camera_next(cam))
		vb2_queue_error(&cam->queue);
	spin_unlock_irqrestore(&cam->qlock, flags);
}

static void esp32s31_camera_return_buffers(struct esp32s31_camera *cam,
					   enum vb2_buffer_state state)
{
	struct esp32s31_camera_buffer *buffer, *next;
	unsigned long flags;

	spin_lock_irqsave(&cam->qlock, flags);
	if (cam->active) {
		vb2_buffer_done(&cam->active->vb.vb2_buf, state);
		cam->active = NULL;
	}
	list_for_each_entry_safe(buffer, next, &cam->buffers, list) {
		list_del(&buffer->list);
		vb2_buffer_done(&buffer->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&cam->qlock, flags);
}

/* Caller holds lock. Stop the receiver before quiescing the DMA callbacks. */
static void esp32s31_camera_halt(struct esp32s31_camera *cam)
{
	struct sg_page_iter page;
	struct sg_table *sgt;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&cam->qlock, flags);
	cam->streaming = false;
	writel(cam->polarity, cam->regs + CAM_CTRL1);
	writel(CAM_VSYNC_EOF | CAM_UPDATE, cam->regs + CAM_CTRL);
	spin_unlock_irqrestore(&cam->qlock, flags);
	ret = dmaengine_terminate_sync(cam->dma);
	if (ret) {
		/* terminate_sync skips synchronization when termination fails. */
		dmaengine_synchronize(cam->dma);
		if (!cam->dma_faulted) {
			/*
			 * AXI retains its active links on failure. This platform uses
			 * direct DMA into MMAP pages: pin their physical backing so
			 * later VB2 teardown cannot recycle memory still owned by DMA.
			 * Recovery requires reboot, not a whole-block DMA reset that
			 * could disrupt other channels.
			 */
			if (cam->active) {
				sgt = vb2_dma_sg_plane_desc(&cam->active->vb.vb2_buf, 0);
				for_each_sgtable_page(sgt, &page, 0)
					get_page(sg_page_iter_page(&page));
			}
			cam->dma_faulted = true;
			dev_err(cam->dev,
				"DMA did not stop; capture memory retained until reboot\n");
		}
		vb2_queue_error(&cam->queue);
	}
	cancel_delayed_work(&cam->watchdog);
}

static void esp32s31_camera_watchdog(struct work_struct *work)
{
	struct esp32s31_camera *cam = container_of(to_delayed_work(work),
						struct esp32s31_camera, watchdog);

	mutex_lock(&cam->lock);
	if (cam->streaming && READ_ONCE(cam->active) &&
	    time_after_eq(jiffies, READ_ONCE(cam->deadline))) {
		dev_warn(cam->dev, "capture timed out\n");
		esp32s31_camera_halt(cam);
		if (cam->sensor)
			v4l2_subdev_call(cam->sensor, video, s_stream, 0);
		esp32s31_camera_return_buffers(cam, VB2_BUF_STATE_ERROR);
		vb2_queue_error(&cam->queue);
	}
	mutex_unlock(&cam->lock);
}

static int esp32s31_camera_queue_setup(struct vb2_queue *queue,
				       unsigned int *count, unsigned int *planes,
				       unsigned int sizes[], struct device *alloc_devs[])
{
	if (*planes)
		return *planes == 1 && sizes[0] >= CAM_BUFFER_SIZE ? 0 : -EINVAL;
	*count = clamp_t(unsigned int, *count, 2, 4);
	*planes = 1;
	sizes[0] = CAM_BUFFER_SIZE;
	return 0;
}

static int esp32s31_camera_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < CAM_BUFFER_SIZE)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, CAM_FRAME_SIZE);
	return 0;
}

static void esp32s31_camera_queue(struct vb2_buffer *vb)
{
	struct esp32s31_camera *cam = vb2_get_drv_priv(vb->vb2_queue);
	struct esp32s31_camera_buffer *buffer = container_of(to_vb2_v4l2_buffer(vb),
						struct esp32s31_camera_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&cam->qlock, flags);
	list_add_tail(&buffer->list, &cam->buffers);
	if (esp32s31_camera_next(cam))
		vb2_queue_error(&cam->queue);
	spin_unlock_irqrestore(&cam->qlock, flags);
}

static int esp32s31_camera_start(struct vb2_queue *queue, unsigned int count)
{
	struct esp32s31_camera *cam = vb2_get_drv_priv(queue);
	unsigned long flags;
	int ret;

	if (cam->dma_faulted) {
		ret = -EIO;
		goto return_buffers;
	}
	if (!cam->sensor || cam->removing) {
		ret = -ENOLINK;
		goto return_buffers;
	}
	cam->sequence = 0;
	writel(cam->polarity | CAM_RESET, cam->regs + CAM_CTRL1);
	writel(0, cam->regs + CAM_RGB_YUV);
	spin_lock_irqsave(&cam->qlock, flags);
	cam->streaming = true;
	ret = esp32s31_camera_next(cam);
	spin_unlock_irqrestore(&cam->qlock, flags);
	if (ret)
		goto halt;
	ret = v4l2_subdev_call(cam->sensor, video, s_stream, 1);
	if (!ret)
		return 0;
halt:
	esp32s31_camera_halt(cam);
return_buffers:
	esp32s31_camera_return_buffers(cam, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void esp32s31_camera_stop(struct vb2_queue *queue)
{
	struct esp32s31_camera *cam = vb2_get_drv_priv(queue);

	if (cam->dma)
		esp32s31_camera_halt(cam);
	if (cam->sensor)
		v4l2_subdev_call(cam->sensor, video, s_stream, 0);
	esp32s31_camera_return_buffers(cam, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops esp32s31_camera_queue_ops = {
	.queue_setup = esp32s31_camera_queue_setup,
	.buf_prepare = esp32s31_camera_prepare,
	.buf_queue = esp32s31_camera_queue,
	.start_streaming = esp32s31_camera_start,
	.stop_streaming = esp32s31_camera_stop,
};

static void esp32s31_camera_format(struct v4l2_format *format)
{
	format->fmt.pix = (struct v4l2_pix_format) {
		.width = CAM_WIDTH,
		.height = CAM_HEIGHT,
		.pixelformat = V4L2_PIX_FMT_UYVY,
		.field = V4L2_FIELD_NONE,
		.bytesperline = CAM_WIDTH * 2,
		.sizeimage = CAM_FRAME_SIZE,
		.colorspace = V4L2_COLORSPACE_SMPTE170M,
		.ycbcr_enc = V4L2_YCBCR_ENC_601,
		.quantization = V4L2_QUANTIZATION_LIM_RANGE,
		.xfer_func = V4L2_XFER_FUNC_709,
	};
}

static int esp32s31_camera_querycap(struct file *file, void *priv,
				    struct v4l2_capability *cap)
{
	strscpy(cap->driver, "esp32s31-camera", sizeof(cap->driver));
	strscpy(cap->card, "ESP32-S31 DVP", sizeof(cap->card));
	return 0;
}

static int esp32s31_camera_enum_fmt(struct file *file, void *priv,
				    struct v4l2_fmtdesc *format)
{
	if (format->index)
		return -EINVAL;
	format->pixelformat = V4L2_PIX_FMT_UYVY;
	return 0;
}

static int esp32s31_camera_get_fmt(struct file *file, void *priv,
				   struct v4l2_format *format)
{
	esp32s31_camera_format(format);
	return 0;
}

static int esp32s31_camera_set_fmt(struct file *file, void *priv,
				   struct v4l2_format *format)
{
	struct esp32s31_camera *cam = video_drvdata(file);

	if (vb2_is_busy(&cam->queue))
		return -EBUSY;
	esp32s31_camera_format(format);
	return 0;
}

static int esp32s31_camera_enum_size(struct file *file, void *priv,
				    struct v4l2_frmsizeenum *size)
{
	if (size->index || size->pixel_format != V4L2_PIX_FMT_UYVY)
		return -EINVAL;
	size->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	size->discrete.width = CAM_WIDTH;
	size->discrete.height = CAM_HEIGHT;
	return 0;
}

static int esp32s31_camera_enum_interval(struct file *file, void *priv,
					struct v4l2_frmivalenum *interval)
{
	if (interval->index || interval->pixel_format != V4L2_PIX_FMT_UYVY ||
	    interval->width != CAM_WIDTH || interval->height != CAM_HEIGHT)
		return -EINVAL;
	interval->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	interval->discrete.numerator = 1;
	interval->discrete.denominator = 15;
	return 0;
}

static int esp32s31_camera_enum_input(struct file *file, void *priv,
				      struct v4l2_input *input)
{
	struct esp32s31_camera *cam = video_drvdata(file);

	if (input->index)
		return -EINVAL;
	strscpy(input->name, "DVP sensor", sizeof(input->name));
	input->type = V4L2_INPUT_TYPE_CAMERA;
	input->capabilities = 0;
	input->status = cam->sensor ? 0 : V4L2_IN_ST_NO_SIGNAL;
	return 0;
}

static int esp32s31_camera_get_input(struct file *file, void *priv, unsigned int *index)
{
	*index = 0;
	return 0;
}

static int esp32s31_camera_set_input(struct file *file, void *priv, unsigned int index)
{
	return index ? -EINVAL : 0;
}

static const struct v4l2_ioctl_ops esp32s31_camera_ioctl_ops = {
	.vidioc_querycap = esp32s31_camera_querycap,
	.vidioc_enum_fmt_vid_cap = esp32s31_camera_enum_fmt,
	.vidioc_g_fmt_vid_cap = esp32s31_camera_get_fmt,
	.vidioc_try_fmt_vid_cap = esp32s31_camera_get_fmt,
	.vidioc_s_fmt_vid_cap = esp32s31_camera_set_fmt,
	.vidioc_enum_framesizes = esp32s31_camera_enum_size,
	.vidioc_enum_frameintervals = esp32s31_camera_enum_interval,
	.vidioc_enum_input = esp32s31_camera_enum_input,
	.vidioc_g_input = esp32s31_camera_get_input,
	.vidioc_s_input = esp32s31_camera_set_input,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations esp32s31_camera_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

static int esp32s31_camera_bound(struct v4l2_async_notifier *notifier,
				 struct v4l2_subdev *sensor,
				 struct v4l2_async_connection *connection)
{
	struct esp32s31_camera *cam = container_of(notifier,
					struct esp32s31_camera, notifier);
	int ret;

	mutex_lock(&cam->lock);
	ret = media_create_pad_link(&sensor->entity, 0, &cam->video.entity, 0,
				    MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	if (!ret)
		cam->sensor = sensor;
	mutex_unlock(&cam->lock);
	return ret;
}

static void esp32s31_camera_unbind(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *sensor,
				   struct v4l2_async_connection *connection)
{
	struct esp32s31_camera *cam = container_of(notifier,
					struct esp32s31_camera, notifier);

	mutex_lock(&cam->lock);
	cam->sensor = NULL;
	if (cam->streaming) {
		esp32s31_camera_halt(cam);
		esp32s31_camera_return_buffers(cam, VB2_BUF_STATE_ERROR);
		vb2_queue_error(&cam->queue);
	}
	mutex_unlock(&cam->lock);
}

static const struct v4l2_async_notifier_operations esp32s31_camera_notify_ops = {
	.bound = esp32s31_camera_bound,
	.unbind = esp32s31_camera_unbind,
};

static void esp32s31_camera_release(struct v4l2_device *v4l2)
{
	struct esp32s31_camera *cam = container_of(v4l2, struct esp32s31_camera, v4l2);

	media_entity_cleanup(&cam->video.entity);
	media_device_cleanup(&cam->media);
	mutex_destroy(&cam->lock);
	kfree(cam);
}

static int esp32s31_camera_probe(struct platform_device *pdev)
{
	static const char * const clock_names[] = { "apb", "sys", "core", "cam" };
	struct device *dev = &pdev->dev;
	struct v4l2_fwnode_endpoint endpoint = { .bus_type = V4L2_MBUS_PARALLEL };
	struct v4l2_async_connection *connection;
	struct reset_control *reset;
	struct fwnode_handle *ep;
	struct esp32s31_camera *cam;
	unsigned int i;
	int ret;

	cam = kzalloc_obj(*cam);
	if (!cam)
		return -ENOMEM;
	cam->dev = dev;
	mutex_init(&cam->lock);
	spin_lock_init(&cam->qlock);
	INIT_LIST_HEAD(&cam->buffers);
	INIT_DELAYED_WORK(&cam->watchdog, esp32s31_camera_watchdog);
	cam->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cam->regs)) {
		ret = PTR_ERR(cam->regs);
		goto free_cam;
	}
	for (i = 0; i < ARRAY_SIZE(clock_names); i++)
		cam->clocks[i].id = clock_names[i];
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(cam->clocks), cam->clocks);
	if (ret)
		goto free_cam;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(cam->clocks), cam->clocks);
	if (ret)
		goto free_cam;
	reset = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(reset)) {
		ret = PTR_ERR(reset);
		goto clocks_off;
	}
	ret = reset_control_assert(reset);
	if (!ret)
		ret = reset_control_deassert(reset);
	if (ret)
		goto clocks_off;
	cam->dma = dma_request_chan(dev, "rx");
	if (IS_ERR(cam->dma)) {
		ret = dev_err_probe(dev, PTR_ERR(cam->dma), "receive DMA unavailable\n");
		goto clocks_off;
	}
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		goto release_dma;
	ep = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	ret = ep ? v4l2_fwnode_endpoint_parse(ep, &endpoint) : -EINVAL;
	if (ret)
		goto put_endpoint;
	if (endpoint.bus.parallel.bus_width != 8) {
		ret = -EINVAL;
		goto put_endpoint;
	}
	if (endpoint.bus.parallel.flags & V4L2_MBUS_VSYNC_ACTIVE_LOW)
		cam->polarity |= CAM_VSYNC_INV;
	if (endpoint.bus.parallel.flags & V4L2_MBUS_HSYNC_ACTIVE_LOW)
		cam->polarity |= CAM_DE_INV;
	if (endpoint.bus.parallel.flags & V4L2_MBUS_PCLK_SAMPLE_FALLING)
		cam->polarity |= CAM_PCLK_INV;

	cam->media.dev = dev;
	strscpy(cam->media.model, "ESP32-S31 DVP", sizeof(cam->media.model));
	media_device_init(&cam->media);
	cam->v4l2.mdev = &cam->media;
	cam->v4l2.release = esp32s31_camera_release;
	ret = v4l2_device_register(dev, &cam->v4l2);
	if (ret)
		goto cleanup_media;
	cam->queue = (struct vb2_queue) {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		/* Imported DMA-BUF needs attachment retention on DMA stop failure. */
		.io_modes = VB2_MMAP,
		.drv_priv = cam,
		.buf_struct_size = sizeof(struct esp32s31_camera_buffer),
		.ops = &esp32s31_camera_queue_ops,
		.mem_ops = &vb2_dma_sg_memops,
		.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC,
		.lock = &cam->lock,
		.dev = dev,
		.dma_dir = DMA_FROM_DEVICE,
		.min_queued_buffers = 2,
	};
	ret = vb2_queue_init(&cam->queue);
	if (ret)
		goto unregister_v4l2;
	cam->video = (struct video_device) {
		.v4l2_dev = &cam->v4l2,
		.fops = &esp32s31_camera_fops,
		.ioctl_ops = &esp32s31_camera_ioctl_ops,
		.release = video_device_release_empty,
		.lock = &cam->lock,
		.queue = &cam->queue,
		.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING,
	};
	strscpy(cam->video.name, "ESP32-S31 DVP", sizeof(cam->video.name));
	video_set_drvdata(&cam->video, cam);
	cam->pad.flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&cam->video.entity, 1, &cam->pad);
	if (ret)
		goto unregister_v4l2;
	ret = video_register_device(&cam->video, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto unregister_v4l2;
	ret = media_device_register(&cam->media);
	if (ret)
		goto unregister_video;
	v4l2_async_nf_init(&cam->notifier, &cam->v4l2);
	connection = v4l2_async_nf_add_fwnode_remote(&cam->notifier, ep,
						   struct v4l2_async_connection);
	fwnode_handle_put(ep);
	ep = NULL;
	if (IS_ERR(connection)) {
		ret = PTR_ERR(connection);
		ep = NULL;
		goto cleanup_notifier;
	}
	cam->notifier.ops = &esp32s31_camera_notify_ops;
	ret = v4l2_async_nf_register(&cam->notifier);
	if (ret)
		goto cleanup_notifier;
	platform_set_drvdata(pdev, cam);
	dev_info(dev, "DVP capture registered as %s\n", video_device_node_name(&cam->video));
	return 0;

cleanup_notifier:
	v4l2_async_nf_cleanup(&cam->notifier);
	media_device_unregister(&cam->media);
unregister_video:
	video_unregister_device(&cam->video);
	/* video_unregister_device drops only the video device's reference. */
	ep = NULL;
unregister_v4l2:
	v4l2_device_unregister(&cam->v4l2);
	/* Remaining cleanup below still needs the platform-owned reference. */
	fwnode_handle_put(ep);
	dma_release_channel(cam->dma);
	clk_bulk_disable_unprepare(ARRAY_SIZE(cam->clocks), cam->clocks);
	v4l2_device_put(&cam->v4l2);
	return ret;
cleanup_media:
	media_device_cleanup(&cam->media);
put_endpoint:
	fwnode_handle_put(ep);
release_dma:
	dma_release_channel(cam->dma);
clocks_off:
	clk_bulk_disable_unprepare(ARRAY_SIZE(cam->clocks), cam->clocks);
free_cam:
	mutex_destroy(&cam->lock);
	kfree(cam);
	return ret;
}

static void esp32s31_camera_remove(struct platform_device *pdev)
{
	struct esp32s31_camera *cam = platform_get_drvdata(pdev);

	mutex_lock(&cam->lock);
	cam->removing = true;
	esp32s31_camera_stop(&cam->queue);
	vb2_queue_error(&cam->queue);
	mutex_unlock(&cam->lock);
	cancel_delayed_work_sync(&cam->watchdog);
	v4l2_async_nf_unregister(&cam->notifier);
	v4l2_async_nf_cleanup(&cam->notifier);
	video_unregister_device(&cam->video);
	v4l2_device_disconnect(&cam->v4l2);
	v4l2_device_unregister(&cam->v4l2);
	media_device_unregister(&cam->media);
	dma_release_channel(cam->dma);
	cam->dma = NULL;
	clk_bulk_disable_unprepare(ARRAY_SIZE(cam->clocks), cam->clocks);
	v4l2_device_put(&cam->v4l2);
}

static const struct of_device_id esp32s31_camera_ids[] = {
	{ .compatible = "esp,esp32s31-camera" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_camera_ids);

static struct platform_driver esp32s31_camera_driver = {
	.probe = esp32s31_camera_probe,
	.remove = esp32s31_camera_remove,
	.driver = {
		.name = "esp32s31-camera",
		.of_match_table = esp32s31_camera_ids,
	},
};
module_platform_driver(esp32s31_camera_driver);

MODULE_DESCRIPTION("ESP32-S31 DVP camera capture");
MODULE_LICENSE("GPL");
