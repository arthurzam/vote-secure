//
// Created by arthur on 8/5/21.
//

#ifndef VOTE_SECURE_EXCHANGE_ITEM_H
#define VOTE_SECURE_EXCHANGE_ITEM_H

#include <atomic>
#include <cassert>
#include <iostream>

#include <cppcoro/coroutine.hpp>

#include "utils.h"
#include "mpc_service.h"

class exchange_item {
public:
    exchange_item() noexcept {
        val[0].m_mask = (1U << mpc_service::D) - 1U;
        val[1].m_mask = (1U << mpc_service::D) - 1U;
    }

    [[nodiscard]] bool is_set() const noexcept {
        return m_state.load(std::memory_order_acquire) == static_cast<const void*>(this);
    }

    void set(utils::share share, unsigned index) {
        for (auto &level : val) {
            if (level.m_mask & (1U << index)) {
                level.m_values[index] = share;
                if ((level.m_mask ^= (1U << index)) == 0) {
                    void *const setState = static_cast<void *>(this);
                    void *oldState = m_state.exchange(setState, std::memory_order_acq_rel);
                    if (oldState != setState) {
                        cppcoro::coroutine_handle<>::from_address(oldState).resume();
                    }
                }
                return;
            }
        }
        std::cout << "set(" << index << ")" << std::endl;
        assert(false);
    }

    auto operator co_await() noexcept {
        class awaiter {
        public:
            explicit awaiter(exchange_item& event) noexcept : m_event(event) {}

            bool await_ready() const noexcept {
                return m_event.is_set();
            }

            bool await_suspend(cppcoro::coroutine_handle<> awaiter) {
                void* oldState = nullptr;

                return m_event.m_state.compare_exchange_strong(
                        oldState,
                        awaiter.address(),
                        std::memory_order_release,
                        std::memory_order_acquire);
            }
            void await_resume() noexcept {}
        private:
            exchange_item& m_event;
        };
        return awaiter{ *this };
    }

    explicit operator cppcoro::task<>() {
        co_await *this;
    }

    std::unique_ptr<utils::share[]> result() {
        std::unique_ptr<utils::share[]> res(new utils::share[mpc_service::D]);
        for (int i = 0; i < mpc_service::D; i++) {
            res[i] = val[0].m_values[i];
            val[0].m_values[i] = val[1].m_values[i];
        }
        val[0].m_mask = (uint32_t)val[1].m_mask;
        val[1].m_mask = ((1U << mpc_service::D) - 1U);

        void* oldState = static_cast<void*>(this);
        m_state.compare_exchange_strong(oldState, nullptr, std::memory_order_relaxed);
        return res;
    }
private:
    std::atomic<void*> m_state = nullptr;
    struct {
        std::atomic<uint32_t> m_mask;
        utils::share m_values[13];
    } val[2];
};

#endif //VOTE_SECURE_EXCHANGE_ITEM_H
