
/*
 * CopyLeft (C) nie950@gmail.com
 */


#include "rtmp_config.h"
#include "rtmp_core.h"

int32_t rtmp_core_cycle(rtmp_session_t *session);
int32_t rtmp_core_handle_recv(rtmp_session_t *session);

int32_t rtmp_core_handle_message(rtmp_session_t *session,
    rtmp_chunk_header_t *chunk,mem_buf_chain_t *chain);

int32_t rtmp_core_update_inbytes(rtmp_session_t *session,
    rtmp_chunk_header_t *h,uint32_t n);

int32_t rtmp_core_message_info(rtmp_session_t *session,
    rtmp_chunk_header_t *h);

int32_t rtmp_core_update_chunk_time(rtmp_session_t *session,
    rtmp_chunk_header_t *h);

void rtmp_core_chunk_recv(rtmp_event_t *ev);
void rtmp_core_send_chunk(rtmp_event_t *ev);

int32_t rtmp_server_handshake(rtmp_session_t *session)
{
    rtmp_connection_t   *c;
    rtmp_event_t        *rev,*wev;
    rtmp_handshake_t    *h;

    c = session->c;
    rev = c->read;
    wev = c->write;

    h = session->handshake;

    rev->handler = rtmp_handshake_recv;
    wev->handler = rtmp_handshake_send;

    h->stage = RTMP_HANDSHAKE_SERVER_INIT;
    h->rbuf.end = h->rbuf.buf;

    rtmp_handshake_recv(rev);

    return RTMP_OK;
}

int32_t rtmp_client_handshake(rtmp_session_t *session)
{
    rtmp_connection_t   *c;
    rtmp_event_t        *rev,*wev;
    rtmp_handshake_t    *h;

    c = session->c;
    rev = c->read;
    wev = c->write;

    h = session->handshake;

    rev->handler = rtmp_handshake_recv;
    wev->handler = rtmp_handshake_send;

    h->stage = RTMP_HANDSHAKE_CLIENT_INIT;
    h->wbuf.end = h->wbuf.buf;

    rtmp_handshake_send(wev);

    return RTMP_OK;
}

void rtmp_core_init_connnect(rtmp_connection_t *conn)
{
    rtmp_session_t  *session;

    session = rtmp_session_create(conn);
    if (session == NULL) {
        rtmp_log(RTMP_LOG_ERR,"[%d]create session error!",conn->fd);
        free_connection(conn);
        return ;
    }

    conn->data = session;
    session->handshake->stage = RTMP_HANDSHAKE_SERVER_C0C1;

    rtmp_server_handshake(session);

    return;
}

int32_t rtmp_server_handshake_done(rtmp_session_t *session)
{
    rtmp_log(RTMP_LOG_INFO,"[%d]handshake done!",session->sid);
    session->in_chain = rtmp_core_alloc_chain(session,session->c->pool,
        session->in_chunk_size);

    if (session->in_chain == NULL) {
        rtmp_session_destroy(session);
        return RTMP_FAILED;
    }

    return rtmp_core_cycle(session);
}

int32_t rtmp_client_handshake_done(rtmp_session_t *session)
{
    return RTMP_OK;
}

int32_t rtmp_core_cycle(rtmp_session_t *session)
{
    rtmp_event_t        *rev,*wev;
    rtmp_connection_t   *conn;

    conn = session->c;

    rev = conn->read;
    wev = conn->write;

    rev->handler = rtmp_core_chunk_recv;
    wev->handler = NULL;

    rtmp_core_chunk_recv(rev);

    return RTMP_OK;
}

