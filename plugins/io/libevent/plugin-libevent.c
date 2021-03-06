/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010, 2011 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/**
 * This file contains IO operations that use libevent
 *
 * @author Trond Norbye
 * @todo add more documentation
 */

#include "config.h"
#include <event.h>
#include "libevent_io_opts.h"
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>

struct libevent_cookie {
    struct event_base *base;
    int allocated;
};


#ifndef HAVE_LIBEVENT2
/* libevent 1.x compatibility layer */
#ifndef evutil_socket_t
#define evutil_socket_t int
#endif

typedef void (*event_callback_fn)(evutil_socket_t, short, void *);

static int
event_assign(struct event *ev,
             struct event_base *base,
             evutil_socket_t fd,
             short events,
             event_callback_fn callback,
             void *arg)
{
    event_base_set(base, ev);
    ev->ev_callback = callback;
    ev->ev_arg = arg;
    ev->ev_fd = fd;
    ev->ev_events = events;
    ev->ev_res = 0;
    ev->ev_flags = EVLIST_INIT;
    ev->ev_ncalls = 0;
    ev->ev_pncalls = NULL;

    return 0;
}

static struct event *
event_new(struct event_base *base,
          evutil_socket_t fd,
          short events,
          event_callback_fn cb,
          void *arg) {
    struct event *ev;
    ev = malloc(sizeof(struct event));
    if (ev == NULL) {
        return NULL;
    }
    if (event_assign(ev, base, fd, events, cb, arg) < 0) {
        free(ev);
        return NULL;
    }
    return ev;
}

static void
event_free(struct event *ev)
{
    /* make sure that this event won't be coming back to haunt us. */
    free(ev);

}
static short
event_get_events(const struct event *ev)
{
    return ev->ev_events;
}

static event_callback_fn
event_get_callback(const struct event *ev)
{
    return ev->ev_callback;
}
#endif
static lcb_ssize_t lcb_io_recv(struct lcb_io_opt_st *iops,
                               lcb_socket_t sock,
                               void *buffer,
                               lcb_size_t len,
                               int flags)
{
    lcb_ssize_t ret = recv(sock, buffer, len, flags);
    if (ret < 0) {
        iops->v.v0.error = errno;
    }
    return ret;
}

static lcb_ssize_t lcb_io_recvv(struct lcb_io_opt_st *iops,
                                lcb_socket_t sock,
                                struct lcb_iovec_st *iov,
                                lcb_size_t niov)
{
    struct msghdr msg;
    struct iovec vec[2];
    lcb_ssize_t ret;

    if (niov != 2) {
        return -1;
    }
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = vec;
    msg.msg_iovlen = iov[1].iov_len ? (lcb_size_t)2 : (lcb_size_t)1;
    msg.msg_iov[0].iov_base = iov[0].iov_base;
    msg.msg_iov[0].iov_len = iov[0].iov_len;
    msg.msg_iov[1].iov_base = iov[1].iov_base;
    msg.msg_iov[1].iov_len = iov[1].iov_len;
    ret = recvmsg(sock, &msg, 0);

    if (ret < 0) {
        iops->v.v0.error = errno;
    }

    return ret;
}

static lcb_ssize_t lcb_io_send(struct lcb_io_opt_st *iops,
                               lcb_socket_t sock,
                               const void *msg,
                               lcb_size_t len,
                               int flags)
{
    lcb_ssize_t ret = send(sock, msg, len, flags);
    if (ret < 0) {
        iops->v.v0.error = errno;
    }
    return ret;
}

static lcb_ssize_t lcb_io_sendv(struct lcb_io_opt_st *iops,
                                lcb_socket_t sock,
                                struct lcb_iovec_st *iov,
                                lcb_size_t niov)
{
    struct msghdr msg;
    struct iovec vec[2];
    lcb_ssize_t ret;

    if (niov != 2) {
        return -1;
    }
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = vec;
    msg.msg_iovlen = iov[1].iov_len ? (lcb_size_t)2 : (lcb_size_t)1;
    msg.msg_iov[0].iov_base = iov[0].iov_base;
    msg.msg_iov[0].iov_len = iov[0].iov_len;
    msg.msg_iov[1].iov_base = iov[1].iov_base;
    msg.msg_iov[1].iov_len = iov[1].iov_len;
    ret = sendmsg(sock, &msg, 0);

    if (ret < 0) {
        iops->v.v0.error = errno;
    }
    return ret;
}

static lcb_socket_t lcb_io_socket(struct lcb_io_opt_st *iops,
                                  int domain,
                                  int type,
                                  int protocol)
{
    lcb_socket_t sock = socket(domain, type, protocol);
    if (sock == INVALID_SOCKET) {
        iops->v.v0.error = errno;
    } else {
        if (evutil_make_socket_nonblocking(sock) != 0) {
            int error = errno;
            iops->v.v0.close(iops, sock);
            iops->v.v0.error = error;
            sock = INVALID_SOCKET;
        }
    }

    return sock;
}

static void lcb_io_close(struct lcb_io_opt_st *iops,
                         lcb_socket_t sock)
{
    (void)iops;
    EVUTIL_CLOSESOCKET(sock);
}

static int lcb_io_connect(struct lcb_io_opt_st *iops,
                          lcb_socket_t sock,
                          const struct sockaddr *name,
                          unsigned int namelen)
{
    int ret = connect(sock, name, (socklen_t)namelen);
    if (ret < 0) {
        iops->v.v0.error = errno;
    }
    return ret;
}

static void *lcb_io_create_event(struct lcb_io_opt_st *iops)
{
    return event_new(((struct libevent_cookie *)iops->v.v0.cookie)->base,
                     INVALID_SOCKET, 0, NULL, NULL);
}

