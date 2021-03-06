/*
 * Copyright (c) 2017, Stanislav Vorobiov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "vdp/usb_gadget.h"
#include "libusb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <poll.h>

struct proxy_device;

static int done = 0;
static struct vdp_usb_device* vdp_devs[5];
static libusb_device_handle* libusb_devs[5];
static struct proxy_device* proxy_devs[5];
static int vdp_busnum = -1;

struct proxy_device
{
    libusb_device_handle* handle;
    struct vdp_usb_gadget* gadget;
};

static struct vdp_usb_descriptor_header** extra_to_descriptors(const unsigned char* extra,
    int extra_length)
{
    const struct vdp_usb_descriptor_header* h = (const void*)extra;
    int cnt = 0, i;
    struct vdp_usb_descriptor_header** headers;

    while (extra_length >= sizeof(*h)) {
        extra_length -= (int)h->bLength;
        ++cnt;
        h = (const void*)h + (int)h->bLength;
    }

    if ((extra_length != 0) || (cnt == 0)) {
        return NULL;
    }

    headers = malloc(sizeof(*headers) * (cnt + 1));

    if (!headers) {
        return NULL;
    }

    for (i = 0; i < cnt; ++i) {
        headers[i] = (void*)extra;
        extra += headers[i]->bLength;
    }

    headers[i] = NULL;

    return headers;
}

struct vdp_usb_string_table* create_string_tables(libusb_device_handle* handle)
{
    vdp_u8 buf1[255];
    vdp_u8 buf2[255];
    int i, j, res, num_langs;
    struct vdp_usb_string_table* tables;

    res = libusb_get_string_descriptor(handle, 0, 0, buf1, sizeof(buf1));
    if (res < 0) {
        printf("libusb_get_string_descriptor(0, 0): %s\n", libusb_error_name(res));
        return NULL;
    }

    if (res < 4) {
        return NULL;
    }

    if (((res - 2) % 2) != 0) {
        return NULL;
    }

    num_langs = (res - 2) / 2;

    tables = malloc(sizeof(*tables) * (num_langs + 1));

    if (!tables) {
        return NULL;
    }

    memset(tables, 0, sizeof(*tables) * (num_langs + 1));

    for (i = 0; i < num_langs; ++i) {
        int idx = 0;

        tables[i].language_id = buf1[(i + 1) * 2] | (buf1[1 + ((i + 1) * 2)] << 8);
        tables[i].strings = malloc(sizeof(*tables[i].strings) * 256);

        if (!tables[i].strings) {
            goto fail;
        }

        memset((void*)tables[i].strings, 0, sizeof(*tables[i].strings) * 256);

        for (j = 1; j <= 255; ++j) {
            int num_chars;
            struct vdp_usb_string* string;

            res = libusb_get_string_descriptor(handle, j, tables[i].language_id, buf2, sizeof(buf2));
            if (res < 0) {
                if (res == LIBUSB_ERROR_TIMEOUT) {
                    /*
                     * Request timed out, this means all the rest might
                     * time out too, so let's just move on to the next language.
                     */
                    break;
                }
                continue;
            }

            if (res < 2) {
                continue;
            }

            if (((res - 2) % 2) != 0) {
                continue;
            }

            num_chars = (res - 2) / 2;

            string = (struct vdp_usb_string*)&tables[i].strings[idx];

            string->index = j;
            string->str = malloc(num_chars * 3 + 1);

            if (!string->str) {
                goto fail;
            }

            res = vdp_usb_utf16le_to_utf8((const vdp_u16*)&buf2[2], (char*)string->str, num_chars);
            ((char*)(string->str))[res] = 0;

            printf("string (0x%X, %d) = %s\n", (int)tables[i].language_id, j, string->str);

            ++idx;
        }
    }

    return tables;

fail:
    for (i = 0; tables[i].strings; ++i) {
        const struct vdp_usb_string* strings = tables[i].strings;
        for (j = 0; strings[j].str; ++j) {
            free((void*)strings[j].str);
        }
        free((void*)strings);
    }
    free(tables);

    return NULL;
}

static vdp_usb_urb_status translate_transfer_status(enum libusb_transfer_status status)
{
    switch (status) {
    case LIBUSB_TRANSFER_COMPLETED: return vdp_usb_urb_status_completed;
    case LIBUSB_TRANSFER_ERROR: return vdp_usb_urb_status_error;
    case LIBUSB_TRANSFER_TIMED_OUT: return vdp_usb_urb_status_error;
    case LIBUSB_TRANSFER_CANCELLED: return vdp_usb_urb_status_unlinked;
    case LIBUSB_TRANSFER_STALL: return vdp_usb_urb_status_stall;
    case LIBUSB_TRANSFER_NO_DEVICE: return vdp_usb_urb_status_error;
    case LIBUSB_TRANSFER_OVERFLOW: return vdp_usb_urb_status_overflow;
    default:
        assert(0);
        return vdp_usb_urb_status_error;
    }
}

