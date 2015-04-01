/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
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

#ifndef WINNT

#include <sys/types.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <malloc.h>
#include <string.h>
#include <xenctrl.h>
#include <unistd.h>
#ifndef CONFIG_STUBDOM
#include "../u2mfn/u2mfnlib.h"
#else
#include <mm.h>
#endif

#if __XEN_INTERFACE_VERSION__ >= 0x00040200
#include <xenstore.h>
#else
#include <xs.h>
#endif

#endif
#include <assert.h>

#include <stdio.h>
#include <stdlib.h>
#include "libvchan.h"


/* Windows interface requires to set system-wide limit of shared pages. 1024
 * should be enough because vchan connection (in this version) uses only one
 * page and Qubes uses three vchan channels: qubesdb, qrexec, gui. Even after
 * switching to new vchan (xen 4.2) it should be enough as there vchan link
 * uses at most 256 pages */
#define GLOBAL_MAX_SHARED_PAGES 1024


static void fill_ctrl(struct libvchan *ctrl, struct vchan_interface *ring, grant_ref_t ring_ref)
{
	ctrl->ring = ring;
	ctrl->ring_ref = ring_ref;

	ring->cons_in = ring->prod_in = ring->cons_out = ring->prod_out =
	    0;
	ring->server_closed = ring->client_closed = 0;
	ring->debug = 0xaabbccdd;
}

#ifdef QREXEC_RING_V2
static int ring_init(struct libvchan *ctrl)
{
	struct gntmem_handle*	h;
	grant_ref_t	grants[1];
	int	result;
	struct vchan_interface *ring;

	h = gntmem_open();
	if (h == INVALID_HANDLE_VALUE)
		return -1;

    // This is not needed with Windows GPL PV drivers anymore.
    // Setting local limit to 1 prevents any other grants because the device instance is reused.
#ifndef WINNT
	if (gntmem_set_local_quota(h, 1))
		goto fail;
	if (gntmem_set_global_quota(h, GLOBAL_MAX_SHARED_PAGES))
		goto fail;
#endif

	memset(grants, 0, sizeof(grants));
	ring = gntmem_grant_pages_to_domain(h, 0, 1, grants);
	if (!ring)
		goto fail;

	fill_ctrl(ctrl, ring, grants[0]);

	ctrl->gmh = h;
	return 0;

fail:
	gntmem_close(h);
	return -1;
}

static void ring_cleanup(struct libvchan *ctrl)
{
	gntmem_close(ctrl->gmh);
}

#else

static int ring_init(struct libvchan *ctrl)
{
	int mfn;
#ifndef CONFIG_STUBDOM
	int u2mfn_fd;
#endif
	struct vchan_interface *ring;
#ifdef CONFIG_STUBDOM
	ring = (struct vchan_interface *) memalign(XC_PAGE_SIZE, sizeof(*ring));

	if (!ring)
		return -1;


	mfn = virtual_to_mfn(ring);
#else
	u2mfn_fd = u2mfn_get_fd();
	if (u2mfn_fd < 0)
		return -1;
	ring = (struct vchan_interface *) u2mfn_alloc_kpage_with_fd (u2mfn_fd);

	if (ring == MAP_FAILED) {
		close(u2mfn_fd);
		return -1;
	}

	if (u2mfn_get_last_mfn_with_fd (u2mfn_fd, &mfn) < 0) {
		u2mfn_free_kpage ((char*)ring);
		close(u2mfn_fd);
		return -1;
	}
	ctrl->u2mfn_fd = u2mfn_fd;
#endif

	fill_ctrl(ctrl, ring, mfn);
	return 0;
}

static void ring_cleanup(struct libvchan *ctrl)
{
#ifdef CONFIG_STUBDOM
	free(ctrl->ring);
#else /* CONFIG_STUBDOM */
	u2mfn_free_kpage((char*)ctrl->ring);
	close(ctrl->u2mfn_fd);
#endif /* CONFIG_STUBDOM */
}

#endif

