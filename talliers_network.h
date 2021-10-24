//
// Created by arthur on 7/20/21.
//

#ifndef VOTE_SECURE_TALLIERS_NETWORK_H
#define VOTE_SECURE_TALLIERS_NETWORK_H

#include <cppcoro/io_service.hpp>
#include <cppcoro/async_scope.hpp>
#include <cppcoro/task.hpp>
#include <cppcoro/net/socket.hpp>
#include <cppcoro/async_manual_reset_event.hpp>
#include <cppcoro/cancellation_source.hpp>
#include <cppcoro/single_consumer_event.hpp>

#include <optional>
#include <memory>

#include "utils.h"
#include "exchange_item.h"

class talliers_network {
public:
    talliers_network(cppcoro::io_service &ioSvc, int8_t tallier_id);
    cppcoro::task<> build_collect();
    cppcoro::task<> close();

    cppcoro::task<std::unique_ptr<utils::share[]>> exchange(uint16_t msg_id, std::span<utils::share> shares);
private:
    cppcoro::task<> server(cppcoro::cancellation_token ct);
    cppcoro::task<> handle_connection(cppcoro::net::socket sock);
    cppcoro::task<> connect(int8_t curr_id, cppcoro::cancellation_token ct);
    cppcoro::task<> recv_loop(cppcoro::net::socket &sock, size_t index);

    cppcoro::io_service &ioSvc;
    cppcoro::async_scope scope;
    std::unique_ptr<std::optional<cppcoro::net::socket>[]> talliers;
    cppcoro::net::ipv4_endpoint server_address;
    int8_t tallier_id;
    uint32_t talliers_waiting;
    cppcoro::async_manual_reset_event all_talliers;
    cppcoro::single_consumer_event end_vote;
    cppcoro::cancellation_source m_stop_recv;

    struct values_table {
        exchange_item items[256 * 256];
    };
    std::unique_ptr<values_table> m_values_table;
};


#endif //VOTE_SECURE_TALLIERS_NETWORK_H