static void proxy_gadget_transfer_cb(struct libusb_transfer* transfer)
{
    struct vdp_usb_gadget_request* request = transfer->user_data;

    if (!request) {
        printf("ep 0x%X transfer done %p: %d\n", (vdp_u32)transfer->endpoint,
            transfer, transfer->status);

        if (transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
            free(transfer->buffer);
        }
        libusb_free_transfer(transfer);
        return;
    } else {
        printf("ep 0x%X transfer done %u: %d\n", (vdp_u32)transfer->endpoint,
            request->id, transfer->status);
    }

    switch (transfer->type) {
    case LIBUSB_TRANSFER_TYPE_CONTROL:
        request->status = translate_transfer_status(transfer->status);
        request->actual_length = transfer->actual_length;
        if (request->in) {
            memcpy(request->transfer_buffer,
                transfer->buffer + sizeof(struct vdp_usb_control_setup),
                transfer->actual_length);
        }
        free(transfer->buffer);
        break;
    case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS: {
        int i;

        for (i = 0; i < transfer->num_iso_packets; i++) {
            request->iso_packets[i].status = translate_transfer_status(transfer->iso_packet_desc[i].status);
            request->iso_packets[i].actual_length = transfer->iso_packet_desc[i].actual_length;
        }
        break;
    }
    case LIBUSB_TRANSFER_TYPE_BULK:
    case LIBUSB_TRANSFER_TYPE_INTERRUPT:
        request->status = translate_transfer_status(transfer->status);
        request->actual_length = transfer->actual_length;
        break;
    default:
        assert(0);
        break;
    }

    request->priv = NULL;
    request->complete(request);
    request->destroy(request);

    libusb_free_transfer(transfer);
}

static void proxy_gadget_ep_enable(struct vdp_usb_gadget_ep* ep, int value)
{
    printf("ep %u enable %d\n", ep->caps.address, value);

    if (!value) {
        int res;
        struct vdp_usb_gadget_request* request;

        vdp_list_for_each(struct vdp_usb_gadget_request,
            request, &ep->requests, entry) {
            struct libusb_transfer* transfer = request->priv;
            res = libusb_cancel_transfer(transfer);
            if (res != 0) {
                printf("libusb_cancel_transfer(): %s\n", libusb_error_name(res));
            }
        }
    }
}

static void proxy_gadget_ep_enqueue(struct vdp_usb_gadget_ep* ep, struct vdp_usb_gadget_request* request)
{
    libusb_device_handle* handle = ep->priv;
    int res, i;
    struct libusb_transfer* transfer;

    printf("ep (addr=%u, in=%d, type=%d) enqueue %u\n", ep->caps.address, request->in, ep->caps.type, request->id);

    switch (ep->caps.type) {
    case vdp_usb_gadget_ep_control: {
        unsigned char* buf = malloc(sizeof(struct vdp_usb_control_setup) + request->transfer_length);

        if (!buf) {
            printf("malloc failed\n");
            request->status = vdp_usb_urb_status_error;
            request->complete(request);
            request->destroy(request);
            return;
        }

        transfer = libusb_alloc_transfer(0);

        if (!transfer) {
            printf("libusb_alloc_transfer() failed\n");
            free(buf);
            request->status = vdp_usb_urb_status_error;
            request->complete(request);
            request->destroy(request);
            return;
        }

        memcpy(buf, request->raw_setup_packet, sizeof(struct vdp_usb_control_setup));
        if (!request->in) {
            memcpy(buf + sizeof(struct vdp_usb_control_setup),
                request->transfer_buffer, request->transfer_length);
        }

        request->priv = transfer;

        libusb_fill_control_transfer(transfer, handle, buf,
            proxy_gadget_transfer_cb, request, 0);

        if (request->in) {
            transfer->endpoint = VDP_USB_ENDPOINT_IN_ADDRESS(ep->caps.address);
        } else {
            transfer->endpoint = VDP_USB_ENDPOINT_OUT_ADDRESS(ep->caps.address);
        }

        res = libusb_submit_transfer(transfer);
        if (res != 0) {
            printf("libusb_submit_transfer(): %s\n", libusb_error_name(res));
            libusb_free_transfer(transfer);
            free(buf);
            request->status = vdp_usb_urb_status_error;
            request->complete(request);
            request->destroy(request);
            return;
        }

        break;
    }
    case vdp_usb_gadget_ep_iso: {
        uint8_t address;
        transfer = libusb_alloc_transfer(request->number_of_packets);

        if (!transfer) {
            printf("libusb_alloc_transfer() failed\n");
            request->status = vdp_usb_urb_status_error;
            request->complete(request);
            request->destroy(request);
            return;
        }

        request->priv = transfer;

        if (request->in) {
            address = VDP_USB_ENDPOINT_IN_ADDRESS(ep->caps.address);
        } else {
            address = VDP_USB_ENDPOINT_OUT_ADDRESS(ep->caps.address);
        }

        libusb_fill_iso_transfer(transfer, handle, address,
            request->transfer_buffer, request->transfer_length,
            request->number_of_packets,
            proxy_gadget_transfer_cb, request, 0);

        for (i = 0; i < transfer->num_iso_packets; i++) {
            transfer->iso_packet_desc[i].length = request->iso_packets[i].length;
        }

        res = libusb_submit_transfer(transfer);
        if (res != 0) {
            printf("libusb_submit_transfer(): %s\n", libusb_error_name(res));
            libusb_free_transfer(transfer);
            request->status = vdp_usb_urb_status_error;
            request->complete(request);
            request->destroy(request);
            return;
        }

        break;
    }
    case vdp_usb_gadget_ep_bulk:
    case vdp_usb_gadget_ep_int: {
        uint8_t address;
        transfer = libusb_alloc_transfer(0);

        if (!transfer) {
            printf("libusb_alloc_transfer() failed\n");
            request->status = vdp_usb_urb_status_error;
            request->complete(request);
            request->destroy(request);
            return;
        }

        request->priv = transfer;

        if (request->in) {
            address = VDP_USB_ENDPOINT_IN_ADDRESS(ep->caps.address);
        } else {
            address = VDP_USB_ENDPOINT_OUT_ADDRESS(ep->caps.address);
        }

        if (ep->caps.type == vdp_usb_gadget_ep_bulk) {
            libusb_fill_bulk_transfer(transfer, handle, address,
                request->transfer_buffer, request->transfer_length,
                proxy_gadget_transfer_cb, request, 0);
        } else {
            libusb_fill_interrupt_transfer(transfer, handle, address,
                request->transfer_buffer, request->transfer_length,
                proxy_gadget_transfer_cb, request, 0);
        }

        res = libusb_submit_transfer(transfer);
        if (res != 0) {
            printf("libusb_submit_transfer(): %s\n", libusb_error_name(res));
            libusb_free_transfer(transfer);
            request->status = vdp_usb_urb_status_error;
            request->complete(request);
            request->destroy(request);
            return;
        }

        break;
    }
    default:
        assert(0);
        request->status = vdp_usb_urb_status_error;
        request->complete(request);
        request->destroy(request);
        return;
    }
}

