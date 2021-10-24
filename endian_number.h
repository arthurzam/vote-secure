//
// Created by arthur on 7/20/21.
//

#ifndef VOTE_SECURE_ENDIAN_NUMBER_H
#define VOTE_SECURE_ENDIAN_NUMBER_H

#include <bit>
#include <cstdlib>
#include <cstdint>


template <typename T>
struct endian_number { };

template <> struct endian_number<uint8_t> {
    static uint8_t convert(uint8_t num) {
        return num;
    }
};

template <> struct endian_number<uint16_t> {
    static uint32_t convert(uint16_t num) {
        if constexpr(std::endian::native == std::endian::big)
            return num;
        else {
            return ::ntohs(num);
        }
    }
};

template <> struct endian_number<uint32_t> {
    static uint32_t convert(uint32_t num) {
        if constexpr(std::endian::native == std::endian::big)
            return num;
        else {
            return ::ntohl(num);
        }
    }
};

#endif //VOTE_SECURE_ENDIAN_NUMBER_H