/**
        creates event channel;
        creates "ring-ref" and "event-channel" xenstore entries;
        waits for connection to event channel from the peer
*/
static int server_interface_init(struct libvchan *ctrl, int devno)
{
	int ret = -1;
	struct xs_handle *xs;
	char buf[64];
	char ref[16];
#ifdef XENCTRL_HAS_XC_INTERFACE
	xc_evtchn *evfd;
#else
	EVTCHN evfd;
#endif
	evtchn_port_or_error_t port;
#ifdef WINNT
	xs = xs_domain_open();
#else
	xs = xs_daemon_open();
#endif
	if (!xs) {
		return ret;
	}
#ifdef XENCTRL_HAS_XC_INTERFACE
	evfd = xc_evtchn_open(NULL, 0);
	if (!evfd)
		goto fail;
#else
	evfd = xc_evtchn_open();
	if (evfd < 0)
		goto fail;
#endif
	ctrl->evfd = evfd;
	// the following hardcoded 0 is the peer domain id
	port = xc_evtchn_bind_unbound_port(evfd, 0);	
	if (port < 0)
		goto fail2;
	ctrl->evport = port;
	ctrl->devno = devno;

#ifdef QREXEC_RING_V2
	snprintf(buf, sizeof buf, "device/vchan/%d/version", devno);
	if (!xs_write(xs, 0, buf, "2", strlen("2")))
		goto fail2;
#endif

	snprintf(ref, sizeof ref, "%d", ctrl->ring_ref);
	snprintf(buf, sizeof buf, "device/vchan/%d/ring-ref", devno);
	if (!xs_write(xs, 0, buf, ref, strlen(ref)))
		goto fail2;
	snprintf(ref, sizeof ref, "%d", ctrl->evport);
	snprintf(buf, sizeof buf, "device/vchan/%d/event-channel", devno);
	if (!xs_write(xs, 0, buf, ref, strlen(ref)))
		goto fail2;
	// do not block in stubdom and windows - libvchan_server_handle_connected will be
	// called on first input
#ifndef ASYNC_INIT
	// wait for the peer to arrive
	if (xc_evtchn_pending(evfd) == -1)
		goto fail2;
        xc_evtchn_unmask(ctrl->evfd, ctrl->evport);
	snprintf(buf, sizeof buf, "device/vchan/%d", devno);
	xs_rm(xs, 0, buf);
#endif

	ret = 0;
      fail2:
	if (ret)
        xc_evtchn_close(evfd);
      fail:
	xs_daemon_close(xs);
	return ret;
}

