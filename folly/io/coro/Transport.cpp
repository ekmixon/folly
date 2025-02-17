/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
 */

#include <folly/Portability.h>

#include <functional>

#include <folly/experimental/coro/Baton.h>
#include <folly/io/async/ssl/SSLErrors.h>
#include <folly/io/coro/Transport.h>

#if FOLLY_HAS_COROUTINES

using namespace folly::coro;

namespace {

//
// Common base for all callbcaks
//

class CallbackBase {
 public:
  explicit CallbackBase(folly::AsyncTransport& transport)
      : transport_{transport} {}

  virtual ~CallbackBase() noexcept = default;

  folly::exception_wrapper& error() noexcept { return error_; }
  void post() noexcept { baton_.post(); }
  Task<void> wait() { co_await baton_; }
  Task<folly::Unit> wait(folly::CancellationToken cancelToken) {
    if (cancelToken.isCancellationRequested()) {
      cancel();
      co_yield folly::coro::co_cancelled;
    }
    folly::CancellationCallback cancellationCallback{
        cancelToken, [this] {
          this->post();
          VLOG(5) << "Cancellation was called";
        }};

    co_await wait();
    VLOG(5) << "After baton await";

    if (cancelToken.isCancellationRequested()) {
      cancel();
      co_yield folly::coro::co_cancelled;
    }
    co_return folly::unit;
  }

 protected:
  // we use this to notify the other side of completion
  Baton baton_;
  // needed to modify AsyncTransport state, e.g. cacncel callbacks
  folly::AsyncTransport& transport_;

  // to wrap AsyncTransport errors
  folly::exception_wrapper error_;

  void storeException(const folly::AsyncSocketException& ex) {
    auto sslErr = dynamic_cast<const folly::SSLException*>(&ex);
    if (sslErr) {
      error_ = folly::make_exception_wrapper<folly::SSLException>(*sslErr);
    } else {
      error_ = folly::make_exception_wrapper<folly::AsyncSocketException>(ex);
    }
  }