static void proxy_gadget_ep_dequeue(struct vdp_usb_gadget_ep* ep, struct vdp_usb_gadget_request* request)
{
    struct libusb_transfer* transfer = request->priv;
    int res;

    printf("ep %u dequeue %u\n", ep->caps.address, request->id);

    res = libusb_cancel_transfer(transfer);
    if (res != 0) {
        printf("libusb_cancel_transfer(): %s\n", libusb_error_name(res));
    }
}

static vdp_usb_urb_status proxy_gadget_ep_clear_stall(struct vdp_usb_gadget_ep* ep)
{
    libusb_device_handle* handle = ep->priv;
    vdp_u8 address;
    int res;

    printf("ep %u clear stall\n", ep->caps.address);

    if (((ep->caps.dir & vdp_usb_gadget_ep_in) != 0) && (ep->caps.type != vdp_usb_gadget_ep_control)) {
        address = VDP_USB_ENDPOINT_IN_ADDRESS(ep->caps.address);
    } else {
        address = VDP_USB_ENDPOINT_OUT_ADDRESS(ep->caps.address);
    }

    res = libusb_clear_halt(handle, address);
    if (res != 0) {
        printf("libusb_clear_halt(): %s\n", libusb_error_name(res));
        return vdp_usb_urb_status_stall;
    } else {
        return vdp_usb_urb_status_completed;
    }
}

static void proxy_gadget_ep_destroy(struct vdp_usb_gadget_ep* ep)
{
    int res;
    struct vdp_usb_gadget_request* request;

    printf("ep %u destroy\n", ep->caps.address);

    vdp_list_for_each(struct vdp_usb_gadget_request,
        request, &ep->requests, entry) {
        struct libusb_transfer* transfer = request->priv;

        request->priv = NULL;
        transfer->user_data = NULL;

        res = libusb_cancel_transfer(transfer);
        if (res != 0) {
            printf("libusb_cancel_transfer(): %s\n", libusb_error_name(res));
        }
    }
}

static void proxy_gadget_interface_enable(struct vdp_usb_gadget_interface* interface, int value)
{
    libusb_device_handle* handle = interface->priv;
    int res;

    printf("interface (%u, %u) enable %d\n", interface->caps.number,
        interface->caps.alt_setting, value);

    if (value) {
        res = libusb_set_interface_alt_setting(handle, interface->caps.number, interface->caps.alt_setting);
        if (res != 0) {
            printf("libusb_set_interface_alt_setting(): %s\n", libusb_error_name(res));
        }
    }
}

static void proxy_gadget_interface_destroy(struct vdp_usb_gadget_interface* interface)
{
    printf("interface (%u, %u) destroy\n", interface->caps.number,
        interface->caps.alt_setting);
}