#define dir_select(dir1, dir2) \
        ctrl->wr_cons = &ctrl->ring->cons_##dir1; \
        ctrl->wr_prod = &ctrl->ring->prod_##dir1; \
        ctrl->rd_cons = &ctrl->ring->cons_##dir2; \
        ctrl->rd_prod = &ctrl->ring->prod_##dir2; \
        ctrl->wr_ring = ctrl->ring->buf_##dir1; \
        ctrl->rd_ring = ctrl->ring->buf_##dir2; \
        ctrl->wr_ring_size = sizeof(ctrl->ring->buf_##dir1); \
        ctrl->rd_ring_size = sizeof(ctrl->ring->buf_##dir2)

/**
	Run in AppVM (any domain).
	Release resources used by vchan link. Should be used after
	libvchan_close() to clean connection shutdown, but can be used alone in
	recovery case.
        \param ctrl connection to cleanup
        \returns -1 on failure (errno for details), 0 on success
*/
int libvchan_cleanup(struct libvchan *ctrl)
{
	if (!ctrl)
		return 0;
	if (!ctrl->is_server)
		return 0;
	/* do not wait flush remaining queue to allow use libvchan_cleanup for
	 * recovery situation. If someone want clean close, should call
	 * libvchan_close() first.
	 */
#if 0
	if (!ctrl->ring->server_closed)
		libvchan_close(ctrl);
#endif
	if (xc_evtchn_unbind(ctrl->evfd, ctrl->evport) < 0)
		return -1;
        xc_evtchn_close(ctrl->evfd);
	ring_cleanup(ctrl);
	free(ctrl);
	return 0;
}

/**
        Run in AppVM (any domain).
        Sleeps until the connection is established. (unless in stubdom)
        \param devno something like a well-known port.
        \returns NULL on failure, handle on success
*/
struct libvchan *libvchan_server_init(int devno)
{
	struct libvchan *ctrl =
	    (struct libvchan *) malloc(sizeof(struct libvchan));
	if (!ctrl)
		return 0;
	if (ring_init(ctrl)) {
		free(ctrl);
		return 0;
	}
	if (server_interface_init(ctrl, devno)) {
		ring_cleanup(ctrl);
		free(ctrl);
		return 0;
	}
/*
        We want the same code for read/write functions, regardless whether
        we are client, or server. Thus, we do not access buf_in nor buf_out
        buffers directly. Instead, in *_init functions, the dir_select
        macro assigns proper values to wr* and rd* pointers, so that they
        point to correct one out of buf_in or buf_out related fields.
*/
	dir_select(in, out);
	ctrl->is_server = 1;
	return ctrl;
}



int libvchan_server_handle_connected(struct libvchan *ctrl)
{
	struct xs_handle *xs;
	char buf[64];
	int ret = -1;

#ifdef WINNT
	xs = xs_domain_open();
#else
	xs = xs_daemon_open();
#endif
	if (!xs) {
		return ret;
	}

#ifndef WINNT
	// clear the pending flag
	xc_evtchn_pending(ctrl->evfd);
#endif

	snprintf(buf, sizeof buf, "device/vchan/%d", ctrl->devno);
	xs_rm(xs, 0, buf);

	ret = 0;

#if 0
fail2:
	if (ret)
        xc_evtchn_close(ctrl->evfd);
#endif
	xs_daemon_close(xs);
	return ret;
}

#ifndef WINNT

/**
        retrieves ring-ref and event-channel numbers from xenstore (if
        they don't exist, return error, because nobody seems to listen);
        map the ring, connect the event channel
*/
static int client_interface_init(struct libvchan *ctrl, int domain, int devno)
{
	int ret = -1;
	unsigned int len;
	struct xs_handle *xs;
#ifdef XENCTRL_HAS_XC_INTERFACE
	xc_interface *xcfd;
	xc_gnttab *xcg = NULL;
#else
	int xcfd;
	int xcg = -1;
#endif
	char buf[64];
	char *ref;
	int version;
#ifdef XENCTRL_HAS_XC_INTERFACE
	xc_evtchn *evfd;
#else
	int evfd;
#endif
	int remote_port;
	xs = xs_daemon_open();
	if (!xs) {
		return ret;
	}

	version = 1;
	snprintf(buf, sizeof buf,
		 "/local/domain/%d/device/vchan/%d/version", domain,
		 devno);
	ref = xs_read(xs, 0, buf, &len);
	if (ref) {
	    version = atoi(ref);
	    free(ref);
	}


	snprintf(buf, sizeof buf,
		 "/local/domain/%d/device/vchan/%d/ring-ref", domain,
		 devno);
	ref = xs_read(xs, 0, buf, &len);
	if (!ref)
		goto fail;
	ctrl->ring_ref = atoi(ref);
	free(ref);
	if (!ctrl->ring_ref)
		goto fail;
	snprintf(buf, sizeof buf,
		 "/local/domain/%d/device/vchan/%d/event-channel", domain,
		 devno);
	ref = xs_read(xs, 0, buf, &len);
	if (!ref)
		goto fail;
	remote_port = atoi(ref);
	free(ref);
	if (!remote_port)
		goto fail;

	switch (version) {
	case 1:
		
#ifdef XENCTRL_HAS_XC_INTERFACE
        	xcfd = xc_interface_open(NULL, NULL, 0);
		if (!xcfd)
			goto fail;
#else
		xcfd = xc_interface_open();
		if (xcfd < 0)
			goto fail;
#endif
		ctrl->ring = (struct vchan_interface *)
		    xc_map_foreign_range(xcfd, domain, 4096,
					 PROT_READ | PROT_WRITE, ctrl->ring_ref);
		xc_interface_close(xcfd);
		break;
	case 2:
		xcg = xc_gnttab_open(NULL, 0);
		if (!xcg)
			goto fail;
		ctrl->ring = (struct vchan_interface *)
		    xc_gnttab_map_grant_ref(xcg, domain, ctrl->ring_ref, PROT_READ | PROT_WRITE);
		break;
	default:
		goto fail;
	}

	if (ctrl->ring == 0 || ctrl->ring == MAP_FAILED)
		goto fail;
#ifdef XENCTRL_HAS_XC_INTERFACE
	evfd = xc_evtchn_open(NULL, 0);
	if (!evfd)
		goto fail2;
#else
	evfd = xc_evtchn_open();
	if (evfd < 0)
		goto fail2;
#endif
	ctrl->evfd = evfd;
	ctrl->evport =
	    xc_evtchn_bind_interdomain(evfd, domain, remote_port);
	if (ctrl->evport < 0 || xc_evtchn_notify(evfd, ctrl->evport)) {
        xc_evtchn_close(evfd);
        goto fail2;
	}

	ret = 0;
	goto cleanup;

fail2:
	switch (version) {
	case 1:
		munmap(ctrl->ring, 4096);
		break;
	case 2:
		xc_gnttab_munmap(xcg, ctrl->ring, 1);
		break;
	}

fail:
cleanup:
#ifdef XENCTRL_HAS_XC_INTERFACE
	if (xcg)
#else
	if (xcg >= 0)
#endif
		xc_gnttab_close(xcg);
	xs_daemon_close(xs);
	return ret;
}

/**
        Run on the client side of connection (currently, must be dom0).
        \returns NULL on failure (e.g. noone listening), handle on success
*/
struct libvchan *libvchan_client_init(int domain, int devno)
{
	struct libvchan *ctrl =
	    (struct libvchan *) malloc(sizeof(struct libvchan));
	if (!ctrl)
		return 0;
	if (client_interface_init(ctrl, domain, devno)) {
		free(ctrl);
		return 0;
	}
//      See comment in libvchan_server_init
	dir_select(out, in);
	ctrl->is_server = 0;
	return ctrl;
}

#else

// Windows domains can not be dom0

struct libvchan *libvchan_client_init(int domain, int devno)
{
	return NULL;
}

#endif