void rtmp_core_chunk_recv(rtmp_event_t *ev)
{
    rtmp_connection_t   *conn;
    rtmp_session_t      *session;
    int32_t              rc,hr,n;

    conn = ev->data;
    session = conn->data;

    if (ev->timer_set) {
        rtmp_event_del_timer(ev);
    }

    if (ev->timeout) {

        if (session->ping_sent == 1) {
            rtmp_log(RTMP_LOG_INFO,"[%d]recv timeout!",session->sid);
            rtmp_session_destroy(session);
            return;
        }

        session->ping_sent = 1;

        rtmp_send_ping_request(session);
        rtmp_event_add_timer(ev,session->ping_timeout);

        return;
    }

    for (;;) {
        rc = rtmp_recv_buf(conn->fd,&session->in_chain->chunk,&n);
        if (rc == SOCK_ERROR) {
            rtmp_log(RTMP_LOG_ERR,"[%d]recv error[%d]!",
                session->sid,sock_errno);
            rtmp_session_destroy(session);
            return ;
        }

        session->ping_sent = 0;

        hr = rtmp_core_update_inbytes(session,NULL,(uint32_t)n);
        if(hr != RTMP_OK) {
            rtmp_session_destroy(session);
            return ;
        }

        /*rtmp_core_handle_recv*/
        hr = rtmp_core_handle_recv(session);
        if (hr != RTMP_OK) {
            rtmp_session_destroy(session);
            return ;
        }

        if(rc == SOCK_EAGAIN) {
            if (!ev->active) {
                rtmp_event_add(conn->read,EVENT_READ);
            }
            rtmp_event_add_timer(ev,session->ping_timeout);
            break;
        }
    }
}

int32_t rtmp_core_handle_recv(rtmp_session_t *session)
{
    mem_buf_t            *rbuf;
    uint8_t              *payload;
    rtmp_chunk_header_t   hdr;
    rtmp_chunk_stream_t **old_streams;
    rtmp_chunk_stream_t  *st;
    mem_buf_t            *temp_buf;
    size_t                old_size,recv_len,need_len,copy_len;
    int32_t               rc;

    do {
        rbuf = &session->in_chain->chunk;

        /*read chunk head*/
        payload = rtmp_chunk_read(rbuf,&hdr);
        if (payload == 0) {
            return RTMP_OK;
        }

        /*realloc stream ?*/
        if (hdr.csid >= session->max_streams) {

            old_size = session->max_streams * sizeof(rtmp_chunk_stream_t*);
            old_streams = session->chunk_streams;

            session->chunk_streams = mem_pcalloc(session->pool,
                (hdr.csid + 1) * sizeof(rtmp_chunk_stream_t*));
            if (session->chunk_streams == NULL) {
                rtmp_log(RTMP_LOG_ERR,"alloc failed,csid:[%d]",hdr.csid);
                return RTMP_FAILED;
            }

            session->max_streams = hdr.csid + 1;
            memcpy(session->chunk_streams,old_streams,old_size);
        }

        /*get message info*/
        if (rtmp_core_message_info(session,&hdr) != RTMP_OK) {

            rtmp_log(RTMP_LOG_ERR,"[%d] invalid chunk,csid:[%d]",
                session->sid,hdr.csid);
            return RTMP_FAILED;
        }

        st = session->chunk_streams[hdr.csid];
        if (st == NULL) {
            st = mem_pcalloc(session->pool,sizeof(rtmp_chunk_stream_t));
            if (st == NULL) {
                rtmp_log(RTMP_LOG_ERR,"alloc failed,csid:[%d]",hdr.csid);
                return RTMP_FAILED;
            }
            session->chunk_streams[hdr.csid] = st;
        }

        if (st->recvlen == 0) {
            st->hdr = hdr;
        }

        /*an entire chunk ?*/
        recv_len = rbuf->last - payload ;

        if ((recv_len >= session->in_chunk_size)
         || (recv_len + st->recvlen >= st->hdr.msglen))
        {
            if (st->chain == NULL) {
                st->chain = st->last = session->in_chain;
            } else {
                st->last->next = session->in_chain;
                st->last = session->in_chain;
            }

            rtmp_log(RTMP_LOG_DEBUG,"[%d]get a chunk",session->sid);
            rtmp_core_update_chunk_time(session,&hdr);

            session->in_chain = rtmp_core_alloc_chain(session,
                session->c->pool,session->in_chunk_size);

            if (session->in_chain == NULL) {
                rtmp_log(RTMP_LOG_ERR,"[%d]alloc link failed,csid:[%d]",
                    session->sid,hdr.csid);
                return RTMP_FAILED;
            }

            temp_buf = &session->in_chain->chunk;
            need_len = st->hdr.msglen - st->recvlen;

            copy_len = recv_len - rtmp_min(
                rtmp_min(session->in_chunk_size,recv_len),need_len);
            st->recvlen += recv_len - copy_len;
            rbuf->last -= copy_len;

            /*copy left*/
            if (copy_len != 0) {
                memcpy(temp_buf->buf,rbuf->last,copy_len);
                temp_buf->last = temp_buf->buf + copy_len;
            }

            /*remove chunk head*/
            st->last->chunk.buf = payload;

            /*an entire message ?*/
            if (st->recvlen == st->hdr.msglen) {
				/*
				[2015-08-10 14:50:45][info][5][0]get a message:[0]:[20]:[99]
string "connect"
double 1.000000
object: [
[app] : string "live_server"
[type] : string "nonprivate"
[tcUrl] : string "rtmp://localhost/live_server"
[] : ]

				*/
                rtmp_log(RTMP_LOG_INFO,"[%d][%d]get a message:[%d]:[%d]:[%d]",
                    session->sid,session->chunk_time,st->hdr.msgsid,
                    st->hdr.msgtid,st->hdr.msglen);

                st->hdr.chunk_time = session->chunk_time;
                rc = rtmp_core_handle_message(session,&st->hdr,st->chain);

                if (rc != RTMP_OK) {
                    rtmp_log(RTMP_LOG_ERR,"[%d]message (%d)(%d) handler "
                        "failed:[%d]",session->sid,st->hdr.csid,
                        st->hdr.msgtid,rc);

                    return RTMP_FAILED;
                }

                st->recvlen = 0;
                rtmp_core_free_chains(session,session->c->pool,st->chain);
                st->chain = st->last = 0;

                continue;
            }
        }

        break;
    } while (1);

    return RTMP_OK;
}

