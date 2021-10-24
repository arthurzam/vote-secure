//
// Created by arthur on 7/20/21.
//

#include "talliers_network.h"
#include "mpc_service.h"
#include "endian_number.h"

#include <iostream>
#include <cppcoro/when_all.hpp>

#include <linux/tcp.h>

struct [[gnu::packed]] msg_format {
    uint16_t msg_id;
    utils::share share;
};
static_assert(sizeof(msg_format) == 6);

static constexpr int port(int diff) {
    return 5010 + diff;
}

static void set_socketopt(int sock) {
    int flag = 1;
    int res;
    if ((res = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&flag, sizeof(flag))) < 0)
        throw std::system_error({res, std::generic_category()}, "setsocketopt(SO_REUSEADDR)");
    if ((res = setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char*)&flag, sizeof(flag))) < 0)
        throw std::system_error({res, std::generic_category()}, "setsocketopt(SO_REUSEPORT)");
//    if ((res = setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag))) < 0)
//        throw std::system_error({res, std::generic_category()}, "setsocketopt(TCP_NODELAY)");
}

talliers_network::talliers_network(cppcoro::io_service &ioSvc, int8_t tallier_id) :
        ioSvc(ioSvc),
        talliers(new std::optional<cppcoro::net::socket>[mpc_service::D]),
        server_address(cppcoro::net::ipv4_address(), port(tallier_id)),
        tallier_id(tallier_id) {
    this->talliers_waiting = ((1U << mpc_service::D) - 1U) ^ (1U << tallier_id);
    m_values_table = std::make_unique<values_table>();
}

static cppcoro::task<> stop_server(cppcoro::cancellation_source &canceller, cppcoro::single_consumer_event &end_vote) {
    co_await end_vote;
    (std::cout << "vote ended" << std::endl).flush();
    canceller.request_cancellation();
}

cppcoro::task<> talliers_network::build_collect() {
    cppcoro::cancellation_source canceller;
    std::vector<cppcoro::task<>> tasks;
    tasks.reserve(mpc_service::D + 1);
    tasks.push_back(stop_server(canceller, end_vote));
    tasks.push_back(server(canceller.token()));
    for (int8_t i = 0; i < mpc_service::D; i++)
        if (i != this->tallier_id)
            tasks.push_back(connect(i, canceller.token()));
    co_await cppcoro::when_all(std::move(tasks));
}

cppcoro::task<> talliers_network::server(cppcoro::cancellation_token ct) {
    try {
        auto listeningSocket = cppcoro::net::socket::create_tcpv4(ioSvc);
        set_socketopt(listeningSocket.native_handle());
        listeningSocket.bind(this->server_address);
        listeningSocket.listen();
        (std::cout << "Server up" << std::endl).flush();

        while (true) {
            auto connection = cppcoro::net::socket::create_tcpv4(ioSvc);
            co_await listeningSocket.accept(connection, ct);
            scope.spawn(handle_connection(std::move(connection)));
        }
    } catch (const cppcoro::operation_cancelled &) {
        std::cout << "cancel server" << std::endl;
    }
}

cppcoro::task<> talliers_network::handle_connection(cppcoro::net::socket sock) {
    try {
        int8_t reply_id;
        co_await sock.send(&this->tallier_id, 1);
        co_await sock.recv(&reply_id, 1);
        switch (reply_id) {
            case -1: // Voter
                break;
            case -2: // End Vote
                (std::cout << "vote ended msg" << std::endl).flush();
                this->end_vote.set();
                break;
            default:
                if (talliers_waiting & (1U << reply_id)) {
                    std::cout << "Loaded (recv) " << (int)reply_id << std::endl;
                    talliers[reply_id] = std::move(sock);
                    talliers_waiting ^= (1U << reply_id);
                    if (talliers_waiting == 0) {
                        std::cout << "loaded all" << std::endl;
                        all_talliers.set();
                        this->end_vote.set();
                    }
                    scope.spawn(recv_loop(*talliers[reply_id], static_cast<size_t>(reply_id)));
                } else {
                    std::cerr << "bad " << talliers_waiting << " when " << (int)reply_id << std::endl;
                    // bad set
                }
                break;
        }
        (std::cout << "fin " << (int)reply_id << std::endl).flush();
    } catch (const cppcoro::operation_cancelled &) {
        (std::cerr << "handle_connection cancelled" << std::endl).flush();
    } catch (const std::system_error &err) {
        (std::cerr << "handle_connection(syserr) :" << err.what() << std::endl).flush();
    }
}

