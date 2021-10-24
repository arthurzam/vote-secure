#include <random>
#include <iostream>
#include <cmath>

#include "utils.h"

unsigned utils::p = (1U << 31) - 1;

namespace utils {
    unsigned random_value() {
        static std::random_device rnd;
        static std::default_random_engine e1(rnd());
        static std::uniform_int_distribution<unsigned> uniform_dist(0, p - 1);
        return uniform_dist(e1);
    }

    static share pow(share base, unsigned exponent) {
        uint64_t result = 1;
        while (exponent > 0) {
            if (exponent % 2 == 1)
                result = (result * base) % p;
            exponent = exponent >> 1;
            base = narrow_cast<share>(((uint64_t)base * base) % p);
        }
        return result;
    }

    static share gcd(share a, share b) {
        while (b != 0) {
            share tmp = b;
            b = a % b;
            a = tmp;
        }
        return a;
    }

    static share order(share x) {
        for (share k = 3;; k++) {
            if (pow(x, k) == 1)
                return k;
        }
    }

    share mod_inverse(share value) {
        int64_t a = value, y = 0, x = 1;
        unsigned m = p, m0 = p;
        while (a > 1) {
            int64_t q = a / m, t = m; // q is quotient

            m = a % m, a = t; // m is remainder now, process same as Euclid's algo
            t = y;

            y = x - q * y; // Update y and x
            x = t;
        }
        if (x < 0)
            x += m0;
        return narrow_cast<share>(x);
    }

    share modular_sqrt(share a) {
        if (p % 4 == 3)
            return pow(a, (p + 1) / 4);
        share s = p - 1, e = 0;
        for (; s % 2 == 0; e++)
            s /= 2;

        share n;
        for (n = 2; pow(n, (p - 1) / 2) != p - 1; n++)
            ;

        share x = pow(a, (s + 1) / 2);
        share b = pow(a, s);
        share g = pow(n, s);
        share r = e;

        for (;;) {
            share t = b, m;
            for (m = 0; m < r; m++) {
                if (t == 1)
                    break;
                t = pow(t, 2);
            }
            if (m == 0)
                return x;

            share gs = pow(g, 1UL << (r - m - 1));
            g = narrow_cast<share>((uint64_t)gs * gs);
            x = narrow_cast<share>((uint64_t)gs * x);
            b = narrow_cast<share>((uint64_t)b * g);
            r = m;
        }
    }

    unsigned short ceil_sqrt(unsigned short val) {
        return (unsigned short)ceilf(sqrtf(val));
    }

    unsigned short ceil_log2(unsigned short val) {
        return (unsigned short)ceilf(log2f(val));
    }

    std::unique_ptr<share[]> vandermond_mat_inv_row(int N) {
        using row_t = std::unique_ptr<uint32_t[]>;

        std::unique_ptr<row_t[]> matrix(new row_t[N]);
        for (int i = 0; i < N; i++) {
            auto row = new uint32_t[2 * N];
            for (int j = 0; j < N; j++) {
                row[j] = pow(i + 1, j);
                row[N + j] = 0;
            }
            row[N + i] = 1;
            matrix[i].reset(row);
        }

        for (int i = N - 1; i > 0; i--)
            if (matrix[i - 1][0] < matrix[i][0])
                std::swap(matrix[i - 1], matrix[i]);

        for (int i = 0; i < N; i++) {
            uint64_t inv = mod_inverse(matrix[i][i]);
            for (int j = 0; j < N; j++) {
                if (i != j) {
                    uint64_t temp = (matrix[j][i] * inv) % p;
                    for (int k = 0; k < 2 * N; k++)
                        matrix[j][k] = narrow_cast<uint32_t>((p + matrix[j][k] - (matrix[i][k] * temp) % p) % p);
                }
            }
        }

        for (int i = 0; i < N; i++) {
            uint64_t inv = mod_inverse(matrix[i][i]);
            for (int j = 0; j < 2 * N; j++)
                matrix[i][j] = narrow_cast<uint32_t>((matrix[i][j] * inv) % p);
        }

        std::unique_ptr<share[]> result(new share[N]);
        for (int i = 0; i < N; i++)
            result[i] = matrix[0][N + i];
        return result;
    }

    std::unique_ptr<share[]> gen_shamir(unsigned value, unsigned shares_count, unsigned threshold) {
        // generate the coefficients for the shamir function
        std::unique_ptr<unsigned[]> coeffs(new unsigned[threshold]);
        coeffs[0] = value;
        for (int i = 1; i < threshold; i++)
            coeffs[i] = random_value();

        // generate the shares to every participant
        std::unique_ptr<share[]> shares(new share[shares_count]);
        for (int i = 0; i < shares_count; i++) {
            uint64_t res = 0;
            uint32_t x = i + 1;
            uint64_t x_i = 1;
            for (int j = 0; j < threshold; j++) {
                res += coeffs[j] * x_i;
                res %= p;
                x_i *= x;
            }
            shares[i] = narrow_cast<share>(res);
        }
        return shares;
    }

    share resolve_shamir(std::unique_ptr<share[]> shares, unsigned shares_size) {
        uint64_t sum = 0;
        for (int i = 0; i < shares_size; i++) {
            uint64_t numerator = 1, denominator = 1;
            for (int j = 0; j < shares_size; j++) {
                if (i == j) continue;
                numerator *= (j + 1);
                if (numerator > (1UL << 32))
                    numerator %= p;
                denominator *= (p + j - i) % p;
                denominator %= p;
            }
            uint64_t l_i = (numerator * mod_inverse(narrow_cast<share>(denominator))) % p;
            sum += (l_i * shares[i]) % p;
        }
        return narrow_cast<share>(sum % p);
    }

    std::unique_ptr<share[]> lagrange_polynomial_fan(unsigned count) {
        std::unique_ptr<share[]> coeffs(new share[count + 1]);
        for (unsigned i = 0; i < count + 1; i++)
            coeffs[i] = 0;
        std::unique_ptr<uint64_t[]> temp(new uint64_t[count + 2]);

        for (unsigned x_j = 2; x_j <= count + 1; x_j++) {
            temp[0] = 1;
            for (unsigned i = 1; i < count + 1; i++)
                temp[i] = 0;
            uint64_t denominator = 1;
            for (unsigned x_m = 1, v = 2; x_m <= count + 1; x_m++) {
                if (x_j != x_m) {
                    denominator = narrow_cast<uint32_t>((denominator * (p + x_j - x_m) % p));
                    for (unsigned pos = v++; pos != 0; pos--)
                        temp[pos] = narrow_cast<uint32_t>((p + temp[pos - 1] - (temp[pos] * x_m) % p) % p);
                    temp[0] = p - (temp[0] * x_m) % p;
                }
            }
            denominator = mod_inverse(narrow_cast<share>(denominator));
            for (unsigned i = 0; i < count + 1; i++)
                coeffs[i] = narrow_cast<uint32_t>((coeffs[i] + (temp[i] * denominator) % p) % p);
        }
        return coeffs;
    }
}