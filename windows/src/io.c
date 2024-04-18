/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk      <rafal@invisiblethingslab.com>
 * Copyright (C) 2013  Marek Marczykowski  <marmarek@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */
#define LIBVCHAN_EXPORTS

#include <stdio.h>
#include <stdlib.h>
#include <xencontrol.h>
#include "libvchan.h"
#include "libvchan_private.h"

int libvchan__check_domain_alive(PXENCONTROL_CONTEXT xc_handle, int dom) {
    //DWORD ret, status;
    /* check if domain still alive */
    /* xc_evtchn_status will return different error depending on
     * existence of "source" domain:
     * ESRCH - domain don't exists
     * EINVAL/EPERM - domain exsts but port is invalid / cannot check
     * its status
     */

    //ret = EvtchnStatus(xc_handle, ~0, &status);
    return 1;
}

int libvchan_write(libvchan_t *ctrl, const void *data, size_t size) {
    return libxenvchan_write(ctrl->xenvchan, (char*)data, size);
}

int libvchan_send(libvchan_t *ctrl, const void *data, size_t size) {
    return libxenvchan_send(ctrl->xenvchan, (char*)data, size);
}

int libvchan_read(libvchan_t *ctrl, void *data, size_t size) {
    return libxenvchan_read(ctrl->xenvchan, (char*)data, size);
}

int libvchan_recv(libvchan_t *ctrl, void *data, size_t size) {
    return libxenvchan_recv(ctrl->xenvchan, (char*)data, size);
}

int libvchan_wait(libvchan_t *ctrl) {
    int ret = -2; /* invalid, so can be distinguished from real
                     libxenvchan_wait return code */
#if 0
    if (ctrl->xenvchan->is_server && libxenvchan_is_open(ctrl->xenvchan) == 2) {
        /* In case of vchan server waiting for a client, we'll not receive any
         * notification if the remote domain dies before connecting. Because of
         * that, check periodically if remote domain is still alive while
         * waiting for a connection. Actually this doesn't cover all the cases
         * - if remote domain is still alive, but remote process dies before
         * connecting, we'll also not receive any notification. But this, in
         * most cases, can be solved by application using libvchan.
         *
         * During normal operation this shouldn't be long - in most cases vchan
         * client will connect almost instantly. So this sleep(10) loop will
         * not hurt. Alternativelly it could be implemented with
         * xs_watch("@releaseDomain"), but such approach will slow down most
         * common execution path (xs_open+xs_watch even if client connects
         * right away).
         */
        while (ret == -2 && libxenvchan_is_open(ctrl->xenvchan) == 2) {
            fd_set rd_set;
            struct timeval tv = { 10, 0 };
            int vchan_fd = libxenvchan_fd_for_select(ctrl->xenvchan);
            FD_ZERO(&rd_set);
            FD_SET(vchan_fd, &rd_set);
            switch (select(vchan_fd+1, &rd_set, NULL, NULL, &tv)) {
                case 0:
                    if (!libvchan__check_domain_alive(ctrl->xc_handle, ctrl->remote_domain))
                        return -1;
                    break;
                case 1:
                    /* break the loop */
                    ret = -1;
                    break;
                default:
                    perror("select");
                    return -1;
            }
        }
    }
#endif
    ret = libxenvchan_wait(ctrl->xenvchan);
    libvchan_cleanup(ctrl);
    return ret;
}

void libvchan_cleanup(libvchan_t* ctrl) {
    if (ctrl->xs_path) {
        Log(XLL_DEBUG, "removing xs path %S", ctrl->xs_path);
        /* remove xenstore entry at first client connection */
        XcStoreRemove(ctrl->xenvchan->xc, ctrl->xs_path);
        free(ctrl->xs_path);
        ctrl->xs_path = NULL;
    }
}

void libvchan_close(libvchan_t *ctrl) {

    if (!ctrl)
        return;

    libvchan_cleanup(ctrl);
    libxenvchan_close(ctrl->xenvchan);
    free(ctrl);
}

EVTCHN libvchan_fd_for_select(libvchan_t *ctrl) {
    return libxenvchan_fd_for_select(ctrl->xenvchan);
}

int libvchan_data_ready(libvchan_t *ctrl) {
    return libxenvchan_data_ready(ctrl->xenvchan);
}

int libvchan_buffer_space(libvchan_t *ctrl) {
    return libxenvchan_buffer_space(ctrl->xenvchan);
}

int libvchan_is_open(libvchan_t *ctrl) {
    int ret;
    //struct evtchn_status evst;

    ret = libxenvchan_is_open(ctrl->xenvchan);
    if (ret == 2) {
        if (!libvchan__check_domain_alive(ctrl->xenvchan->xc, ctrl->remote_domain))
            return VCHAN_DISCONNECTED;
        return VCHAN_WAITING;
    }
    
    if (!ret)
        return VCHAN_DISCONNECTED;
#if 0
    /* slow check in case of domain destroy */
    evst.port = ctrl->xenvchan->event_port;
    evst.dom = DOMID_SELF;
    if (xc_evtchn_status(ctrl->xc_handle, &evst)) {
        perror("xc_evtchn_status");
        return VCHAN_DISCONNECTED;
    }
    if (evst.status != EVTCHNSTAT_interdomain) {
        if (!ctrl->xenvchan->is_server)
            ctrl->xenvchan->ring->srv_live = 0;
        return VCHAN_DISCONNECTED;
    }
#endif
    return VCHAN_CONNECTED;
}
