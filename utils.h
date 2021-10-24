#ifndef VOTE_SECURE_UTILS_H
#define VOTE_SECURE_UTILS_H

#include <memory>
#include <cstdint>

namespace utils {
    template <class T, class U>
    constexpr T narrow_cast(U&& u) noexcept {
        return static_cast<T>(std::forward<U>(u));
    }

    using share = uint32_t;
    extern unsigned p;
    unsigned random_value();

//    share pow(share base, unsigned exponent);
//    share gcd(share a, share b);
    share mod_inverse(share value);
    share modular_sqrt(share a);
    unsigned short ceil_sqrt(unsigned short val);
    unsigned short ceil_log2(unsigned short val);
    std::unique_ptr<share[]> vandermond_mat_inv_row(int N);

    std::unique_ptr<share[]> gen_shamir(uint32_t value, unsigned shares_count, unsigned threshold);
    share resolve_shamir(std::unique_ptr<share[]> shares, unsigned shares_size);
    std::unique_ptr<share[]> lagrange_polynomial_fan(unsigned count);
};


#endif //VOTE_SECURE_UTILS_H
