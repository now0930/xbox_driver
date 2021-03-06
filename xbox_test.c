#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/usb/input.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/usb/input.h>
#include <linux/slab.h>

#define USB_XBOX_MINOR_BASE	120
#define XPAD_PKT_LEN 20 
#define XPAD_OUT_CMD_IDX	0
#define XPAD_OUT_FF_IDX		1
#define XPAD_OUT_LED_IDX	(1 + IS_ENABLED(CONFIG_JOYSTICK_XPAD_FF))
#define XPAD_NUM_OUT_PACKETS	(1 + \
				 IS_ENABLED(CONFIG_JOYSTICK_XPAD_FF) + \
				 IS_ENABLED(CONFIG_JOYSTICK_XPAD_LEDS))


static DEFINE_IDA(xpad_pad_seq);

//led command용 packet
struct xpad_output_packet {
	u8 data[XPAD_PKT_LEN];
	u8 len;
	bool pending;
};

//사용할 구조체 선언.
struct usb_xpad{
	struct input_dev *dev;		/* input device interface */
	struct usb_interface *intf;	/* usb interface */
	struct usb_device *udev;	/* usb device */
	__u8    irq_in_endpointAddr;    /* interrupt in address*/
	__u8    irq_out_endpointAddr;   /* interrupt out address*/
	struct usb_endpoint_descriptor *endpoint_in, *endpoint_out;

	signed char *idata;		/* input data */
	dma_addr_t idata_dma;
	unsigned char *odata;		/* output data */
	dma_addr_t odata_dma;
	struct urb *irq_in;		/* urb for interrupt in report*/
	struct urb *irq_out;		/* led controle용 urb*/
	struct usb_anchor irq_out_anchor;
	struct work_struct work;	/* init/remove device from callback */
	char phys[64];			/* physical device path */
	struct xpad_led *led;		/* led*/
	struct xpad_output_packet led_command[XPAD_NUM_OUT_PACKETS];	/*led command*/
	spinlock_t odata_lock;
	spinlock_t idata_lock;
	bool irq_out_active;            /* we must not use an active URB */
	int pad_nr;			// order
	char *name;			/* device name*/
	int last_out_packet;
	bool input_created;		/* input was created? */
	bool mouse_mode;		/* xbox 360 as mouse, false: joystick, true: mouse*/
	int r_edge;		/*edge filter, 0b100이면 동작*/


};

static const struct xpad_device{
	char *name;
}	
xpad_device[]={
	{"xbox360 test driver"}
};

#if defined(CONFIG_JOYSTICK_XPAD_LEDS)
#include <linux/leds.h>
#include <linux/idr.h>

struct xpad_led {
	char name[16];
	struct led_classdev led_cdev;
	struct usb_xpad *xpad;
};
#endif


static int init_output(struct usb_xpad *xpad,
		struct usb_endpoint_descriptor *ep_irq_out);
static int init_input(struct usb_xpad *xpad);
static void deinit_input(struct usb_xpad *xpad);

static int xpad_led_probe(struct usb_xpad *xpad);
static void xpad_led_disconnect(struct usb_xpad *xpad);
static void xpad_irq_outfn(struct urb *urb);
static bool xpad_prepare_next_out_packet(struct usb_xpad *xpad);
static void xpad_send_led_command(struct usb_xpad *xpad, int command);


