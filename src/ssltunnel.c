/* ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

/*** Setup a SSL tunnel over a HTTP proxy, according to RFC 2817. ***/

#include <apr_pools.h>
#include <apr_strings.h>

#include "serf.h"
#include "serf_private.h"


/* Structure passed around as baton for the CONNECT request and respone. */
typedef struct req_ctx_t {
    apr_pool_t *pool;
    const char *uri;
} req_ctx_t;

/* forward declaration. */
static apr_status_t setup_request(serf_request_t *request,
                                  void *setup_baton,
                                  serf_bucket_t **req_bkt,
                                  serf_response_acceptor_t *acceptor,
                                  void **acceptor_baton,
                                  serf_response_handler_t *handler,
                                  void **handler_baton,
                                  apr_pool_t *pool);

static serf_bucket_t* accept_response(serf_request_t *request,
                                      serf_bucket_t *stream,
                                      void *acceptor_baton,
                                      apr_pool_t *pool)
{
    serf_bucket_t *c;
    serf_bucket_alloc_t *bkt_alloc;
#if 0
    req_ctx_t *ctx = acceptor_baton;
#endif

    /* get the per-request bucket allocator */
    bkt_alloc = serf_request_get_alloc(request);

    /* Create a barrier so the response doesn't eat us! */
    c = serf_bucket_barrier_create(stream, bkt_alloc);

    return serf_bucket_response_create(c, bkt_alloc);
}

/* If a 200 OK was received for the CONNECT request, consider the connection
   as ready for use. */
static apr_status_t handle_response(serf_request_t *request,
                                    serf_bucket_t *response,
                                    void *handler_baton,
                                    apr_pool_t *pool)
{
    apr_status_t status;
    serf_status_line sl;
    req_ctx_t *ctx = handler_baton;
    serf_connection_t *conn = request->conn;

    /* CONNECT request was cancelled. Assuming that this is during connection
       reset, we can safely discard the request as a new one will be created
       when setting up the next connection. */
    if (!response)
        return APR_SUCCESS;

    status = serf_bucket_response_status(response, &sl);
    if (SERF_BUCKET_READ_ERROR(status)) {
        return status;
    }
    if (!sl.version && (APR_STATUS_IS_EOF(status) ||
                      APR_STATUS_IS_EAGAIN(status)))
    {
        return status;
    }

    status = serf_bucket_response_wait_for_headers(response);
    if (status && !APR_STATUS_IS_EOF(status)) {
        return status;
    }

    /* RFC 2817:  Any successful (2xx) response to a CONNECT request indicates
       that the proxy has established a connection to the requested host and
       port, and has switched to tunneling the current connection to that server
       connection.
    */
    if (sl.code >= 200 && sl.code < 300) {
        serf_bucket_t *hdrs;
        const char *val;



        /* Body is supposed to be empty. */
        apr_pool_destroy(ctx->pool);

        /* If there was outgoing data waiting, we can't use it
           any more. It's lifetime is limited by ostream_head
           ... (There shouldn't be any, as we disabled pipelining) */
        conn->pump.vec_len = 0;

        conn->state = SERF_CONN_CONNECTED;

        /* Destroy the unencrypted head */
        serf_bucket_destroy(conn->pump.ostream_head);
        conn->pump.ostream_head = NULL;
        /* And the unencrypted stream */
        serf_bucket_destroy(conn->pump.stream);
        conn->pump.stream = NULL;

        /* New ones will be created in the normal setup code */
        ctx = NULL;

        serf__log(LOGLVL_INFO, LOGCOMP_CONN, __FILE__, conn->config,
                  "successfully set up ssl tunnel.\n");

        /* Fix for issue #123: ignore the "Connection: close" header here,
           leaving the header in place would make the serf's main context
           loop close this connection immediately after reading the 200 OK
           response. */

        hdrs = serf_bucket_response_get_headers(response);
        val = serf_bucket_headers_get(hdrs, "Connection");
        if (val && strcasecmp("close", val) == 0) {
            serf__log(LOGLVL_DEBUG, LOGCOMP_CONN, __FILE__, conn->config,
                      "Ignore Connection: close header on this reponse, don't "
                      "close the connection now that the tunnel is set up.\n");
            serf__bucket_headers_remove(hdrs, "Connection");
        }

        status = serf_connection__perform_setup(conn);

        if (status)
            return SERF_BUCKET_READ_ERROR(status) ? status : APR_EGENERAL;

        return APR_EOF;
    }

    /* Authentication failure and 2xx Ok are handled at this point,
       the rest are errors. */
    return SERF_ERROR_SSLTUNNEL_SETUP_FAILED;
}

