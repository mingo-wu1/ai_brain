﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_RTPSESSION_H
#define ZLMEDIAKIT_RTPSESSION_H

#if defined(ENABLE_RTPPROXY)
#include "Network/TcpSession.h"
#include "RtpSplitter.h"
#include "RtpProcess.h"
#include "Util/TimeTicker.h"
using namespace toolkit;

namespace mediakit{

class RtpSession : public Session , public RtpSplitter , public MediaSourceEvent{
public:
    static const string kStreamID;
    static const string kIsUDP;

    RtpSession(const Socket::Ptr &sock);
    ~RtpSession() override;
    void onRecv(const Buffer::Ptr &) override;
    void onError(const SockException &err) override;
    void onManager() override;
    void attachServer(const Server &server) override;

protected:
    // 通知其停止推流
    bool close(MediaSource &sender,bool force) override;
    // 观看总人数
    int totalReaderCount(MediaSource &sender) override;
    // 收到rtp回调
    void onRtpPacket(const char *data, size_t len) override;

    const char *onSearchPacketTail(const char *data, size_t len) override;

private:
    bool _is_udp = false;
    bool _search_rtp = false;
    bool _search_rtp_finished = false;
    uint32_t _ssrc = 0;
    Ticker _ticker;
    string _stream_id;
    struct sockaddr _addr;
    RtpProcess::Ptr _process;
    std::shared_ptr<ObjectStatistic<TcpSession> > _statistic_tcp;
    std::shared_ptr<ObjectStatistic<UdpSession> > _statistic_udp;
};

}//namespace mediakit
#endif//defined(ENABLE_RTPPROXY)
#endif //ZLMEDIAKIT_RTPSESSION_H
