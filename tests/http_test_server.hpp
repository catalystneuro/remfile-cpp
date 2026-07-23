/*
 * A minimal HTTP/1.1 server for testing the remfile VFD.
 *
 * It serves a fixed byte buffer over loopback with byte-range support, so the
 * tests are hermetic (no network) and deterministic. Beyond serving bytes, it
 * exists to make the driver's *behavior* observable:
 *
 *   - request_count() lets a test assert that sequential reads are coalesced
 *     into a few large range requests rather than many small ones
 *   - requested_ranges() records exactly which byte ranges were asked for
 *   - fail_next(n) makes the next n requests return 500, exercising the retry
 *     path without waiting on a real flaky network
 *   - set_support_ranges(false) makes the server ignore Range headers and
 *     return 200 with the whole body, which is how a server that cannot do
 *     range requests behaves
 *
 * POSIX sockets only (Linux/macOS), which matches where this driver is built.
 */

#ifndef REMFILE_HTTP_TEST_SERVER_HPP
#define REMFILE_HTTP_TEST_SERVER_HPP

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
typedef SOCKET socket_t;
#define SHUT_RDWR SD_BOTH
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int socket_t;
#define close_socket ::close
#endif

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace remfile_test
{

class HttpTestServer
{
public:
  /* Serve `data` on a loopback port chosen by the OS. */
  explicit HttpTestServer(std::vector<uint8_t> data)
      : m_data(std::move(data))
  {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    m_listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_listen_fd == (socket_t)-1)
      throw std::runtime_error("HttpTestServer: socket() failed");

    int yes = 1;
    ::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  /* let the OS pick a free port */
    if (::bind(m_listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0)
      throw std::runtime_error("HttpTestServer: bind() failed");
    if (::listen(m_listen_fd, 16) < 0)
      throw std::runtime_error("HttpTestServer: listen() failed");

    socklen_t len = sizeof(addr);
    ::getsockname(m_listen_fd, (sockaddr*)&addr, &len);
    m_port = ntohs(addr.sin_port);

    m_thread = std::thread([this] { run(); });
  }

  ~HttpTestServer()
  {
    m_stop = true;
    ::shutdown(m_listen_fd, SHUT_RDWR);
    close_socket(m_listen_fd);
    if (m_thread.joinable())
      m_thread.join();

    /* Force every live connection closed before joining its thread.
     *
     * Closing the listening socket does NOT unblock a thread already sitting
     * in recv() on an accepted connection. Those threads only exit when the
     * client hangs up — and the client may never hang up: if a test assertion
     * fails, Catch2 unwinds before H5Fclose runs, so the driver's curl handle
     * (and its keep-alive connection) is never cleaned up. Without this
     * shutdown, a failing test would hang in teardown instead of reporting
     * the failure. */
    {
      std::lock_guard<std::mutex> lock(m_conn_mutex);
      for (socket_t fd : m_conn_fds)
        ::shutdown(fd, SHUT_RDWR);
    }
    for (auto& t : m_conn_threads)
      if (t.joinable())
        t.join();

#ifdef _WIN32
    WSACleanup();
#endif
  }

  HttpTestServer(const HttpTestServer&) = delete;
  HttpTestServer& operator=(const HttpTestServer&) = delete;

  std::string url() const
  {
    return "http://127.0.0.1:" + std::to_string(m_port) + "/test.h5";
  }

  /* Number of HTTP requests served (including failed ones). */
  int request_count() const { return m_request_count.load(); }

  void reset_counts()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_request_count = 0;
    m_ranges.clear();
  }

  /* The (start, end) byte ranges requested so far, in order. */
  std::vector<std::pair<uint64_t, uint64_t>> requested_ranges() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ranges;
  }

  /* Total bytes handed out in successful responses. */
  uint64_t bytes_served() const { return m_bytes_served.load(); }

  /* Make the next n requests fail with 500, to exercise the retry path. */
  void fail_next(int n) { m_fail_next = n; }

  /* When false, Range headers are ignored and the full body is returned with
   * a 200, like a server that does not implement range requests. */
  void set_support_ranges(bool v) { m_support_ranges = v; }

  /* When false, every request 404s. */
  void set_found(bool v) { m_found = v; }