static void proxy_gadget_config_enable(struct vdp_usb_gadget_config* config, int value)
{
    libusb_device_handle* handle = config->priv;
    int res;

    printf("config %u enable %d\n", config->caps.number, value);

    if (value) {
        struct libusb_config_descriptor* desc = NULL;

        res = libusb_get_active_config_descriptor(libusb_get_device(handle), &desc);
        if (res == 0) {
            uint8_t i;

            for (i = 0; i < desc->bNumInterfaces; ++i) {
                res = libusb_detach_kernel_driver(handle,
                    desc->interface[i].altsetting[0].bInterfaceNumber);
                if ((res != 0) && (res != LIBUSB_ERROR_NOT_FOUND)) {
                    printf("libusb_detach_kernel_driver(): %s\n", libusb_error_name(res));
                }
            }

            libusb_free_config_descriptor(desc);
        } else {
            printf("libusb_get_active_config_descriptor() 1: %s\n", libusb_error_name(res));
        }

        res = libusb_set_configuration(handle, config->caps.number);
        if (res != 0) {
            printf("libusb_set_configuration(): %s\n", libusb_error_name(res));
        } else {
            res = libusb_get_active_config_descriptor(libusb_get_device(handle), &desc);
            if (res == 0) {
                uint8_t i;

                for (i = 0; i < desc->bNumInterfaces; ++i) {
                    res = libusb_detach_kernel_driver(handle,
                        desc->interface[i].altsetting[0].bInterfaceNumber);
                    if ((res != 0) && (res != LIBUSB_ERROR_NOT_FOUND)) {
                        printf("libusb_detach_kernel_driver(): %s\n", libusb_error_name(res));
                    }
                    res = libusb_claim_interface(handle,
                        desc->interface[i].altsetting[0].bInterfaceNumber);
                    if (res != 0) {
                        printf("libusb_claim_interface(): %s\n", libusb_error_name(res));
                    }
                }

                libusb_free_config_descriptor(desc);
            } else {
                printf("libusb_get_active_config_descriptor 2(): %s\n", libusb_error_name(res));
            }
        }
    } else {
        struct libusb_config_descriptor* desc = NULL;

        res = libusb_get_active_config_descriptor(libusb_get_device(handle), &desc);
        if (res == 0) {
            uint8_t i;

            for (i = 0; i < desc->bNumInterfaces; ++i) {
                res = libusb_release_interface(handle,
                    desc->interface[i].altsetting[0].bInterfaceNumber);
                if (res != 0) {
                    printf("libusb_release_interface(): %s\n", libusb_error_name(res));
                }
            }

            libusb_free_config_descriptor(desc);
        } else {
            printf("libusb_get_active_config_descriptor(): %s\n", libusb_error_name(res));
        }

        res = libusb_set_configuration(handle, -1);
        if (res != 0) {
            printf("libusb_set_configuration(): %s\n", libusb_error_name(res));
        }
    }
}

static void proxy_gadget_config_destroy(struct vdp_usb_gadget_config* config)
{
    printf("config %u destroy\n", config->caps.number);
}

static void proxy_gadget_reset(struct vdp_usb_gadget* gadget, int start)
{
    libusb_device_handle* handle = gadget->priv;
    int res;

    printf("gadget reset %d\n", start);

    if (!start) {
        res = libusb_reset_device(handle);
        if (res != 0) {
            printf("libusb_reset_device(): %s\n", libusb_error_name(res));
        }
    }
}

static void proxy_gadget_power(struct vdp_usb_gadget* gadget, int on)
{
    printf("gadget power %d\n", on);
}

static void proxy_gadget_set_address(struct vdp_usb_gadget* gadget, vdp_u32 address)
{
    printf("gadget set_address %u\n", address);
}

static void proxy_gadget_destroy(struct vdp_usb_gadget* gadget)
{
    printf("gadget destroy\n");
}

static struct vdp_usb_gadget_ep* create_proxy_gadget_ep(libusb_device_handle* handle,
    const struct libusb_endpoint_descriptor* desc,
    vdp_usb_gadget_ep_dir dir)
{
    static const struct vdp_usb_gadget_ep_ops ops =
    {
        .enable = proxy_gadget_ep_enable,
        .enqueue = proxy_gadget_ep_enqueue,
        .dequeue = proxy_gadget_ep_dequeue,
        .clear_stall = proxy_gadget_ep_clear_stall,
        .destroy = proxy_gadget_ep_destroy
    };
    struct vdp_usb_gadget_ep_caps caps;
    struct vdp_usb_gadget_ep* ep;

    memset(&caps, 0, sizeof(caps));

    caps.address = VDP_USB_URB_ENDPOINT_NUMBER(desc->bEndpointAddress);
    caps.dir = dir;
    caps.type = VDP_USB_ENDPOINT_TYPE(desc->bmAttributes);
    caps.sync = VDP_USB_ENDPOINT_SYNC(desc->bmAttributes);
    caps.usage = VDP_USB_ENDPOINT_USAGE(desc->bmAttributes);
    caps.max_packet_size = desc->wMaxPacketSize;
    caps.interval = desc->bInterval;
    caps.descriptors = extra_to_descriptors(desc->extra, desc->extra_length);

    ep = vdp_usb_gadget_ep_create(&caps, &ops, handle);

    free(caps.descriptors);

    return ep;
}

