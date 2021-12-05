/*
 * CanoKey QEMU device implementation.
 *
 * Copyright (c) 2021-2022 Canokeys.org <contact@canokeys.org>
 * Written by Hongren (Zenithal) Zheng <i@zenithal.me>
 *
 * This code is licensed under the Apache-2.0.
 */

#include "qemu/osdep.h"
#include <canokey-qemu.h>

#include "qemu/module.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "hw/usb.h"
#include "hw/qdev-properties.h"
#include "desc.h"
#include "canokey.h"

#define CANOKEY_EP_IN(ep) ((ep) & 0x7F)

#define CANOKEY_VENDOR_NUM     0x20a0
#define CANOKEY_PRODUCT_NUM    0x42d2

/*
 * placeholder, canokey-qemu implements its own usb desc
 * Namely we do not use usb_desc_handle_contorl
 */
enum {
    STR_MANUFACTURER = 1,
    STR_PRODUCT,
    STR_SERIALNUMBER
};

static const USBDescStrings desc_strings = {
    [STR_MANUFACTURER]     = "canokeys.org",
    [STR_PRODUCT]          = "CanoKey QEMU",
    [STR_SERIALNUMBER]     = "0"
};

static const USBDescDevice desc_device_canokey = {
    .bcdUSB                        = 0x0,
    .bMaxPacketSize0               = 16,
    .bNumConfigurations            = 0,
    .confs = NULL,
};

static const USBDesc desc_canokey = {
    .id = {
        .idVendor          = CANOKEY_VENDOR_NUM,
        .idProduct         = CANOKEY_PRODUCT_NUM,
        .bcdDevice         = 0x0100,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT,
        .iSerialNumber     = STR_SERIALNUMBER,
    },
    .full = &desc_device_canokey,
    .str  = desc_strings,
};


/* Implement canokey-qemu functions */
int canokey_emu_stall_ep(void *base, uint8_t ep)
{
    CanoKeyState *key = base;
    uint8_t ep_in = CANOKEY_EP_IN(ep); /* INTR IN has ep 129 */
    qemu_mutex_lock(&key->ep_in_mutex[ep_in]);
    key->ep_in_size[ep_in] = 0;
    key->ep_in_status[ep_in] = CANOKEY_EP_IN_STALL;
    qemu_mutex_unlock(&key->ep_in_mutex[ep_in]);
    return 0;
}

int canokey_emu_set_address(void *base, uint8_t addr)
{
    CanoKeyState *key = base;
    key->dev.addr = addr;
    return 0;
}

int canokey_emu_prepare_receive(
        void *base, uint8_t ep, uint8_t *pbuf, uint16_t size)
{
    CanoKeyState *key = base;
    /*
     * No mutex here because it is usually called by
     * canokey_emu_data_out (qemu thread), which already has mutex
     */
    key->ep_out[ep] = pbuf;
    key->ep_out_size[ep] = size;
    return 0;
}

int canokey_emu_transmit(
        void *base, uint8_t ep, const uint8_t *pbuf, uint16_t size)
{
    CanoKeyState *key = base;
    uint8_t ep_in = CANOKEY_EP_IN(ep); /* INTR IN has ep 129 */
    qemu_mutex_lock(&key->ep_in_mutex[ep_in]);
    memcpy(key->ep_in[ep_in], pbuf, size);
    key->ep_in_size[ep_in] = size;
    key->ep_in_status[ep_in] = CANOKEY_EP_IN_READY;
    qemu_mutex_unlock(&key->ep_in_mutex[ep_in]);
    return 0;
}

uint32_t canokey_emu_get_rx_data_size(void *base, uint8_t ep)
{
    CanoKeyState *key = base;
    return key->ep_out_size[ep];
}

static void *canokey_thread(void *arg)
{
    CanoKeyState *key = arg;

    while (true) {
        /* Wait signal */
        qemu_mutex_lock(&key->key_mutex);
        qemu_cond_wait(&key->key_cond, &key->key_mutex);
        qemu_mutex_unlock(&key->key_mutex);

        /* Exit thread check */
        if (key->stop_thread) {
            key->stop_thread = false;
            break;
        }

        canokey_emu_device_loop();
    }
    return NULL;
}

static void canokey_handle_reset(USBDevice *dev)
{
    CanoKeyState *key = CANOKEY(dev);
    for (int i = 0; i != CANOKEY_EP_NUM; ++i) {
        key->ep_in_status[i] = CANOKEY_EP_IN_WAIT;
        key->ep_in_pos[i] = 0;
    }
    canokey_emu_reset();
}

static void canokey_handle_control(USBDevice *dev, USBPacket *p,
               int request, int value, int index, int length, uint8_t *data)
{
    CanoKeyState *key = CANOKEY(dev);

    canokey_emu_setup(request, value, index, length);
    qemu_cond_signal(&key->key_cond);

    uint8_t ep_in = CANOKEY_EP_IN(p->ep->nr);
    uint32_t dir_in = request & DeviceRequest;
    if (!dir_in) {
        /* OUT */
        qemu_mutex_lock(&key->key_mutex);
        if (key->ep_out[0] != NULL) {
            memcpy(key->ep_out[0], data, length);
        }
        canokey_emu_data_out(p->ep->nr, data);
        qemu_cond_signal(&key->key_cond);
        qemu_mutex_unlock(&key->key_mutex);
    }

    /* IN */
    qemu_mutex_lock(&key->ep_in_mutex[ep_in]);
    if (key->ep_in_status[ep_in] == CANOKEY_EP_IN_WAIT) {
        p->status = USB_RET_NAK;
        qemu_mutex_unlock(&key->ep_in_mutex[ep_in]);
        return;
    }
    if (key->ep_in_status[ep_in] == CANOKEY_EP_IN_STALL) {
        p->status = USB_RET_STALL;
    }
    key->ep_in_status[ep_in] = CANOKEY_EP_IN_WAIT;
    memcpy(data, key->ep_in[ep_in], key->ep_in_size[ep_in]);
    p->actual_length = key->ep_in_size[ep_in];

    qemu_mutex_unlock(&key->ep_in_mutex[ep_in]);
}

