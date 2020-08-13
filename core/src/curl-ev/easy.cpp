/**
        curl-ev: wrapper for integrating libcurl with libev applications
        Copyright (c) 2013 Oliver Kuckertz <oliver.kuckertz@mologie.de>
        See COPYING for license information.

        C++ wrapper for libcurl's easy interface
*/

#include <sys/socket.h>
#include <unistd.h>

#include <utility>

#include <boost/algorithm/string/predicate.hpp>

#include <curl-ev/easy.hpp>
#include <curl-ev/error_code.hpp>
#include <curl-ev/form.hpp>
#include <curl-ev/multi.hpp>
#include <curl-ev/share.hpp>
#include <curl-ev/string_list.hpp>

#include <engine/async.hpp>
#include <logging/log.hpp>
#include <server/net/listener_impl.hpp>
#include <utils/strerror.hpp>

// NOLINTNEXTLINE(google-build-using-namespace)
using namespace curl;
using BusyMarker = ::utils::statistics::BusyMarker;

easy::easy(native::CURL* easy_handle, multi* multi_handle)
    : handle_(easy_handle), multi_(multi_handle), multi_registered_(false) {
  initref_ = initialization::ensure_initialization();
  UASSERT(handle_);
  set_private(this);
}

easy::~easy() {
  cancel();

  if (handle_) {
    native::curl_easy_cleanup(handle_);
    handle_ = nullptr;
  }
}

std::shared_ptr<const easy> easy::Create() {
  auto* handle = native::curl_easy_init();
  if (!handle) {
    throw std::bad_alloc();
  }

  return std::make_shared<const easy>(handle, nullptr);
}

std::shared_ptr<easy> easy::GetBound(multi& multi_handle) const {
  auto* cloned = native::curl_easy_duphandle(handle_);
  if (!cloned) {
    throw std::bad_alloc();
  }

  return std::make_shared<easy>(cloned, &multi_handle);
}

easy* easy::from_native(native::CURL* native_easy) {
  easy* easy_handle;
  native::curl_easy_getinfo(native_easy, native::CURLINFO_PRIVATE,
                            &easy_handle);
  return easy_handle;
}

engine::ev::ThreadControl& easy::GetThreadControl() {
  return multi_->GetThreadControl();
}

void easy::async_perform(handler_type handler) {
  LOG_TRACE() << "easy::async_perform start " << reinterpret_cast<long>(this);
  size_t request_num = ++request_counter_;
  if (multi_) {
    multi_->GetThreadControl().RunInEvLoopAsync(
        [self = shared_from_this(), this, handler = std::move(handler),
         request_num]() mutable {
          return do_ev_async_perform(std::move(handler), request_num);
        });
  } else {
    throw std::runtime_error("no multi!");
  }
  LOG_TRACE() << "easy::async_perform finished "
              << reinterpret_cast<long>(this);
}

void easy::do_ev_async_perform(handler_type handler, size_t request_num) {
  if (request_num <= cancelled_request_max_) {
    LOG_DEBUG() << "already cancelled";
    return;
  }

  LOG_TRACE() << "easy::do_ev_async_perform start "
              << reinterpret_cast<long>(this);
  timings_.mark_start_performing();
  if (!multi_) {
    throw std::runtime_error(
        "attempt to perform async. operation without assigning a multi object");
  }

  BusyMarker busy(multi_->Statistics().get_busy_storage());

  // Cancel all previous async. operations
  cancel(request_num - 1);

  // Keep track of all new sockets
  set_opensocket_function(&easy::opensocket);
  set_opensocket_data(this);

  // This one is tricky: Although sockets are opened in the context of an easy
  // object, they can outlive the easy objects and be transferred into a multi
  // object's connection pool. Why there is no connection pool interface in the
  // multi interface to plug into to begin with is still a mystery to me. Either
  // way, the close events have to be tracked by the multi object as sockets are
  // usually closed when curl_multi_cleanup is invoked.
  set_closesocket_function(&easy::closesocket);
  set_closesocket_data(multi_);

  handler_ = std::move(handler);
  multi_registered_ = true;

  // Registering the easy handle with the multi handle might invoke a set of
  // callbacks right away which cause the completion event to fire from within
  // this function.
  LOG_TRACE() << "easy::do_ev_async_perform before multi_->add() "
              << reinterpret_cast<long>(this);
  multi_->add(this);
}

