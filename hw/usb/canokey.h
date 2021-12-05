/*
 * CanoKey QEMU device header.
 *
 * Copyright (c) 2021-2022 Canokeys.org <contact@canokeys.org>
 * Written by Hongren (Zenithal) Zheng <i@zenithal.me>
 *
 * This code is licensed under the Apache-2.0.
 */

#ifndef CANOKEY_H
#define CANOKEY_H

#include "hw/qdev-core.h"

#define TYPE_CANOKEY "canokey"
#define CANOKEY(obj) \
    OBJECT_CHECK(CanoKeyState, (obj), TYPE_CANOKEY)

/*
 * State of the Canokey (i.e. hw/canokey.c)
 */

/* CTRL INTR BULK */
#define CANOKEY_EP_NUM 3
/* BULK IN CAN BE UP TO 500 bytes */
#define CANOKEY_EP_IN_BUFFER_SIZE 1024

typedef enum {
    CANOKEY_EP_IN_WAIT,
    CANOKEY_EP_IN_READY,
    CANOKEY_EP_IN_STALL
} CanoKeyEPStatus;

typedef struct CanoKeyState {
    USBDevice dev;
    uint8_t idle;

    /* IN packets from canokey device loop */
    uint8_t ep_in[CANOKEY_EP_NUM][CANOKEY_EP_IN_BUFFER_SIZE];
    /* for IN larger than p->iov.size, we would do multiple handle_data() */
    uint32_t ep_in_pos[CANOKEY_EP_NUM];
    uint32_t ep_in_size[CANOKEY_EP_NUM];
    CanoKeyEPStatus ep_in_status[CANOKEY_EP_NUM];
    QemuMutex ep_in_mutex[CANOKEY_EP_NUM];

    /* OUT pointer to canokey recv buffer */
    uint8_t *ep_out[CANOKEY_EP_NUM];
    uint32_t ep_out_size[CANOKEY_EP_NUM];

    /* Properties */
    char *file; /* canokey-file */

    /* Emulation thread and sync */
    QemuCond key_cond;
    QemuMutex key_mutex;
    QemuThread key_thread;
    bool stop_thread;
} CanoKeyState;

#endif /* CANOKEY_H */