static struct vdp_usb_gadget_interface* create_proxy_gadget_interface(libusb_device_handle* handle,
    const struct libusb_interface_descriptor* desc)
{
    static const struct vdp_usb_gadget_interface_ops ops =
    {
        .enable = proxy_gadget_interface_enable,
        .destroy = proxy_gadget_interface_destroy
    };
    struct vdp_usb_gadget_interface_caps caps;
    uint8_t i, j, num_endpoints = 0;
    struct vdp_usb_gadget_interface* interface = NULL;

    memset(&caps, 0, sizeof(caps));

    caps.number = desc->bInterfaceNumber;
    caps.alt_setting = desc->bAlternateSetting;
    caps.klass = desc->bInterfaceClass;
    caps.subklass = desc->bInterfaceSubClass;
    caps.protocol = desc->bInterfaceProtocol;
    caps.description = desc->iInterface;
    caps.descriptors = extra_to_descriptors(desc->extra, desc->extra_length);
    caps.endpoints = malloc(sizeof(*caps.endpoints) * ((int)desc->bNumEndpoints + 1));

    if (!caps.endpoints) {
        goto out1;
    }

    for (i = 0; i < desc->bNumEndpoints; ++i) {
        uint8_t number = VDP_USB_URB_ENDPOINT_NUMBER(desc->endpoint[i].bEndpointAddress);
        int found = 0;

        for (j = 0; j < num_endpoints; ++j) {
            if (caps.endpoints[j]->caps.address == number) {
                found = 1;
                break;
            }
        }

        if (found) {
            continue;
        }

        vdp_usb_gadget_ep_dir dir = VDP_USB_URB_ENDPOINT_IN(desc->endpoint[i].bEndpointAddress) ?
            vdp_usb_gadget_ep_in : vdp_usb_gadget_ep_out;

        for (j = i + 1; j < desc->bNumEndpoints; ++j) {
            if (VDP_USB_URB_ENDPOINT_NUMBER(desc->endpoint[j].bEndpointAddress) == number) {
                dir |= VDP_USB_URB_ENDPOINT_IN(desc->endpoint[j].bEndpointAddress) ?
                    vdp_usb_gadget_ep_in : vdp_usb_gadget_ep_out;
            }
        }

        caps.endpoints[num_endpoints] = create_proxy_gadget_ep(handle, &desc->endpoint[i], dir);
        if (!caps.endpoints[num_endpoints]) {
            goto out3;
        }

        ++num_endpoints;
    }

    caps.endpoints[num_endpoints] = NULL;

    interface = vdp_usb_gadget_interface_create(&caps, &ops, handle);

    if (interface) {
        goto out2;
    }

out3:
    for (i = 0; i < num_endpoints; ++i) {
        vdp_usb_gadget_ep_destroy(caps.endpoints[i]);
    }
out2:
    free(caps.endpoints);
out1:
    free(caps.descriptors);

    return interface;
}

static struct vdp_usb_gadget_config* create_proxy_gadget_config(libusb_device_handle* handle,
    const struct libusb_config_descriptor* desc)
{
    static const struct vdp_usb_gadget_config_ops ops =
    {
        .enable = proxy_gadget_config_enable,
        .destroy = proxy_gadget_config_destroy
    };
    struct vdp_usb_gadget_config_caps caps;
    uint8_t i;
    int num_interfaces = 0, j;
    struct vdp_usb_gadget_config* config = NULL;

    memset(&caps, 0, sizeof(caps));

    for (i = 0; i < desc->bNumInterfaces; ++i) {
        num_interfaces += desc->interface[i].num_altsetting;
    }

    caps.number = desc->bConfigurationValue;
    caps.attributes = desc->bmAttributes;
    caps.max_power = desc->MaxPower;
    caps.description = desc->iConfiguration;
    caps.descriptors = extra_to_descriptors(desc->extra, desc->extra_length);
    caps.interfaces = malloc(sizeof(*caps.interfaces) * (num_interfaces + 1));

    if (!caps.interfaces) {
        goto out1;
    }

    num_interfaces = 0;

    for (i = 0; i < desc->bNumInterfaces; ++i) {
        for (j = 0; j < desc->interface[i].num_altsetting; ++j) {
            caps.interfaces[num_interfaces] = create_proxy_gadget_interface(handle, &desc->interface[i].altsetting[j]);
            if (!caps.interfaces[num_interfaces]) {
                goto out3;
            }
            ++num_interfaces;
        }
    }

    caps.interfaces[num_interfaces] = NULL;

    config = vdp_usb_gadget_config_create(&caps, &ops, handle);

    if (config) {
        goto out2;
    }

out3:
    for (j = 0; j < num_interfaces; ++j) {
        vdp_usb_gadget_interface_destroy(caps.interfaces[j]);
    }
out2:
    free(caps.interfaces);
out1:
    free(caps.descriptors);

    return config;
}