cppcoro::task<> talliers_network::connect(int8_t curr_id, cppcoro::cancellation_token ct) {
    try {
        std::cout << "connect " << (int)curr_id << std::endl;
        auto sock = cppcoro::net::socket::create_tcpv4(ioSvc);
        set_socketopt(sock.native_handle());
        sock.bind(this->server_address);
        auto address = cppcoro::net::ipv4_endpoint(cppcoro::net::ipv4_address::loopback(), port(curr_id));
        co_await sock.connect(std::move(address), ct);

        int8_t reply_id;
        co_await sock.send(&this->tallier_id, 1, ct);
        co_await sock.recv(&reply_id, 1, ct);

        if (talliers_waiting & (1U << reply_id)) {
            std::cout << "Loaded (connect) " << (int)reply_id << std::endl;
            talliers[reply_id] = std::move(sock);
            talliers_waiting ^= (1U << reply_id);
            if (talliers_waiting == 0) {
                std::cout << "loaded all" << std::endl;
                all_talliers.set();
                this->end_vote.set();
            }
            scope.spawn(recv_loop(*talliers[reply_id], static_cast<size_t>(reply_id)));
        } else {
            std::cerr << "bad " << talliers_waiting << " when " << (int)reply_id << std::endl;
            // bad set
        }
        std::cout << "fin " << (int)reply_id << std::endl;
    } catch (const cppcoro::operation_cancelled &) {
        std::cerr << "connect " << (int)curr_id << "cancelled" << std::endl;
    } catch (const std::system_error &err) {
        std::cerr << "connect(syserr) " << (int)curr_id << ":" << err.what() << std::endl;
    }
}

cppcoro::task<> talliers_network::recv_loop(cppcoro::net::socket &sock, size_t index) {
    constexpr size_t bufferSize = 16384;
    size_t bytesRead;
    auto buffer = std::make_unique<unsigned char[]>(bufferSize);
    msg_format msg = {0, 0};
    auto cancel_token = m_stop_recv.token();
    try {
        do {
            bytesRead = co_await sock.recv(buffer.get(), bufferSize, cancel_token);
//            std::cout << '[' << index << "] recv " << bytesRead << std::endl;
            for (size_t idx = 0; idx < bytesRead; idx += sizeof(msg_format)) {
                std::memcpy(&msg, &buffer[idx], sizeof(msg_format));
                msg.msg_id = endian_number<uint16_t>::convert(msg.msg_id);
                msg.share = endian_number<utils::share>::convert(msg.share);
                m_values_table->items[msg.msg_id].set(msg.share, index);
            }
        } while (bytesRead > 0);
    } catch (const cppcoro::operation_cancelled &) {
        co_await sock.disconnect();
        std::cerr << "recv_loop " << index << "cancelled" << std::endl;
    } catch (const std::system_error &err) {
        std::cerr << "recv_loop(syserr) " << index << ":" << err.what() << std::endl;
    }
}

cppcoro::task<std::unique_ptr<utils::share[]>> talliers_network::exchange(uint16_t msg_id, std::span<utils::share> shares) {
    auto exchange_one = [](cppcoro::net::socket &sock, uint16_t msg_id, utils::share value) -> cppcoro::task<> {
        msg_format msg{msg_id, endian_number<utils::share>::convert(value)};
        co_await sock.send(&msg, sizeof (msg));
    };
    auto &item = m_values_table->items[msg_id];
    item.set(shares[tallier_id], tallier_id);
    msg_id = endian_number<uint16_t>::convert(msg_id);
    std::vector<cppcoro::task<>> tasks;
    tasks.reserve(shares.size());
    for (int i = 0; i < shares.size(); i++)
        if (this->talliers[i])
            tasks.push_back(exchange_one(*this->talliers[i], msg_id, shares[i]));
    tasks.push_back(static_cast<cppcoro::task<>>(item));
    co_await cppcoro::when_all(std::move(tasks));
    co_return item.result();
}