 private:
  virtual void cancel() noexcept = 0;
};

//
// Handle connect for AsyncSocket
//

class ConnectCallback : public CallbackBase,
                        public folly::AsyncSocket::ConnectCallback {
 public:
  explicit ConnectCallback(folly::AsyncSocket& socket)
      : CallbackBase(socket), socket_(socket) {}

 private:
  void cancel() noexcept override { socket_.cancelConnect(); }

  void connectSuccess() noexcept override { post(); }

  void connectErr(const folly::AsyncSocketException& ex) noexcept override {
    storeException(ex);
    post();
  }
  folly::AsyncSocket& socket_;
};

//
// Handle data read for AsyncTransport
//

class ReadCallback : public CallbackBase,
                     public folly::AsyncTransport::ReadCallback,
                     public folly::HHWheelTimer::Callback {
 public:
  // we need to pass the socket into ReadCallback so we can clear the callback
  // pointer in the socket, thus preventing multiple callbacks from happening
  // in one run of event loop. This may happen, for example, when one fiber
  // writes and immediately closes the socket - this would cause the async
  // socket to call readDataAvailable and readEOF in sequence, causing the
  // promise to be fulfilled twice (oops!)
  ReadCallback(
      folly::HHWheelTimer& timer,
      folly::AsyncTransport& transport,
      folly::MutableByteRange buf,
      std::chrono::milliseconds timeout)
      : CallbackBase(transport), buf_{buf} {
    if (timeout.count() > 0) {
      timer.scheduleTimeout(this, timeout);
    }
  }

  ReadCallback(
      folly::HHWheelTimer& timer,
      folly::AsyncTransport& transport,
      folly::IOBufQueue* readBuf,
      size_t minReadSize,
      size_t newAllocationSize,
      std::chrono::milliseconds timeout)
      : CallbackBase(transport),
        readBuf_(readBuf),
        minReadSize_(minReadSize),
        newAllocationSize_(newAllocationSize) {
    if (timeout.count() > 0) {
      timer.scheduleTimeout(this, timeout);
    }
  }

  // how much was read during operation
  size_t length{0};
  bool eof{false};

 private:
  // the read buffer we store to hand off to callback - obtained from user
  folly::MutableByteRange buf_;
  folly::IOBufQueue* readBuf_{nullptr};
  size_t minReadSize_{0};
  size_t newAllocationSize_{0};

  void cancel() noexcept override {
    transport_.setReadCB(nullptr);
    cancelTimeout();
  }

  //
  // ReadCallback methods
  //

  // this is called right before readDataAvailable(), always
  // in the same sequence
  void getReadBuffer(void** buf, size_t* len) override {
    if (readBuf_) {
      auto rbuf = readBuf_->preallocate(minReadSize_, newAllocationSize_);
      *buf = rbuf.first;
      *len = rbuf.second;
    } else {
      VLOG(5) << "getReadBuffer, size: " << buf_.size();
      *buf = buf_.begin() + length;
      *len = buf_.size() - length;
    }
  }

  // once we get actual data, uninstall callback and clear timeout
  void readDataAvailable(size_t len) noexcept override {
    VLOG(5) << "readDataAvailable: " << len << " bytes";
    length += len;
    if (readBuf_) {
      readBuf_->postallocate(len);
    } else if (length == buf_.size()) {
      transport_.setReadCB(nullptr);
      cancelTimeout();
    }
    post();
  }

  void readEOF() noexcept override {
    VLOG(5) << "readEOF()";
    // disable callbacks
    transport_.setReadCB(nullptr);
    cancelTimeout();
    eof = true;
    post();
  }

  void readErr(const folly::AsyncSocketException& ex) noexcept override {
    VLOG(5) << "readErr()";
    // disable callbacks
    transport_.setReadCB(nullptr);
    cancelTimeout();
    storeException(ex);
    post();
  }

  //
  // AsyncTimeout method
  //

  void timeoutExpired() noexcept override {
    VLOG(5) << "timeoutExpired()";

    using Error = folly::AsyncSocketException::AsyncSocketExceptionType;

    // uninstall read callback. it takes another read to bring it back.
    transport_.setReadCB(nullptr);
    // If the timeout fires but this ReadCallback did get some data, ignore it.
    // post() has already happend from readDataAvailable.
    if (length == 0) {
      error_ = folly::make_exception_wrapper<folly::AsyncSocketException>(
          Error::TIMED_OUT, "Timed out waiting for data", errno);
      post();
    }
  }
};

//
// Handle data write for AsyncTransport
//

class WriteCallback : public CallbackBase,
                      public folly::AsyncTransport::WriteCallback {
 public:
  explicit WriteCallback(folly::AsyncTransport& transport)
      : CallbackBase(transport) {}
  ~WriteCallback() override = default;

  size_t bytesWritten{0};
  std::optional<folly::AsyncSocketException> error;

 private:
  void cancel() noexcept override { transport_.closeWithReset(); }
  //
  // Methods of WriteCallback
  //

  void writeSuccess() noexcept override {
    VLOG(5) << "writeSuccess";
    post();
  }

  void writeErr(
      size_t bytes, const folly::AsyncSocketException& ex) noexcept override {
    VLOG(5) << "writeErr, wrote " << bytesWritten << " bytes";
    bytesWritten = bytes;
    error = ex;
    post();
  }
};

} // namespace