static struct vdp_usb_gadget* create_proxy_gadget(libusb_device_handle* handle,
    const struct libusb_device_descriptor* dev_desc,
    struct libusb_config_descriptor** config_descs)
{
    static const struct vdp_usb_gadget_ops ops =
    {
        .reset = proxy_gadget_reset,
        .power = proxy_gadget_power,
        .set_address = proxy_gadget_set_address,
        .destroy = proxy_gadget_destroy
    };
    struct vdp_usb_gadget_caps caps;
    struct libusb_endpoint_descriptor ep0_desc;
    uint8_t i, num_configs = 0;
    int j, k;
    struct vdp_usb_gadget* gadget = NULL;

    memset(&caps, 0, sizeof(caps));
    memset(&ep0_desc, 0, sizeof(ep0_desc));

    ep0_desc.bEndpointAddress = 0;
    ep0_desc.bmAttributes = VDP_USB_ENDPOINT_XFER_CONTROL;
    ep0_desc.wMaxPacketSize = dev_desc->bMaxPacketSize0;
    ep0_desc.bInterval = 0;

    caps.bcd_usb = dev_desc->bcdUSB;
    caps.bcd_device = dev_desc->bcdDevice;
    caps.klass = dev_desc->bDeviceClass;
    caps.subklass = dev_desc->bDeviceSubClass;
    caps.protocol = dev_desc->bDeviceProtocol;
    caps.vendor_id = dev_desc->idVendor;
    caps.product_id = dev_desc->idProduct;
    caps.manufacturer = dev_desc->iManufacturer;
    caps.product = dev_desc->iProduct;
    caps.serial_number = dev_desc->iSerialNumber;
    caps.string_tables = create_string_tables(handle);
    caps.configs = malloc(sizeof(*caps.configs) * ((int)dev_desc->bNumConfigurations + 1));

    if (!caps.configs) {
        goto out1;
    }

    caps.endpoint0 = create_proxy_gadget_ep(handle, &ep0_desc, vdp_usb_gadget_ep_inout);

    if (!caps.endpoint0) {
        goto out2;
    }

    for (i = 0; i < dev_desc->bNumConfigurations; ++i) {
        caps.configs[num_configs] = create_proxy_gadget_config(handle, config_descs[i]);
        if (!caps.configs[num_configs]) {
            goto out3;
        }

        ++num_configs;
    }

    caps.configs[num_configs] = NULL;

    gadget = vdp_usb_gadget_create(&caps, &ops, handle);

    if (gadget) {
        goto out2;
    }

out3:
    for (i = 0; i < num_configs; ++i) {
        vdp_usb_gadget_config_destroy(caps.configs[i]);
    }
    vdp_usb_gadget_ep_destroy(caps.endpoint0);
out2:
    free(caps.configs);
out1:
    for (j = 0; caps.string_tables && caps.string_tables[j].strings; ++j) {
        const struct vdp_usb_string* strings = caps.string_tables[j].strings;
        for (k = 0; strings[k].str != NULL; ++k) {
            free((void*)strings[k].str);
        }
        free((void*)strings);
    }
    free(caps.string_tables);

    return gadget;
}

static struct proxy_device* proxy_device_create(libusb_device_handle* handle)
{
    struct proxy_device* proxy_dev;
    struct libusb_device_descriptor dev_desc;
    int res;
    libusb_device* dev = libusb_get_device(handle);
    uint8_t i;
    struct libusb_config_descriptor** config_descs;

    proxy_dev = malloc(sizeof(*proxy_dev));
    if (!proxy_dev) {
        goto fail1;
    }

    proxy_dev->handle = handle;

    res = libusb_get_device_descriptor(dev, &dev_desc);
    if (res != LIBUSB_SUCCESS) {
        printf("error getting device descriptor\n");
        goto fail2;
    }

    config_descs = malloc(sizeof(*config_descs) * (int)dev_desc.bNumConfigurations);
    if (!config_descs) {
        printf("cannot allocate mem for config descs\n");
        goto fail2;
    }

    memset(config_descs, 0,
        sizeof(*config_descs) * (int)dev_desc.bNumConfigurations);

    for (i = 0; i < dev_desc.bNumConfigurations; ++i) {
        res = libusb_get_config_descriptor(dev, i, &config_descs[i]);
        if (res != LIBUSB_SUCCESS) {
            printf("error getting config descriptor\n");
            goto fail3;
        }
    }

    proxy_dev->gadget = create_proxy_gadget(handle, &dev_desc, config_descs);
    if (!proxy_dev->gadget) {
        printf("cannot create proxy gadget\n");
        goto fail3;
    }

    for (i = 0; i < dev_desc.bNumConfigurations; ++i) {
        libusb_free_config_descriptor(config_descs[i]);
    }
    free(config_descs);

    return proxy_dev;

fail3:
    for (i = 0; i < dev_desc.bNumConfigurations; ++i) {
        libusb_free_config_descriptor(config_descs[i]);
    }
    free(config_descs);
fail2:
    free(proxy_dev);
fail1:

    return NULL;
}

static void proxy_device_destroy(struct proxy_device* proxy)
{
    vdp_usb_gadget_destroy(proxy->gadget);
    libusb_close(proxy->handle);
    free(proxy);
}

