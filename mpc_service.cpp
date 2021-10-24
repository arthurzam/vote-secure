#include "mpc_service.h"

#include <cppcoro/when_all.hpp>
#include <iostream>

#include "endian_number.h"
#include "talliers_network.h"

#include <span>

using utils::p;
using utils::share;

namespace calc {
    share sum(const std::span<utils::share> numbers, uint64_t init = 0) {
        uint64_t sum = init;
        for (auto num : numbers)
            sum += num;
        return utils::narrow_cast<share>(sum % p);
    }

    std::unique_ptr<share[]> to_bits(share number, unsigned size) {
        std::unique_ptr<share[]> res(new share[size]);
        for (unsigned i = 0; i < size; i++) {
            res[i] = number % 2;
            number /= 2;
        }
        return res;
    }
}

mpc_service::mpc_service(talliers_network &network) :
    network(network),
    vandermond_mat_inv_row(utils::vandermond_mat_inv_row(D)),
    p_bits_size(utils::ceil_log2(utils::p)),
    block_size(utils::block_size(utils::p))
{ }

cppcoro::task<utils::share> mpc_service::multiply(uint16_t msg_id, share a, share b) {
    auto h_i = utils::gen_shamir(utils::narrow_cast<share>(((uint64_t)a * b) % p), D, t);
    auto results = co_await network.exchange(msg_id, {h_i.get(), D});

    uint64_t sum = 0;
    for (unsigned i = 0; i < D; i++) {
        sum += utils::narrow_cast<share>(((uint64_t)results[i] * vandermond_mat_inv_row[i]) % p);
    }
    co_return utils::narrow_cast<share>(sum % p);
}

cppcoro::task<share> mpc_service::resolve(uint16_t msg_id, share part) {
    std::unique_ptr<share[]> shares(new share[D]);
    for (unsigned i = 0; i < D; i++)
        shares[i] = part;
    auto answers = co_await network.exchange(msg_id, {shares.get(), D});
    co_return utils::resolve_shamir({answers.get(), D});
}

