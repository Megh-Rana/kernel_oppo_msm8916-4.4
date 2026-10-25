ccflags-y += -I$(srctree)/drivers/usb/gadget/function -I$(srctree)/drivers/usb/gadget/udc -I$(srctree)/drivers/usb/gadget
#
# USB peripheral controller drivers
#
subdir-ccflags-$(CONFIG_USB_GADGET_DEBUG)	:= -DDEBUG
subdir-ccflags-$(CONFIG_USB_GADGET_VERBOSE)	+= -DVERBOSE_DEBUG
ccflags-y				+= -I$(srctree)/drivers/usb/gadget/udc

obj-$(CONFIG_USB_LIBCOMPOSITE)	+= libcomposite.o
libcomposite-y			:= usbstring.o config.o epautoconf.o
libcomposite-y			+= composite.o functions.o configfs.o u_f.o

obj-$(CONFIG_USB_GADGET)	+= udc/ function/ legacy/
obj-$(CONFIG_USB_G_ANDROID)	+= android.o
obj-y				+= debug.o