void rtmp_chain_send(rtmp_event_t *ev)
{
    rtmp_connection_t   *conn;
    rtmp_session_t      *session;
    mem_buf_t           *sbuf,wbuf;
    int32_t              rc;
    uint32_t             out_rear,out_front;
    mem_buf_chain_t     *chain;
    rtmp_message_t      *msg;

    conn = ev->data;
    session = conn->data;

    if (ev->timeout) {
        rtmp_log(RTMP_LOG_WARNING,"[%d]send timeout!",conn->fd);
        rtmp_session_destroy(session);
        return;
    }

    if (ev->timer_set) {
        rtmp_event_del_timer(ev);
    }

    if (ev->active) {
        rtmp_event_delete(ev,EVENT_WRITE);
    }

    while (1) {
        out_rear = session->out_rear;
        out_front = session->out_front;

        if (session->out_chunk == NULL) {
            if (out_rear == out_front) {
                return ;
            }

            msg = & session->out_message[out_rear];
            rc = rtmp_send_buf(conn->fd,&msg->head,NULL);

            if (rc == SOCK_ERROR) {
                rtmp_log(RTMP_LOG_ERR,"[%d]send error:%p,%d",session->sid,rc);

                return;
            }

            if (rc == SOCK_EAGAIN) {
                rtmp_log(RTMP_LOG_DEBUG,"[%d]send again:%d",session->sid,rc);

                if (!ev->active) {
                    rtmp_event_add(ev,EVENT_WRITE);
                }
                rtmp_event_add_timer(ev,6000);

                return ;
            }

            session->out_chunk = msg->chain;
            if (session->out_chunk == NULL) {

                rtmp_log(RTMP_LOG_WARNING,"[%d]pick null message",
                    session->sid);

                session->out_rear = (out_rear + 1) % session->out_queue;

                continue;
            }

            session->out_last = session->out_chunk->chunk.last;
        }

        while (session->out_chunk) {
            sbuf = & session->out_chunk->chunk;

            if ((session->out_last < sbuf->last)
             || (session->out_last >= sbuf->end))
            {
                rtmp_log(RTMP_LOG_WARNING,"[%d]send error chunk",session->sid);
                session->out_last = sbuf->last;
            }

            wbuf.buf = sbuf->buf;
            wbuf.last = session->out_last;
            wbuf.end = sbuf->end;

            rtmp_log(RTMP_LOG_DEBUG,"[%d]send (%p,%d) bytes",session->sid,
                session->out_last,wbuf.end - wbuf.last);

            rc = rtmp_send_buf(conn->fd,&wbuf,NULL);

            if (rc == SOCK_ERROR) {
                rtmp_log(RTMP_LOG_ERR,"[%d]send error:%p,%d",session->sid,rc);

                return;
            }

            if (rc == SOCK_EAGAIN) {
                rtmp_log(RTMP_LOG_DEBUG,"[%d]send again:%d",session->sid,rc);

                session->out_last = wbuf.last;

                if (!ev->active) {
                    rtmp_event_add(ev,EVENT_WRITE);
                }
                rtmp_event_add_timer(ev,6000);

                return ;
            }

            session->ping_sent = 0;

            chain = session->out_chunk->next;
            session->out_chunk = chain;

            if (chain) {
                session->out_last = chain->chunk.last;
            }
        }

        chain = session->out_message[out_rear].chain;

        rtmp_log(RTMP_LOG_INFO,"[%d]send a message",session->sid);

        rtmp_core_free_chain(session,session->chunk_pool,chain);

        session->out_message[out_rear].chain = NULL;
        session->out_last = NULL;

        session->out_rear = (out_rear + 1) % session->out_queue;
    }

    return ;
}

