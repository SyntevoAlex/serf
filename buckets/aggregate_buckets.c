/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2003 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 */

#include <apr_pools.h>

#include "serf.h"
#include "serf_bucket_util.h"

/* Should be an APR_RING? */
typedef struct bucket_list {
    serf_bucket_t *bucket;
    struct bucket_list *next;
} bucket_list;

typedef struct serf_aggregate_context_t {
    bucket_list *list;
} serf_aggregate_context_t;

SERF_DECLARE(serf_bucket_t *) serf_bucket_aggregate_create(
    serf_bucket_alloc_t *allocator)
{
    serf_aggregate_context_t *agg_context;

    serf_bucket_mem_alloc(allocator, sizeof(*agg_context));

    /* Theoretically, we *could* store this in the metadata of our bucket,
     * but that'd be ridiculously slow.
     */
    agg_context->list = NULL;

    return serf_bucket_create(serf_bucket_type_aggregate, allocator,
                              agg_context);
}

SERF_DECLARE(void) serf_bucket_aggregate_become(serf_bucket_t *bucket)
{
    /* Create a new bucket and swap their internal pointers? */
}


SERF_DECLARE(void) serf_bucket_aggregate_prepend(
    serf_bucket_t *aggregate_bucket,
    serf_bucket_t *prepend_bucket)
{
    serf_aggregate_context_t *agg_context;
    bucket_list *new_bucket;

    agg_context = (serf_aggregate_context_t*)aggregate_bucket->data;
    new_bucket = serf_bucket_mem_alloc(aggregate_bucket->allocator,
                                       sizeof(*bucket_list));

    new_bucket->bucket = prepend_bucket;
    new_bucket->next = agg_context->list;
    agg_context->list = new_bucket;
}

SERF_DECLARE(void) serf_bucket_aggregate_append(
    serf_bucket_t *aggregate_bucket,
    serf_bucket_t *prepend_bucket)
{
    serf_aggregate_context_t *agg_context;
    bucket_list *new_bucket;

    agg_context = (serf_aggregate_context_t*)aggregate_bucket->data;
    new_bucket = serf_bucket_mem_alloc(aggregate_bucket->allocator,
                                       sizeof(*bucket_list));

    /* If we use APR_RING, this is trivial.  So, wait. 
    new_bucket->bucket = prepend_bucket;
    new_bucket->next = agg_context->list;
    agg_context->list = new_bucket;
    */
}

static apr_status_t serf_aggregate_read(serf_bucket_t *bucket,
                                        apr_size_t requested,
                                        const char **data, apr_size_t *len)
{
    apr_status_t status;
    serf_aggregate_context_t *agg_context;

    agg_context = (serf_aggregate_context_t*)aggregate_bucket->data;
    if (!agg_context->list) {
        *len = 0;
        return APR_SUCCESS;
    }

    status = serf_bucket_read(agg_context->list->bucket, requested, data, len);

    /* Somehow, we need to know whether we're exhausted! */
    if (!status && *len == 0) {
        agg_context->list = agg_context->list->next;
        /* Avoid recursive call here.  Too lazy now.  */
        return serf_aggregate_read(bucket, request, data, len);
    }

    return status;
}

static apr_status_t serf_aggregate_readline(serf_bucket_t *bucket,
                                            int acceptable, int *found,
                                            const char **data, apr_size_t *len)
{
    /* Follow pattern from serf_aggregate_read. */
    return APR_ENOTIMPL;
}

static apr_status_t serf_aggregate_peek(serf_bucket_t *bucket,
                                        const char **data,
                                        apr_size_t *len)
{
    /* Follow pattern from serf_aggregate_read. */
    return APR_ENOTIMPL;
}

static serf_bucket_t * serf_aggregate_read_bucket(serf_bucket_t *bucket,
                                                  serf_bucket_type_t *type)
{
    apr_status_t status;
    serf_aggregate_context_t *agg_context;

    agg_context = (serf_aggregate_context_t*)aggregate_bucket->data;
    if (!agg_context->list) {
        return NULL;
    }

    /* Call read_bucket on first one in our list. */
    return serf_bucket_read_bucket(agg_context->list->bucket, type);
}

SERF_DECLARE_DATA serf_bucket_type_t serf_bucket_type_aggregate {
    "AGGREGATE",
    serf_aggregate_read,
    serf_aggregate_readline,
    serf_aggregate_peek,
    serf_aggregate_read_bucket,
    NULL, /* set_metadata */
    NULL, /* get_metadata */
    serf_default_destroy,
};