static void xpad_irq_outfn(struct urb *urb){
	struct usb_xpad *xpad = urb->context;
	struct device *dev = &xpad->intf->dev;
	int status = urb->status;
	int error;
	unsigned long flags;

	spin_lock_irqsave(&xpad->odata_lock, flags);
	switch (status){
		case 0:
			/* success */
			//pr_info("%s: submit completed\n",__func__);
			//무제한으로 실행하지 않도록 방지
			xpad->irq_out_active = xpad_prepare_next_out_packet(xpad);
			break;
		case -ECONNRESET:
		case -ENOENT:
		case -ESHUTDOWN:
			/* this urb is terminated, clean up */
			dev_dbg(dev, "%s - urb shutting down with status: %d\n",
					__func__, status);
			xpad->irq_out_active = false;
			break;

		default:
			dev_dbg(dev, "%s - nonzero urb status received: %d\n",
					__func__, status);
			break;
	}
	if (xpad->irq_out_active) {
		usb_anchor_urb(urb, &xpad->irq_out_anchor);
		error = usb_submit_urb(urb, GFP_ATOMIC);
		if (error) {
			dev_err(dev,
					"%s - usb_submit_urb failed with result %d\n",
					__func__, error);
			usb_unanchor_urb(urb);
			xpad->irq_out_active = false;
		}
	}
	spin_unlock_irqrestore(&xpad->odata_lock, flags);

}
static void xpad360_process_packet(struct usb_xpad *xpad, struct input_dev *dev,
				   u16 cmd, unsigned char *data)
{
	int rel_x, rel_y; /* relative movement.to scale*/
	rel_x=0;
	rel_y=0;
	if (data[0] != 0x00)
		return;


	input_report_key(dev, BTN_BACK,		data[2] & 0x20);
	input_report_key(dev, BTN_START,	data[2] & 0x10);

	if(!xpad->mouse_mode){
		/* buttons A,B,X,Y,TL,TR and MODE */
		input_report_key(dev, BTN_Y,		data[3] & 0x40);
		input_report_key(dev, BTN_X,		data[3] & 0x80);
		input_report_key(dev, BTN_A,		data[3] & 0x10);
		input_report_key(dev, BTN_B,		data[3] & 0x20);

		input_report_key(dev, BTN_THUMBL,	data[3] & 0x01);
		input_report_key(dev, BTN_THUMBR,	data[3] & 0x02);

		input_report_key(dev, BTN_TRIGGER_HAPPY1,data[2] & 0x01);
		input_report_key(dev, BTN_TRIGGER_HAPPY2,data[2] & 0x02);
		input_report_key(dev, BTN_TRIGGER_HAPPY3,data[2] & 0x04);
		input_report_key(dev, BTN_TRIGGER_HAPPY4,data[2] & 0x08);
		input_report_abs(dev, ABS_Z, data[4]);
		input_report_abs(dev, ABS_RZ, data[5]);
		

		/* set up axises*/
		input_report_abs(dev, ABS_X, (__s16) le16_to_cpup((__le16 *)(data + 6)));
		input_report_abs(dev, ABS_Y, (__s16) le16_to_cpup((__le16 *)(data + 8)));
		input_report_abs(dev, ABS_RX,(__s16) le16_to_cpup((__le16 *)(data + 10)));
		input_report_abs(dev, ABS_RY,(__s16) le16_to_cpup((__le16 *)(data + 12)));


	}

	else{
		rel_x = (__s16) le16_to_cpup((__le16 *)(data + 6))/2000;
		rel_y = -(__s16) le16_to_cpup((__le16 *)(data + 8))/2000;


		input_report_key(dev, BTN_LEFT, xpad->idata[3] & 0x10);
		input_report_key(dev, BTN_RIGHT, xpad->idata[3] & 0x20);
		input_report_rel(dev, REL_X, rel_x);
		input_report_rel(dev, REL_Y, rel_y);
	}
	input_sync(dev);

	/* rising edge, falling edge 두 번 동작*/
	if (data[3] & 0x04){		/*xbox lamp 버튼*/
		xpad->r_edge = xpad->r_edge << 1;
		//pr_info("%s: hit x button \n",__func__);
		//pr_info("value: %x\n", xpad->r_edge);
	}

}


static void mouse_change(struct work_struct *work)
{
	struct usb_xpad *xpad = container_of(work, struct usb_xpad, work);
	unsigned char *data;
	int *edge;
	data = xpad->idata;
	edge = &xpad->r_edge;

	if( *edge==0b100 ){
		/* 과거 기록을 보고 모드 변환*/
		xpad->mouse_mode = !xpad->mouse_mode;
		*edge = 1;
	}

	/* 특정 수가 넘어가면 다시 초기화*/
	if ( *edge > 0b100){
		*edge = 1;
	}


	/* Lamp 버튼을 모드 체인지로 사용*/
	if(xpad->mouse_mode){
		led_set_brightness(&xpad->led->led_cdev, 10);
	}
	else
		led_set_brightness(&xpad->led->led_cdev, (xpad->pad_nr%4) + 6);

}