void easy::cancel() { cancel(request_counter_); }

void easy::cancel(size_t request_num) {
  if (multi_) {
    multi_->GetThreadControl().RunInEvLoopSync(
        [this, request_num] { do_ev_cancel(request_num); });
  }
}

void easy::do_ev_cancel(size_t request_num) {
  // RunInEvLoopAsync(do_ev_async_perform) and RunInEvLoopSync(do_ev_cancel) are
  // not synchronized. So we need to count last cancelled request to prevent its
  // execution in do_ev_async_perform().
  if (cancelled_request_max_ < request_num)
    cancelled_request_max_ = request_num;
  if (multi_registered_) {
    BusyMarker busy(multi_->Statistics().get_busy_storage());

    handle_completion(std::make_error_code(std::errc::operation_canceled));
    multi_->remove(this);
  }
}

void easy::reset() {
  LOG_TRACE() << "easy::reset start " << reinterpret_cast<long>(this);

  url_.clear();
  post_fields_.clear();
  form_.reset();
  if (headers_) headers_->clear();
  if (http200_aliases_) http200_aliases_->clear();
  if (resolved_hosts_) resolved_hosts_->clear();
  share_.reset();
  timings_.reset();

  set_custom_request(nullptr);
  set_no_body(false);
  set_post(false);
  set_ssl_ctx_data(nullptr);
  set_ssl_ctx_function(nullptr);

  if (multi_) {
    multi_->GetThreadControl().RunInEvLoopSync([this] { do_ev_reset(); });
  }

  LOG_TRACE() << "easy::reset finished " << reinterpret_cast<long>(this);
}

void easy::do_ev_reset() {
  if (multi_registered_) {
    native::curl_easy_reset(handle_);
  }
}

void easy::set_source(std::shared_ptr<std::istream> source) {
  std::error_code ec;
  set_source(std::move(source), ec);
  throw_error(ec, "set_source");
}

void easy::set_source(std::shared_ptr<std::istream> source,
                      std::error_code& ec) {
  source_ = std::move(source);
  set_read_function(&easy::read_function, ec);
  if (!ec) set_read_data(this, ec);
  if (!ec) set_seek_function(&easy::seek_function, ec);
  if (!ec) set_seek_data(this, ec);
}

void easy::set_sink(std::ostream* sink) {
  std::error_code ec;
  set_sink(sink, ec);
  throw_error(ec, "set_sink");
}

size_t easy::header_function(void*, size_t size, size_t nmemb, void*) {
  return size * nmemb;
}

void easy::set_sink(std::ostream* sink, std::error_code& ec) {
  sink_ = sink;
  set_write_function(&easy::write_function);
  if (!ec) set_write_data(this);
}

void easy::unset_progress_callback() {
  set_no_progress(true);
  set_xferinfo_function(nullptr);
  set_xferinfo_data(nullptr);
}

void easy::set_progress_callback(progress_callback_t progress_callback) {
  progress_callback_ = std::move(progress_callback);
  set_no_progress(false);
  set_xferinfo_function(&easy::xferinfo_function);
  set_xferinfo_data(this);
}

void easy::set_url(const char* url) {
  url_ = url;
  do_set_url(url);
}

const std::string& easy::get_url() const { return url_; }

void easy::set_post_fields(const std::string& post_fields) {
  std::error_code ec;
  set_post_fields(post_fields, ec);
  throw_error(ec, "set_post_fields");
}

void easy::set_post_fields(const std::string& post_fields,
                           std::error_code& ec) {
  post_fields_ = post_fields;
  ec = std::error_code(native::curl_easy_setopt(
      handle_, native::CURLOPT_POSTFIELDS, post_fields_.c_str()));

  if (!ec)
    set_post_field_size_large(
        static_cast<native::curl_off_t>(post_fields_.length()), ec);
}

