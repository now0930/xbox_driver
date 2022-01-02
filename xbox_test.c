#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/usb/input.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/usb/input.h>
#include <linux/slab.h>
#define USB_XBOX_MINOR_BASE	1
//사용할 구조체 선언.
struct usb_xpad{
	struct input_dev *dev;		/* input device interface */
	struct usb_interface *intf;	/* usb interface */
	struct usb_device *udev;	/* usb device */
	__u8    irq_in_endpointAddr;    /* interrupt in address*/
	__u8    irq_out_endpointAddr;   /* interrupt out address*/
	struct usb_endpoint_descriptor *endpoint_in, *endpoint_out;
	signed char *data;		/* input data */
	dma_addr_t data_dma;
	struct urb *irq_in;		/* urb for interrupt in report*/
	struct work_struct work;	/* init/remove device from callback */
	char phys[64];			/* physical device path */

};
static struct usb_xpad *myPad;
static int xpad_init_input(struct usb_xpad *xpad);
static void xpad_deinit_input(struct usb_xpad *xpad);
static int xpad_open(struct input_dev *dev);
static void xpad_close(struct input_dev *dev);
static void usb_xpad_irq(struct urb *urb){
	struct input_dev *dev;
	signed char *data;
	struct usb_xpad *xpad;
	int status;
	dev = myPad->dev;
	xpad = urb->context;
	data = xpad->data;
	//pr_info("test\n");

	switch(urb->status){
		case 0:			/* success */
			break;
		case -ECONNRESET:	/* unlink */
		case -ENOENT:
		case -ESHUTDOWN:
			return;
			/* -EPIPE:  should clear the halt */
		default:		/* error */
			goto resubmit;
	}
	input_report_key(dev, BTN_A, data[0] & 0x01);
	input_report_key(dev, BTN_B, data[0] & 0x02);
	input_report_key(dev, BTN_X, data[0] & 0x04);
	input_report_key(dev, BTN_Y, data[0] & 0x08);
	input_report_key(dev, BTN_START, data[1] & 0x10);
	input_report_key(dev, BTN_SELECT, data[1] & 0x12);
	input_report_key(dev, BTN_THUMBL, data[1] & 0x14);
	input_report_key(dev, BTN_THUMBR, data[1] & 0x18);
	input_sync(dev);
	//usb_submit_urb(urb, GFP_KERNEL);
resubmit:
	status = usb_submit_urb(urb, GFP_ATOMIC);
	if(status)
		dev_err(&dev->dev,"error\n");
}
static ssize_t xbox_read(struct file *file, char *buffer, size_t count,
		loff_t *ppos)
{
	pr_info("device was read.\n");
	return 0;
}
static ssize_t xbox_write(struct file *file, const char *user_buffer,
		size_t count, loff_t *ppos)
{
	return 0;
}
static int xbox_open(struct inode *inode, struct file *file)
{
	pr_info("xbox was opened\n");
	return 0;
}
static int xbox_release(struct inode *inode, struct file *file)
{
	pr_info("xbox was released\n");
	return 0;
}
static int xbox_flush(struct file *file, fl_owner_t id)
{
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
	.name =		"xbox360 driver%d",
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
	BTN_START, BTN_SELECT, BTN_THUMBL, BTN_THUMBR,	/* start/back/sticks */
	-1						/* terminating entry */
};
MODULE_DEVICE_TABLE(usb, xpad_table);
static int xpad_init_input(struct usb_xpad *xpad)
{
	struct input_dev *input_dev;
	int i, error;
	int pipe, maxp;
	input_dev = input_allocate_device();
	if (!input_dev)
		return -ENOMEM;
	xpad->dev = input_dev;
	input_dev->name  = "xbox360";


	set_bit(EV_KEY, input_dev->evbit);
	set_bit(BTN_A, input_dev->keybit);


	usb_make_path(xpad->udev, xpad->phys, sizeof(xpad->phys));
	strlcat(xpad->phys, "/input0", sizeof(xpad->phys));
	usb_to_input_id(xpad->udev, &input_dev->id);
	input_dev->dev.parent = &xpad->intf->dev;

	/* set up standard buttons */
	for (i = 0; xpad_common_btn[i] >= 0; i++)
		input_set_capability(input_dev, EV_KEY, xpad_common_btn[i]);

	input_set_drvdata(input_dev, xpad);
	pr_info("chech here\n");

	input_dev->open = xpad_open;
	input_dev->close = xpad_close;

	pipe = usb_rcvintpipe(xpad->udev, xpad->irq_in_endpointAddr);
	maxp = usb_maxpacket(xpad->udev, pipe, usb_pipeout(pipe));
	usb_fill_int_urb(xpad->irq_in, xpad->udev,
			pipe, xpad->data,
			(maxp > 8 ? 8 : maxp),
			usb_xpad_irq, xpad, 
			xpad->endpoint_in->bInterval);
	xpad->irq_in->transfer_dma = xpad->data_dma;
	xpad->irq_in->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	error = input_register_device(input_dev);
	if (error)
		goto err_free_dev;
	pr_info("usb input was registered\n");
	return 0;	//return ok;
err_free_dev:
	input_free_device(input_dev);
	return error;
}
static void xpad_deinit_input(struct usb_xpad *xpad)
{
	pr_info("xpad is %p, ->dev is %p.\n",xpad, xpad->dev);
	if(xpad->dev)
		input_unregister_device(xpad->dev);
}