static void xpad_irq_infn(struct urb *urb)
{
	struct usb_xpad *xpad = urb->context;
	struct device *dev = &xpad->intf->dev;
	int retval, status;

	status = urb->status;
	/* try */
	//pr_info("%s: submit tried\n",__func__);


	switch (status) {
		case 0:
			/* success */
			//pr_info("%s: submit suceed\n",__func__);
			break;
		case -ECONNRESET:
		case -ENOENT:
		case -ESHUTDOWN:
			/* this urb is terminated, clean up */
			dev_dbg(dev, "%s - urb shutting down with status: %d\n",
					__func__, status);
			return;
		default:
			dev_dbg(dev, "%s - nonzero urb status received: %d\n",
					__func__, status);
			goto exit;
	}

	xpad360_process_packet(xpad, xpad->dev, 0, xpad->idata);
	schedule_work(&xpad->work);

exit:
	retval = usb_submit_urb(urb, GFP_ATOMIC);
	if (retval)
		dev_err(dev, "%s - usb_submit_urb failed with result %d\n",
				__func__, retval);
}



//static void usb_xpad_irq(struct urb *urb){
//	struct input_dev *dev;
//	signed char *data;
//	struct usb_xpad *xpad;
//	int status;
//	dev = myPad->dev;
//	xpad = urb->context;
//	data = xpad->idata;
//	//pr_info("test\n");
//
//	switch(urb->status){
//		case 0:			/* success */
//			break;
//		case -ECONNRESET:	/* unlink */
//		case -ENOENT:
//		case -ESHUTDOWN:
//			return;
//			/* -EPIPE:  should clear the halt */
//		default:		/* error */
//			goto resubmit;
//	}
//	input_report_key(dev, BTN_A, data[0] & 0x01);
//	input_report_key(dev, BTN_B, data[0] & 0x02);
//	input_report_key(dev, BTN_X, data[0] & 0x04);
//	input_report_key(dev, BTN_Y, data[0] & 0x08);
//	input_report_key(dev, BTN_START, data[1] & 0x10);
//	input_report_key(dev, BTN_SELECT, data[1] & 0x12);
//	input_report_key(dev, BTN_THUMBL, data[1] & 0x14);
//	input_report_key(dev, BTN_THUMBR, data[1] & 0x18);
//	input_sync(dev);
//	//usb_submit_urb(urb, GFP_KERNEL);
//resubmit:
//	status = usb_submit_urb(urb, GFP_ATOMIC);
//	if(status)
//		dev_err(&dev->dev,"error\n");
//}
static ssize_t xbox_read(struct file *file, char *buffer, size_t count,
		loff_t *ppos)
{
	pr_info("%s: device was read.\n", __func__);
	return 0;
}
static ssize_t xbox_write(struct file *file, const char *user_buffer,
		size_t count, loff_t *ppos)
{
	pr_info("%s: device was written.\n", __func__);

	return 0;
}
static int xbox_open(struct inode *inode, struct file *file)
{
	pr_info("%s: xbox was opened\n", __func__);
	return 0;
}
static int xbox_release(struct inode *inode, struct file *file)
{
	pr_info("%s: xbox was released\n", __func__);
	return 0;
}
static int xbox_flush(struct file *file, fl_owner_t id)
{
	pr_info("%s: xbox was flushed\n", __func__);
	return 0;
}
static const struct file_operations xbox360_fops = {
	.owner =	THIS_MODULE,
	.read =		xbox_read,
	.write =	xbox_write,
	.open =		xbox_open,
	.release =	xbox_release,
	.flush =	xbox_flush,
	.llseek =	noop_llseek,
};
/*
 * usb class driver info in order to get a minor number from the usb core,
 * and to have the device registered with the driver core
 */
