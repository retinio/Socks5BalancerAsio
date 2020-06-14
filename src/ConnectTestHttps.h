/**
 * Socks5BalancerAsio : A Simple TCP Socket Balancer for balance Multi Socks5 Proxy Backend Powered by Boost.Asio
 * Copyright (C) <2020>  <Jeremie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef SOCKS5BALANCERASIO_CONNECTTESTHTTPS_H
#define SOCKS5BALANCERASIO_CONNECTTESTHTTPS_H

#ifdef MSVC
#pragma once
#endif

#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <memory>
#include <utility>
#include <iostream>
#include <string>
#include <sstream>
#include <list>
#include <vector>
#include <functional>
#include <openssl/opensslv.h>

#ifdef _WIN32

#include <wincrypt.h>
#include <tchar.h>

#endif // _WIN32

class ConnectTestHttpsSession : public std::enable_shared_from_this<ConnectTestHttpsSession> {
    boost::asio::ip::tcp::resolver resolver_;
    boost::beast::ssl_stream<boost::beast::tcp_stream> stream_;
    boost::beast::flat_buffer buffer_; // (Must persist between reads)
    boost::beast::http::request<boost::beast::http::empty_body> req_;
    boost::beast::http::response<boost::beast::http::string_body> res_;


    const std::string targetHost;
    uint16_t targetPort;
    const std::string targetPath;
    int httpVersion;
    const std::string &socks5Host;
    const std::string &socks5Port;

    enum {
        MAX_LENGTH = 8192
    };

public:
    ConnectTestHttpsSession(
            boost::asio::executor executor,
            const std::shared_ptr<boost::asio::ssl::context> &ssl_context,
            const std::string &targetHost,
            int targetPort,
            const std::string &targetPath,
            int httpVersion,
            const std::string &socks5Host,
            const std::string &socks5Port
    ) :
            resolver_(executor),
            stream_(executor, *ssl_context),
            targetHost(targetHost),
            targetPort(targetPort),
            targetPath(targetPath),
            httpVersion(httpVersion),
            socks5Host(socks5Host),
            socks5Port(socks5Port) {

        // Set SNI Hostname (many hosts need this to handshake successfully)
        if (!SSL_set_tlsext_host_name(stream_.native_handle(), targetHost.c_str())) {
            boost::beast::error_code ec{static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category()};
            std::cerr << ec.message() << "\n";
            return;
        }

        // Set up an HTTP GET request message
        req_.version(httpVersion);
        req_.method(boost::beast::http::verb::get);
        req_.target(targetPath);
        req_.set(boost::beast::http::field::host, targetHost);
        req_.set(boost::beast::http::field::user_agent, BOOST_BEAST_VERSION_STRING);

    }

    using SuccessfulInfo = boost::beast::http::response<boost::beast::http::string_body>;
    std::function<void(SuccessfulInfo info)> successfulCallback;
    std::function<void(std::string reason)> failedCallback;

    void run(std::function<void(SuccessfulInfo info)> onOk, std::function<void(std::string reason)> onErr) {
        successfulCallback = std::move(onOk);
        failedCallback = std::move(onErr);
        do_resolve();
    }

private:

    void
    fail(boost::beast::error_code ec, char const *what) {
        std::stringstream ss;
        ss << what << ": " << ec.message();

        std::cerr << ss.str() << "\n";
        if (failedCallback) {
            failedCallback(ss.str());
        }
    }

    void
    allOk() {
        std::cout << res_ << std::endl;
        if (successfulCallback) {
            successfulCallback(res_);
        }
    }

    void
    do_resolve() {

        // Look up the domain name
        resolver_.async_resolve(
                socks5Host,
                socks5Port,
                boost::beast::bind_front_handler(
                        [this, self = shared_from_this()](
                                boost::beast::error_code ec,
                                const boost::asio::ip::tcp::resolver::results_type &results) {
                            if (ec) {
                                return fail(ec, "resolve");
                            }

                            do_tcp_connect(results);
                        }));

    }

    void
    do_tcp_connect(const boost::asio::ip::tcp::resolver::results_type &results) {


        // Set a timeout on the operation
        boost::beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));

        // Make the connection on the IP address we get from a lookup
        boost::beast::get_lowest_layer(stream_).async_connect(
                results,
                boost::beast::bind_front_handler(
                        [this, self = shared_from_this()](
                                boost::beast::error_code ec,
                                const boost::asio::ip::tcp::resolver::results_type::endpoint_type &) {
                            if (ec) {
                                return fail(ec, "tcp_connect");
                            }

                            do_socks5_handshake_write();
                        }));

    }

    void
    do_socks5_handshake_write() {

        // send socks5 client handshake
        // +----+----------+----------+
        // |VER | NMETHODS | METHODS  |
        // +----+----------+----------+
        // | 1  |    1     | 1 to 255 |
        // +----+----------+----------+
        auto data_send = std::make_shared<std::string>(
                "\x05\x01\x00", 3
        );

        // Set a timeout on the operation
        boost::beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));

        boost::asio::async_write(
                boost::beast::get_lowest_layer(stream_),
                boost::asio::buffer(*data_send),
                boost::beast::bind_front_handler(
                        [this, self = shared_from_this(), data_send](
                                const boost::system::error_code &ec,
                                std::size_t bytes_transferred_) {
                            if (!ec || bytes_transferred_ != data_send->size()) {
                                return fail(ec, "socks5_handshake_write");
                            }

                            do_socks5_handshake_read();
                        })
        );
    }

    void
    do_socks5_handshake_read() {
        auto socks5_read_buf = std::make_shared<std::vector<uint8_t>>(MAX_LENGTH);

        // Set a timeout on the operation
        boost::beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));

        // Make the connection on the IP address we get from a lookup
        boost::beast::get_lowest_layer(stream_).async_read_some(
                boost::asio::buffer(*socks5_read_buf, MAX_LENGTH),
                boost::beast::bind_front_handler(
                        [this, self = shared_from_this(), socks5_read_buf](
                                boost::beast::error_code ec,
                                const size_t &bytes_transferred) {
                            if (ec) {
                                return fail(ec, "socks5_handshake_read");
                            }

                            // check server response
                            //  +----+--------+
                            //  |VER | METHOD |
                            //  +----+--------+
                            //  | 1  |   1    |
                            //  +----+--------+
                            if (bytes_transferred < 2
                                || socks5_read_buf->at(0) != 5
                                || socks5_read_buf->at(1) != 0x00) {
                                do_shutdown();
                                return fail(ec, "socks5_handshake_read (bytes_transferred < 2)");
                            }

                            do_socks5_connect_write();
                        }));
    }

    void
    do_socks5_connect_write() {


        // analysis targetHost and targetPort
        // targetHost,
        // targetPort,
        boost::beast::error_code ec;
        auto addr = boost::asio::ip::make_address(targetHost, ec);

        // send socks5 client connect
        // +----+-----+-------+------+----------+----------+
        // |VER | CMD |  RSV  | ATYP | DST.ADDR | DST.PORT |
        // +----+-----+-------+------+----------+----------+
        // | 1  |  1  | X'00' |  1   | Variable |    2     |
        // +----+-----+-------+------+----------+----------+
        std::shared_ptr<std::vector<uint8_t>> data_send;
        data_send->insert(data_send->end(), {0x05, 0x01, 0x00});
        if (ec) {
            // is a domain name
            data_send->push_back(0x03); // ATYP
            if (targetHost.size() > 253) {
                do_shutdown();
                return fail(ec, "socks5_connect_write (targetHost.size() > 253)");
            }
            data_send->push_back(targetHost.size());
            data_send->insert(data_send->end(), targetHost.begin(), targetHost.end());
        } else if (addr.is_v4()) {
            data_send->push_back(0x01); // ATYP
            auto v4 = addr.to_v4().to_bytes();
            data_send->insert(data_send->end(), v4.begin(), v4.end());
        } else if (addr.is_v6()) {
            data_send->push_back(0x04); // ATYP
            auto v6 = addr.to_v6().to_bytes();
            data_send->insert(data_send->end(), v6.begin(), v6.end());
        }
        // port
        data_send->push_back(static_cast<uint8_t>(targetPort >> 8));
        data_send->push_back(static_cast<uint8_t>(targetPort & 0xff));

        // Set a timeout on the operation
        boost::beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));

        boost::asio::async_write(
                boost::beast::get_lowest_layer(stream_),
                boost::asio::buffer(*data_send),
                boost::beast::bind_front_handler(
                        [this, self = shared_from_this(), data_send](
                                const boost::system::error_code &ec,
                                std::size_t bytes_transferred_) {
                            if (!ec || bytes_transferred_ != data_send->size()) {
                                return fail(ec, "socks5_connect_write");
                            }

                            do_socks5_connect_read();
                        })
        );
    }

    void
    do_socks5_connect_read() {
        auto socks5_read_buf = std::make_shared<std::vector<uint8_t>>(MAX_LENGTH);

        // Set a timeout on the operation
        boost::beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));

        // Make the connection on the IP address we get from a lookup
        boost::beast::get_lowest_layer(stream_).async_read_some(
                boost::asio::buffer(*socks5_read_buf, MAX_LENGTH),
                boost::beast::bind_front_handler(
                        [this, self = shared_from_this(), socks5_read_buf](
                                boost::beast::error_code ec,
                                const size_t &bytes_transferred) {
                            if (ec) {
                                return fail(ec, "socks5_connect_read");
                            }

                            // check server response
                            // +----+-----+-------+------+----------+----------+
                            // |VER | REP |  RSV  | ATYP | BND.ADDR | BND.PORT |
                            // +----+-----+-------+------+----------+----------+
                            // | 1  |  1  | X'00' |  1   | Variable |    2     |
                            // +----+-----+-------+------+----------+----------+
                            if (bytes_transferred < 6
                                || socks5_read_buf->at(0) != 5
                                || socks5_read_buf->at(1) != 0x00
                                || socks5_read_buf->at(2) != 0x00
                                || (
                                        socks5_read_buf->at(3) != 0x01 &&
                                        socks5_read_buf->at(3) != 0x03 &&
                                        socks5_read_buf->at(3) != 0x04
                                )
                                    ) {
                                do_shutdown();
                                return fail(ec, "socks5_connect_read (bytes_transferred < 6)");
                            }
                            if (socks5_read_buf->at(3) == 0x03
                                && bytes_transferred != (socks5_read_buf->at(4) + 4 + 1 + 2)
                                    ) {
                                do_shutdown();
                                return fail(ec, "socks5_connect_read (socks5_read_buf->at(3) == 0x03)");
                            }
                            if (socks5_read_buf->at(3) == 0x01
                                && bytes_transferred != (4 + 4 + 2)
                                    ) {
                                do_shutdown();
                                return fail(ec, "socks5_connect_read (socks5_read_buf->at(3) == 0x01)");
                            }
                            if (socks5_read_buf->at(3) == 0x04
                                && bytes_transferred != (4 + 16 + 2)
                                    ) {
                                do_shutdown();
                                return fail(ec, "socks5_connect_read (socks5_read_buf->at(3) == 0x04)");
                            }

                            // socks5 handshake now complete
                            do_ssl_handshake();
                        }));
    }

    void
    do_ssl_handshake() {
        // Perform the SSL handshake
        stream_.async_handshake(
                boost::asio::ssl::stream_base::client,
                boost::beast::bind_front_handler(
                        [this, self = shared_from_this()](boost::beast::error_code ec) {
                            if (ec) {
                                return fail(ec, "ssl_handshake");
                            }

                            do_write();
                        }));
    }

    void
    do_write() {

        // Set a timeout on the operation
        boost::beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));

        // Send the HTTP request to the remote host
        boost::beast::http::async_write(
                stream_, req_,
                boost::beast::bind_front_handler(
                        [this, self = shared_from_this()](
                                boost::beast::error_code ec,
                                std::size_t bytes_transferred) {
                            boost::ignore_unused(bytes_transferred);

                            if (ec) {
                                return fail(ec, "write");
                            }

                            do_read();
                        }));
    }

    void
    do_read() {
        // Receive the HTTP response
        boost::beast::http::async_read(
                stream_, buffer_, res_,
                boost::beast::bind_front_handler(
                        [this, self = shared_from_this()](
                                boost::beast::error_code ec,
                                std::size_t bytes_transferred) {
                            boost::ignore_unused(bytes_transferred);

                            if (ec) {
                                return fail(ec, "read");
                            }

                            // Write the message to standard out
                            // std::cout << res_ << std::endl;

                            do_shutdown(true);
                        }));

    }

    void
    do_shutdown(bool isOn = false) {

        // Set a timeout on the operation
        boost::beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));

        // Gracefully close the stream
        stream_.async_shutdown(
                boost::beast::bind_front_handler(
                        [this, self = shared_from_this(), isOn](boost::beast::error_code ec) {
                            if (ec == boost::asio::error::eof) {
                                // Rationale:
                                // http://stackoverflow.com/questions/25587403/boost-asio-ssl-async-shutdown-always-finishes-with-an-error
                                ec = {};
                            }
                            if (ec) {
                                return fail(ec, "shutdown");
                            }

                            if (isOn) {
                                // If we get here then the connection is closed gracefully
                                allOk();
                            }
                        }));
    }

};

class ConnectTestHttps : public std::enable_shared_from_this<ConnectTestHttps> {
    boost::asio::executor executor;
    std::shared_ptr<boost::asio::ssl::context> ssl_context;
    bool need_verify_ssl = true;
    std::list<std::weak_ptr<ConnectTestHttpsSession>> sessions;
public:
    ConnectTestHttps(boost::asio::executor ex) :
            executor(std::move(ex)),
            ssl_context(std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::sslv23)) {

        if (need_verify_ssl) {
            ssl_context->set_verify_mode(boost::asio::ssl::verify_peer);
            ssl_context->set_default_verify_paths();
#ifdef _WIN32
            HCERTSTORE h_store = CertOpenSystemStore(0, _T("ROOT"));
            if (h_store) {
                X509_STORE *store = SSL_CTX_get_cert_store(ssl_context->native_handle());
                PCCERT_CONTEXT p_context = NULL;
                while ((p_context = CertEnumCertificatesInStore(h_store, p_context))) {
                    const unsigned char *encoded_cert = p_context->pbCertEncoded;
                    X509 *x509 = d2i_X509(NULL, &encoded_cert, p_context->cbCertEncoded);
                    if (x509) {
                        X509_STORE_add_cert(store, x509);
                        X509_free(x509);
                    }
                }
                CertCloseStore(h_store, 0);
            }
#endif // _WIN32
        } else {
            ssl_context->set_verify_mode(boost::asio::ssl::verify_none);
        }

    }

    std::shared_ptr<ConnectTestHttpsSession> &&createTest(
            const std::string &socks5Host,
            const std::string &socks5Port,
            const std::string &targetHost,
            int targetPort,
            const std::string &targetPath,
            int httpVersion = 11
    ) {
        auto s = std::make_shared<ConnectTestHttpsSession>(
                this->executor,
                this->ssl_context,
                targetHost,
                targetPort,
                targetPath,
                httpVersion,
                socks5Host,
                socks5Port
        );
        sessions.push_back(s->weak_from_this());
        // sessions.front().lock();
        return std::move(s);
    }
};


#endif //SOCKS5BALANCERASIO_CONNECTTESTHTTPS_H
