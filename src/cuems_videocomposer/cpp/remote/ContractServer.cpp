/*
 * SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "ContractServer.h"
#include "../utils/Logger.h"

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/reqrep0/rep.h>
#include <nng/protocol/reqrep0/req.h>

#include <unistd.h>

namespace videocomposer {

struct ContractWorker {
    enum class State { Init, Recv, Send };
    State state = State::Init;
    nng_aio* aio = nullptr;
    nng_ctx ctx = NNG_CTX_INITIALIZER;
    ContractServer* owner = nullptr;
    int sendTimeoutMs = 1000;
    // The reply handed to nng_ctx_send and not yet confirmed by the callback.
    // If the server stops mid-send, nng cancels without calling back and the
    // message is still ours to free (stop()).
    nng_msg* pendingSend = nullptr;

    static void callback(void* arg);
};

struct ContractServer::Impl {
    nng_socket rep = NNG_SOCKET_INITIALIZER;
    nng_socket pub = NNG_SOCKET_INITIALIZER;
    bool repOpen = false;
    bool pubOpen = false;
    std::vector<std::unique_ptr<ContractWorker>> workers;
};

namespace {
// True when an ipc:// path already has a live listener. Checked BEFORE creating
// our listener: nng unlinks the socket file when a listener that failed to bind
// is closed, which would silently cut a running videocomposer off its clients.
bool ipcInUse(const std::string& url) {
    if (url.rfind("ipc://", 0) != 0) {
        return false;
    }
    if (::access(url.substr(6).c_str(), F_OK) != 0) {
        return false;
    }
    nng_socket probe;
    if (nng_req0_open(&probe) != 0) {
        return false;
    }
    const bool live = nng_dial(probe, url.c_str(), nullptr, 0) == 0;
    nng_close(probe);
    return live;
}

bool listen(nng_socket sock, const std::string& url, int permissions) {
    if (ipcInUse(url)) {
        LOG_WARNING << "ContractServer: " << url << " is already served (another videocomposer "
                    << "instance?) -- contract disabled in this process";
        return false;
    }
    nng_listener listener;
    int rv = nng_listener_create(&listener, sock, url.c_str());
    if (rv != 0) {
        LOG_WARNING << "ContractServer: listener_create " << url << ": " << nng_strerror(rv);
        return false;
    }
    if (url.rfind("ipc://", 0) == 0) {
        // Permissions must be set before the listener starts.
        rv = nng_listener_set_int(listener, NNG_OPT_IPC_PERMISSIONS, permissions);
        if (rv != 0) {
            LOG_WARNING << "ContractServer: ipc permissions on " << url << ": " << nng_strerror(rv);
        }
    }
    rv = nng_listener_start(listener, 0);
    if (rv != 0) {
        LOG_WARNING << "ContractServer: cannot listen on " << url << ": " << nng_strerror(rv)
                    << " (another videocomposer instance?) -- contract disabled";
        return false;
    }
    return true;
}
}  // namespace

void ContractWorker::callback(void* arg) {
    auto* w = static_cast<ContractWorker*>(arg);
    switch (w->state) {
    case State::Init:
        w->state = State::Recv;
        nng_ctx_recv(w->ctx, w->aio);
        break;
    case State::Recv: {
        const int rv = nng_aio_result(w->aio);
        if (rv == NNG_ECLOSED || rv == NNG_ECANCELED) {
            return;
        }
        if (rv != 0) {
            nng_ctx_recv(w->ctx, w->aio);
            return;
        }
        nng_msg* msg = nng_aio_get_msg(w->aio);
        const std::string request(static_cast<const char*>(nng_msg_body(msg)), nng_msg_len(msg));
        const std::string reply = w->owner->handle(request);
        nng_msg_clear(msg);
        if (nng_msg_append(msg, reply.data(), reply.size()) != 0) {
            nng_msg_free(msg);
            nng_ctx_recv(w->ctx, w->aio);
            return;
        }
        nng_aio_set_msg(w->aio, msg);
        nng_aio_set_timeout(w->aio, w->sendTimeoutMs);
        w->state = State::Send;
        w->pendingSend = msg;
        nng_ctx_send(w->ctx, w->aio);
        break;
    }
    case State::Send: {
        w->pendingSend = nullptr;
        const int rv = nng_aio_result(w->aio);
        if (rv != 0) {
            nng_msg_free(nng_aio_get_msg(w->aio));
            if (rv == NNG_ECLOSED || rv == NNG_ECANCELED) {
                return;
            }
        }
        nng_aio_set_timeout(w->aio, NNG_DURATION_INFINITE);
        w->state = State::Recv;
        nng_ctx_recv(w->ctx, w->aio);
        break;
    }
    }
}

ContractServer::ContractServer(ContractSnapshotHolder& holder, ContractProtocol::Identity identity)
    : ContractServer(holder, std::move(identity), Config{}) {}

ContractServer::ContractServer(ContractSnapshotHolder& holder, ContractProtocol::Identity identity,
                               Config config)
    : impl_(std::make_unique<Impl>()), holder_(holder), identity_(std::move(identity)),
      config_(std::move(config)) {}

ContractServer::~ContractServer() {
    stop();
}

std::string ContractServer::handle(const std::string& request) {
    uint64_t generation = 0;
    const ContractSnapshot snapshot = holder_.get(&generation);
    std::string hardware;
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        if (generation != cachedGeneration_) {
            cachedHardware_ = ContractProtocol::hardwareJson(snapshot);
            cachedGeneration_ = generation;
        }
        hardware = cachedHardware_;
    }
    return ContractProtocol::handle(request, snapshot, identity_, &hardware);
}

bool ContractServer::start() {
    if (running_.load()) {
        return true;
    }
    int rv = nng_rep0_open(&impl_->rep);
    if (rv != 0) {
        LOG_WARNING << "ContractServer: rep0_open: " << nng_strerror(rv) << " -- contract disabled";
        return false;
    }
    impl_->repOpen = true;
    nng_socket_set_size(impl_->rep, NNG_OPT_RECVMAXSZ, config_.recvMaxBytes);
    if (!listen(impl_->rep, config_.repUrl, config_.ipcPermissions)) {
        stop();
        return false;
    }
    rv = nng_pub0_open(&impl_->pub);
    if (rv != 0) {
        LOG_WARNING << "ContractServer: pub0_open: " << nng_strerror(rv) << " -- contract disabled";
        stop();
        return false;
    }
    impl_->pubOpen = true;
    if (!listen(impl_->pub, config_.pubUrl, config_.ipcPermissions)) {
        stop();
        return false;
    }

    for (int i = 0; i < config_.workers; ++i) {
        auto w = std::make_unique<ContractWorker>();
        w->owner = this;
        w->sendTimeoutMs = config_.sendTimeoutMs;
        if (nng_aio_alloc(&w->aio, &ContractWorker::callback, w.get()) != 0 ||
            nng_ctx_open(&w->ctx, impl_->rep) != 0) {
            LOG_WARNING << "ContractServer: cannot allocate a worker -- contract disabled";
            if (w->aio) {
                nng_aio_free(w->aio);
            }
            stop();
            return false;
        }
        impl_->workers.push_back(std::move(w));
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stopping_ = false;
    }
    pubThread_ = std::thread(&ContractServer::pubLoop, this);
    running_.store(true);
    for (auto& w : impl_->workers) {
        ContractWorker::callback(w.get());  // Init -> first recv
    }
    LOG_INFO << "ContractServer: serving " << config_.repUrl << " (events " << config_.pubUrl
             << "), contract v" << ContractProtocol::kVersion;
    return true;
}

void ContractServer::stop() {
    // Idempotent: also reached from the destructor and from a failed start().
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stopping_ = true;
    }
    queueCv_.notify_all();
    if (pubThread_.joinable()) {
        pubThread_.join();
    }
    // Stop the aios first: nng_aio_stop cancels and waits for a running
    // callback, so no worker touches a context or 'this' afterwards.
    for (auto& w : impl_->workers) {
        if (w->aio) {
            nng_aio_stop(w->aio);
        }
    }
    for (auto& w : impl_->workers) {
        if (w->pendingSend) {
            nng_msg_free(w->pendingSend);  // cancelled before the callback ran
            w->pendingSend = nullptr;
        }
        nng_ctx_close(w->ctx);
        if (w->aio) {
            nng_aio_free(w->aio);
            w->aio = nullptr;
        }
    }
    impl_->workers.clear();
    if (impl_->repOpen) {
        nng_close(impl_->rep);
        impl_->repOpen = false;
    }
    if (impl_->pubOpen) {
        nng_close(impl_->pub);
        impl_->pubOpen = false;
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.clear();
    }
    running_.store(false);
}

void ContractServer::publish(std::string event) {
    if (!running_.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (queue_.size() >= config_.pubQueueMax) {
            queue_.pop_front();
            dropped_.fetch_add(1);
        }
        queue_.push_back(std::move(event));
    }
    queueCv_.notify_one();
}

void ContractServer::pubLoop() {
    for (;;) {
        std::string event;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            event = std::move(queue_.front());
            queue_.pop_front();
        }
        // pub0 never blocks a sender: slow subscribers lose messages (the
        // contract tells clients to re-read state and watch 'seq').
        nng_send(impl_->pub, event.data(), event.size(), 0);
    }
}

} // namespace videocomposer
