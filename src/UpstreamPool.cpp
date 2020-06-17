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

#include "UpstreamPool.h"

std::chrono::time_point<std::chrono::system_clock> UpstreamTimePointNow() {
    return std::chrono::system_clock::now();
}

std::string printUpstreamTimePoint(std::chrono::time_point<std::chrono::system_clock> p) {
    using namespace std;
    using namespace std::chrono;
    // https://stackoverflow.com/questions/12835577/how-to-convert-stdchronotime-point-to-calendar-datetime-string-with-fraction

    auto ttime_t = system_clock::to_time_t(p);
    auto tp_sec = system_clock::from_time_t(ttime_t);
    milliseconds ms = duration_cast<milliseconds>(p - tp_sec);

    std::tm *ttm = localtime(&ttime_t);

    char date_time_format[] = "%Y.%m.%d-%H.%M.%S";

    char time_str[] = "yyyy.mm.dd.HH-MM.SS.fff";

    strftime(time_str, strlen(time_str), date_time_format, ttm);

    string result(time_str);
    result.append(".");
    result.append(to_string(ms.count()));

    return result;
}

std::string UpstreamServer::print() {
    std::stringstream ss;
    ss << "["
       << "index:" << index << ", "
       << "name:" << name << ", "
       << "host:" << host << ", "
       << "port:" << port << ", "
       << "]";
    return ss.str();
}

void UpstreamServer::updateOnlineTime() {
    isOffline = false;
    lastOnlineTime = UpstreamTimePointNow();
}

UpstreamPool::UpstreamPool(boost::asio::executor ex, std::shared_ptr<TcpTest> tcpTest,
                           std::shared_ptr<ConnectTestHttps> connectTestHttps)
        : ex(ex),
          tcpTest(std::move(tcpTest)),
          lastConnectComeTime(UpstreamTimePointNow()),
          connectTestHttps(std::move(connectTestHttps)),
          lastChangeUpstreamTime(UpstreamTimePointNow()) {}

const std::deque<UpstreamServerRef> &UpstreamPool::pool() {
    return _pool;
}

void UpstreamPool::setConfig(std::shared_ptr<ConfigLoader> configLoader) {
    _configLoader = std::move(configLoader);
    const auto &c = _configLoader->config.upstream;
    _pool.clear();
    for (size_t i = 0; i != c.size(); ++i) {
        auto &r = c[i];
        UpstreamServerRef u = std::make_shared<UpstreamServer>(
                i, r.name, r.host, r.port, r.disable
        );
        _pool.push_back(u);
    }
}

void UpstreamPool::forceSetLastUseUpstreamIndex(size_t i) {
    if (i >= 0 && i < _pool.size()) {
        lastUseUpstreamIndex = i;
    }
}

size_t UpstreamPool::getLastUseUpstreamIndex() {
    return lastUseUpstreamIndex;
}

bool UpstreamPool::checkServer(const UpstreamServerRef &u) const {
    return u
           && u->lastConnectTime.has_value()
           && u->lastOnlineTime.has_value()
           && !u->lastConnectFailed
           && !u->isOffline
           && !u->isManualDisable;
}

auto UpstreamPool::getNextServer() -> UpstreamServerRef {
    const auto _lastUseUpstreamIndex = lastUseUpstreamIndex;
    if (_pool.empty()) {
        return UpstreamServerRef{};
    }
    while (true) {
        ++lastUseUpstreamIndex;
        if (lastUseUpstreamIndex >= _pool.size()) {
            lastUseUpstreamIndex = 0;
        }
        if (checkServer(_pool[lastUseUpstreamIndex])) {
            return _pool[lastUseUpstreamIndex]->shared_from_this();
        }
        if (_lastUseUpstreamIndex == lastUseUpstreamIndex) {
            // cannot find
            return UpstreamServerRef{};
        }
    }
}