static struct usb_class_driver xbox_class = {
	///dev에 아래 이름 +숫자로 파일을 만듦.
	//dev에길이에 제한이 있음
	/*
	pi@raspberrypi:/dev $ ls now0930xbox* -l
	crw------- 1 root root 180, 1 Jan  3 10:01 now0930xbox0
	crw------- 1 root root 180, 2 Jan  3 10:01 now0930xbox1
	*/
	.name =		"now0930xbox%d",
	.fops =		&xbox360_fops,
	.minor_base =	USB_XBOX_MINOR_BASE,
};
static struct usb_driver xpad_test_driver;
static const struct usb_device_id xpad_table[] = {
	//{ USB_INTERFACE_INFO('X', 'B', 0) },	/* X-Box USB-IF not approved class */
	//XPAD_XBOX360_VENDOR(0x045e),		/* Microsoft X-Box 360 controllers */
	{USB_DEVICE(0x045e, 0x028e)},
	{ }
};
static const signed short xpad_common_btn[] = {
	BTN_A, BTN_B, BTN_X, BTN_Y,			/* "analog" buttons */
	BTN_START, BTN_BACK, BTN_THUMBL, BTN_THUMBR,	/* start/back/sticks */
	-1						/* terminating entry */
};

/* used when triggers are mapped to buttons */
static const signed short xpad_btn_triggers[] = {
	ABS_Z, ABS_RZ,		/* triggers left/right */
	-1
};

/* used when dpad is mapped to buttons */
static const signed short xpad_btn_pad[] = {
	BTN_TRIGGER_HAPPY1, BTN_TRIGGER_HAPPY2,		/* d-pad left, right */
	BTN_TRIGGER_HAPPY3, BTN_TRIGGER_HAPPY4,		/* d-pad up, down */
	-1				/* terminating entry */
};



static const signed short xpad_abs[] = {
	ABS_X, ABS_Y,		/* left stick */
	ABS_RX, ABS_RY,		/* right stick */
	-1			/* terminating entry */
};


static const signed short xpad_rel[] = {

	REL_X, REL_Y,		/*mouse emulate*/
	-1
};





MODULE_DEVICE_TABLE(usb, xpad_table);


static int xpad_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_xpad *xpad;
	//struct usb_device *udev;
	//struct usb_endpoint_descriptor *ep_irq_in,*ep_irq_out;
	//struct usb_host_interface *intf_tmp;
	struct usb_endpoint_descriptor *itrp_in, *itrp_out;
	int retval;


	//endpoint 2번 한개만 등록..
	//여기 없으면 인터록으로 설정 가능한 2개 device를 등록함.
	if (intf->cur_altsetting->desc.bNumEndpoints != 2)
		return -ENODEV;

	//register
	//초기화
	//kzalloc으로 0으로 초기화
	xpad = kzalloc(sizeof(struct usb_xpad), GFP_KERNEL);
	if (!xpad)
		return -ENOMEM;


	xpad->name=xpad_device[0].name;




	//init work queue
	INIT_WORK(&xpad->work, mouse_change); 
	xpad->mouse_mode = false;
	xpad->r_edge = 1;


	xpad->udev = usb_get_dev(interface_to_usbdev(intf));

	/* usb make path.*/
	usb_make_path(xpad->udev, xpad->phys, sizeof(xpad->phys));
	strlcat(xpad->phys, "/input0", sizeof(xpad->phys));
	pr_info("phys: %s\n", xpad->phys);

	//xpad->odata_serial = 0;
	xpad->intf = usb_get_intf(intf);
	pr_info("xpad is %p, xpad->udev is %p\n", xpad, xpad->udev);
	pr_info("interface is %p\n", xpad->intf);
	pr_info("cur_altsetting is %d\n",intf->cur_altsetting->desc.bNumEndpoints);
	

	/* set up the endpoint information */
	/* use only the first bulk-in and bulk-out endpoints */
	retval = usb_find_common_endpoints(intf->cur_altsetting,NULL,NULL,&itrp_in, &itrp_out);
	//retval = usb_find_last_int_out_endpoint(intf->cur_altsetting,&itrp_out);

	xpad->endpoint_in = itrp_in;
	xpad->endpoint_out = itrp_out;

	if (retval) {
		dev_err(&intf->dev,"Could not find both interrupt-in and interrpt-out endpoints\n");
		pr_err("error %d\n",retval);
		goto error;
	}
	dev_info(&intf->dev,"interrupt in, out found. %p, %p\n",itrp_in, itrp_out);


	//save dev to interface.
	usb_set_intfdata(intf, xpad);
	retval = usb_register_dev(intf, &xbox_class);
	if(retval){
		dev_err(&intf->dev,
				"Not able to get a minor for this device.\n");
		usb_set_intfdata(intf, NULL);
		goto error;
	}

	dev_info(&intf->dev, "usb xbox360 driver was registerd\n");


	//led 등록
	init_output(xpad, itrp_out);
	retval = xpad_led_probe(xpad);

	retval = init_input(xpad);




	return 0;