int32_t rtmp_core_handle_message(rtmp_session_t *session,
    rtmp_chunk_header_t *chunk,mem_buf_chain_t *chain)
{
    rtmp_cycle_t        *cycle;
    rtmp_msg_handler_t **handler;

    cycle = session->c->listening->cycle;
    if (chunk->msgtid >= RTMP_MSG_MAX) {
        rtmp_log(RTMP_LOG_WARNING,"unknown message type id:%d",chunk->msgtid);
        return RTMP_OK;
    }

    handler = (rtmp_msg_handler_t**)cycle->msg_handler.elts;
    if (handler[chunk->msgtid]->pt) {
        mem_reset_pool(session->temp_pool);
        return handler[chunk->msgtid]->pt(session,chunk,chain);
    }

    return RTMP_OK;
}

int32_t rtmp_core_message_info(rtmp_session_t *session,
    rtmp_chunk_header_t *h)
{
    rtmp_chunk_stream_t *last;
    last = session->chunk_streams[h->csid];

    if (((last == NULL) && (h->fmt > 0)) || (h->csid < 2)) {
        return RTMP_FAILED;
    }

    if ((session->chunk_time == (uint32_t)-1) && h->fmt > 0) {
        return RTMP_FAILED;
    }

    switch (h->fmt) {
    case 3:
        h->dtime = last->hdr.dtime;
        h->extend = last->hdr.extend;

    case 2:
        h->msglen = last->hdr.msglen;
        h->msgtid = last->hdr.msgtid;

    case 1:
        h->msgsid = last->hdr.msgsid;

    case 0:
        break;

    default:
        rtmp_log(RTMP_LOG_DEBUG,"[%d]never reach here, csid:[%d]",
            session->sid,h->csid);

        break;
    }

    return RTMP_OK;
}

int32_t rtmp_core_update_chunk_time(rtmp_session_t *session,
    rtmp_chunk_header_t *h)
{
    rtmp_chunk_stream_t *st;

    st = session->chunk_streams[h->csid];

    /*ignore fragment*/
    if (st->recvlen == 0) {
        switch (st->hdr.fmt) {

        case 0:
            if (st->hdr.dtime == 0xffffff) {
                session->chunk_time = st->hdr.extend;
            } else {
                session->chunk_time = st->hdr.dtime;
            }
            break;

        case 1:
        case 2:
        case 3:

            if (st->hdr.dtime == 0xffffff) {
                session->chunk_time += st->hdr.extend;
            } else {
                session->chunk_time += st->hdr.dtime;
            }
            break;
        }
    }

    return RTMP_OK;
}

