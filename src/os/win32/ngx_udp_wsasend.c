
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


ssize_t
ngx_udp_wsasend(ngx_connection_t *c, u_char *buf, size_t size)
{
    int           rc;
    u_long        sent;
    WSABUF        wsabuf[1];
    ngx_err_t     err;
    ngx_event_t  *wev;

    wev = c->write;

    for ( ;; ) {
        wsabuf[0].buf = (char *) buf;
        wsabuf[0].len = size;
        sent = 0;

        rc = WSASendTo(c->fd, wsabuf, 1, &sent, 0,
                       c->sockaddr, c->socklen, NULL, NULL);

        ngx_log_debug4(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "WSASendTo: fd:%d rc:%d %ul of %uz",
                       c->fd, rc, sent, size);

        if (rc == -1) {
            err = ngx_socket_errno;

            if (err == WSAEWOULDBLOCK) {
                wev->ready = 0;
                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,
                               "WSASendTo() not ready");
                return NGX_AGAIN;
            }

            if (err != WSAEINTR) {
                wev->error = 1;
                ngx_connection_error(c, err, "WSASendTo() failed");
                return NGX_ERROR;
            }

        } else {
            if ((size_t) sent != size) {
                wev->error = 1;
                (void) ngx_connection_error(c, 0, "WSASendTo() incomplete");
                return NGX_ERROR;
            }

            c->sent += sent;

            return sent;
        }
    }
}


ngx_chain_t *
ngx_udp_wsasend_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit)
{
    u_char        *prev;
    ssize_t        n, size, sent;
    off_t          send, prev_send;
    ngx_chain_t   *cl;
    ngx_event_t   *wev;
    ngx_chain_t   *out;
    static u_char  sockaddr[NGX_SOCKADDRLEN];

    wev = c->write;

    if (!wev->ready) {
        return in;
    }

    /* the maximum limit size is the maximum size_t value - the page size */

    if (limit == 0 || limit > (off_t) (NGX_MAX_SIZE_T_VALUE - ngx_pagesize)) {
        limit = NGX_MAX_SIZE_T_VALUE - ngx_pagesize;
    }

    send = 0;

    for ( ;; ) {
        prev = NULL;
        prev_send = send;
        out = NULL;

        /* create the WSABUF and coalesce the neighbouring bufs */

        for (cl = in; cl && send < limit; cl = cl->next) {

            if (ngx_buf_special(cl->buf)) {
                continue;
            }

            size = cl->buf->last - cl->buf->pos;

            if (send + size > limit) {
                size = (ssize_t) (limit - send);
            }

            if (prev == cl->buf->pos) {
                out->buf->last = cl->buf->last;

            } else {
                if (out == NULL) {
                    out = ngx_alloc_chain_link(c->pool);
                    if (out == NULL) {
                        return NGX_CHAIN_ERROR;
                    }

                    out->buf = ngx_calloc_buf(c->pool);
                    if (out->buf == NULL) {
                        return NGX_CHAIN_ERROR;
                    }

                    out->buf->temporary = 1;
                    out->buf->pos = cl->buf->pos;
                    out->buf->last = cl->buf->last;
                    out->buf->start = cl->buf->start;
                    out->buf->end = cl->buf->end;
                }
            }

            prev = cl->buf->last;
            send += size;
        }

        if (out == NULL) {
            return in;
        }

        size = out->buf->last - out->buf->pos;

        if (send < prev_send + size) {
            size = (ssize_t) (send - prev_send);
        }

        n = ngx_udp_wsasend(c, out->buf->pos, size);

        if (n == NGX_ERROR) {
            return NGX_CHAIN_ERROR;
        }

        if (n == NGX_AGAIN) {
            wev->ready = 0;
            return in;
        }

        sent = n;

        c->sent += sent;

        for (cl = in; cl; cl = cl->next) {

            if (ngx_buf_special(cl->buf)) {
                continue;
            }

            if (sent == 0) {
                break;
            }

            size = cl->buf->last - cl->buf->pos;

            if (sent >= size) {
                sent -= size;
                cl->buf->pos = cl->buf->last;

                continue;
            }

            cl->buf->pos += sent;

            break;
        }

        if (cl == NULL) {
            return NULL;
        }

        in = cl;
        send = 0;
    }
}


ssize_t
ngx_udp_overlapped_wsasend(ngx_connection_t *c, u_char *buf, size_t size)
{
    int               rc;
    u_long            sent, flags;
    WSABUF            wsabuf[1];
    ngx_err_t         err;
    ngx_event_t      *wev;
    LPWSAOVERLAPPED   ovlp;

    wev = c->write;

    if (!wev->ready) {
        ngx_log_error(NGX_LOG_ALERT, c->log, 0, "second wsa post");
        return NGX_AGAIN;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "wev->complete: %d", wev->complete);

    if (wev->complete) {
        wev->complete = 0;

        if (ngx_event_flags & NGX_USE_IOCP_EVENT) {
            if (wev->ovlp.error) {
                ngx_connection_error(c, wev->ovlp.error, "WSASendTo() failed");
                return NGX_ERROR;
            }

            ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "WSASendTo ovlp: fd:%d %ul of %uz",
                           c->fd, wev->available, size);

            if (size != (size_t) wev->available) {
                wev->error = 1;
                (void) ngx_connection_error(c, 0, "WSASendTo() incomplete");
                return NGX_ERROR;
            }

            c->sent += wev->available;

            return wev->available;
        }

        if (WSAGetOverlappedResult(c->fd, (LPWSAOVERLAPPED) &wev->ovlp,
                                   &sent, 0, &flags)
            == 0)
        {
            ngx_connection_error(c, ngx_socket_errno,
                               "WSASendTo() or WSAGetOverlappedResult() failed");
            return NGX_ERROR;
        }

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "WSASendTo: fd:%d %ul of %uz", c->fd, sent, size);

        if ((size_t) sent != size) {
            wev->error = 1;
            (void) ngx_connection_error(c, 0, "WSASendTo() incomplete");
            return NGX_ERROR;
        }

        c->sent += sent;

        return sent;
    }

    ovlp = (LPWSAOVERLAPPED) &wev->ovlp;
    ngx_memzero(ovlp, sizeof(WSAOVERLAPPED));
    wsabuf[0].buf = (char *) buf;
    wsabuf[0].len = size;
    sent = 0;

    rc = WSASendTo(c->fd, wsabuf, 1, &sent, 0,
                   c->sockaddr, c->socklen, ovlp, NULL);

    wev->complete = 0;

    ngx_log_debug4(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "WSASendTo ovlp: fd:%d rc:%d %ul of %uz",
                   c->fd, rc, sent, size);

    if (rc == -1) {
        err = ngx_socket_errno;
        if (err == WSA_IO_PENDING) {
            wev->active = 1;
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,
                           "WSASendTo() posted");
            return NGX_AGAIN;
        }

        wev->error = 1;
        ngx_connection_error(c, err, "WSASendTo() failed");
        return NGX_ERROR;
    }

    if (ngx_event_flags & NGX_USE_IOCP_EVENT) {

        /*
         * if a socket was bound with I/O completion port then
         * GetQueuedCompletionStatus() would anyway return its status
         * despite that WSASendTo() was already complete
         */

        wev->active = 1;
        return NGX_AGAIN;
    }

    wev->active = 0;

    if (sent != size) {
        wev->error = 1;
        (void) ngx_connection_error(c, 0, "WSASendTo() incomplete");
        return NGX_ERROR;
    }

    c->sent += sent;

    return sent;
}
