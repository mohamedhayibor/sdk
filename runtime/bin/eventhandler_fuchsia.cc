// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#if !defined(DART_IO_DISABLED)

#include "platform/globals.h"
#if defined(TARGET_OS_FUCHSIA)

#include "bin/eventhandler.h"
#include "bin/eventhandler_fuchsia.h"

#include <magenta/status.h>
#include <magenta/syscalls.h>

#include "bin/log.h"
#include "bin/thread.h"
#include "bin/utils.h"

#if defined(EVENTHANDLER_LOGGING)
#define LOG_ERR(msg, ...) Log::PrintErr(msg, ##__VA_ARGS__)
#define LOG_INFO(msg, ...) Log::Print(msg, ##__VA_ARGS__)
#else
#define LOG_ERR(msg, ...)
#define LOG_INFO(msg, ...)
#endif  // defined(EVENTHANDLER_LOGGING)

namespace dart {
namespace bin {

MagentaWaitManyInfo::MagentaWaitManyInfo()
    : capacity_(kInitialCapacity), size_(0) {
  descriptor_infos_ = static_cast<DescriptorInfo**>(
      malloc(kInitialCapacity * sizeof(*descriptor_infos_)));
  if (descriptor_infos_ == NULL) {
    FATAL("Failed to allocate descriptor_infos array");
  }
  items_ =
      static_cast<mx_wait_item_t*>(malloc(kInitialCapacity * sizeof(*items_)));
  if (items_ == NULL) {
    FATAL("Failed to allocate items array");
  }
}


MagentaWaitManyInfo::~MagentaWaitManyInfo() {
  free(descriptor_infos_);
  free(items_);
}


void MagentaWaitManyInfo::AddHandle(mx_handle_t handle,
                                    mx_signals_t signals,
                                    DescriptorInfo* di) {
#if defined(DEBUG)
  // Check that the handle is not already in the list.
  for (intptr_t i = 0; i < size_; i++) {
    if (items_[i].handle == handle) {
      FATAL("The handle is already in the list!");
    }
  }
#endif
  intptr_t new_size = size_ + 1;
  GrowArraysIfNeeded(new_size);
  descriptor_infos_[size_] = di;
  items_[size_].handle = handle;
  items_[size_].waitfor = signals;
  items_[size_].pending = 0;
  size_ = new_size;
  LOG_INFO("AddHandle(%ld, %ld, %p), size = %ld\n", handle, signals, di, size_);
}


void MagentaWaitManyInfo::RemoveHandle(mx_handle_t handle) {
  intptr_t idx;
  for (idx = 1; idx < size_; idx++) {
    if (handle == items_[idx].handle) {
      break;
    }
  }
  if (idx == size_) {
    FATAL("Handle is not in the list!");
  }

  if (idx != (size_ - 1)) {
    descriptor_infos_[idx] = descriptor_infos_[size_ - 1];
    items_[idx] = items_[size_ - 1];
  }
  descriptor_infos_[size_ - 1] = NULL;
  items_[size_ - 1] = {MX_HANDLE_INVALID, 0, 0};
  size_ = size_ - 1;
  LOG_INFO("RemoveHandle(%ld), size = %ld\n", handle, size_);
}


void MagentaWaitManyInfo::GrowArraysIfNeeded(intptr_t desired_size) {
  if (desired_size < capacity_) {
    return;
  }
  intptr_t new_capacity = desired_size + (desired_size >> 1);
  descriptor_infos_ = static_cast<DescriptorInfo**>(
      realloc(descriptor_infos_, new_capacity * sizeof(*descriptor_infos_)));
  if (descriptor_infos_ == NULL) {
    FATAL("Failed to grow descriptor_infos array");
  }
  items_ = static_cast<mx_wait_item_t*>(
      realloc(items_, new_capacity * sizeof(*items_)));
  if (items_ == NULL) {
    FATAL("Failed to grow items array");
  }
  capacity_ = new_capacity;
  LOG_INFO("GrowArraysIfNeeded(%ld), capacity = %ld\n", desired_size,
           capacity_);
}


EventHandlerImplementation::EventHandlerImplementation() {
  mx_status_t status =
      mx_channel_create(0, &interrupt_handles_[0], &interrupt_handles_[1]);
  if (status != NO_ERROR) {
    FATAL1("mx_channel_create failed: %s\n", mx_status_get_string(status));
  }
  shutdown_ = false;
  info_.AddHandle(interrupt_handles_[0],
                  MX_SIGNAL_READABLE | MX_SIGNAL_PEER_CLOSED, NULL);
  LOG_INFO("EventHandlerImplementation initialized\n");
}


EventHandlerImplementation::~EventHandlerImplementation() {
  mx_status_t status = mx_handle_close(interrupt_handles_[0]);
  if (status != NO_ERROR) {
    FATAL1("mx_handle_close failed: %s\n", mx_status_get_string(status));
  }
  status = mx_handle_close(interrupt_handles_[1]);
  if (status != NO_ERROR) {
    FATAL1("mx_handle_close failed: %s\n", mx_status_get_string(status));
  }
  LOG_INFO("EventHandlerImplementation destroyed\n");
}


void EventHandlerImplementation::WakeupHandler(intptr_t id,
                                               Dart_Port dart_port,
                                               int64_t data) {
  InterruptMessage msg;
  msg.id = id;
  msg.dart_port = dart_port;
  msg.data = data;

  mx_status_t status =
      mx_channel_write(interrupt_handles_[1], 0, &msg, sizeof(msg), NULL, 0);
  if (status != NO_ERROR) {
    FATAL1("mx_channel_write failed: %s\n", mx_status_get_string(status));
  }
  LOG_INFO("WakeupHandler(%ld, %ld, %lld)\n", id, dart_port, data);
}


void EventHandlerImplementation::HandleInterruptFd() {
  LOG_INFO("HandleInterruptFd entry\n");
  InterruptMessage msg;
  uint32_t bytes = kInterruptMessageSize;
  mx_status_t status;
  while (true) {
    status = mx_channel_read(interrupt_handles_[0], 0, &msg, bytes, &bytes,
                             NULL, 0, NULL);
    if (status != NO_ERROR) {
      break;
    }
    ASSERT(bytes == kInterruptMessageSize);
    if (msg.id == kTimerId) {
      LOG_INFO("HandleInterruptFd read timer update\n");
      timeout_queue_.UpdateTimeout(msg.dart_port, msg.data);
    } else if (msg.id == kShutdownId) {
      LOG_INFO("HandleInterruptFd read shutdown\n");
      shutdown_ = true;
    } else {
      // TODO(zra): Handle commands to add and remove handles from the
      // MagentaWaitManyInfo.
      UNIMPLEMENTED();
    }
  }
  // status == ERR_SHOULD_WAIT when we try to read and there are no messages
  // available, so it is an error if we get here and status != ERR_SHOULD_WAIT.
  if (status != ERR_SHOULD_WAIT) {
    FATAL1("mx_channel_read failed: %s\n", mx_status_get_string(status));
  }
  LOG_INFO("HandleInterruptFd exit\n");
}


void EventHandlerImplementation::HandleEvents() {
  LOG_INFO("HandleEvents entry\n");
  for (intptr_t i = 1; i < info_.size(); i++) {
    const mx_wait_item_t& wait_item = info_.items()[i];
    if (wait_item.pending & wait_item.waitfor) {
      // Only the control handle has no descriptor info.
      ASSERT(info_.descriptor_infos()[i] != NULL);
      ASSERT(wait_item.handle != interrupt_handles_[0]);
      // TODO(zra): Handle events on other handles. At the moment we are
      // only interrupted when there is a message on interrupt_handles_[0].
      UNIMPLEMENTED();
    }
  }

  if ((info_.items()[0].pending & MX_SIGNAL_PEER_CLOSED) != 0) {
    FATAL("EventHandlerImplementation::Poll: Unexpected peer closed\n");
  }
  if ((info_.items()[0].pending & MX_SIGNAL_READABLE) != 0) {
    LOG_INFO("HandleEvents interrupt_handles_[0] readable\n");
    HandleInterruptFd();
  } else {
    LOG_INFO("HandleEvents interrupt_handles_[0] not readable\n");
  }
}


int64_t EventHandlerImplementation::GetTimeout() const {
  if (!timeout_queue_.HasTimeout()) {
    return kInfinityTimeout;
  }
  int64_t millis =
      timeout_queue_.CurrentTimeout() - TimerUtils::GetCurrentMonotonicMillis();
  return (millis < 0) ? 0 : millis;
}


void EventHandlerImplementation::HandleTimeout() {
  if (timeout_queue_.HasTimeout()) {
    int64_t millis = timeout_queue_.CurrentTimeout() -
                     TimerUtils::GetCurrentMonotonicMillis();
    if (millis <= 0) {
      DartUtils::PostNull(timeout_queue_.CurrentPort());
      timeout_queue_.RemoveCurrent();
    }
  }
}


void EventHandlerImplementation::Poll(uword args) {
  EventHandler* handler = reinterpret_cast<EventHandler*>(args);
  EventHandlerImplementation* handler_impl = &handler->delegate_;
  ASSERT(handler_impl != NULL);

  while (!handler_impl->shutdown_) {
    int64_t millis = handler_impl->GetTimeout();
    ASSERT((millis == kInfinityTimeout) || (millis >= 0));
    mx_time_t timeout =
        millis * kMicrosecondsPerMillisecond * kNanosecondsPerMicrosecond;
    const MagentaWaitManyInfo& info = handler_impl->info();
    LOG_INFO("mx_handle_wait_many(%p, %ld, %lld)\n", info.items(), info.size(),
             timeout);
    mx_status_t status =
        mx_handle_wait_many(info.items(), info.size(), timeout);
    if ((status != NO_ERROR) && (status != ERR_TIMED_OUT)) {
      FATAL1("mx_handle_wait_many failed: %s\n", mx_status_get_string(status));
    } else {
      LOG_INFO("mx_handle_wait_many returned: %ld\n", status);
      handler_impl->HandleTimeout();
      handler_impl->HandleEvents();
    }
  }
  handler->NotifyShutdownDone();
  LOG_INFO("EventHandlerImplementation notifying about shutdown\n");
}


void EventHandlerImplementation::Start(EventHandler* handler) {
  int result = Thread::Start(&EventHandlerImplementation::Poll,
                             reinterpret_cast<uword>(handler));
  if (result != 0) {
    FATAL1("Failed to start event handler thread %d", result);
  }
}


void EventHandlerImplementation::Shutdown() {
  SendData(kShutdownId, 0, 0);
}


void EventHandlerImplementation::SendData(intptr_t id,
                                          Dart_Port dart_port,
                                          int64_t data) {
  WakeupHandler(id, dart_port, data);
}

}  // namespace bin
}  // namespace dart

#endif  // defined(TARGET_OS_FUCHSIA)

#endif  // !defined(DART_IO_DISABLED)
