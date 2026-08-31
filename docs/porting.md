# Porting guide

The library has no operating system dependency. Porting it to a new target means
implementing two interfaces.

## The contract

```cpp
class mqtt::Transport
{
public:
    virtual Error connect() noexcept = 0;
    virtual Error send(etl::span<const uint8_t> data, size_t& written) noexcept = 0;
    virtual Error recv(etl::span<uint8_t> buffer, size_t& read) noexcept = 0;
    virtual void  close() noexcept = 0;
    virtual bool  is_connected() const noexcept = 0;
};

class mqtt::Clock
{
public:
    virtual uint32_t now_ms() const noexcept = 0;
};
```

### Rules

**Never block.** Every method must return promptly. `Client::step()` is called
from your superloop or task, so a blocking implementation stalls everything else
in it.

**Return `Error::WouldBlock` when you made no progress.** This is not an error;
the client treats it as normal.

**Partial transfers are expected.** `send()` may accept one byte of a
200-byte packet. `recv()` may produce half a header. The client handles both;
it reassembles packets across arbitrary fragmentation and resumes partial
writes on the next `step()`.

**Set the out-parameter to 0 on any non-`Ok` return.** `written` and `read` are
only meaningful when you return `Error::Ok`.

**`close()` must be idempotent and must not fail.** It is called on every error
path, including from destructors.

### Return values that mean something specific

| Return | Meaning to the client |
|---|---|
| `Error::Ok` | progress made (`written`/`read` may still be 0) |
| `Error::WouldBlock` | no progress, try again later — not an error |
| `Error::TransportClosed` | peer closed cleanly; session ends |
| `Error::TransportFailure` | unrecoverable; session ends |

Returning `Ok` with `read == 0` is allowed and is treated as "nothing
available", same as `WouldBlock`.

### The clock

Monotonic milliseconds from any fixed origin. It may wrap: every comparison
inside the client uses unsigned difference arithmetic (`elapsed_ms(now, then)`
is `now - then`), so a 32-bit counter rolling over every 49.7 days is handled.
Do not compensate for rollover yourself.

Resolution of 1 ms is not required; anything up to about 100 ms is fine, since
keep-alive intervals are measured in seconds.

## A minimal implementation

```cpp
class MyTransport final : public mqtt::Transport
{
public:
    mqtt::Error connect() noexcept override
    {
        if (state_ == State::Up)
            return mqtt::Error::Ok;

        if (state_ == State::Down)
        {
            if (!my_tcp_begin_connect(host_, port_))
                return mqtt::Error::TransportFailure;
            state_ = State::Connecting;
        }

        switch (my_tcp_poll_connect())
        {
            case PENDING: return mqtt::Error::WouldBlock;
            case FAILED:  state_ = State::Down; return mqtt::Error::TransportFailure;
            case DONE:    state_ = State::Up;   return mqtt::Error::Ok;
        }
        return mqtt::Error::TransportFailure;
    }

    mqtt::Error send(etl::span<const uint8_t> data, size_t& written) noexcept override
    {
        written = 0;
        if (state_ != State::Up)
            return mqtt::Error::TransportClosed;

        const int n = my_tcp_write(data.data(), data.size());
        if (n > 0) { written = static_cast<size_t>(n); return mqtt::Error::Ok; }
        if (n == MY_EWOULDBLOCK)                       return mqtt::Error::WouldBlock;
        if (n == 0 || n == MY_ECLOSED)                 return mqtt::Error::TransportClosed;
        return mqtt::Error::TransportFailure;
    }

    mqtt::Error recv(etl::span<uint8_t> buffer, size_t& read) noexcept override
    {
        read = 0;
        if (state_ != State::Up)
            return mqtt::Error::TransportClosed;

        const int n = my_tcp_read(buffer.data(), buffer.size());
        if (n > 0)  { read = static_cast<size_t>(n); return mqtt::Error::Ok; }
        if (n == 0)                                   return mqtt::Error::TransportClosed;
        if (n == MY_EWOULDBLOCK)                      return mqtt::Error::WouldBlock;
        return mqtt::Error::TransportFailure;
    }

    void close() noexcept override { my_tcp_close(); state_ = State::Down; }
    bool is_connected() const noexcept override { return state_ == State::Up; }

private:
    enum class State { Down, Connecting, Up };
    State state_ = State::Down;
};
```

## Platform notes

### lwIP (raw / netconn API)

With the **netconn** API, put the socket in non-blocking mode
(`netconn_set_nonblocking`) and map `ERR_WOULDBLOCK` / `ERR_INPROGRESS` to
`Error::WouldBlock`, `ERR_CLSD` and `ERR_RST` to `Error::TransportClosed`.

With the **raw** API there is no read call to make — data arrives in your `recv`
callback. Buffer incoming pbufs in a small queue owned by your transport, have
`Transport::recv()` drain that queue, and return `WouldBlock` when it is empty.
Remember to call `tcp_recved()` as you consume, or the window closes on you.

