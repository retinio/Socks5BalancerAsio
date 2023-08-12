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

#include "TcpRelayStatisticsInfo.h"
#include "TcpRelayServer.h"
#include <chrono>
#include <limits>

void TcpRelayStatisticsInfo::Info::removeExpiredSession() {
    auto it = sessions.begin();
    while (it != sessions.end()) {
        if ((*it).ptr.expired()) {
            it = sessions.erase(it);
        } else {
            ++it;
        }
    }
}

void TcpRelayStatisticsInfo::Info::closeAllSession() {
    auto it = sessions.begin();
    while (it != sessions.end()) {
        auto p = (*it).ptr.lock();
        if (p) {
            p->forceClose();
            ++it;
        } else {
            it = sessions.erase(it);
        }
    }
}

void TcpRelayStatisticsInfo::Info::calcByte() {
    size_t newByteUp = byteUp.load();
    size_t newByteDown = byteDown.load();
    byteUpChange = newByteUp - byteUpLast;
    byteDownChange = newByteDown - byteDownLast;
    byteUpLast = newByteUp;
    byteDownLast = newByteDown;
    if (byteUpChange > byteUpChangeMax) {
        byteUpChangeMax = byteUpChange;
    }
    if (byteDownChange > byteDownChangeMax) {
        byteDownChangeMax = byteDownChange;
    }
}

void TcpRelayStatisticsInfo::Info::connectCountAdd() {
    ++connectCount;
}

void TcpRelayStatisticsInfo::Info::connectCountSub() {
    --connectCount;
}

size_t TcpRelayStatisticsInfo::Info::calcSessionsNumber() {
    removeExpiredSession();
    return sessions.size();
}

TcpRelayStatisticsInfo::SessionInfo::SessionInfo(const std::shared_ptr<TcpRelaySession> &s) :
        upstreamIndex(s ? s->getNowServer()->index : std::numeric_limits<decltype(upstreamIndex)>::max()),
        clientEndpointAddrString(s ? s->getClientEndpointAddrString() : ""),
        listenEndpointAddrString(s ? s->getListenEndpointAddrString() : ""),
        rawPtr(s ? s.get() : nullptr),
        ptr(s ? s : nullptr),
        startTime(duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()
        ) {
    BOOST_ASSERT(s);
    updateTargetInfo(s);
}

TcpRelayStatisticsInfo::SessionInfo::SessionInfo(const std::weak_ptr<TcpRelaySession> &s) : SessionInfo(s.lock()) {
}

void TcpRelayStatisticsInfo::SessionInfo::updateTargetInfo(const std::shared_ptr<TcpRelaySession> &s) {
    if (const auto &p = s) {
        auto pp = p->getTargetEndpointAddr();
        host = pp.first;
        post = pp.second;
        targetEndpointAddrString = p->getTargetEndpointAddrString();
    }
}

void TcpRelayStatisticsInfo::addSession(size_t index, const std::weak_ptr<TcpRelaySession> &s) {
    if (upstreamIndex.find(index) == upstreamIndex.end()) {
        upstreamIndex.try_emplace(index, std::make_shared<Info>());
    }
    auto n = upstreamIndex.at(index);
    n->sessions.emplace_back(s);
    if (auto ptr = s.lock()) {
        if (auto ns = ptr->getNowServer()) {
            n->lastUseUpstreamIndex = ns->index;
        }
    }
}

void TcpRelayStatisticsInfo::addSessionClient(const std::string &addr, const std::weak_ptr<TcpRelaySession> &s) {
    if (clientIndex.find(addr) == clientIndex.end()) {
        clientIndex.try_emplace(addr, std::make_shared<Info>());
    }
    auto n = clientIndex.at(addr);
    n->sessions.emplace_back(s);
    if (auto ptr = s.lock()) {
        if (auto ns = ptr->getNowServer()) {
            n->lastUseUpstreamIndex = ns->index;
        }
    }
}

