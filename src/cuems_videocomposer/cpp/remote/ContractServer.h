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

#ifndef VIDEOCOMPOSER_CONTRACTSERVER_H
#define VIDEOCOMPOSER_CONTRACTSERVER_H

#include "ContractProtocol.h"
#include "ContractTypes.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace videocomposer {

/**
 * ContractServer - the transport of the hardware-configuration contract
 * (docs/contract-v1.md): NNG Rep for requests, Pub for events, JSON payloads.
 *
 * Nothing here runs on the render thread. Requests are served by a small pool
 * of asynchronous Rep contexts (one stalled client cannot block another);
 * events go through a bounded queue (drop-oldest) drained by one thread, so
 * publish() never blocks its caller. start() never throws: on any failure it
 * logs one WARNING and the videocomposer runs without the contract. stop() is
 * idempotent and safe from a destructor.
 */
class ContractServer {
public:
    struct Config {
        std::string repUrl = "ipc:///tmp/cuems-videocomposer.ipc";
        std::string pubUrl = "ipc:///tmp/cuems-videocomposer-events.ipc";
        int ipcPermissions = 0660;
        size_t recvMaxBytes = 64 * 1024;
        int sendTimeoutMs = 1000;
        int workers = 2;
        size_t pubQueueMax = 256;
    };

    ContractServer(ContractSnapshotHolder& holder, ContractProtocol::Identity identity);
    ContractServer(ContractSnapshotHolder& holder, ContractProtocol::Identity identity, Config config);
    ~ContractServer();

    ContractServer(const ContractServer&) = delete;
    ContractServer& operator=(const ContractServer&) = delete;

    bool start();
    void stop();
    bool running() const { return running_.load(); }

    /// Queue an event document (topic-prefixed, see ContractProtocol::event*).
    void publish(std::string event);
    uint64_t nextSeq() { return ++seq_; }
    uint64_t droppedEvents() const { return dropped_.load(); }
    int64_t pid() const { return identity_.pid; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ContractSnapshotHolder& holder_;
    ContractProtocol::Identity identity_;
    Config config_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> dropped_{0};

    std::thread pubThread_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<std::string> queue_;
    bool stopping_ = false;

    // get_hardware result serialised once per snapshot generation.
    std::mutex cacheMutex_;
    uint64_t cachedGeneration_ = ~0ull;
    std::string cachedHardware_;

    void pubLoop();
    friend struct ContractWorker;
    std::string handle(const std::string& request);
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_CONTRACTSERVER_H