static int hotplug_callback_attach(libusb_context* ctx, libusb_device* dev, libusb_hotplug_event event, void* user_data)
{
    int i;

    if (libusb_get_bus_number(dev) == vdp_busnum) {
        return 0;
    }

    printf("device attached: %d:%d\n",
        libusb_get_bus_number(dev), libusb_get_port_number(dev));

    for (i = 0; i < sizeof(proxy_devs)/sizeof(proxy_devs[0]); ++i) {
        if (!libusb_devs[i] && !proxy_devs[i]) {
            int res;

            res = libusb_open(dev, &libusb_devs[i]);
            if (res != LIBUSB_SUCCESS) {
                printf("error opening device\n");
                libusb_devs[i] = NULL;
            }

            sleep(1);

            break;
        }
    }

    return 0;
}

static int hotplug_callback_detach(libusb_context* ctx, libusb_device* dev, libusb_hotplug_event event, void* user_data)
{
    int i;

    if (libusb_get_bus_number(dev) == vdp_busnum) {
        return 0;
    }

    printf("device detached: %d:%d\n",
        libusb_get_bus_number(dev), libusb_get_port_number(dev));

    for (i = 0; i < sizeof(proxy_devs)/sizeof(proxy_devs[0]); ++i) {
        if (libusb_devs[i]) {
            libusb_device* other_dev = libusb_get_device(libusb_devs[i]);
            if ((libusb_get_bus_number(dev) == libusb_get_bus_number(other_dev)) &&
                (libusb_get_port_number(dev) == libusb_get_port_number(other_dev))) {
                if (!proxy_devs[i]) {
                    libusb_close(libusb_devs[i]);
                }
                libusb_devs[i] = NULL;
                break;
            }
        }
    }

    return 0;
}

static void sig_handler(int signum)
{
    done = 1;
}