mem_buf_chain_t* rtmp_core_alloc_chain(rtmp_session_t *session,
    mem_pool_t *pool,int32_t chunk_size)
{
    mem_buf_chain_t *chain;
    uint8_t         *buf;

    chain = mem_alloc_chain_link(pool);
    if (chain) {
        buf = chain->chunk_body;
        if (chain->chunk_size < (uint32_t)chunk_size) {
            buf = NULL;
        }

        if (buf == NULL) {
            buf = mem_pcalloc(pool,chunk_size + RTMP_MAX_CHUNK_HEADER);

            if (buf == NULL) {
                mem_free_chain_link(pool,chain);
                return NULL;
            }

            chain->chunk_body = buf;
            chain->chunk_size = chunk_size;
        }

        chain->chunk.buf  = buf;
        chain->chunk.last = buf;
        chain->chunk.end  = buf + chunk_size + RTMP_MAX_CHUNK_HEADER;
        chain->locked = 1;
        chain->next = NULL;

        rtmp_log(RTMP_LOG_DEBUG,"[%d]alloc chain:%p .buf=%p .last=%p .end=%p",
            session->sid,chain,chain->chunk.buf,
            chain->chunk.last,chain->chunk.end);
    }

    return chain;
}

void rtmp_core_lock_chains(mem_buf_chain_t *chain)
{
    while (chain) {
        chain->locked++;
        chain = chain->next;
    }
}

void rtmp_core_lock_chain(mem_buf_chain_t *chain)
{
    chain->locked++;
}

void rtmp_core_free_chain(rtmp_session_t *session,
    mem_pool_t *pool,mem_buf_chain_t *chain)
{
    if (--chain->locked <= 0) {
        chain->chunk.last = chain->chunk.buf = chain->chunk_body;
        mem_free_chain_link(pool,chain);

        rtmp_log(RTMP_LOG_DEBUG,"[%d]free chain:%p .buf=%p .last=%p .end=%p",
            session->sid,chain,chain->chunk.buf,
            chain->chunk.last,chain->chunk.end);
    }
}

void rtmp_core_free_chains(rtmp_session_t *session,
    mem_pool_t *pool,mem_buf_chain_t *chain)
{
    mem_buf_chain_t *next;

    while (chain) {
        next = chain->next;
        rtmp_core_free_chain(session,pool,chain);
        chain = next;
    }
}

mem_buf_t* rtmp_prepare_amf_buffer(mem_pool_t *temp_pool,
    amf_data_t **amf,uint32_t num)
{
    mem_buf_t *buf,*tbuf;
    int32_t   r,i,failed,n;

    buf = mem_buf_pcalloc(temp_pool,NULL,1024);
    if (buf == NULL) {
        return NULL;
    }
    failed = 0;

    for (i = 0;i < (int32_t)num; i++) {

        if (failed > 10) {
            return NULL;
        }

        n = buf->end - buf->last;
        r = amf_encode(amf[i],(char*)buf->last,n);

        if (r == -1) {
            failed++;

            tbuf = mem_buf_pcalloc(temp_pool,NULL,
                buf->end - buf->buf + 12);
            if (tbuf == NULL) {
                return NULL;
            }

            tbuf->last = tbuf->buf + (buf->last - buf->buf);
            memcpy(tbuf->buf,buf->buf,buf->last - buf->buf);

            buf = tbuf;
            i--;
        } else {
            buf->last += n - r;
        }
    }

    return buf;
}

uint32_t rtmp_hash_string(const char *data)
{
    uint32_t  i, key;

    key = 0;

    for (i = 0; data[i]; i++) {
        key = rtmp_hash(key, data[i]);
    }

    return key;
}

uint32_t rtmp_hash_key(const u_char *data, size_t len)
{
    uint32_t  i, key;

    key = 0;

    for (i = 0; i < len; i++) {
        key = rtmp_hash(key, data[i]);
    }

    return key;
}

int32_t rtmp_core_update_inbytes(rtmp_session_t *session,
    rtmp_chunk_header_t *h,uint32_t n)
{
    int32_t      rc;

    session->in_bytes += n;
    if (session->in_bytes >= 0xf0000000) {
        session->in_bytes = 0;
        session->in_last_ack = 0;
        rtmp_log(RTMP_LOG_DEBUG,"reset byte counter");
    }

    n = session->in_bytes - session->in_last_ack;
    if (n > session->ack_window) {
        session->in_last_ack = session->in_bytes;
        rc = rtmp_create_append_chain(session,rtmp_create_ack,h);
        if (rc != RTMP_OK) {
            rtmp_log(RTMP_LOG_WARNING,"[%d]append ack "
                "message failed!",session->sid);
            return RTMP_FAILED;
        }
    }

    return RTMP_OK;
}