error:
	kfree(xpad);
	return retval;


}

static int xpad_open(struct input_dev *dev)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);
	int retval;
	
	retval = usb_submit_urb(xpad->irq_in, GFP_KERNEL);
	if( retval)
	{
		return retval;
	}
	pr_info("%s: opened, xpad is %p\n",__func__, xpad);

	return 0;
}


static void xpad_close(struct input_dev *dev)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);
	usb_kill_urb(xpad->irq_in);
	flush_work(&xpad->work);
	pr_info("%s: closed\n",__func__);


}


static int init_output(struct usb_xpad *xpad,
		struct usb_endpoint_descriptor *ep_irq_out){
	//http://www.makelinux.net/ldd3/chp-13-sect-3.shtml
	int retval, pipe;

	xpad->odata = usb_alloc_coherent(xpad->udev, XPAD_PKT_LEN, GFP_KERNEL, &xpad->odata_dma); 
	pr_info("dma output address %p with size %d, point to %p was allocated\n", &xpad->odata_dma, XPAD_PKT_LEN, xpad->odata);
	pr_info("actual address is %p\n",&xpad->odata_dma);

	init_usb_anchor(&xpad->irq_out_anchor);
	xpad->irq_out = usb_alloc_urb(0, GFP_KERNEL);

	if(!xpad->irq_out){
		retval = -ENOMEM;
		goto err_free_coherent;
	}
	pr_info("urb was allocated");


	//urb settup
	pipe = usb_sndintpipe(xpad->udev, xpad->endpoint_out->bEndpointAddress);
	pr_info("xpad->endpoint_out->bInterval is %d\n", xpad->endpoint_out->bInterval);

	usb_fill_int_urb(xpad->irq_out, xpad->udev, pipe,
			xpad->odata, XPAD_PKT_LEN,
			xpad_irq_outfn, xpad, xpad->endpoint_out->bInterval);
	xpad->irq_out->transfer_dma = xpad->odata_dma;
	xpad->irq_out->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

		
	pr_info("XPAD_OUT_LED_IDX: %d",XPAD_OUT_LED_IDX);
	pr_info("XPAD_NUM_OUT_PACKETS: %d",XPAD_NUM_OUT_PACKETS);

	return 0;


err_free_coherent:
	usb_free_coherent(xpad->udev, XPAD_PKT_LEN, xpad->odata, xpad->odata_dma);

	return retval;
}
static int init_input(struct usb_xpad *xpad)
{
	int retval, pipe;
	int i;
	struct input_dev *input_dev;
	input_dev = input_allocate_device();

	if (!input_dev)
	{
		return -ENOMEM;

	}
	xpad->dev = input_dev;

	/* name을 설정해야 xserver에 기록됨*/
	input_dev->name = xpad->name;
	input_dev->phys = xpad->phys;

	xpad->idata = usb_alloc_coherent(xpad->udev, XPAD_PKT_LEN, GFP_KERNEL, &xpad->idata_dma); 

	xpad->irq_in = usb_alloc_urb(0, GFP_KERNEL);
	pr_info("input urb allocated\n");

	//urb settup
	pipe = usb_rcvintpipe(xpad->udev, xpad->endpoint_in->bEndpointAddress);
	usb_fill_int_urb(xpad->irq_in, xpad->udev, pipe,
			xpad->idata, XPAD_PKT_LEN,
			xpad_irq_infn, xpad, xpad->endpoint_in->bInterval);

	xpad->irq_in->transfer_dma = xpad->idata_dma;
	xpad->irq_in->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	input_set_drvdata(input_dev, xpad);
	input_dev->open = xpad_open;
	input_dev->close = xpad_close;

	/* set up standard buttons */
	for (i = 0; xpad_common_btn[i] >= 0; i++)
		input_set_capability(input_dev, EV_KEY, xpad_common_btn[i]);

	/* set up d pad*/
	for (i = 0; xpad_btn_pad[i] >= 0; i++)
		input_set_capability(input_dev, EV_KEY, xpad_btn_pad[i]);

	/* set up triggers*/
	for (i = 0; xpad_btn_triggers[i] >= 0; i++)
		input_set_abs_params(input_dev, xpad_btn_triggers[i],0,255,0,0);


	/* set up axis*/
	for (i = 0; xpad_abs[i] >= 0; i++){
		input_set_abs_params(input_dev, xpad_abs[i],0,255,0,0);
	}


	/* set up mouse relative coordinate */
	for (i = 0; xpad_rel[i] >= 0; i++){
		input_set_capability(input_dev, EV_REL, xpad_rel[i]);

	}

	/* work queue 대용 */
	input_set_capability(input_dev, EV_KEY, BTN_LEFT);
	input_set_capability(input_dev, EV_KEY, BTN_RIGHT);


	retval = input_register_device(xpad->dev);
	if( retval )
	{
		goto err_register;

	}
	xpad->input_created = true;
	pr_info("%s: executed\n",__func__);
	return 0;
err_register:
	return retval;
}

