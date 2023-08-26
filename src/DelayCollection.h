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

#ifndef SOCKS5BALANCERASIO_DELAYCOLLECTION_H
#define SOCKS5BALANCERASIO_DELAYCOLLECTION_H

#ifdef MSVC
#pragma once
#endif

#include <memory>
#include <string>
#include <utility>
#include <deque>
#include <map>
#include <atomic>
#include <mutex>
#include <chrono>
#include <limits>

#include "./log/Log.h"

namespace DelayCollection {
    using TimeMs = std::chrono::milliseconds;
    constexpr TimeMs TimeMsInvalid{-1};
    using TimePoint = std::chrono::steady_clock::time_point;
    using TimePointLocalClock = std::chrono::local_time<std::chrono::system_clock::duration>;

    inline TimePoint nowTimePoint() {
        return std::chrono::steady_clock::now();
    }

    inline TimePointLocalClock nowTimePointClock() {
        return std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
    }

    class TimeHistory {
    public:

        struct DelayInfo {
            TimeMs delay;
            TimePointLocalClock timeClock;

            auto operator<=>(const DelayInfo &o) const {
                if ((timeClock <=> o.timeClock) != std::strong_ordering::equal) {
                    return timeClock <=> o.timeClock;
                } else if ((delay <=> o.delay) != std::strong_ordering::equal) {
                    return delay <=> o.delay;
                }
                return std::strong_ordering::equal;
            }

            DelayInfo(TimeMs delay) : delay(delay), timeClock(nowTimePointClock()) {}

            DelayInfo &operator=(const DelayInfo &o) = default;

            DelayInfo(const DelayInfo &o) = default;

            DelayInfo(DelayInfo &o) = default;

            DelayInfo(DelayInfo &&o) = default;
        };

    private:
        std::recursive_mutex mtx;
        std::deque<DelayInfo> q;

        size_t maxSize = 8192;
        // size_t maxSize = std::numeric_limits<decltype(maxSize)>::max() / 2;

        void trim() {
            std::lock_guard lg{mtx};
            if (q.size() > maxSize) {
                // remove front
                size_t needRemove = q.size() - maxSize;
                if (needRemove == 1) [[likely]] {
                    // only remove first, we use the impl of `deque` to speed up the remove speed
                    // often into this way, so we mark it use c++20 `[[likely]]`
                    q.pop_front();
                } else {
                    BOOST_LOG_S5B(warning) << "TimeHistory::trim() re-create,"
                                           << " needRemove:" << needRemove
                                           << " maxSize:" << maxSize
                                           << " q.size:" << q.size();
                    // we need remove more than 1 , this only happened when maxSize changed
                    // we copy and re-create it, this will wast much time
                    q = decltype(q){q.begin() + needRemove, q.end()};
                }
            }
        }

    public:
        const DelayInfo &addDelayInfo(TimeMs delay) {
            std::lock_guard lg{mtx};
            auto &n = q.emplace_back(delay);
            trim();
            return n;
        }

        std::deque<DelayInfo> history() {
            std::lock_guard lg{mtx};
            // deep copy
            return std::move(std::deque<DelayInfo>{q.begin(), q.end()});
        }

        void setMaxSize(size_t m) {
            std::lock_guard lg{mtx};
            maxSize = m;
            trim();
        }
    };

    class DelayCollect : public std::enable_shared_from_this<DelayCollect> {
    private:

        TimeMs lastTcpPing{TimeMsInvalid};
        TimeMs lastHttpPing{TimeMsInvalid};
        TimeMs lastRelayFirstDelay{TimeMsInvalid};

        TimeHistory historyTcpPing;
        TimeHistory historyHttpPing;
        TimeHistory historyRelayFirstDelay;

    public:

        std::deque<TimeHistory::DelayInfo> getHistoryTcpPing() {
            return std::move(historyTcpPing.history());
        }

        std::deque<TimeHistory::DelayInfo> getHistoryHttpPing() {
            return std::move(historyHttpPing.history());
        }

        std::deque<TimeHistory::DelayInfo> getHistoryRelayFirstDelay() {
            return std::move(historyRelayFirstDelay.history());
        }

        void setMaxSizeTcpPing(size_t m) {
            historyTcpPing.setMaxSize(m);
        }

        void setMaxSizeHttpPing(size_t m) {
            historyHttpPing.setMaxSize(m);
        }

        void setMaxSizeFirstDelay(size_t m) {
            historyRelayFirstDelay.setMaxSize(m);
        }

    public:
        void pushTcpPing(TimeMs t) {
            lastTcpPing = t;
            historyTcpPing.addDelayInfo(t);
        }

        void pushHttpPing(TimeMs t) {
            lastHttpPing = t;
            historyHttpPing.addDelayInfo(t);
        }

        void pushRelayFirstDelay(TimeMs t) {
            lastRelayFirstDelay = t;
            historyRelayFirstDelay.addDelayInfo(t);
        }

    };

}


#endif //SOCKS5BALANCERASIO_DELAYCOLLECTION_H