auto UpstreamPool::tryGetLastServer() -> UpstreamServerRef {
    const auto _lastUseUpstreamIndex = lastUseUpstreamIndex;
    if (_pool.empty()) {
        return UpstreamServerRef{};
    }
    while (true) {
        if (lastUseUpstreamIndex >= _pool.size()) {
            lastUseUpstreamIndex = 0;
        }
        if (checkServer(_pool[lastUseUpstreamIndex])) {
            return _pool[lastUseUpstreamIndex]->shared_from_this();
        }
        ++lastUseUpstreamIndex;
        if (lastUseUpstreamIndex >= _pool.size()) {
            lastUseUpstreamIndex = 0;
        }
        if (_lastUseUpstreamIndex == lastUseUpstreamIndex) {
            // cannot find
            return UpstreamServerRef{};
        }
    }
}

auto UpstreamPool::filterValidServer() -> std::vector<UpstreamServerRef> {
    std::vector<UpstreamServerRef> r;
    for (auto &a:_pool) {
        if (checkServer(a)) {
            r.emplace_back(a->shared_from_this());
        }
    }
    return r;
}

auto UpstreamPool::getServerBasedOnAddress() -> UpstreamServerRef {
    const auto upstreamSelectRule = _configLoader->config.upstreamSelectRule;

    UpstreamServerRef s{};
    switch (upstreamSelectRule) {
        case RuleEnum::loop:
            s = getNextServer();
            std::cout << "getServerBasedOnAddress:" << (s ? s->print() : "nullptr") << "\n";
            return s;
        case RuleEnum::one_by_one:
            s = tryGetLastServer();
            std::cout << "getServerBasedOnAddress:" << (s ? s->print() : "nullptr") << "\n";
            return s;
        case RuleEnum::change_by_time: {
            UpstreamTimePoint t = UpstreamTimePointNow();
            const auto &d = _configLoader->config.serverChangeTime;
            if ((t - lastChangeUpstreamTime) > d) {
                s = getNextServer();
                lastChangeUpstreamTime = UpstreamTimePointNow();
            } else {
                s = tryGetLastServer();
            }
            std::cout << "getServerBasedOnAddress:" << (s ? s->print() : "nullptr") << "\n";
            return s;
        }
        case RuleEnum::random:
        default: {
            auto rs = filterValidServer();
            if (!rs.empty()) {
                std::uniform_int_distribution<size_t> distribution(0, rs.size() - 1);
                size_t i = distribution(randomGenerator);
                s = rs[i];
            } else {
                s.reset();
            }
            std::cout << "getServerBasedOnAddress:" << (s ? s->print() : "nullptr") << "\n";
            return s;
        }
    }
}

void UpstreamPool::endAdditionTimer() {
    if (additionTimer) {
        additionTimer->cancel();
        additionTimer.reset();
    }
}

void UpstreamPool::endCheckTimer() {
    if (tcpCheckerTimer) {
        tcpCheckerTimer->cancel();
        tcpCheckerTimer.reset();
    }
    if (connectCheckerTimer) {
        connectCheckerTimer->cancel();
        connectCheckerTimer.reset();
    }
}

void UpstreamPool::startAdditionTimer() {
    if (!additionTimer) {
        additionTimer = std::make_shared<boost::asio::steady_timer>(
                ex,
                (_configLoader->config.tcpCheckStart
                 + _configLoader->config.connectCheckStart
                 + _configLoader->config.tcpCheckPeriod
                ) * 2
        );
        do_AdditionTimer();
    }
}

void UpstreamPool::startCheckTimer() {
    if (tcpCheckerTimer && connectCheckerTimer) {
        return;
    }
    endCheckTimer();
    endAdditionTimer();
    startAdditionTimer();

    tcpCheckerTimer = std::make_shared<CheckerTimerType>(ex, _configLoader->config.tcpCheckStart);
    do_tcpCheckerTimer();

    connectCheckerTimer = std::make_shared<CheckerTimerType>(ex, _configLoader->config.connectCheckStart);
    do_connectCheckerTimer();

}