static void deinit_input(struct usb_xpad *xpad)
{
	pr_info("%s: deinited\n",__func__);
	if(xpad->input_created)
		input_unregister_device(xpad->dev);
	usb_free_urb(xpad->irq_in);
	usb_free_coherent(xpad->udev, XPAD_PKT_LEN, xpad->idata, xpad->idata_dma);
	pr_info("dma input address %p with size %d was freed\n", &xpad->idata_dma, XPAD_PKT_LEN);


}


static void free_output(struct usb_xpad *xpad)
{
	usb_free_urb(xpad->irq_out);
	usb_free_coherent(xpad->udev, XPAD_PKT_LEN, xpad->odata, xpad->odata_dma);
	pr_info("dma output address %p with size %d was freed\n", &xpad->odata_dma, XPAD_PKT_LEN);



}

/* Callers must hold xpad->odata_lock spinlock */
static bool xpad_prepare_next_out_packet(struct usb_xpad *xpad)
{
	struct xpad_output_packet *pkt, *packet = NULL;
	int i;

	for (i = 0; i < XPAD_NUM_OUT_PACKETS; i++) {
		if (++xpad->last_out_packet >= XPAD_NUM_OUT_PACKETS)
			xpad->last_out_packet = 0;

		pkt = &xpad->led_command[xpad->last_out_packet];
		if (pkt->pending) {
			dev_dbg(&xpad->intf->dev,
					"%s - found pending output packet %d\n",
					__func__, xpad->last_out_packet);
			packet = pkt;
			break;
		}
	}

	if (packet) {
		memcpy(xpad->odata, packet->data, packet->len);
		xpad->irq_out->transfer_buffer_length = packet->len;
		packet->pending = false;
		return true;
	}

	return false;
}




static int xpad_try_sending_next_output(struct usb_xpad *xpad)
{
	int retval;
	if (!xpad->irq_out_active && xpad_prepare_next_out_packet(xpad)){
		usb_anchor_urb(xpad->irq_out, &xpad->irq_out_anchor);
		retval= usb_submit_urb(xpad->irq_out, GFP_ATOMIC);
		if (retval) {
			//https://stackoverflow.com/questions/10006071/is-there-an-equvalent-for-perror-in-the-kernel
			dev_err(&xpad->intf->dev,"%s - usb_submit_urb failed with %pe\n",
					__func__, ERR_PTR(retval));

			usb_unanchor_urb(xpad->irq_out);
			return -EIO;
		}
		else
			//pr_info("%s: completed\n",__func__);
		xpad->irq_out_active = true;
	}

	return 0;
}