private:
  void run()
  {
    while (!m_stop) {
      socket_t fd = ::accept(m_listen_fd, nullptr, nullptr);
      if (fd == (socket_t)-1) {
        if (m_stop)
          break;
        continue;
      }
      /* One thread per connection. A single-threaded loop would deadlock the
       * moment the client opened a second connection (curl does, e.g. after a
       * dropped keep-alive), since we would still be blocked in recv() on the
       * first one and never reach accept() again. */
      std::lock_guard<std::mutex> lock(m_conn_mutex);
      m_conn_fds.push_back(fd);
      m_conn_threads.emplace_back(
          [this, fd]
          {
            handle_connection(fd);
            close_socket(fd);
          });
    }
  }

  /* Read a full request header block (keep-alive: one connection may carry
   * several requests, which is exactly what the driver's curl handle does). */
  void handle_connection(socket_t fd)
  {
    std::string buf;
    char chunk[4096];
    while (!m_stop) {
      int n = ::recv(fd, chunk, sizeof(chunk), 0);
      if (n <= 0)
        return;
      buf.append(chunk, (size_t)n);

      /* Process each complete request in the buffer. */
      size_t end;
      while ((end = buf.find("\r\n\r\n")) != std::string::npos) {
        std::string request = buf.substr(0, end);
        buf.erase(0, end + 4);
        if (!handle_request(fd, request))
          return;
      }
    }
  }

  /* Returns false if the connection should be closed. */
  bool handle_request(socket_t fd, const std::string& request)
  {
    m_request_count++;

    if (m_fail_next > 0) {
      m_fail_next--;
      std::string resp =
          "HTTP/1.1 500 Internal Server Error\r\n"
          "Content-Length: 0\r\n"
          "Connection: keep-alive\r\n\r\n";
      ::send(fd, resp.data(), (int)resp.size(), 0);
      return true;
    }

    if (!m_found) {
      std::string resp =
          "HTTP/1.1 404 Not Found\r\n"
          "Content-Length: 0\r\n"
          "Connection: keep-alive\r\n\r\n";
      ::send(fd, resp.data(), (int)resp.size(), 0);
      return true;
    }

    uint64_t start = 0;
    uint64_t end = m_data.size() ? m_data.size() - 1 : 0;
    bool has_range = false;

    size_t rpos = find_header(request, "range:");
    if (rpos != std::string::npos && m_support_ranges) {
      /* Range: bytes=START-END */
      size_t eq = request.find('=', rpos);
      size_t dash = request.find('-', eq == std::string::npos ? rpos : eq);
      if (eq != std::string::npos && dash != std::string::npos) {
        start = strtoull(request.c_str() + eq + 1, nullptr, 10);
        /* An open-ended "bytes=N-" means through end of file. */
        if (dash + 1 < request.size() && isdigit((unsigned char)request[dash + 1]))
          end = strtoull(request.c_str() + dash + 1, nullptr, 10);
        has_range = true;
      }
    }

    if (start >= m_data.size()) {
      std::string resp =
          "HTTP/1.1 416 Range Not Satisfiable\r\n"
          "Content-Length: 0\r\n"
          "Connection: keep-alive\r\n\r\n";
      ::send(fd, resp.data(), (int)resp.size(), 0);
      return true;
    }
    if (end >= m_data.size())
      end = m_data.size() - 1;

    if (has_range) {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_ranges.emplace_back(start, end);
    }

    uint64_t len = end - start + 1;
    char header[512];
    int hlen;
    if (has_range) {
      hlen = snprintf(header, sizeof(header),
                      "HTTP/1.1 206 Partial Content\r\n"
                      "Content-Length: %llu\r\n"
                      "Content-Range: bytes %llu-%llu/%zu\r\n"
                      "Accept-Ranges: bytes\r\n"
                      "Connection: keep-alive\r\n\r\n",
                      (unsigned long long)len, (unsigned long long)start,
                      (unsigned long long)end, m_data.size());
    } else {
      /* No range support: return the whole body. */
      start = 0;
      len = m_data.size();
      hlen = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Length: %llu\r\n"
                      "Connection: keep-alive\r\n\r\n",
                      (unsigned long long)len);
    }
    if (::send(fd, header, hlen, 0) < 0)
      return false;

    size_t sent = 0;
    while (sent < len) {
      int n = ::send(fd, (const char*)(m_data.data() + start + sent), (int)(len - sent), 0);
      if (n <= 0)
        return false;
      sent += (size_t)n;
    }
    m_bytes_served += len;
    return true;
  }

  /* Case-insensitive header search. */
  static size_t find_header(const std::string& req, const char* name)
  {
    std::string lower;
    lower.reserve(req.size());
    for (char c : req)
      lower.push_back((char)tolower((unsigned char)c));
    return lower.find(name);
  }

  std::vector<uint8_t> m_data;
  socket_t m_listen_fd = (socket_t)-1;
  int m_port = 0;
  std::thread m_thread;
  std::atomic<bool> m_stop {false};
  std::atomic<int> m_request_count {0};
  std::atomic<uint64_t> m_bytes_served {0};
  std::atomic<int> m_fail_next {0};
  std::atomic<bool> m_support_ranges {true};
  std::atomic<bool> m_found {true};
  mutable std::mutex m_mutex;
  std::vector<std::pair<uint64_t, uint64_t>> m_ranges;
  std::mutex m_conn_mutex;
  std::vector<std::thread> m_conn_threads;
  std::vector<socket_t> m_conn_fds;
};

}  // namespace remfile_test

#endif /* REMFILE_HTTP_TEST_SERVER_HPP */