cppcoro::task<share> mpc_service::random_number(uint16_t msg_id) {
    auto r_i = utils::gen_shamir(utils::random_value(), D, t);
    auto all_rnd = co_await network.exchange(msg_id, {r_i.get(), D});
    co_return calc::sum({all_rnd.get(), D});
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

cppcoro::task<share> mpc_service::fan_in_or(uint16_t msg_id, const std::span<utils::share> bits) {
    const share A = calc::sum(bits, 1);
    const auto alpha_i = utils::lagrange_polynomial_fan(bits.size());

    uint64_t res = utils::narrow_cast<uint32_t>((alpha_i[0] + ((uint64_t)alpha_i[1] * A) % p) % p);
    share mul_A = A;
    for (unsigned i = 1; i < bits.size(); i++) {
        mul_A = co_await this->multiply(msg_id, A, mul_A);
        res += ((uint64_t)alpha_i[i + 1] * mul_A) % p;
    }
    co_return utils::narrow_cast<uint32_t>(res % p);
}

cppcoro::task<std::unique_ptr<share[]>> mpc_service::prefix_or(uint16_t msg_id, const std::span<share> a_i) {
    const auto lam = utils::ceil_sqrt(a_i.size());

    // calc x
    std::vector<cppcoro::task<share>> x_i_tasks;
    x_i_tasks.reserve(lam);
    for (unsigned i = 0, msg = msg_id; i < a_i.size(); i += lam, msg += 2 * lam)
        x_i_tasks.push_back(this->fan_in_or(msg, {a_i.data() + i, std::min<unsigned>(lam, a_i.size() - i)}));
    auto x_i = co_await cppcoro::when_all(std::move(x_i_tasks));

    // calc y
    std::vector<cppcoro::task<share>> y_i_tasks;
    y_i_tasks.reserve(lam);
    for (unsigned i = 1, msg = msg_id; i <= lam; i++, msg += 2 * lam)
        y_i_tasks.push_back(this->fan_in_or(msg, {x_i.data(), i}));
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
        h_j_tasks.push_back(this->fan_in_or(msg, {c_j.get(), j}));
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

cppcoro::task<share> mpc_service::less_bitwise(uint16_t msg_id, const std::span<share> a_i, const std::span<share> b_i) {
    assert(a_i.size() == b_i.size());
    // calc c
    std::vector<cppcoro::task<share>> c_i_tasks;
    c_i_tasks.reserve(a_i.size());
    for (unsigned i = 0; i < a_i.size(); i++)
        c_i_tasks.push_back(this->multiply(msg_id + i, a_i[i], b_i[i]));
    auto c_i = co_await cppcoro::when_all(std::move(c_i_tasks));
    for (unsigned i = 0; i < a_i.size(); i++)
        c_i[i] = utils::narrow_cast<share>((((uint64_t)p - c_i[i]) * 2 + a_i[i] + b_i[i]) % p);
    std::reverse(c_i.begin(), c_i.end());

    // calc d
    auto d_i = co_await this->prefix_or(msg_id, c_i);
    std::reverse(d_i.get(), d_i.get() + a_i.size());

    // calc e inside d
    for (unsigned i = 0; i < a_i.size() - 1; i++)
        d_i[i] = utils::narrow_cast<share>(((uint64_t)p + d_i[i] - d_i[i + 1]) % p);

    // calc h
    std::vector<cppcoro::task<share>> h_i_tasks;
    h_i_tasks.reserve(a_i.size());
    for (unsigned i = 0; i < a_i.size(); i++)
        h_i_tasks.push_back(this->multiply(msg_id + i, d_i[i], b_i[i]));
    auto h_i = co_await cppcoro::when_all(std::move(h_i_tasks));

    co_return calc::sum(h_i);
}

cppcoro::task<std::unique_ptr<share[]>> mpc_service::random_number_bits(uint16_t msg_id) {
    for (;;) {
        std::vector<cppcoro::task<share>> r_i_tasks;
        r_i_tasks.reserve(p_bits_size);
        for (unsigned i = 0; i < p_bits_size; i++)
            r_i_tasks.push_back(this->random_bit(msg_id + i));
        auto r_i = co_await cppcoro::when_all(std::move(r_i_tasks));

        auto p_i = calc::to_bits(p, p_bits_size);
        auto check_bit = co_await this->resolve(msg_id, co_await this->less_bitwise(msg_id, r_i, {p_i.get(), p_bits_size}));
        if (check_bit == 1) {
            std::unique_ptr<share[]> res(new share[p_bits_size]);
            std::copy(r_i.begin(), r_i.end(), res.get());
            co_return res;
        }
    }
}

cppcoro::task<share> mpc_service::is_odd(uint16_t msg_id, share x) {
    auto r_i = co_await this->random_number_bits(msg_id);
    auto r = [&]() -> share {
        uint64_t r = r_i[0];
        for (unsigned i = 1; i < p_bits_size; i++)
            r += ((uint64_t)r_i[i] * (1UL << i)) % p;
        return utils::narrow_cast<share>(r % p);
    }();
    auto c = co_await this->resolve(msg_id, utils::narrow_cast<share>(((uint64_t)x + r) % p));
    auto d = (c % 2 == 0 ? r_i[0] : (p - r_i[0] + 1) % p);
    auto c_i = calc::to_bits(c, p_bits_size);
    auto e = co_await this->less_bitwise(msg_id, {c_i.get(), p_bits_size}, {r_i.get(), p_bits_size});
    auto ed = co_await this->multiply(msg_id, e, d);
    co_return utils::narrow_cast<share>((((uint64_t)p - ed) * 2 + e + d) % p);
}

cppcoro::task<share> mpc_service::less(uint16_t msg_id, share a, share b) {
    auto [w, x, y] = co_await cppcoro::when_all(this->is_odd(msg_id, utils::narrow_cast<share>(((uint64_t)a * 2) % p)),
                                                this->is_odd(msg_id + block_size, utils::narrow_cast<share>(((uint64_t)b * 2) % p)),
                                                this->is_odd(msg_id + 2 * block_size, utils::narrow_cast<share>((((uint64_t)p + a - b) * 2) % p)));
    x = p - x + 1;
    y = p - y + 1;
    w = p - w + 1;
    auto c = co_await this->multiply(msg_id, x, y);
    auto d = utils::narrow_cast<share>(((uint64_t)p + x + y - c) % p);
    auto e = co_await this->multiply(msg_id, w, utils::narrow_cast<share>(((uint64_t)p + d - c) % p));
    co_return utils::narrow_cast<share>(((uint64_t)p + 1 + e - d) % p);
}