void easy::set_post_fields(std::string&& post_fields) {
  std::error_code ec;
  set_post_fields(std::forward<std::string>(post_fields), ec);
  throw_error(ec, "set_post_fields");
}

void easy::set_post_fields(std::string&& post_fields, std::error_code& ec) {
  post_fields_ = std::move(post_fields);
  ec = std::error_code(native::curl_easy_setopt(
      handle_, native::CURLOPT_POSTFIELDS, post_fields_.c_str()));

  if (!ec)
    set_post_field_size_large(
        static_cast<native::curl_off_t>(post_fields_.length()), ec);
}

void easy::set_http_post(std::shared_ptr<form> form) {
  std::error_code ec;
  set_http_post(std::move(form), ec);
  throw_error(ec, "set_http_post");
}

void easy::set_http_post(std::shared_ptr<form> form, std::error_code& ec) {
  form_ = std::move(form);

  if (form_) {
    ec = std::error_code(native::curl_easy_setopt(
        handle_, native::CURLOPT_HTTPPOST, form_->native_handle()));
  } else {
    ec = std::error_code(
        native::curl_easy_setopt(handle_, native::CURLOPT_HTTPPOST, NULL));
  }
}

void easy::add_header(const std::string& name, const std::string& value,
                      EmptyHeaderAction empty_header_action) {
  std::error_code ec;
  add_header(name, value, ec, empty_header_action);
  throw_error(ec, "add_header");
}

void easy::add_header(const std::string& name, const std::string& value,
                      std::error_code& ec,
                      EmptyHeaderAction empty_header_action) {
  if (empty_header_action == EmptyHeaderAction::kSend && value.empty())
    add_header(name + ';', ec);
  else
    add_header(name + ": " + value, ec);
}

void easy::add_header(const std::string& header) {
  std::error_code ec;
  add_header(header, ec);
  throw_error(ec, "add_header");
}

void easy::add_header(const std::string& header, std::error_code& ec) {
  if (!headers_) {
    headers_ = std::make_shared<string_list>();
  }

  headers_->add(header);
  ec = std::error_code(native::curl_easy_setopt(
      handle_, native::CURLOPT_HTTPHEADER, headers_->native_handle()));
}

void easy::set_headers(std::shared_ptr<string_list> headers) {
  std::error_code ec;
  set_headers(std::move(headers), ec);
  throw_error(ec, "set_headers");
}

void easy::set_headers(std::shared_ptr<string_list> headers,
                       std::error_code& ec) {
  headers_ = std::move(headers);

  if (headers_) {
    ec = std::error_code(native::curl_easy_setopt(
        handle_, native::CURLOPT_HTTPHEADER, headers_->native_handle()));
  } else {
    ec = std::error_code(
        native::curl_easy_setopt(handle_, native::CURLOPT_HTTPHEADER, NULL));
  }
}

void easy::add_http200_alias(const std::string& http200_alias) {
  std::error_code ec;
  add_http200_alias(http200_alias, ec);
  throw_error(ec, "add_http200_alias");
}

void easy::add_http200_alias(const std::string& http200_alias,
                             std::error_code& ec) {
  if (!http200_aliases_) {
    http200_aliases_ = std::make_shared<string_list>();
  }

  http200_aliases_->add(http200_alias);
  ec = std::error_code(
      native::curl_easy_setopt(handle_, native::CURLOPT_HTTP200ALIASES,
                               http200_aliases_->native_handle()));
}

void easy::set_http200_aliases(std::shared_ptr<string_list> http200_aliases) {
  std::error_code ec;
  set_http200_aliases(std::move(http200_aliases), ec);
  throw_error(ec, "set_http200_aliases");
}

