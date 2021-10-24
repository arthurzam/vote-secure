//
// Created by arthur on 7/16/21.
//

#include "mpc_service.h"

#include <cppcoro/when_all.hpp>
#include <iostream>

#include "endian_number.h"
#include "talliers_network.h"


using utils::p;
using utils::share;

namespace calc {
    template<typename II>
    share sum(II begin, II end, uint64_t init = 0) {
        uint64_t sum = init;
        for (; begin != end; ++begin) {
            sum += *begin;
        }
        return utils::narrow_cast<share>(sum % p);
    }
}

cppcoro::task<utils::share> mpc_service::multiply(uint16_t msg_id, share a, share b) {
    auto h_i = utils::gen_shamir(utils::narrow_cast<share>(((uint64_t)a * b) % p), D, t);
    auto results = co_await network.exchange(msg_id, std::move(h_i));

    uint64_t sum = 0;
    for (int i = 0; i < D; i++) {
        sum += utils::narrow_cast<share>(((uint64_t)results[i] * vandermond_mat_inv_row[i]) % p);
    }
    co_return utils::narrow_cast<share>(sum % p);
}

cppcoro::task<share> mpc_service::resolve(uint16_t msg_id, share part) {
    std::unique_ptr<share[]> shares(new share[D]);
    for (int i = 0; i < D; i++)
        shares[i] = part;
    auto answers = co_await network.exchange(msg_id, std::move(shares));
    co_return utils::resolve_shamir(std::move(answers), D);
}

cppcoro::task<share> mpc_service::random_number(uint16_t msg_id) {
    auto r_i = utils::gen_shamir(utils::random_value(), D, t);
    auto all_rnd = co_await network.exchange(msg_id, std::move(r_i));
    co_return calc::sum(all_rnd.get(), all_rnd.get() + D);
}

cppcoro::task<share> mpc_service::random_bit(uint16_t msg_id) {
    static uint64_t inverse_2 = utils::mod_inverse(2);
    for (;;) {
        auto r = co_await this->random_number(msg_id);
        auto r2 = co_await this->resolve(msg_id, co_await this->multiply(msg_id, r, r));
        if (r2 != 0) {
            uint64_t root_inv = utils::mod_inverse(utils::modular_sqrt(r2));
            co_return utils::narrow_cast<share>((((root_inv * r + 1) % p) * inverse_2) % p);
        }
    }
}

cppcoro::task<share> mpc_service::fan_in_or(uint16_t msg_id, const share *begin, const share *end) {
    const share A = calc::sum(begin, end, 1);
    const auto alpha_i = utils::lagrange_polynomial_fan(end - begin);

    uint64_t res = utils::narrow_cast<uint32_t>((alpha_i[0] + ((uint64_t)alpha_i[1] * A) % p) % p);
    share mul_A = A;
    for (unsigned i = 1; i < end - begin; i++) {
        mul_A = co_await this->multiply(msg_id, A, mul_A);
        res += ((uint64_t)alpha_i[i + 1] * mul_A) % p;
    }
    co_return utils::narrow_cast<uint32_t>(res % p);
}