void TcpRelayStatisticsInfo::addSessionListen(const std::string &addr, const std::weak_ptr<TcpRelaySession> &s) {
    if (listenIndex.find(addr) == listenIndex.end()) {
        listenIndex.try_emplace(addr, std::make_shared<Info>());
    }
    auto n = listenIndex.at(addr);
    n->sessions.emplace_back(s);
    if (auto ptr = s.lock()) {
        if (auto ns = ptr->getNowServer()) {
            n->lastUseUpstreamIndex = ns->index;
        }
    }
}

void TcpRelayStatisticsInfo::updateSessionInfo(std::shared_ptr<TcpRelaySession> s) {
    BOOST_ASSERT(s);
    if (s) {
        {
            auto ui = upstreamIndex.find(s->getNowServer()->index);
            if (ui == upstreamIndex.end()) {
                auto &kp = ui->second->sessions.get<SessionInfo::ListenClientAddrPair>();
                auto it = kp.find(std::make_tuple(
                        s->getClientEndpointAddrString(),
                        s->getListenEndpointAddrString()
                ));
                if (it != kp.end()) {
                    kp.modify(it, [&](SessionInfo &a) {
                        a.updateTargetInfo(s);
                    });
                }
            }
        }
        {
            auto ci = clientIndex.find(s->getClientEndpointAddrString());
            if (ci == clientIndex.end()) {
                auto &kp = ci->second->sessions.get<SessionInfo::ListenClientAddrPair>();
                auto it = kp.find(std::make_tuple(
                        s->getClientEndpointAddrString(),
                        s->getListenEndpointAddrString()
                ));
                if (it != kp.end()) {
                    kp.modify(it, [&](SessionInfo &a) {
                        a.updateTargetInfo(s);
                    });
                }
            }
        }
        {
            auto li = listenIndex.find(s->getListenEndpointAddrString());
            if (li == listenIndex.end()) {
                auto &kp = li->second->sessions.get<SessionInfo::ListenClientAddrPair>();
                auto it = kp.find(std::make_tuple(
                        s->getClientEndpointAddrString(),
                        s->getListenEndpointAddrString()
                ));
                if (it != kp.end()) {
                    kp.modify(it, [&](SessionInfo &a) {
                        a.updateTargetInfo(s);
                    });
                }
            }
        }
    }
}

std::shared_ptr<TcpRelayStatisticsInfo::Info> TcpRelayStatisticsInfo::getInfo(size_t index) {
    auto n = upstreamIndex.find(index);
    if (n != upstreamIndex.end()) {
        return n->second;
    } else {
        return {};
    }
}

std::shared_ptr<TcpRelayStatisticsInfo::Info> TcpRelayStatisticsInfo::getInfoClient(const std::string &addr) {
    auto n = clientIndex.find(addr);
    if (n != clientIndex.end()) {
        return n->second;
    } else {
        return {};
    }
}

std::shared_ptr<TcpRelayStatisticsInfo::Info> TcpRelayStatisticsInfo::getInfoListen(const std::string &addr) {
    auto n = listenIndex.find(addr);
    if (n != listenIndex.end()) {
        return n->second;
    } else {
        return {};
    }
}

void TcpRelayStatisticsInfo::removeExpiredSession(size_t index) {
    auto p = getInfo(index);
    if (p) {
        p->removeExpiredSession();
    }
}

void TcpRelayStatisticsInfo::removeExpiredSessionClient(const std::string &addr) {
    auto p = getInfoClient(addr);
    if (p) {
        p->removeExpiredSession();
    }
}

void TcpRelayStatisticsInfo::removeExpiredSessionListen(const std::string &addr) {
    auto p = getInfoListen(addr);
    if (p) {
        p->removeExpiredSession();
    }
}

void TcpRelayStatisticsInfo::addByteUp(size_t index, size_t b) {
    auto p = getInfo(index);
    if (p) {
        p->byteUp += b;
    }
}