void easy::set_http200_aliases(std::shared_ptr<string_list> http200_aliases,
                               std::error_code& ec) {
  http200_aliases_ = std::move(http200_aliases);

  if (http200_aliases) {
    ec = std::error_code(
        native::curl_easy_setopt(handle_, native::CURLOPT_HTTP200ALIASES,
                                 http200_aliases_->native_handle()));
  } else {
    ec = std::error_code(native::curl_easy_setopt(
        handle_, native::CURLOPT_HTTP200ALIASES, nullptr));
  }
}

void easy::add_resolve(const std::string& resolved_host) {
  std::error_code ec;
  add_resolve(resolved_host, ec);
  throw_error(ec, "add_resolve");
}

void easy::add_resolve(const std::string& resolved_host, std::error_code& ec) {
  if (!resolved_hosts_) {
    resolved_hosts_ = std::make_shared<string_list>();
  }

  resolved_hosts_->add(resolved_host);
  ec = std::error_code(native::curl_easy_setopt(
      handle_, native::CURLOPT_RESOLVE, resolved_hosts_->native_handle()));
}

void easy::set_resolves(std::shared_ptr<string_list> resolved_hosts) {
  std::error_code ec;
  set_resolves(std::move(resolved_hosts), ec);
  throw_error(ec, "set_resolves");
}

void easy::set_resolves(std::shared_ptr<string_list> resolved_hosts,
                        std::error_code& ec) {
  resolved_hosts_ = std::move(resolved_hosts);

  if (resolved_hosts_) {
    ec = std::error_code(native::curl_easy_setopt(
        handle_, native::CURLOPT_RESOLVE, resolved_hosts_->native_handle()));
  } else {
    ec = std::error_code(
        native::curl_easy_setopt(handle_, native::CURLOPT_RESOLVE, NULL));
  }
}

void easy::set_share(std::shared_ptr<share> share) {
  std::error_code ec;
  set_share(std::move(share), ec);
  throw_error(ec, "set_share");
}

void easy::set_share(std::shared_ptr<share> share, std::error_code& ec) {
  share_ = std::move(share);

  if (share) {
    ec = std::error_code(native::curl_easy_setopt(
        handle_, native::CURLOPT_SHARE, share_->native_handle()));
  } else {
    ec = std::error_code(
        native::curl_easy_setopt(handle_, native::CURLOPT_SHARE, NULL));
  }
}

bool easy::has_post_data() const { return !post_fields_.empty() || form_; }

void easy::handle_completion(const std::error_code& err) {
  LOG_TRACE() << "easy::handle_completion easy="
              << reinterpret_cast<long>(this);

  timings_.mark_complete();
  if (sink_) {
    sink_->flush();
  }

  multi_registered_ = false;

  auto handler = std::function<void(const std::error_code& err)>(
      [](const std::error_code&) {});
  swap(handler, handler_);

  /* It's OK to call handler in libev thread context as it is limited to
   * Request::on_retry and Request::on_completed. All user code is executed in
   * coro context.
   */
  handler(err);
}

native::curl_socket_t easy::open_tcp_socket(native::curl_sockaddr* address) {
  std::error_code ec;

  LOG_TRACE() << "open_tcp_socket family=" << address->family;

  int fd = socket(address->family, address->socktype, address->protocol);
  if (fd == -1) {
    const auto old_errno = errno;
    LOG_ERROR() << "socket(2) failed with error: "
                << utils::strerror(old_errno);
    return CURL_SOCKET_BAD;
  }
  multi_->BindEasySocket(*this, fd);
  return fd;
}

size_t easy::write_function(char* ptr, size_t size, size_t nmemb,
                            void* userdata) noexcept {
  easy* self = static_cast<easy*>(userdata);
  size_t actual_size = size * nmemb;

  if (!actual_size) {
    return 0;
  }

  if (!self->sink_->write(ptr, actual_size)) {
    return 0;
  } else {
    return actual_size;
  }
}