cppcoro::task<std::unique_ptr<share[]>> mpc_service::prefix_or(uint16_t msg_id, std::vector<share> a_i) {
    const auto lam = utils::ceil_sqrt(a_i.size());

    // calc x
    std::vector<cppcoro::task<share>> x_i_tasks;
    x_i_tasks.reserve(lam);
    for (unsigned i = 0, msg = msg_id; i < a_i.size(); i += lam, msg += 2 * lam)
        x_i_tasks.push_back(this->fan_in_or(msg, a_i.data() + i, a_i.data() + std::min<unsigned>(a_i.size(), i + lam)));
    const auto x_i = co_await cppcoro::when_all(std::move(x_i_tasks));

    // calc y
    std::vector<cppcoro::task<share>> y_i_tasks;
    y_i_tasks.reserve(lam);
    for (unsigned i = 1, msg = msg_id; i <= lam; i++, msg += 2 * lam)
        y_i_tasks.push_back(this->fan_in_or(msg, x_i.data(), x_i.data() + i));
    auto y_i = co_await cppcoro::when_all(std::move(y_i_tasks));

    // calc f inside y
    std::unique_ptr<share[]> f_i(new share[lam]);
    f_i[0] = y_i[0];
    for (unsigned i = 1; i < y_i.size(); i++)
        f_i[i] = y_i[i] - y_i[i - 1];

    // calc g
    std::vector<cppcoro::task<share>> g_ij_tasks;
    g_ij_tasks.reserve(a_i.size());
    for (unsigned i = 0, ij = 0; i < lam; i++)
        for (unsigned j = 0; j < lam && ij < a_i.size(); j++, ij++)
            g_ij_tasks.push_back(this->multiply(msg_id + ij, f_i[i], a_i[ij]));
    auto g_ij = co_await cppcoro::when_all(std::move(g_ij_tasks));

    // calc c
    std::unique_ptr<share[]> c_j(new share[lam]);
    for (unsigned j = 0; j < lam; j++)
        c_j[j] = 0;
    for (unsigned ij = 0; ij < g_ij.size(); )
        for (unsigned j = 0; j < lam && ij < g_ij.size(); j++, ij++)
            c_j[j] = (c_j[j] + (uint64_t)g_ij[ij]) % p;

    // calc h
    std::vector<cppcoro::task<share>> h_j_tasks;
    h_j_tasks.reserve(lam);
    for (unsigned j = 1, msg = msg_id; j <= lam; j++, msg += 2 * lam)
        h_j_tasks.push_back(this->fan_in_or(msg, c_j.get(), c_j.get() + j));
    auto h_j = co_await cppcoro::when_all(std::move(h_j_tasks));

    // calc s
    std::vector<cppcoro::task<share>> s_ij_tasks;
    s_ij_tasks.reserve(lam * lam);
    for (unsigned i = 0, msg = msg_id; i < lam; i++)
        for (unsigned j = 0; j < lam; j++, msg++)
            s_ij_tasks.push_back(this->multiply(msg, f_i[i], h_j[j]));
    auto s_ij = co_await cppcoro::when_all(std::move(s_ij_tasks));

    std::unique_ptr<share[]> b_i(new share[a_i.size()]);
    for (unsigned i = 0, ij = 0; i < lam; i++)
        for (unsigned j = 0; j < lam && ij < a_i.size(); j++, ij++)
            b_i[ij] = utils::narrow_cast<share>(((uint64_t)p + s_ij[ij] + y_i[i] - f_i[i]) % p);
    co_return b_i;
}

cppcoro::task<share> mpc_service::less_bitwise(uint16_t msg_id, std::unique_ptr<share[]> a_i, std::unique_ptr<share[]> b_i, unsigned size) {
    // calc c
    std::vector<cppcoro::task<share>> c_i_tasks;
    c_i_tasks.reserve(size);
    for(unsigned i = 0; i < size; i++)
        c_i_tasks.push_back(this->multiply(msg_id + i, a_i[i], b_i[i]));
    auto c_i = co_await cppcoro::when_all(std::move(c_i_tasks));
    for(unsigned i = 0; i < size; i++)
        c_i[i] = utils::narrow_cast<uint32_t>((((uint64_t)p - c_i[i]) * 2 + a_i[i] + b_i[i]) % p);
    std::reverse(c_i.begin(), c_i.end());

    // calc d
    auto d_i = co_await this->prefix_or(msg_id, std::move(c_i));
    std::reverse(d_i.get(), d_i.get() + size);

    // calc e inside d
    for(unsigned i = 0; i < size - 1; i++)
        d_i[i] = utils::narrow_cast<uint32_t>(((uint64_t)p + d_i[i] - d_i[i + 1]) % p);

    // calc h
    std::vector<cppcoro::task<share>> h_i_tasks;
    h_i_tasks.reserve(size);
    for(unsigned i = 0; i < size; i++)
        h_i_tasks.push_back(this->multiply(msg_id + i, d_i[i], b_i[i]));
    auto h_i = co_await cppcoro::when_all(std::move(h_i_tasks));

    co_return calc::sum(h_i.begin(), h_i.end());
}

cppcoro::task<std::unique_ptr<share[]>> mpc_service::random_number_bits(uint16_t msg_id) {
    static uint64_t bits_count = utils::ceil_sqrt(p);
    for (;;) {
        std::vector<cppcoro::task<share>> r_i_tasks;
        r_i_tasks.reserve(bits_count);
        for(unsigned i = 0; i < bits_count; i++)
            r_i_tasks.push_back(this->random_bit(msg_id + i));
        auto r_i_t = co_await cppcoro::when_all(std::move(r_i_tasks));


    }
}