/* Prepare the CONNECT request. */
static apr_status_t setup_request(serf_request_t *request,
                                  void *setup_baton,
                                  serf_bucket_t **req_bkt,
                                  serf_response_acceptor_t *acceptor,
                                  void **acceptor_baton,
                                  serf_response_handler_t *handler,
                                  void **handler_baton,
                                  apr_pool_t *pool)
{
    req_ctx_t *ctx = setup_baton;
    serf_bucket_t *hdrs_bkt;

    *req_bkt = serf_bucket_request_create("CONNECT", ctx->uri, NULL,
                                          serf_request_get_alloc(request));

    hdrs_bkt = serf_bucket_request_get_headers(*req_bkt);
    serf_bucket_headers_setn(hdrs_bkt, "Host", ctx->uri);

    /* If proxy authn is required, then set it up.  */
    serf__auth_setup_request(PROXY, request, "CONNECT", ctx->uri, hdrs_bkt);

    *acceptor = accept_response;
    *acceptor_baton = ctx;
    *handler = handle_response;
    *handler_baton = ctx;

    return APR_SUCCESS;
}

static apr_status_t ssltunnel_ostream_destroyed(void *baton,
                                                apr_uint64_t bytes_read)
{
    serf_connection_t *conn = baton;

    if (conn->state == SERF_CONN_SETUP_SSLTUNNEL) {
        /* Connection is destroyed while not connected.
           Destroy tail to avoid leaking memory */
        serf_bucket_destroy(conn->pump.ostream_tail);
        conn->pump.ostream_tail = NULL;
    }
    return APR_SUCCESS;
}

/* SSL tunnel is needed, push a CONNECT request on the connection. */
apr_status_t serf__ssltunnel_connect(serf_connection_t *conn)
{
    req_ctx_t *ctx;
    apr_pool_t *ssltunnel_pool;
    serf_bucket_t *stream, *ostream;

    apr_pool_create(&ssltunnel_pool, conn->pool);

    ctx = apr_palloc(ssltunnel_pool, sizeof(*ctx));
    ctx->pool = ssltunnel_pool;
    ctx->uri = apr_psprintf(ctx->pool, "%s:%d", conn->host_info.hostname,
                            conn->host_info.port);

    /* We want to setup a plain http request to be sent before the
       actual streams are connected... */
    serf_pump__prepare_setup(&conn->pump);

    /* Ok, we now have a head and a tail bucket. The tail has pump
       events attached to it so we don't want to destroy that one
       later. Let's create a barrier around it and manage the lifetime
       ourself. */

    ostream = serf_bucket_barrier_create(conn->pump.ostream_tail,
                                         conn->allocator);

    ostream = serf__bucket_event_create(ostream,
                                        conn, NULL, NULL,
                                        ssltunnel_ostream_destroyed,
                                        conn->allocator);

    stream = serf_context_bucket_socket_create(conn->ctx,
                                               conn->skt,
                                               conn->allocator);

    serf_pump__complete_setup(&conn->pump, stream, ostream);


    serf__ssltunnel_request_create(conn,
                                   setup_request,
                                   ctx);

    conn->state = SERF_CONN_SETUP_SSLTUNNEL;
    serf__log(LOGLVL_DEBUG, LOGCOMP_CONN, __FILE__, conn->config,
              "setting up ssl tunnel on connection.\n");

    return APR_SUCCESS;
}