namespace folly {
namespace coro {

Task<Transport> Transport::newConnectedSocket(
    folly::EventBase* evb,
    const folly::SocketAddress& destAddr,
    std::chrono::milliseconds connectTimeout) {
  auto socket = AsyncSocket::newSocket(evb);

  socket->setReadCB(nullptr);
  ConnectCallback cb{*socket};
  socket->connect(&cb, destAddr, connectTimeout.count());
  auto waitRet =
      co_await co_awaitTry(cb.wait(co_await co_current_cancellation_token));
  if (waitRet.hasException()) {
    co_yield co_error(std::move(waitRet.exception()));
  }
  if (cb.error()) {
    co_yield co_error(std::move(cb.error()));
  }
  co_return Transport(evb, std::move(socket));
}

Task<size_t> Transport::read(
    folly::MutableByteRange buf, std::chrono::milliseconds timeout) {
  if (deferredReadEOF_) {
    deferredReadEOF_ = false;
    co_return 0;
  }
  VLOG(5) << "Transport::read(), expecting max len " << buf.size();

  ReadCallback cb{eventBase_->timer(), *transport_, buf, timeout};
  transport_->setReadCB(&cb);
  auto waitRet =
      co_await co_awaitTry(cb.wait(co_await co_current_cancellation_token));
  if (cb.error()) {
    co_yield co_error(std::move(cb.error()));
  }
  if (waitRet.hasException() &&
      (!waitRet.tryGetExceptionObject<OperationCancelled>() ||
       (!cb.eof && cb.length == 0))) {
    // Got a non-cancel exception, or cancel with nothing read
    co_yield co_error(std::move(waitRet.exception()));
  }
  transport_->setReadCB(nullptr);
  deferredReadEOF_ = (cb.eof && cb.length > 0);
  co_return cb.length;
}

Task<size_t> Transport::read(
    folly::IOBufQueue& readBuf,
    std::size_t minReadSize,
    std::size_t newAllocationSize,
    std::chrono::milliseconds timeout) {
  if (deferredReadEOF_) {
    deferredReadEOF_ = false;
    co_return 0;
  }
  VLOG(5) << "Transport::read(), expecting minReadSize=" << minReadSize;

  ReadCallback cb{
      eventBase_->timer(),
      *transport_,
      &readBuf,
      minReadSize,
      newAllocationSize,
      timeout};
  transport_->setReadCB(&cb);
  auto waitRet =
      co_await co_awaitTry(cb.wait(co_await co_current_cancellation_token));
  if (cb.error()) {
    co_yield co_error(std::move(cb.error()));
  }
  if (waitRet.hasException() &&
      (!waitRet.tryGetExceptionObject<OperationCancelled>() ||
       (!cb.eof && cb.length == 0))) {
    // Got a non-cancel exception, or cancel with nothing read
    co_yield co_error(std::move(waitRet.exception()));
  }
  transport_->setReadCB(nullptr);
  deferredReadEOF_ = (cb.eof && cb.length > 0);
  co_return cb.length;
}

Task<folly::Unit> Transport::write(
    folly::ByteRange buf,
    std::chrono::milliseconds timeout,
    WriteInfo* writeInfo) {
  transport_->setSendTimeout(timeout.count());
  WriteCallback cb{*transport_};
  transport_->write(&cb, buf.begin(), buf.size());
  auto waitRet =
      co_await co_awaitTry(cb.wait(co_await co_current_cancellation_token));
  if (waitRet.hasException()) {
    if (writeInfo) {
      writeInfo->bytesWritten = cb.bytesWritten;
    }
    co_yield co_error(std::move(waitRet.exception()));
  }

  if (cb.error) {
    if (writeInfo) {
      writeInfo->bytesWritten = cb.bytesWritten;
    }
    co_yield co_error(std::move(*cb.error));
  }
  co_return unit;
}

Task<folly::Unit> Transport::write(
    folly::IOBufQueue& ioBufQueue,
    std::chrono::milliseconds timeout,
    WriteInfo* writeInfo) {
  transport_->setSendTimeout(timeout.count());
  WriteCallback cb{*transport_};
  auto iovec = ioBufQueue.front()->getIov();
  transport_->writev(&cb, iovec.data(), iovec.size());
  auto waitRet =
      co_await co_awaitTry(cb.wait(co_await co_current_cancellation_token));
  if (waitRet.hasException()) {
    if (writeInfo) {
      writeInfo->bytesWritten = cb.bytesWritten;
    }
    co_yield co_error(std::move(waitRet.exception()));
  }

  if (cb.error) {
    if (writeInfo) {
      writeInfo->bytesWritten = cb.bytesWritten;
    }
    co_yield co_error(std::move(*cb.error));
  }
  co_return unit;
}

} // namespace coro
} // namespace folly

#endif // FOLLY_HAS_COROUTINES