size_t easy::read_function(void* ptr, size_t size, size_t nmemb,
                           void* userdata) noexcept {
  // FIXME readsome doesn't work with TFTP (see cURL docs)

  easy* self = static_cast<easy*>(userdata);
  size_t actual_size = size * nmemb;

  if (!self->source_) return CURL_READFUNC_ABORT;

  if (self->source_->eof()) {
    return 0;
  }

  std::streamsize chars_stored =
      self->source_->readsome(static_cast<char*>(ptr), actual_size);

  if (!*self->source_) {
    return CURL_READFUNC_ABORT;
  } else {
    return static_cast<size_t>(chars_stored);
  }
}

int easy::seek_function(void* instream, native::curl_off_t offset,
                        int origin) noexcept {
  // TODO we could allow the user to define an offset which this library should
  // consider as position zero for uploading chunks of the file
  // alternatively do tellg() on the source stream when it is first passed to
  // use_source() and use that as origin

  easy* self = static_cast<easy*>(instream);

  std::ios::seekdir dir;

  switch (origin) {
    case SEEK_SET:
      dir = std::ios::beg;
      break;

    case SEEK_CUR:
      dir = std::ios::cur;
      break;

    case SEEK_END:
      dir = std::ios::end;
      break;

    default:
      return CURL_SEEKFUNC_FAIL;
  }

  if (!self->source_->seekg(offset, dir)) {
    return CURL_SEEKFUNC_FAIL;
  } else {
    return CURL_SEEKFUNC_OK;
  }
}

int easy::xferinfo_function(void* clientp, native::curl_off_t dltotal,
                            native::curl_off_t dlnow,
                            native::curl_off_t ultotal,
                            native::curl_off_t ulnow) noexcept {
  easy* self = static_cast<easy*>(clientp);
  return self->progress_callback_(dltotal, dlnow, ultotal, ulnow) ? 0 : 1;
}

native::curl_socket_t easy::opensocket(
    void* clientp, native::curlsocktype purpose,
    struct native::curl_sockaddr* address) noexcept {
  easy* self = static_cast<easy*>(clientp);
  multi* multi_handle = self->multi_;
  native::curl_socket_t s = -1;

  if (multi_handle) {
    bool is_https = boost::algorithm::istarts_with(self->url_, "https://");
    bool is_under_ratelimit =
        is_https ? multi_handle->MayAcquireConnectionHttps(self->url_)
                 : multi_handle->MayAcquireConnectionHttp(self->url_);
    if (is_under_ratelimit) {
      LOG_TRACE() << "not throttled";
    } else {
      multi_handle->Statistics().mark_socket_ratelimited();
      return CURL_SOCKET_BAD;
    }
  } else {
    LOG_TRACE() << "skip throttle check";
  }

  // NOLINTNEXTLINE(hicpp-multiway-paths-covered)
  switch (purpose) {
    case native::CURLSOCKTYPE_IPCXN:
      switch (address->socktype) {
        case SOCK_STREAM:
          // Note to self: Why is address->protocol always set to zero?
          s = self->open_tcp_socket(address);
          if (s != -1 && multi_handle) {
            multi_handle->Statistics().mark_open_socket();
            self->timings().mark_open_socket();
          }
          return s;

        case SOCK_DGRAM:
          // TODO implement - I've seen other libcurl wrappers with UDP
          // implementation, but have yet to read up on what this is used for
          return CURL_SOCKET_BAD;

        default:
          // unknown or invalid socket type
          return CURL_SOCKET_BAD;
      }
      break;

#ifdef CURLSOCKTYPE_ACCEPT
    case native::CURLSOCKTYPE_ACCEPT:
      // TODO implement - is this used for active FTP?
      return CURL_SOCKET_BAD;
#endif

    default:
      // unknown or invalid purpose
      return CURL_SOCKET_BAD;
  }
}

int easy::closesocket(void* clientp, native::curl_socket_t item) noexcept {
  auto* multi_handle = static_cast<multi*>(clientp);
  multi_handle->UnbindEasySocket(item);

  int ret = close(item);
  if (ret == -1) {
    const auto old_errno = errno;
    LOG_ERROR() << "close(2) failed with error: " << utils::strerror(old_errno);
  }

  multi_handle->Statistics().mark_close_socket();
  return 0;
}