std::string UpstreamPool::print() {
    std::stringstream ss;
    for (size_t i = 0; i != _pool.size(); ++i) {
        const auto &r = _pool[i];
        ss << r->index << ":[" << "\n"
           << "\t" << "name :" << r->name << "\n"
           << "\t" << "host :" << r->host << "\n"
           << "\t" << "port :" << r->port << "\n"
           << "\t" << "isOffline :" << r->isOffline << "\n"
           << "\t" << "lastConnectFailed :" << r->lastConnectFailed << "\n"
           << "\t" << "lastOnlineTime :" << (
                   r->lastOnlineTime.has_value() ?
                   printUpstreamTimePoint(r->lastOnlineTime.value()) : "empty") << "\n"
           << "\t" << "lastConnectTime :" << (
                   r->lastConnectTime.has_value() ?
                   printUpstreamTimePoint(r->lastConnectTime.value()) : "empty") << "\n"
           << "\t" << "lastConnectCheckResult :" << r->lastConnectCheckResult << "\n"
           << "\t" << "disable :" << r->disable << "\n"
           << "\t" << "isManualDisable :" << r->isManualDisable << "\n"
           << "\t" << "connectCount :" << r->connectCount << "\n"
           << "]" << "\n";
    }
    return ss.str();
}

void UpstreamPool::do_tcpCheckerTimer_impl() {
    for (auto &a: _pool) {
        if (!a->isManualDisable) {
            auto p = std::to_string(a->port);
            auto t = tcpTest->createTest(a->host, p);
            t->run([t, a]() {
                       // on ok
                       if (a->isOffline) {
                           a->lastConnectFailed = false;
                       }
                       a->lastOnlineTime = UpstreamTimePointNow();
                       a->isOffline = false;
                   },
                   [t, a](std::string reason) {
                       boost::ignore_unused(reason);
                       // ok error
                       a->isOffline = true;
                   });
        }
    }
}

void UpstreamPool::do_tcpCheckerOne_impl(UpstreamServerRef a) {
    auto p = std::to_string(a->port);
    auto t = tcpTest->createTest(a->host, p);
    t->run([t, a]() {
               // on ok
               if (a->isOffline) {
                   a->lastConnectFailed = false;
               }
               a->lastOnlineTime = UpstreamTimePointNow();
               a->isOffline = false;
           },
           [t, a](std::string reason) {
               boost::ignore_unused(reason);
               // ok error
               a->isOffline = true;
           });
}

void UpstreamPool::do_AdditionTimer() {
    auto c = [this, self = shared_from_this()](const boost::system::error_code &e) {
        if (e) {
            return;
        }
//        std::cout << "do_AdditionTimer()" << std::endl;

        bool isAllDown = std::all_of(
                _pool.begin(),
                _pool.end(),
                [this, self = shared_from_this()](const decltype(_pool)::value_type &a) {
                    return !checkServer(a);
                }
        );
        if (isAllDown) {
            if ((UpstreamTimePointNow() - lastConnectComeTime) <= _configLoader->config.sleepTime) {
                do_AdditionTimer_impl();
            }
        }

        additionTimer->expires_at(additionTimer->expiry() + _configLoader->config.additionCheckPeriod);
        do_AdditionTimer();
    };
    additionTimer->async_wait(c);
}

void UpstreamPool::do_AdditionTimer_impl() {
    bool old = false;
    if (!isAdditionTimerRunning.compare_exchange_strong(old, true)) {
        return;
    }
    std::cout << "do_AdditionTimer_impl()" << std::endl;
    auto ct = std::make_shared<boost::asio::steady_timer>(ex, _configLoader->config.additionCheckPeriod * 3);
    ct->async_wait([this, self = shared_from_this(), ct](const boost::system::error_code &e) {
        isAdditionTimerRunning.store(false);
    });

    do_tcpCheckerTimer_impl();
    do_connectCheckerTimer_impl();
}

void UpstreamPool::do_tcpCheckerTimer() {
    auto c = [this, self = shared_from_this()](const boost::system::error_code &e) {
        if (e) {
            return;
        }
//        std::cout << "do_tcpCheckerTimer()" << std::endl;
//        std::cout << print() << std::endl;

        if ((UpstreamTimePointNow() - lastConnectComeTime) <= _configLoader->config.sleepTime) {
            do_tcpCheckerTimer_impl();
        }

        tcpCheckerTimer->expires_at(tcpCheckerTimer->expiry() + _configLoader->config.tcpCheckPeriod);
        do_tcpCheckerTimer();
    };
    tcpCheckerTimer->async_wait(c);
}

