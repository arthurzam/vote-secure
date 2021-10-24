#ifndef VOTE_SECURE_MPC_SERVICE_H
#define VOTE_SECURE_MPC_SERVICE_H

#include <vector>
#include <memory>

#include <cppcoro/task.hpp>
#include <cppcoro/net/socket.hpp>

#include "utils.h"

class talliers_network;

class mpc_service {
public:
    static constexpr unsigned D = 3;
    static constexpr unsigned t = 2;
private:
    talliers_network &network;
    const std::unique_ptr<utils::share[]> vandermond_mat_inv_row;
    const unsigned short p_bits_size;
    const unsigned short block_size;
public:
    explicit mpc_service(talliers_network &network);

    cppcoro::task<utils::share> multiply(uint16_t msg_id, utils::share a, utils::share b);
    cppcoro::task<utils::share> resolve(uint16_t msg_id, utils::share share);
    cppcoro::task<utils::share> random_number(uint16_t msg_id);
    cppcoro::task<utils::share> random_bit(uint16_t msg_id);
    cppcoro::task<utils::share> fan_in_or(uint16_t msg_id, std::span<utils::share> bits);
    cppcoro::task<std::unique_ptr<utils::share[]>> prefix_or(uint16_t msg_id, std::span<utils::share> a_i);
    cppcoro::task<utils::share> less_bitwise(uint16_t msg_id, std::span<utils::share> a_i, std::span<utils::share> b_i);
    cppcoro::task<std::unique_ptr<utils::share[]>> random_number_bits(uint16_t msg_id);
    cppcoro::task<utils::share> is_odd(uint16_t msg_id, utils::share x);
    cppcoro::task<utils::share> less(uint16_t msg_id, utils::share a, utils::share b);
};


#endif //VOTE_SECURE_MPC_SERVICE_H