If lwIP runs on its own thread, take the lwIP lock (`LOCK_TCPIP_CORE()` or
`sys_mutex`) inside your transport methods. The MQTT client itself needs no
locking as long as only one thread calls `step()`.

### Zephyr

`zsock_socket()` with `O_NONBLOCK`, then `zsock_send`/`zsock_recv`. Map
`EAGAIN`/`EWOULDBLOCK` to `WouldBlock` and `ECONNRESET`/`ENOTCONN` to
`TransportClosed`. For the clock, `k_uptime_get_32()` is exactly the right
shape.

For TLS, Zephyr's native socket TLS (`IPPROTO_TLS_1_2` with
`TLS_SEC_TAG_LIST`) keeps everything behind the same socket calls, so the
transport looks identical to the plain-TCP one.

### FreeRTOS + FreeRTOS-Plus-TCP

`FreeRTOS_setsockopt()` with `FREERTOS_SO_RCVTIMEO` and `FREERTOS_SO_SNDTIMEO`
set to `0` gives non-blocking behaviour. `FreeRTOS_recv()` returns
`-pdFREERTOS_ERRNO_EWOULDBLOCK` when there is nothing to read. Use
`xTaskGetTickCount()` scaled by `portTICK_PERIOD_MS` for the clock.

### Bare metal with an AT-command modem

The interface was designed with this case in mind. `connect()` runs your
`AT+CIPSTART` state machine, returning `WouldBlock` until the modem reports
success. `send()` drives `AT+CIPSEND`, returning `WouldBlock`
while a previous send is still outstanding. `recv()` drains whatever your URC
handler has buffered. The client's tolerance of partial transfers and the
8-round receive cap matter more here than anywhere else.

### POSIX and Windows

`examples/posix_transport.hpp` is a complete, working implementation for
development on a host. On Windows, the same structure works with Winsock:
`ioctlsocket(FIONBIO)` for non-blocking, `WSAGetLastError() == WSAEWOULDBLOCK`
for the block condition.

### Fixed IPv4 address, no resolver

`examples/tcp_ip_transport.hpp` is the shape most devices ship, and the one
worth reading before porting. It drops `getaddrinfo` and fills a `sockaddr_in`
from four octets: a resolver is frequently absent on an embedded stack, and
where it exists it allocates and blocks.

Its `parse_ipv4()` is `constexpr` and hand-rolled rather than `inet_pton()`, so
a compiled-in broker address is validated at build time:

```cpp
constexpr example::Ipv4 kBroker = example::ipv4("192.168.1.50");
static_assert(kBroker.octets[0] == 192, "broker address failed to parse");
```

Porting it to lwIP is close to mechanical: `sockaddr_in` becomes `ip4_addr_t`,
the socket calls become their `lwip_` equivalents, and the `poll()` for connect
completion becomes `lwip_poll` or a netconn callback. If you resolve a hostname
once at startup and cache the address, this is the transport you want at
run time.

## TLS

TLS requires no support from the MQTT layer. It is a `Transport` like any other:
do the TCP connect *and* the handshake inside `connect()`, returning
`WouldBlock` until the handshake completes, and afterwards move ciphertext in
`send()`/`recv()`. The client never learns the difference.

`examples/tls_transport.hpp` shows this against mbedTLS. The mapping is direct:

| mbedTLS | Transport |
|---|---|
| `mbedtls_ssl_handshake()` returns 0 | `connect()` returns `Ok` |
| `MBEDTLS_ERR_SSL_WANT_READ` / `WANT_WRITE` | `WouldBlock` |
| `mbedtls_ssl_read/write()` returns > 0 | `Ok` with `read`/`written` set |
| `MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY` | `TransportClosed` |

wolfSSL (`SSL_ERROR_WANT_READ`) and BearSSL map the same way.

**One caveat on the zero-heap property.** The MQTT layer allocates nothing, but
mbedTLS allocates internally by default. If you need the guarantee to hold all
the way down, build mbedTLS with `MBEDTLS_MEMORY_BUFFER_ALLOC_C` and give it a
static arena via `mbedtls_memory_buffer_alloc_init()`. Verify with the same
technique this project uses in `tests/test_no_alloc.cpp`.

## Verifying your port

1. **Fragmentation.** Make `recv()` return one byte per call and `send()` accept
   one byte per call. Everything must still work, just slower.
   `client_survives_byte_at_a_time_transport` in `tests/test_client.cpp` is
   exactly this test against the fake.
2. **Back-pressure.** Make `send()` return `WouldBlock` for a while. The client
   must keep the data queued and stay connected.
3. **Clock rollover.** Start `now_ms()` near `0xFFFFFF00` and let it wrap during
   a session. Keep-alive must be unaffected.
4. **Zero allocation.** Replace global `operator new` with a counter, as
   `tests/test_no_alloc.cpp` does, and run your application. Anything that trips
   it is in your transport, not in the client.

The fakes in `tests/fakes.hpp` implement all of these knobs and are worth
reading before writing your own.