void UpstreamPool::do_connectCheckerTimer_impl() {
    for (const auto &a: _pool) {
        if (!a->isManualDisable) {
            auto t = connectTestHttps->createTest(
                    a->host,
                    std::to_string(a->port),
                    _configLoader->config.testRemoteHost,
                    _configLoader->config.testRemotePort,
                    R"(\)"
            );
            t->run([t, a](ConnectTestHttpsSession::SuccessfulInfo info) {
                       // on ok
                       // std::cout << "SuccessfulInfo:" << info << std::endl;
                       a->lastConnectTime = UpstreamTimePointNow();
                       a->lastConnectFailed = false;

                       std::stringstream ss;
                       ss << "status_code:" << static_cast<int>(info.base().result());
                       a->lastConnectCheckResult = ss.str();
                   },
                   [t, a](std::string reason) {
                       boost::ignore_unused(reason);
                       // ok error
                       a->lastConnectFailed = true;
                   });
        }
    }
}

void UpstreamPool::do_connectCheckerOne_impl(UpstreamServerRef a) {
    auto t = connectTestHttps->createTest(
            a->host,
            std::to_string(a->port),
            _configLoader->config.testRemoteHost,
            _configLoader->config.testRemotePort,
            R"(\)"
    );
    t->run([t, a](ConnectTestHttpsSession::SuccessfulInfo info) {
               // on ok
               // std::cout << "SuccessfulInfo:" << info << std::endl;
               a->lastConnectTime = UpstreamTimePointNow();
               a->lastConnectFailed = false;

               std::stringstream ss;
               ss << "status_code:" << static_cast<int>(info.base().result());
               a->lastConnectCheckResult = ss.str();
           },
           [t, a](std::string reason) {
               boost::ignore_unused(reason);
               // ok error
               a->lastConnectFailed = true;
           });
}

void UpstreamPool::do_connectCheckerTimer() {
    auto c = [this, self = shared_from_this()](const boost::system::error_code &e) {
        if (e) {
            return;
        }
//        std::cout << "do_connectCheckerTimer()" << std::endl;

        if ((UpstreamTimePointNow() - lastConnectComeTime) <= _configLoader->config.sleepTime) {
            do_connectCheckerTimer_impl();
        }

        connectCheckerTimer->expires_at(tcpCheckerTimer->expiry() + _configLoader->config.connectCheckPeriod);
        do_connectCheckerTimer();
    };
    connectCheckerTimer->async_wait(c);
}

void UpstreamPool::forceCheckNow() {
    if (!forceCheckerTimer.expired()) {
        return;
    }

    auto _forceCheckerTimer = std::make_shared<CheckerTimerType>(ex, ConfigTimeDuration{500});
    do_forceCheckNow(_forceCheckerTimer);
    forceCheckerTimer = _forceCheckerTimer;
}

void UpstreamPool::forceCheckOne(size_t index) {
    if (index >= 0 && index < _pool.size()) {
        auto ref = _pool.at(index);
        boost::asio::post(ex, [this, self = shared_from_this(), ref]() {
            do_tcpCheckerOne_impl(ref);
            do_connectCheckerOne_impl(ref);
        });
    }
}

void UpstreamPool::do_forceCheckNow(std::shared_ptr<CheckerTimerType> _forceCheckerTimer) {
    auto c = [this, self = shared_from_this(), _forceCheckerTimer](const boost::system::error_code &e) {
        if (e) {
            return;
        }
        std::cout << "do_forceCheckNow()" << std::endl;

        do_tcpCheckerTimer_impl();
        do_connectCheckerTimer_impl();

        // now _forceCheckerTimer reset auto
    };
    _forceCheckerTimer->async_wait(c);
}

void UpstreamPool::updateLastConnectComeTime() {
    this->lastConnectComeTime = UpstreamTimePointNow();
}

UpstreamTimePoint UpstreamPool::getLastConnectComeTime() {
    return this->lastConnectComeTime;
}