static int lcb_io_update_event(struct lcb_io_opt_st *iops,
                               lcb_socket_t sock,
                               void *event,
                               short flags,
                               void *cb_data,
                               void (*handler)(lcb_socket_t sock,
                                               short which,
                                               void *cb_data))
{
    flags |= EV_PERSIST;
    if (flags == event_get_events(event) &&
            handler == event_get_callback(event)) {
        /* no change! */
        return 0;
    }

    if (event_pending(event, EV_READ | EV_WRITE, 0)) {
        event_del(event);
    }

    event_assign(event, ((struct libevent_cookie *)iops->v.v0.cookie)->base, sock, flags, handler, cb_data);
    return event_add(event, NULL);
}


static void lcb_io_delete_timer(struct lcb_io_opt_st *iops,
                                void *event)
{
    (void)iops;
    if (event_pending(event, EV_TIMEOUT, 0) != 0 && event_del(event) == -1) {
        iops->v.v0.error = EINVAL;
    }
    event_assign(event, ((struct libevent_cookie *)iops->v.v0.cookie)->base, -1, 0, NULL, NULL);
}

static int lcb_io_update_timer(struct lcb_io_opt_st *iops,
                               void *timer,
                               lcb_uint32_t usec,
                               void *cb_data,
                               void (*handler)(lcb_socket_t sock,
                                               short which,
                                               void *cb_data))
{
    short flags = EV_TIMEOUT | EV_PERSIST;
    struct timeval tmo;
    if (flags == event_get_events(timer) &&
            handler == event_get_callback(timer)) {
        /* no change! */
        return 0;
    }

    if (event_pending(timer, EV_TIMEOUT, 0)) {
        event_del(timer);
    }

    event_assign(timer, ((struct libevent_cookie *)iops->v.v0.cookie)->base, -1, flags, handler, cb_data);
    tmo.tv_sec = usec / 1000000;
    tmo.tv_usec = usec % 1000000;
    return event_add(timer, &tmo);
}

static void lcb_io_destroy_event(struct lcb_io_opt_st *iops,
                                 void *event)
{
    (void)iops;
    if (event_pending(event, EV_READ | EV_WRITE | EV_TIMEOUT, 0)) {
        event_del(event);
    }
    event_free(event);
}

static void lcb_io_delete_event(struct lcb_io_opt_st *iops,
                                lcb_socket_t sock,
                                void *event)
{
    (void)iops;
    (void)sock;
    if (event_del(event) == -1) {
        iops->v.v0.error = EINVAL;
    }
    event_assign(event, ((struct libevent_cookie *)iops->v.v0.cookie)->base, -1, 0, NULL, NULL);
}

static void lcb_io_stop_event_loop(struct lcb_io_opt_st *iops)
{
    event_base_loopbreak(((struct libevent_cookie *)iops->v.v0.cookie)->base);
}

static void lcb_io_run_event_loop(struct lcb_io_opt_st *iops)
{
    event_base_loop(((struct libevent_cookie *)iops->v.v0.cookie)->base, 0);
}

static void lcb_destroy_io_opts(struct lcb_io_opt_st *iops)
{
    if (((struct libevent_cookie *)iops->v.v0.cookie)->allocated) {
        event_base_free(((struct libevent_cookie *)iops->v.v0.cookie)->base);
    }
    free(iops->v.v0.cookie);
    free(iops);
}

LIBCOUCHBASE_API
lcb_error_t lcb_create_libevent_io_opts(int version, lcb_io_opt_t *io, void *arg)
{
    struct event_base *base = arg;
    struct lcb_io_opt_st *ret;
    struct libevent_cookie *cookie;
    if (version != 0) {
        return LCB_PLUGIN_VERSION_MISMATCH;
    }

    ret = calloc(1, sizeof(*ret));
    cookie = calloc(1, sizeof(*cookie));
    if (ret == NULL || cookie == NULL) {
        free(ret);
        free(cookie);
        return LCB_CLIENT_ENOMEM;
    }

    /* setup io iops! */
    ret->version = 0;
    ret->dlhandle = NULL;
    ret->destructor = lcb_destroy_io_opts;
    /* consider that struct isn't allocated by the library,
     * `need_cleanup' flag might be set in lcb_create() */
    ret->v.v0.need_cleanup = 0;
    ret->v.v0.recv = lcb_io_recv;
    ret->v.v0.send = lcb_io_send;
    ret->v.v0.recvv = lcb_io_recvv;
    ret->v.v0.sendv = lcb_io_sendv;
    ret->v.v0.socket = lcb_io_socket;
    ret->v.v0.close = lcb_io_close;
    ret->v.v0.connect = lcb_io_connect;
    ret->v.v0.delete_event = lcb_io_delete_event;
    ret->v.v0.destroy_event = lcb_io_destroy_event;
    ret->v.v0.create_event = lcb_io_create_event;
    ret->v.v0.update_event = lcb_io_update_event;

    ret->v.v0.delete_timer = lcb_io_delete_timer;
    ret->v.v0.destroy_timer = lcb_io_destroy_event;
    ret->v.v0.create_timer = lcb_io_create_event;
    ret->v.v0.update_timer = lcb_io_update_timer;

    ret->v.v0.run_event_loop = lcb_io_run_event_loop;
    ret->v.v0.stop_event_loop = lcb_io_stop_event_loop;

    if (base == NULL) {
        if ((cookie->base = event_base_new()) == NULL) {
            free(ret);
            free(cookie);
            return LCB_CLIENT_ENOMEM;
        }
        cookie->allocated = 1;
    } else {
        cookie->base = base;
        cookie->allocated = 0;
    }
    ret->v.v0.cookie = cookie;

    *io = ret;
    return LCB_SUCCESS;
}