static void xpad_send_led_command(struct usb_xpad *xpad, int command)
{
	unsigned long flags;
	struct xpad_output_packet *packet;
	packet = &xpad->led_command[XPAD_OUT_LED_IDX];
	command %= 16;
	spin_lock_irqsave(&xpad->odata_lock, flags);
	packet->data[0] = 0x01;
	packet->data[1] = 0x03;
	packet->data[2] = command;
	packet->len = 3;
	packet->pending = true;
	xpad_try_sending_next_output(xpad);
	spin_unlock_irqrestore(&xpad->odata_lock, flags);

}

static void xpad_led_set(struct led_classdev *led_cdev,
		enum led_brightness value)
{
	struct xpad_led *xpad_led = container_of(led_cdev,
			struct xpad_led, led_cdev);

	xpad_send_led_command(xpad_led->xpad, value);
}




static int xpad_led_probe(struct usb_xpad *xpad)
{
	struct xpad_led *led;
	struct led_classdev *led_cdev;	//임시 변수
	int retval;
	led = kzalloc(sizeof(struct xpad_led), GFP_KERNEL);
	//ida 할당
	//pad_nr을 계속 증가..
	//led가 몇번에 연결되어 있는지 알 수 있음.
	//0~0x8,000,000 - 1까지 증가
	xpad->pad_nr = ida_simple_get(&xpad_pad_seq, 0, 0, GFP_KERNEL);
	if (xpad->pad_nr < 0){
		retval = xpad->pad_nr;
		goto free_mem;
	}
	//pr_info
	pr_info("xpad->nr is %d\n",xpad->pad_nr);

	//xpad로 접근하기 쉽게 기록
	xpad->led = led;
	snprintf(led->name, sizeof(led->name),"xpad_led%d", xpad->pad_nr);
	led->xpad = xpad;

	led_cdev = &led->led_cdev;
	led_cdev->name = led->name;
	led_cdev->brightness_set = xpad_led_set;
	led_cdev->flags = LED_CORE_SUSPENDRESUME;

	retval = led_classdev_register(&xpad->udev->dev, led_cdev);
	if (retval)
		goto err_free_id;

	led_set_brightness(&xpad->led->led_cdev, (xpad->pad_nr%4) + 6);
	return 0;

err_free_id:
	ida_simple_remove(&xpad_pad_seq, xpad->pad_nr);
	return retval;
free_mem:
	kfree(led);
	return retval;
}


static void xpad_stop_output(struct usb_xpad *xpad){
	if(!usb_wait_anchor_empty_timeout
			(&xpad->irq_out_anchor, 5000)){
		pr_info("%s anchord_urb was stopped\n",__func__);
		usb_kill_anchored_urbs(&xpad->irq_out_anchor);

	}

}



static void xpad_led_disconnect(struct usb_xpad *xpad){
	struct xpad_led *xpad_led = xpad->led;
	if(xpad_led){
		led_classdev_unregister(&xpad_led->led_cdev);
		ida_simple_remove(&xpad_pad_seq, xpad->pad_nr);
		kfree(xpad_led);
		pr_info("xpad led was unregistered\n");

	}

}


static void xpad_disconnect(struct usb_interface *intf)
{
	struct usb_xpad *xpad;
	xpad = usb_get_intfdata(intf);
	pr_info("xpad address is %p, intf is %p\n", xpad, intf);
	deinit_input(xpad);
	xpad_stop_output(xpad);
	xpad_led_disconnect(xpad);
	usb_deregister_dev(intf, &xbox_class);
	free_output(xpad);
	kfree(xpad);

	usb_set_intfdata(intf, NULL);
	pr_info("disconnected\n");

}
static int xpad_suspend(struct usb_interface *intf, pm_message_t message)
{
	pr_info("suspendes\n");
	return 0;
}
static int xpad_resume(struct usb_interface *intf)
{
	pr_info("resumed\n");
	return 0;
}


//메인 부분.
static struct usb_driver xpad_test_driver = {
	.name		= "xbox360",
	.probe		= xpad_probe,
	.disconnect	= xpad_disconnect,
	.suspend	= xpad_suspend,
	.resume		= xpad_resume,
	.id_table	= xpad_table,
};

module_usb_driver(xpad_test_driver);



MODULE_LICENSE("GPL");
MODULE_AUTHOR("now0930");
MODULE_DESCRIPTION("Hello, xbox pad!");