static void canokey_handle_data(USBDevice *dev, USBPacket *p)
{
    CanoKeyState *key = CANOKEY(dev);

    uint8_t ep_in = CANOKEY_EP_IN(p->ep->nr);
    uint8_t ep_out = p->ep->nr;
    uint32_t in_len;
    switch (p->pid) {
    case USB_TOKEN_OUT:
        qemu_mutex_lock(&key->key_mutex);
        if (p->iov.size > key->ep_out_size[ep_out]) {
            /* unlikely we will reach here, but check still needed */
            p->status = USB_RET_NAK;
            qemu_mutex_unlock(&key->key_mutex);
            break;
        }
        usb_packet_copy(p, key->ep_out[ep_out], p->iov.size);
        key->ep_out_size[ep_out] = p->iov.size;
        canokey_emu_data_out(ep_out, NULL);
        qemu_cond_signal(&key->key_cond);
        qemu_mutex_unlock(&key->key_mutex);
        break;
    case USB_TOKEN_IN:
        qemu_mutex_lock(&key->ep_in_mutex[ep_in]);
        if (key->ep_in_pos[ep_in] == 0) {
            canokey_emu_data_in(ep_in);
            qemu_cond_signal(&key->key_cond);
            if (key->ep_in_status[ep_in] == CANOKEY_EP_IN_WAIT) {
                p->status = USB_RET_NAK;
                qemu_mutex_unlock(&key->ep_in_mutex[ep_in]);
                break;
            }
            if (key->ep_in_status[ep_in] == CANOKEY_EP_IN_STALL) {
                p->status = USB_RET_STALL;
            }
            key->ep_in_status[ep_in] = CANOKEY_EP_IN_WAIT;

            in_len = MIN(key->ep_in_size[ep_in], p->iov.size);
            usb_packet_copy(p, key->ep_in[ep_in], in_len);
            if (in_len < key->ep_in_size[ep_in]) {
                key->ep_in_pos[ep_in] = in_len;
            }
        } else {
            in_len = MIN(key->ep_in_size[ep_in] - key->ep_in_pos[ep_in],
                        p->iov.size);
            usb_packet_copy(p,
                    key->ep_in[ep_in] + key->ep_in_pos[ep_in], in_len);
            key->ep_in_pos[ep_in] += in_len;
            if (key->ep_in_pos[ep_in] == key->ep_in_size[ep_in]) {
                key->ep_in_pos[ep_in] = 0;
            }
        }
        qemu_mutex_unlock(&key->ep_in_mutex[ep_in]);
        break;
    default:
        p->status = USB_RET_STALL;
        break;
    }
}

static void canokey_realize(USBDevice *base, Error **errp)
{
    CanoKeyState *key = CANOKEY(base);

    if (key->file == NULL) {
        error_setg(errp, "You must provide file=/path/to/canokey-file");
        return;
    }

    usb_desc_init(base);

    /* Synchronization */
    qemu_cond_init(&key->key_cond);
    qemu_mutex_init(&key->key_mutex);
    for (int i = 0; i != CANOKEY_EP_NUM; ++i) {
        qemu_mutex_init(&key->ep_in_mutex[i]);
        key->ep_in_status[i] = CANOKEY_EP_IN_WAIT;
        key->ep_in_pos[i] = 0;
    }

    if (canokey_emu_init(key, key->file)) {
        error_setg(errp, "canokey can not create or read %s", key->file);
        return;
    }

    /* Thread */
    key->stop_thread = false;
    qemu_thread_create(&key->key_thread, "canokey", canokey_thread,
                       key, QEMU_THREAD_JOINABLE);
}

static void canokey_unrealize(USBDevice *base)
{
    CanoKeyState *key = CANOKEY(base);

    /* Thread */
    key->stop_thread = true;
    qemu_cond_signal(&key->key_cond);
    qemu_thread_join(&key->key_thread);

    /* Synchronization */
    qemu_cond_destroy(&key->key_cond);
    qemu_mutex_destroy(&key->key_mutex);
    for (int i = 0; i != CANOKEY_EP_NUM; ++i) {
        qemu_mutex_destroy(&key->ep_in_mutex[i]);
    }
}

static Property canokey_properties[] = {
    DEFINE_PROP_STRING("file", CanoKeyState, file),
    DEFINE_PROP_END_OF_LIST(),
};

static void canokey_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->product_desc   = "CanoKey QEMU";
    uc->usb_desc       = &desc_canokey;
    uc->handle_reset   = canokey_handle_reset;
    uc->handle_control = canokey_handle_control;
    uc->handle_data    = canokey_handle_data;
    uc->handle_attach  = usb_desc_attach;
    uc->realize        = canokey_realize;
    uc->unrealize      = canokey_unrealize;
    dc->desc           = "CanoKey QEMU";
    device_class_set_props(dc, canokey_properties);
}

static const TypeInfo canokey_info = {
    .name = TYPE_CANOKEY,
    .parent = TYPE_USB_DEVICE,
    .instance_size = sizeof(CanoKeyState),
    .class_init = canokey_class_init
};

static void canokey_register_types(void)
{
    type_register_static(&canokey_info);
}

type_init(canokey_register_types)
