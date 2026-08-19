// SPDX-License-Identifier: MIT
//
// TLS adapter skeleton.
//
// The point of this file is that there is nothing MQTT-specific in it. TLS is
// just another mqtt::Transport: the handshake happens inside connect(), which
// keeps returning WouldBlock until it finishes, and after that send()/recv()
// move ciphertext instead of plaintext. The client above never learns the
// difference.
//
// Written against mbedTLS's API shape because that is the common choice on
// Cortex-M, but wolfSSL and BearSSL map onto the same three methods. The
// mbedTLS calls are behind MQTT_EXAMPLE_USE_MBEDTLS so this header still
// compiles in a tree without mbedTLS present; define it and link mbedtls to
// turn it into working code.

#ifndef MQTT_TLS_TRANSPORT_HPP
#define MQTT_TLS_TRANSPORT_HPP

#include "mqtt/transport.hpp"

#if defined(MQTT_EXAMPLE_USE_MBEDTLS)

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

namespace example {

/// Non-blocking TLS transport.
///
/// Storage note: every mbedTLS context here is a by-value member, so this class
/// allocates nothing itself. mbedTLS *will* allocate internally unless you build
/// it with MBEDTLS_MEMORY_BUFFER_ALLOC_C and hand it a static arena --
/// mbedtls_memory_buffer_alloc_init(). Do that if you need the zero-heap
/// property to hold all the way down; the MQTT layer above is already there.
class MbedTlsTransport final : public mqtt::Transport
{
public:
    MbedTlsTransport(const char* host, const char* port, const char* ca_pem, size_t ca_pem_len)
        : host_(host), port_(port), ca_pem_(ca_pem), ca_pem_len_(ca_pem_len)
    {
        mbedtls_net_init(&net_);
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&conf_);
        mbedtls_x509_crt_init(&ca_);
        mbedtls_ctr_drbg_init(&drbg_);
        mbedtls_entropy_init(&entropy_);
    }

    ~MbedTlsTransport() override
    {
        close();
        mbedtls_entropy_free(&entropy_);
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_x509_crt_free(&ca_);
        mbedtls_ssl_config_free(&conf_);
        mbedtls_ssl_free(&ssl_);
        mbedtls_net_free(&net_);
    }

    mqtt::Error connect() noexcept override
    {
        if (connected_)
            return mqtt::Error::Ok;

        if (!setup_done_)
        {
            if (setup() != 0)
                return mqtt::Error::TransportFailure;
            setup_done_ = true;
        }

        // mbedtls_ssl_handshake drives the whole state machine and returns
        // WANT_READ / WANT_WRITE until it is done -- which maps exactly onto
        // the WouldBlock contract, so no extra bookkeeping is needed.
        const int rc = mbedtls_ssl_handshake(&ssl_);
        if (rc == 0)
        {
            // Verification result is only meaningful once the handshake lands.
            if (mbedtls_ssl_get_verify_result(&ssl_) != 0)
            {
                close();
                return mqtt::Error::TransportFailure;
            }
            connected_ = true;
            return mqtt::Error::Ok;
        }

        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            return mqtt::Error::WouldBlock;

        close();
        return mqtt::Error::TransportFailure;
    }

    mqtt::Error send(etl::span<const uint8_t> data, size_t& written) noexcept override
    {
        written = 0;
        if (!connected_)
            return mqtt::Error::TransportClosed;

        const int rc = mbedtls_ssl_write(&ssl_, data.data(), data.size());
        if (rc > 0)
        {
            written = static_cast<size_t>(rc);
            return mqtt::Error::Ok;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            return mqtt::Error::WouldBlock;
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            return mqtt::Error::TransportClosed;

        return mqtt::Error::TransportFailure;
    }

    mqtt::Error recv(etl::span<uint8_t> buffer, size_t& read) noexcept override
    {
        read = 0;
        if (!connected_)
            return mqtt::Error::TransportClosed;

        const int rc = mbedtls_ssl_read(&ssl_, buffer.data(), buffer.size());
        if (rc > 0)
        {
            read = static_cast<size_t>(rc);
            return mqtt::Error::Ok;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            return mqtt::Error::WouldBlock;
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == 0)
            return mqtt::Error::TransportClosed;

        return mqtt::Error::TransportFailure;
    }

    void close() noexcept override
    {
        if (connected_)
            mbedtls_ssl_close_notify(&ssl_);
        mbedtls_net_free(&net_);
        connected_  = false;
        setup_done_ = false;
    }

    bool is_connected() const noexcept override { return connected_; }

private:
    int setup() noexcept
    {
        static const char kPers[] = "mqtt-embedded";

        if (mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_,
                                  reinterpret_cast<const unsigned char*>(kPers),
                                  sizeof(kPers) - 1) != 0)
            return -1;

        if (mbedtls_x509_crt_parse(&ca_, reinterpret_cast<const unsigned char*>(ca_pem_),
                                   ca_pem_len_) != 0)
            return -1;

        if (mbedtls_net_connect(&net_, host_, port_, MBEDTLS_NET_PROTO_TCP) != 0)
            return -1;

        // Non-blocking is what makes this usable from step().
        if (mbedtls_net_set_nonblock(&net_) != 0)
            return -1;

        if (mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0)
            return -1;

        mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conf_, &ca_, nullptr);
        mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &drbg_);

        if (mbedtls_ssl_setup(&ssl_, &conf_) != 0)
            return -1;

        // Server Name Indication, and the hostname checked during verification.
        if (mbedtls_ssl_set_hostname(&ssl_, host_) != 0)
            return -1;

        mbedtls_ssl_set_bio(&ssl_, &net_, mbedtls_net_send, mbedtls_net_recv, nullptr);
        return 0;
    }

    const char* host_       = nullptr;
    const char* port_       = nullptr;
    const char* ca_pem_     = nullptr;
    size_t      ca_pem_len_ = 0;

    mbedtls_net_context      net_{};
    mbedtls_ssl_context      ssl_{};
    mbedtls_ssl_config       conf_{};
    mbedtls_x509_crt         ca_{};
    mbedtls_ctr_drbg_context drbg_{};
    mbedtls_entropy_context  entropy_{};

    bool connected_  = false;
    bool setup_done_ = false;
};

}   // namespace example

#endif   // MQTT_EXAMPLE_USE_MBEDTLS
#endif   // MQTT_TLS_TRANSPORT_HPP