static int xpad_open(struct input_dev *dev)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);
	if (usb_submit_urb(xpad->irq_in, GFP_KERNEL))
		return -EIO;
	pr_info("device was opened\n");
	return 0;

}

static void xpad_close(struct input_dev *dev)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);
	usb_kill_urb(xpad->irq_in);
	pr_info("device was closed\n");

}

static int xpad_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_xpad *xpad;
	//struct usb_device *udev;
	//struct usb_endpoint_descriptor *ep_irq_in,*ep_irq_out;
	//struct usb_host_interface *intf_tmp;
	struct usb_endpoint_descriptor *itrp_in, *itrp_out;
	int retval;
	//register
	//초기화
	//kzalloc으로 0으로 초기화
	xpad = kzalloc(sizeof(struct usb_xpad), GFP_KERNEL);
	if (!xpad)
		return -ENOMEM;


	xpad->udev = usb_get_dev(interface_to_usbdev(intf));
	//xpad->odata_serial = 0;
	xpad->intf = usb_get_intf(intf);
	myPad = xpad;
	pr_info("xpad is %p, xpad->udev is %p\n", xpad, xpad->udev);
	pr_info("interface is %p\n", xpad->intf);


	/* set up the endpoint information */
	/* use only the first bulk-in and bulk-out endpoints */
	retval = usb_find_common_endpoints(intf->cur_altsetting,NULL,NULL,&itrp_in, &itrp_out);

	if (retval) {
		dev_err(&intf->dev,"Could not find both interrupt-in and interrpt-out endpoints\n");
		pr_err("error %d\n",retval);
		goto error;
	}
	dev_info(&intf->dev,"interrupt in, out found. %p, %p\n",itrp_in, itrp_out);

	//devie용 데이터 할당.
	//xpad->data = usb_alloc_coherent(udev, 8, GFP_ATOMIC, &xpad->data_dma);
	//if(!xpad->data)
	//		goto error;

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


	//register usb device

	return 0;
error:
	kfree(xpad);
err_free_in_urb:
	usb_free_urb(xpad->irq_in);

	return retval;

}
static void xpad_disconnect(struct usb_interface *intf)
{
	struct usb_xpad *xpad;
	xpad = usb_get_intfdata(intf);
	usb_set_intfdata(intf, NULL);
	pr_info("xpad address is %p, intf is %p\n", xpad, intf);
	//xpad_deinit_input(xpad);
	usb_deregister_dev(intf, &xbox_class);
	kfree(xpad);
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
