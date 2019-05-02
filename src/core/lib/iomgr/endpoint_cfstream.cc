/*
 *
 * Copyright 2018 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <grpc/support/port_platform.h>

#include "src/core/lib/iomgr/port.h"

#ifdef GRPC_CFSTREAM_ENDPOINT

#import <CoreFoundation/CoreFoundation.h>
#import "src/core/lib/iomgr/endpoint_cfstream.h"

#include <grpc/slice_buffer.h>
#include <grpc/support/alloc.h>
#include <grpc/support/string_util.h>

#include "src/core/lib/gpr/string.h"
#include "src/core/lib/iomgr/cfstream_handle.h"
#include "src/core/lib/iomgr/closure.h"
#include "src/core/lib/iomgr/endpoint.h"
#include "src/core/lib/iomgr/error_cfstream.h"
#include "src/core/lib/iomgr/timer.h"

#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"

extern grpc_core::TraceFlag grpc_tcp_trace;

typedef struct {
  grpc_endpoint base;
  gpr_refcount refcount;

  CFReadStreamRef read_stream;
  CFWriteStreamRef write_stream;
  CFStreamHandle* stream_sync;

  grpc_closure* read_cb;
  grpc_closure* write_cb;
  grpc_slice_buffer* read_slices;
  grpc_slice_buffer* write_slices;

  grpc_closure read_action;
  grpc_closure write_action;

  char* peer_string;
  grpc_resource_user* resource_user;
  grpc_resource_user_slice_allocator slice_allocator;
  gpr_mu mu;
  bool timer_armed;
  grpc_timer timer;
  uint32_t total_bytes_read;
} CFStreamEndpoint;

static void CFStreamFree(CFStreamEndpoint* ep) {
  gpr_log(GPR_ERROR, "CFStreamFree ep=%p", ep);
  grpc_resource_user_unref(ep->resource_user);
  CFRelease(ep->read_stream);
  CFRelease(ep->write_stream);
  CFSTREAM_HANDLE_UNREF(ep->stream_sync, "free");
  gpr_free(ep->peer_string);
  gpr_free(ep);
}

#ifndef NDEBUG
#define EP_REF(ep, reason) CFStreamRef((ep), (reason), __FILE__, __LINE__)
#define EP_UNREF(ep, reason) CFStreamUnref((ep), (reason), __FILE__, __LINE__)
static void CFStreamUnref(CFStreamEndpoint* ep, const char* reason,
                          const char* file, int line) {
  if (grpc_tcp_trace.enabled()) {
    gpr_atm val = gpr_atm_no_barrier_load(&ep->refcount.count);
    gpr_log(file, line, GPR_LOG_SEVERITY_DEBUG,
            "CFStream endpoint unref %p : %s %" PRIdPTR " -> %" PRIdPTR, ep,
            reason, val, val - 1);
  }
  if (gpr_unref(&ep->refcount)) {
    CFStreamFree(ep);
  }
}
static void CFStreamRef(CFStreamEndpoint* ep, const char* reason,
                        const char* file, int line) {
  if (grpc_tcp_trace.enabled()) {
    gpr_atm val = gpr_atm_no_barrier_load(&ep->refcount.count);
    gpr_log(file, line, GPR_LOG_SEVERITY_DEBUG,
            "CFStream endpoint ref %p : %s %" PRIdPTR " -> %" PRIdPTR, ep,
            reason, val, val + 1);
  }
  gpr_ref(&ep->refcount);
}
#else
#define EP_REF(ep, reason) CFStreamRef((ep))
#define EP_UNREF(ep, reason) CFStreamUnref((ep))
static void CFStreamUnref(CFStreamEndpoint* ep) {
  if (gpr_unref(&ep->refcount)) {
    CFStreamFree(ep);
  }
}
static void CFStreamRef(CFStreamEndpoint* ep) { gpr_ref(&ep->refcount); }
#endif

static grpc_error* CFStreamAnnotateError(grpc_error* src_error,
                                         CFStreamEndpoint* ep) {
  return grpc_error_set_str(
      grpc_error_set_int(src_error, GRPC_ERROR_INT_GRPC_STATUS,
                         GRPC_STATUS_UNAVAILABLE),
      GRPC_ERROR_STR_TARGET_ADDRESS,
      grpc_slice_from_copied_string(ep->peer_string));
}

static void CallReadCb(CFStreamEndpoint* ep, grpc_error* error) {
  if (grpc_tcp_trace.enabled()) {
    gpr_log(GPR_DEBUG, "CFStream endpoint:%p call_read_cb %p %p:%p", ep,
            ep->read_cb, ep->read_cb->cb, ep->read_cb->cb_arg);
    size_t i;
    const char* str = grpc_error_string(error);
    gpr_log(GPR_DEBUG, "read: error=%s", str);

    for (i = 0; i < ep->read_slices->count; i++) {
      char* dump = grpc_dump_slice(ep->read_slices->slices[i],
                                   GPR_DUMP_HEX | GPR_DUMP_ASCII);
      gpr_log(GPR_DEBUG, "READ %p (peer=%s): %s", ep, ep->peer_string, dump);
      gpr_free(dump);
    }
  }
  grpc_closure* cb = ep->read_cb;
  ep->read_cb = nullptr;
  ep->read_slices = nullptr;
  GRPC_CLOSURE_SCHED(cb, error);
}

static void CallWriteCb(CFStreamEndpoint* ep, grpc_error* error) {
  if (grpc_tcp_trace.enabled()) {
    gpr_log(GPR_DEBUG, "CFStream endpoint:%p call_write_cb %p %p:%p", ep,
            ep->write_cb, ep->write_cb->cb, ep->write_cb->cb_arg);
    const char* str = grpc_error_string(error);
    gpr_log(GPR_DEBUG, "write: error=%s", str);
  }
  grpc_closure* cb = ep->write_cb;
  ep->write_cb = nullptr;
  ep->write_slices = nullptr;
  GRPC_CLOSURE_SCHED(cb, error);
}

static void ReadAction(void* arg, grpc_error* error) {
  CFStreamEndpoint* ep = static_cast<CFStreamEndpoint*>(arg);
  GPR_ASSERT(ep->read_cb != nullptr);
  if (error) {
    grpc_slice_buffer_reset_and_unref_internal(ep->read_slices);
    CallReadCb(ep, GRPC_ERROR_REF(error));
    EP_UNREF(ep, "read");
    return;
  }

  GPR_ASSERT(ep->read_slices->count == 1);
  grpc_slice slice = ep->read_slices->slices[0];
  size_t len = GRPC_SLICE_LENGTH(slice);
  CFIndex read_size =
      CFReadStreamRead(ep->read_stream, GRPC_SLICE_START_PTR(slice), len);
  gpr_log(GPR_ERROR, "ReadAction read_size=%lu CFReadStreamHasBytesAvailable %u", read_size, CFReadStreamHasBytesAvailable(ep->read_stream));
  if (read_size == -1) {
    grpc_slice_buffer_reset_and_unref_internal(ep->read_slices);
    CFErrorRef stream_error = CFReadStreamCopyError(ep->read_stream);
    if (stream_error != nullptr) {
      error = CFStreamAnnotateError(
          GRPC_ERROR_CREATE_FROM_CFERROR(stream_error, "Read error"), ep);
      CFRelease(stream_error);
    } else {
      error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("Read error");
    }
    CallReadCb(ep, error);
    EP_UNREF(ep, "read");
  } else if (read_size == 0) {
    grpc_slice_buffer_reset_and_unref_internal(ep->read_slices);
    CallReadCb(ep,
               CFStreamAnnotateError(
                   GRPC_ERROR_CREATE_FROM_STATIC_STRING("Socket closed"), ep));
    EP_UNREF(ep, "read");
  } else {
    ep->total_bytes_read += read_size;
    if (read_size < static_cast<CFIndex>(len)) {
      grpc_slice_buffer_trim_end(ep->read_slices, len - read_size, nullptr);
    }
    CallReadCb(ep, GRPC_ERROR_NONE);
    EP_UNREF(ep, "read");
  }
  gpr_mu_lock(&ep->mu);
  if (ep->timer_armed) {
    gpr_log(GPR_ERROR, "ReadAction canceling timer");
    grpc_timer_cancel(&ep->timer);
    ep->timer_armed = false;
  }
  gpr_mu_unlock(&ep->mu);
}

static void WriteAction(void* arg, grpc_error* error) {
  CFStreamEndpoint* ep = static_cast<CFStreamEndpoint*>(arg);
  GPR_ASSERT(ep->write_cb != nullptr);
  if (error) {
    grpc_slice_buffer_reset_and_unref_internal(ep->write_slices);
    CallWriteCb(ep, GRPC_ERROR_REF(error));
    EP_UNREF(ep, "write");
    return;
  }

  grpc_slice slice = grpc_slice_buffer_take_first(ep->write_slices);
  size_t slice_len = GRPC_SLICE_LENGTH(slice);
  CFIndex write_size = CFWriteStreamWrite(
      ep->write_stream, GRPC_SLICE_START_PTR(slice), slice_len);
  if (write_size == -1) {
    grpc_slice_buffer_reset_and_unref_internal(ep->write_slices);
    CFErrorRef stream_error = CFWriteStreamCopyError(ep->write_stream);
    if (stream_error != nullptr) {
      error = CFStreamAnnotateError(
          GRPC_ERROR_CREATE_FROM_CFERROR(stream_error, "write failed."), ep);
      CFRelease(stream_error);
    } else {
      error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("write failed.");
    }
    CallWriteCb(ep, error);
    EP_UNREF(ep, "write");
  } else {
    if (write_size < static_cast<CFIndex>(GRPC_SLICE_LENGTH(slice))) {
      grpc_slice_buffer_undo_take_first(
          ep->write_slices, grpc_slice_sub(slice, write_size, slice_len));
    }
    if (ep->write_slices->length > 0) {
      ep->stream_sync->NotifyOnWrite(&ep->write_action);
    } else {
      CallWriteCb(ep, GRPC_ERROR_NONE);
      EP_UNREF(ep, "write");
    }

    if (grpc_tcp_trace.enabled()) {
      grpc_slice trace_slice = grpc_slice_sub(slice, 0, write_size);
      char* dump = grpc_dump_slice(trace_slice, GPR_DUMP_HEX | GPR_DUMP_ASCII);
      gpr_log(GPR_DEBUG, "WRITE %p (peer=%s): %s", ep, ep->peer_string, dump);
      gpr_free(dump);
      grpc_slice_unref_internal(trace_slice);
    }
  }
  grpc_slice_unref_internal(slice);
}

static void CFStreamReadAllocationDone(void* arg, grpc_error* error) {
  CFStreamEndpoint* ep = static_cast<CFStreamEndpoint*>(arg);
  if (error == GRPC_ERROR_NONE) {
    ep->stream_sync->NotifyOnRead(&ep->read_action);
  } else {
    grpc_slice_buffer_reset_and_unref_internal(ep->read_slices);
    CallReadCb(ep, error);
    EP_UNREF(ep, "read");
  }
}

static void err_cb(void* arg, grpc_error* error) {
  if (error != GRPC_ERROR_NONE) {
    gpr_log(GPR_ERROR, "err_cb got error: %s", grpc_error_string(error));
    return;
  }
  CFStreamEndpoint* ep_impl = reinterpret_cast<CFStreamEndpoint*>(arg);
  gpr_log(GPR_ERROR, "ReadAction was not called for 60 seconds!");
  uint8_t *buf = (uint8_t* )gpr_malloc(GRPC_DEFAULT_MAX_RECV_MESSAGE_LENGTH);
  CFIndex read_size =
      CFReadStreamRead(ep_impl->read_stream, buf, GRPC_DEFAULT_MAX_RECV_MESSAGE_LENGTH);
  gpr_log(GPR_ERROR, "err_cb: read %lu bytes total_bytes_read %u", read_size, ep_impl->total_bytes_read);
  gpr_free(buf);
  gpr_log(GPR_ERROR, "err_cb: grabbing mutex");
  gpr_mu_lock(&ep_impl->mu);
  ep_impl->timer_armed = false;
  gpr_mu_unlock(&ep_impl->mu);
  gpr_log(GPR_ERROR, "calling RunOnQueue");
  ep_impl->stream_sync->RunOnQueue();
  gpr_log(GPR_ERROR, "returned from RunOnQueue");
  gpr_log(GPR_ERROR, "err_cb: aborting");
  abort();
}

static void CFStreamRead(grpc_endpoint* ep, grpc_slice_buffer* slices,
                         grpc_closure* cb, bool urgent) {
  CFStreamEndpoint* ep_impl = reinterpret_cast<CFStreamEndpoint*>(ep);
  if (true) {
    gpr_log(GPR_ERROR, "CFStream endpoint:%p read (%p, %p) length:%zu CFReadStreamGetStatus: %ld CFReadStreamHasBytesAvailable: %u", ep_impl,
            slices, cb, slices->length, CFReadStreamGetStatus(ep_impl->read_stream), CFReadStreamHasBytesAvailable(ep_impl->read_stream));
  }
  grpc_millis now = grpc_core::ExecCtx::Get()->Now();
  gpr_mu_lock(&ep_impl->mu);
  if (!ep_impl->timer_armed) {
    gpr_log(GPR_ERROR, "CFStreamRead initializing timer");
    grpc_timer_init(&ep_impl->timer, now + 1000*60, GRPC_CLOSURE_CREATE(err_cb, (void*)ep_impl, grpc_schedule_on_exec_ctx));
    ep_impl->timer_armed = true;
  }
  gpr_mu_unlock(&ep_impl->mu);
  GPR_ASSERT(ep_impl->read_cb == nullptr);
  ep_impl->read_cb = cb;
  ep_impl->read_slices = slices;
  grpc_slice_buffer_reset_and_unref_internal(slices);
  grpc_resource_user_alloc_slices(&ep_impl->slice_allocator,
                                  GRPC_TCP_DEFAULT_READ_SLICE_SIZE, 1,
                                  ep_impl->read_slices);
  EP_REF(ep_impl, "read");
}

static void CFStreamWrite(grpc_endpoint* ep, grpc_slice_buffer* slices,
                          grpc_closure* cb, void* arg) {
  CFStreamEndpoint* ep_impl = reinterpret_cast<CFStreamEndpoint*>(ep);
  if (grpc_tcp_trace.enabled()) {
    gpr_log(GPR_DEBUG, "CFStream endpoint:%p write (%p, %p) length:%zu",
            ep_impl, slices, cb, slices->length);
  }
  GPR_ASSERT(ep_impl->write_cb == nullptr);
  ep_impl->write_cb = cb;
  ep_impl->write_slices = slices;
  EP_REF(ep_impl, "write");
  ep_impl->stream_sync->NotifyOnWrite(&ep_impl->write_action);
}

void CFStreamShutdown(grpc_endpoint* ep, grpc_error* why) {
  CFStreamEndpoint* ep_impl = reinterpret_cast<CFStreamEndpoint*>(ep);
  if (true) {
    gpr_log(GPR_ERROR, "CFStream endpoint:%p shutdown (%p)", ep_impl, grpc_error_string(why));
  }
  gpr_mu_lock(&ep_impl->mu);
  if (ep_impl->timer_armed) {
    gpr_log(GPR_ERROR, "shutdown canceling timer");
    grpc_timer_cancel(&ep_impl->timer);
    ep_impl->timer_armed = false;
  }
  gpr_mu_unlock(&ep_impl->mu);

  CFReadStreamClose(ep_impl->read_stream);
  CFWriteStreamClose(ep_impl->write_stream);
  ep_impl->stream_sync->Shutdown(why);
  grpc_resource_user_shutdown(ep_impl->resource_user);
  if (grpc_tcp_trace.enabled()) {
    gpr_log(GPR_DEBUG, "CFStream endpoint:%p shutdown DONE (%p)", ep_impl, why);
  }
}

void CFStreamDestroy(grpc_endpoint* ep) {
  CFStreamEndpoint* ep_impl = reinterpret_cast<CFStreamEndpoint*>(ep);
  if (true) {
    gpr_log(GPR_ERROR, "CFStream endpoint:%p destroy", ep_impl);
  }
  EP_UNREF(ep_impl, "destroy");
}

grpc_resource_user* CFStreamGetResourceUser(grpc_endpoint* ep) {
  CFStreamEndpoint* ep_impl = reinterpret_cast<CFStreamEndpoint*>(ep);
  return ep_impl->resource_user;
}

char* CFStreamGetPeer(grpc_endpoint* ep) {
  CFStreamEndpoint* ep_impl = reinterpret_cast<CFStreamEndpoint*>(ep);
  return gpr_strdup(ep_impl->peer_string);
}

int CFStreamGetFD(grpc_endpoint* ep) { return 0; }

bool CFStreamCanTrackErr(grpc_endpoint* ep) { return false; }

void CFStreamAddToPollset(grpc_endpoint* ep, grpc_pollset* pollset) {}
void CFStreamAddToPollsetSet(grpc_endpoint* ep, grpc_pollset_set* pollset) {}
void CFStreamDeleteFromPollsetSet(grpc_endpoint* ep,
                                  grpc_pollset_set* pollset) {}

static const grpc_endpoint_vtable vtable = {CFStreamRead,
                                            CFStreamWrite,
                                            CFStreamAddToPollset,
                                            CFStreamAddToPollsetSet,
                                            CFStreamDeleteFromPollsetSet,
                                            CFStreamShutdown,
                                            CFStreamDestroy,
                                            CFStreamGetResourceUser,
                                            CFStreamGetPeer,
                                            CFStreamGetFD,
                                            CFStreamCanTrackErr};

grpc_endpoint* grpc_cfstream_endpoint_create(
    CFReadStreamRef read_stream, CFWriteStreamRef write_stream,
    const char* peer_string, grpc_resource_quota* resource_quota,
    CFStreamHandle* stream_sync) {
  CFStreamEndpoint* ep_impl =
      static_cast<CFStreamEndpoint*>(gpr_malloc(sizeof(CFStreamEndpoint)));
  gpr_mu_init(&ep_impl->mu);
  ep_impl->timer_armed = false;
  ep_impl->total_bytes_read = 0;
  if (true) {
    gpr_log(GPR_ERROR,
            "CFStream endpoint:%p create readStream:%p writeStream: %p",
            ep_impl, read_stream, write_stream);
  }
  ep_impl->base.vtable = &vtable;
  gpr_ref_init(&ep_impl->refcount, 1);
  ep_impl->read_stream = read_stream;
  ep_impl->write_stream = write_stream;
  CFRetain(read_stream);
  CFRetain(write_stream);
  ep_impl->stream_sync = stream_sync;
  CFSTREAM_HANDLE_REF(ep_impl->stream_sync, "endpoint create");

  ep_impl->peer_string = gpr_strdup(peer_string);
  ep_impl->read_cb = nil;
  ep_impl->write_cb = nil;
  ep_impl->read_slices = nil;
  ep_impl->write_slices = nil;
  GRPC_CLOSURE_INIT(&ep_impl->read_action, ReadAction,
                    static_cast<void*>(ep_impl), grpc_schedule_on_exec_ctx);
  GRPC_CLOSURE_INIT(&ep_impl->write_action, WriteAction,
                    static_cast<void*>(ep_impl), grpc_schedule_on_exec_ctx);
  ep_impl->resource_user =
      grpc_resource_user_create(resource_quota, peer_string);
  grpc_resource_user_slice_allocator_init(&ep_impl->slice_allocator,
                                          ep_impl->resource_user,
                                          CFStreamReadAllocationDone, ep_impl);

  return &ep_impl->base;
}

#endif /* GRPC_CFSTREAM_ENDPOINT */