int main(int argc, char* argv[])
{
    libusb_hotplug_callback_handle hp[2];
    int product_id, vendor_id;
    int res;
    struct vdp_usb_context* ctx;
    vdp_usb_result vdp_res;
    int i;

    signal(SIGINT, &sig_handler);

    if (argc < 3) {
        printf("usage: vdpusb-proxy <vendor_id> <product_id>\n");
        res = 1;
        goto out1;
    }

    vendor_id = (int)strtol(argv[1], NULL, 16);
    product_id = (int)strtol(argv[2], NULL, 16);

    res = libusb_init(NULL);
    if (res != 0) {
        printf("failed to initialise libusb: %s\n", libusb_error_name(res));
        res = 1;
        goto out1;
    }

    vdp_res = vdp_usb_init(stdout, vdp_log_debug, &ctx);
    if (vdp_res != vdp_usb_success) {
        printf("failed to initialise vdpusb: %s\n", vdp_usb_result_to_str(vdp_res));
        res = 1;
        goto out2;
    }

    for (i = 0; i < sizeof(vdp_devs)/sizeof(vdp_devs[0]); ++i) {
        vdp_res = vdp_usb_device_open(ctx, i, &vdp_devs[i]);
        if (vdp_res != vdp_usb_success) {
            res = 1;
            goto out3;
        }
        vdp_busnum = vdp_usb_device_get_busnum(vdp_devs[i]);
    }

    if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        printf("libusb hotplug capabilites are not supported on this platform\n");
        res = 1;
        goto out3;
    }

    res = libusb_hotplug_register_callback(NULL, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED, LIBUSB_HOTPLUG_ENUMERATE, vendor_id,
        product_id, LIBUSB_HOTPLUG_MATCH_ANY, hotplug_callback_attach, NULL, &hp[0]);
    if (res != LIBUSB_SUCCESS) {
        printf("error registering callback 0\n");
        res = 1;
        goto out3;
    }

    res = libusb_hotplug_register_callback(NULL, LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT, 0, vendor_id,
        product_id, LIBUSB_HOTPLUG_MATCH_ANY, hotplug_callback_detach, NULL, &hp[1]);
    if (res != LIBUSB_SUCCESS) {
        printf("error registering callback 1\n");
        res = 1;
        goto out3;
    }

    printf("waiting for %04x:%04x\n", vendor_id, product_id);

    while (!done) {
        fd_set read_fds, write_fds;
        const struct libusb_pollfd** libusb_fds;
        struct timeval tv, zero_tv;
        int have_tv, i, max_fd = 0;
        int have_libusb_events = 0;

        zero_tv.tv_sec = 0;
        zero_tv.tv_usec = 0;

        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);

        libusb_fds = libusb_get_pollfds(NULL);
        if (!libusb_fds) {
            printf("libusb_get_pollfds() failed\n");
            break;
        }

        have_tv = libusb_get_next_timeout(NULL, &tv);
        if (have_tv < 0) {
            printf("libusb_get_next_timeout() failed\n");
            libusb_free_pollfds(libusb_fds);
            break;
        }

        for (i = 0; i < sizeof(proxy_devs)/sizeof(proxy_devs[0]); ++i) {
            vdp_fd fd = -1;

            if (!proxy_devs[i]) {
                continue;
            }

            vdp_usb_device_wait_event(vdp_devs[i], &fd);

            FD_SET(fd, &read_fds);
            if (fd > max_fd) {
                max_fd = fd;
            }
        }

        for (i = 0; libusb_fds[i]; ++i) {
            if (libusb_fds[i]->events & POLLIN) {
                FD_SET(libusb_fds[i]->fd, &read_fds);
                if (libusb_fds[i]->fd > max_fd) {
                    max_fd = libusb_fds[i]->fd;
                }
            } else if (libusb_fds[i]->events & POLLOUT) {
                FD_SET(libusb_fds[i]->fd, &write_fds);
                if (libusb_fds[i]->fd > max_fd) {
                    max_fd = libusb_fds[i]->fd;
                }
            }
        }

        assert(max_fd > 0);

        res = select(max_fd + 1, &read_fds, &write_fds, NULL, (have_tv ? &tv : NULL));

        if (res < 0) {
            printf("select error: %s\n", strerror(errno));
            libusb_free_pollfds(libusb_fds);
            break;
        }

        if (res == 0) {
            have_libusb_events = 1;
        } else {
            for (i = 0; libusb_fds[i]; ++i) {
                if (libusb_fds[i]->events & POLLIN) {
                    if (FD_ISSET(libusb_fds[i]->fd, &read_fds)) {
                        have_libusb_events = 1;
                        break;
                    }
                } else if (libusb_fds[i]->events & POLLOUT) {
                    if (FD_ISSET(libusb_fds[i]->fd, &write_fds)) {
                        have_libusb_events = 1;
                        break;
                    }
                }
            }
        }

        libusb_free_pollfds(libusb_fds);

        for (i = 0; i < sizeof(proxy_devs)/sizeof(proxy_devs[0]); ++i) {
            vdp_fd fd = -1;

            if (!proxy_devs[i]) {
                continue;
            }

            vdp_usb_device_wait_event(vdp_devs[i], &fd);

            if (FD_ISSET(fd, &read_fds)) {
                struct vdp_usb_event event;

                vdp_res = vdp_usb_device_get_event(vdp_devs[i], &event);

                if (vdp_res != vdp_usb_success) {
                    printf("failed to get event: %s\n", vdp_usb_result_to_str(vdp_res));
                    i = 0;
                    break;
                }

                vdp_usb_gadget_event(proxy_devs[i]->gadget, &event);
            }
        }

        if (i == 0) {
            break;
        }

        if (have_libusb_events) {
            res = libusb_handle_events_timeout_completed(NULL, &zero_tv, NULL);
            if (res != 0) {
                printf("libusb_handle_events_timeout_completed() failed: %s\n", libusb_error_name(res));
                break;
            }

            for (i = 0; i < sizeof(libusb_devs)/sizeof(libusb_devs[0]); ++i) {
                if (libusb_devs[i] && !proxy_devs[i]) {
                    vdp_usb_speed speed;

                    proxy_devs[i] = proxy_device_create(libusb_devs[i]);
                    if (!proxy_devs[i]) {
                        libusb_close(libusb_devs[i]);
                        libusb_devs[i] = NULL;
                        continue;
                    }

                    switch (libusb_get_device_speed(libusb_get_device(libusb_devs[i]))) {
                    case LIBUSB_SPEED_LOW:
                        speed = vdp_usb_speed_low;
                        break;
                    case LIBUSB_SPEED_FULL:
                        speed = vdp_usb_speed_full;
                        break;
                    case LIBUSB_SPEED_HIGH:
                        speed = vdp_usb_speed_high;
                        break;
                    default:
                        speed = vdp_usb_speed_high;
                        break;
                    }

                    vdp_res = vdp_usb_device_attach(vdp_devs[i], speed);
                    if (vdp_res != vdp_usb_success) {
                        printf("failed to attach device: %s\n", vdp_usb_result_to_str(vdp_res));
                        proxy_device_destroy(proxy_devs[i]);
                        proxy_devs[i] = NULL;
                        libusb_devs[i] = NULL;
                    }
                } else if (!libusb_devs[i] && proxy_devs[i]) {
                    proxy_device_destroy(proxy_devs[i]);
                    proxy_devs[i] = NULL;
                    vdp_usb_device_detach(vdp_devs[i]);
                }
            }
        }
    }

    for (i = 0; i < sizeof(proxy_devs)/sizeof(proxy_devs[0]); ++i) {
        if (proxy_devs[i]) {
            proxy_device_destroy(proxy_devs[i]);
            vdp_usb_device_detach(vdp_devs[i]);
        }
    }

    res = 0;

out3:
    for (i = 0; i < sizeof(vdp_devs)/sizeof(vdp_devs[0]); ++i) {
        if (vdp_devs[i]) {
            vdp_usb_device_close(vdp_devs[i]);
        }
    }
    vdp_usb_cleanup(ctx);
out2:
    libusb_exit(NULL);
out1:

    return res;
}