void TcpRelayStatisticsInfo::addByteUpClient(const std::string &addr, size_t b) {
    auto p = getInfoClient(addr);
    if (p) {
        p->byteUp += b;
    }
}

void TcpRelayStatisticsInfo::addByteUpListen(const std::string &addr, size_t b) {
    auto p = getInfoListen(addr);
    if (p) {
        p->byteUp += b;
    }
}

void TcpRelayStatisticsInfo::addByteDown(size_t index, size_t b) {
    auto p = getInfo(index);
    if (p) {
        p->byteDown += b;
    }
}

void TcpRelayStatisticsInfo::addByteDownClient(const std::string &addr, size_t b) {
    auto p = getInfoClient(addr);
    if (p) {
        p->byteDown += b;
    }
}

void TcpRelayStatisticsInfo::addByteDownListen(const std::string &addr, size_t b) {
    auto p = getInfoListen(addr);
    if (p) {
        p->byteDown += b;
    }
}

void TcpRelayStatisticsInfo::calcByteAll() {
    for (auto &a: upstreamIndex) {
        a.second->calcByte();
    }
    for (auto &a: clientIndex) {
        a.second->calcByte();
    }
    for (auto &a: listenIndex) {
        a.second->calcByte();
    }
}

void TcpRelayStatisticsInfo::removeExpiredSessionAll() {
    for (auto &a: upstreamIndex) {
        a.second->removeExpiredSession();
    }
    for (auto &a: clientIndex) {
        a.second->removeExpiredSession();
    }
    for (auto &a: listenIndex) {
        a.second->removeExpiredSession();
    }
}

void TcpRelayStatisticsInfo::closeAllSession(size_t index) {
    auto p = getInfo(index);
    if (p) {
        p->closeAllSession();
    }
}

void TcpRelayStatisticsInfo::closeAllSessionClient(const std::string &addr) {
    auto p = getInfoClient(addr);
    if (p) {
        p->closeAllSession();
    }
}

void TcpRelayStatisticsInfo::closeAllSessionListen(const std::string &addr) {
    auto p = getInfoListen(addr);
    if (p) {
        p->closeAllSession();
    }
}

void TcpRelayStatisticsInfo::connectCountAdd(size_t index) {
    auto p = getInfo(index);
    if (p) {
        p->connectCountAdd();
    }
}

void TcpRelayStatisticsInfo::connectCountAddClient(const std::string &addr) {
    auto p = getInfoClient(addr);
    if (p) {
        p->connectCountAdd();
    }
}

void TcpRelayStatisticsInfo::connectCountAddListen(const std::string &addr) {
    auto p = getInfoListen(addr);
    if (p) {
        p->connectCountAdd();
    }
}

void TcpRelayStatisticsInfo::connectCountSub(size_t index) {
    auto p = getInfo(index);
    if (p) {
        p->connectCountSub();
    }
}

void TcpRelayStatisticsInfo::connectCountSubClient(const std::string &addr) {
    auto p = getInfoClient(addr);
    if (p) {
        p->connectCountSub();
    }
}

void TcpRelayStatisticsInfo::connectCountSubListen(const std::string &addr) {
    auto p = getInfoListen(addr);
    if (p) {
        p->connectCountSub();
    }
}

std::map<size_t, std::shared_ptr<TcpRelayStatisticsInfo::Info>> &TcpRelayStatisticsInfo::getUpstreamIndex() {
    return upstreamIndex;
}

std::map<std::string, std::shared_ptr<TcpRelayStatisticsInfo::Info>> &TcpRelayStatisticsInfo::getClientIndex() {
    return clientIndex;
}

std::map<std::string, std::shared_ptr<TcpRelayStatisticsInfo::Info>> &TcpRelayStatisticsInfo::getListenIndex() {
    return listenIndex;
}
