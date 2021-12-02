﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <atomic>
#include <iomanip>
#include "Common/config.h"
#include "UDPServer.h"
#include "RtspSession.h"
#include "Util/MD5.h"
#include "Util/base64.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

/**
 * rtsp协议有多种方式传输rtp数据包，目前已支持包括以下4种
 * 1: rtp over udp ,这种方式是rtp通过单独的udp端口传输
 * 2: rtp over udp_multicast,这种方式是rtp通过共享udp组播端口传输
 * 3: rtp over tcp,这种方式是通过rtsp信令tcp通道完成传输
 * 4: rtp over http，下面着重讲解：rtp over http
 *
 * rtp over http 是把rtsp协议伪装成http协议以达到穿透防火墙的目的，
 * 此时播放器会发送两次http请求至rtsp服务器，第一次是http get请求，
 * 第二次是http post请求。
 *
 * 这两次请求通过http请求头中的x-sessioncookie键完成绑定
 *
 * 第一次http get请求用于接收rtp、rtcp和rtsp回复，后续该链接不再发送其他请求
 * 第二次http post请求用于发送rtsp请求，rtsp握手结束后可能会断开连接，此时我们还要维持rtp发送
 * 需要指出的是http post请求中的content负载就是base64编码后的rtsp请求包，
 * 播放器会把rtsp请求伪装成http content负载发送至rtsp服务器，然后rtsp服务器又把回复发送给第一次http get请求的tcp链接
 * 这样，对防火墙而言，本次rtsp会话就是两次http请求，防火墙就会放行数据
 *
 * zlmediakit在处理rtsp over http的请求时，会把http poster中的content数据base64解码后转发给http getter处理
 */


//rtsp over http 情况下get请求实例，在请求实例用于接收rtp数据包
static unordered_map<string, weak_ptr<RtspSession> > g_mapGetter;
//对g_mapGetter上锁保护
static recursive_mutex g_mtxGetter;

RtspSession::RtspSession(const Socket::Ptr &sock) : TcpSession(sock) {
    DebugP(this);
    GET_CONFIG(uint32_t,keep_alive_sec,Rtsp::kKeepAliveSecond);
    sock->setSendTimeOutSecond(keep_alive_sec);
}

RtspSession::~RtspSession() {
    DebugP(this);
}

void RtspSession::onError(const SockException &err) {
    bool isPlayer = !_push_src;
    uint64_t duration = _alive_ticker.createdTime() / 1000;
    WarnP(this) << (isPlayer ? "RTSP播放器(" : "RTSP推流器(")
                << _media_info._vhost << "/"
                << _media_info._app << "/"
                << _media_info._streamid
                << ")断开:" << err.what()
                << ",耗时(s):" << duration;

    if (_rtp_type == Rtsp::RTP_MULTICAST) {
        //取消UDP端口监听
        UDPServer::Instance().stopListenPeer(get_peer_ip().data(), this);
    }

    if (_http_x_sessioncookie.size() != 0) {
        //移除http getter的弱引用记录
        lock_guard<recursive_mutex> lock(g_mtxGetter);
        g_mapGetter.erase(_http_x_sessioncookie);
    }

    //流量统计事件广播
    GET_CONFIG(uint32_t,iFlowThreshold,General::kFlowThreshold);
    if(_bytes_usage >= iFlowThreshold * 1024){
        NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastFlowReport, _media_info, _bytes_usage, duration, isPlayer, static_cast<SockInfo &>(*this));
    }

}

void RtspSession::onManager() {
    GET_CONFIG(uint32_t,handshake_sec,Rtsp::kHandshakeSecond);
    GET_CONFIG(uint32_t,keep_alive_sec,Rtsp::kKeepAliveSecond);

    if (_alive_ticker.createdTime() > handshake_sec * 1000) {
        if (_sessionid.size() == 0) {
            shutdown(SockException(Err_timeout,"illegal connection"));
            return;
        }
    }

    if (_push_src && _alive_ticker.elapsedTime() > keep_alive_sec * 1000) {
        //推流超时
        shutdown(SockException(Err_timeout, "pusher session timeout"));
        return;
    }

    if (!_push_src && _rtp_type == Rtsp::RTP_UDP && _alive_ticker.elapsedTime() > keep_alive_sec * 4000) {
        //rtp over udp播放器超时
        shutdown(SockException(Err_timeout, "rtp over udp player timeout"));
    }
}

void RtspSession::onRecv(const Buffer::Ptr &buf) {
    _alive_ticker.resetTime();
    _bytes_usage += buf->size();
    if (_on_recv) {
        //http poster的请求数据转发给http getter处理
        _on_recv(buf);
    } else {
        input(buf->data(), buf->size());
    }
}

void RtspSession::onWholeRtspPacket(Parser &parser) {
    string method = parser.Method(); //提取出请求命令字
    _cseq = atoi(parser["CSeq"].data());
    if (_content_base.empty() && method != "GET") {
        _content_base = parser.Url();
        _media_info.parse(parser.FullUrl());
        _media_info._schema = RTSP_SCHEMA;
    }

    using rtsp_request_handler = void (RtspSession::*)(const Parser &parser);
    static unordered_map<string, rtsp_request_handler> s_cmd_functions;
    static onceToken token([]() {
        s_cmd_functions.emplace("OPTIONS", &RtspSession::handleReq_Options);
        s_cmd_functions.emplace("DESCRIBE", &RtspSession::handleReq_Describe);
        s_cmd_functions.emplace("ANNOUNCE", &RtspSession::handleReq_ANNOUNCE);
        s_cmd_functions.emplace("RECORD", &RtspSession::handleReq_RECORD);
        s_cmd_functions.emplace("SETUP", &RtspSession::handleReq_Setup);
        s_cmd_functions.emplace("PLAY", &RtspSession::handleReq_Play);
        s_cmd_functions.emplace("PAUSE", &RtspSession::handleReq_Pause);
        s_cmd_functions.emplace("TEARDOWN", &RtspSession::handleReq_Teardown);
        s_cmd_functions.emplace("GET", &RtspSession::handleReq_Get);
        s_cmd_functions.emplace("POST", &RtspSession::handleReq_Post);
        s_cmd_functions.emplace("SET_PARAMETER", &RtspSession::handleReq_SET_PARAMETER);
        s_cmd_functions.emplace("GET_PARAMETER", &RtspSession::handleReq_SET_PARAMETER);
    });

    auto it = s_cmd_functions.find(method);
    if (it == s_cmd_functions.end()) {
        sendRtspResponse("403 Forbidden");
        throw SockException(Err_shutdown, StrPrinter << "403 Forbidden:" << method);
    }

    (this->*(it->second))(parser);
    parser.Clear();
}

void RtspSession::onRtpPacket(const char *data, size_t len) {
    uint8_t interleaved = data[1];
    if (interleaved % 2 == 0) {
        if (!_push_src) {
            return;
        }
        auto track_idx = getTrackIndexByInterleaved(interleaved);
        handleOneRtp(track_idx, _sdp_track[track_idx]->_type, _sdp_track[track_idx]->_samplerate, (uint8_t *) data + RtpPacket::kRtpTcpHeaderSize, len - RtpPacket::kRtpTcpHeaderSize);
    } else {
        auto track_idx = getTrackIndexByInterleaved(interleaved - 1);
        onRtcpPacket(track_idx, _sdp_track[track_idx], data + RtpPacket::kRtpTcpHeaderSize, len - RtpPacket::kRtpTcpHeaderSize);
    }
}

void RtspSession::onRtcpPacket(int track_idx, SdpTrack::Ptr &track, const char *data, size_t len){
    auto rtcp_arr = RtcpHeader::loadFromBytes((char *) data, len);
    for (auto &rtcp : rtcp_arr) {
        _rtcp_context[track_idx]->onRtcp(rtcp);
        if ((RtcpType) rtcp->pt == RtcpType::RTCP_SR) {
            auto sr = (RtcpSR *) (rtcp);
            //设置rtp时间戳与ntp时间戳的对应关系
            setNtpStamp(track_idx, sr->rtpts, sr->getNtpUnixStampMS());
        }
    }
}

ssize_t RtspSession::getContentLength(Parser &parser) {
    if(parser.Method() == "POST"){
        //http post请求的content数据部分是base64编码后的rtsp请求信令包
        return remainDataSize();
    }
    return RtspSplitter::getContentLength(parser);
}

void RtspSession::handleReq_Options(const Parser &parser) {
    //支持这些命令
    sendRtspResponse("200 OK",{"Public" , "OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, ANNOUNCE, RECORD, SET_PARAMETER, GET_PARAMETER"});
}

void RtspSession::handleReq_ANNOUNCE(const Parser &parser) {
    auto src = dynamic_pointer_cast<RtspMediaSource>(MediaSource::find(RTSP_SCHEMA,
                                                                       _media_info._vhost,
                                                                       _media_info._app,
                                                                       _media_info._streamid));
    if (src) {
        sendRtspResponse("406 Not Acceptable", {"Content-Type", "text/plain"}, "Already publishing.");
        string err = StrPrinter << "ANNOUNCE:"
                                << "Already publishing:"
                                << _media_info._vhost << " "
                                << _media_info._app << " "
                                << _media_info._streamid << endl;
        throw SockException(Err_shutdown, err);
    }

    auto full_url = parser.FullUrl();
    _content_base = full_url;
    if (end_with(full_url, ".sdp")) {
        //去除.sdp后缀，防止EasyDarwin推流器强制添加.sdp后缀
        full_url = full_url.substr(0, full_url.length() - 4);
        _media_info.parse(full_url);
    }

    if (_media_info._app.empty() || _media_info._streamid.empty()) {
        //推流rtsp url必须最少两级(rtsp://host/app/stream_id)，不允许莫名其妙的推流url
        static constexpr auto err = "rtsp推流url非法,最少确保两级rtsp url";
        sendRtspResponse("403 Forbidden", {"Content-Type", "text/plain"}, err);
        throw SockException(Err_shutdown, StrPrinter << err << ":" << full_url);
    }

    auto onRes = [this, parser, full_url](const string &err, bool enableHls, bool enableMP4){
        bool authSuccess = err.empty();
        if (!authSuccess) {
            sendRtspResponse("401 Unauthorized", {"Content-Type", "text/plain"}, err);
            shutdown(SockException(Err_shutdown, StrPrinter << "401 Unauthorized:" << err));
            return;
        }

        SdpParser sdpParser(parser.Content());
        _sessionid = makeRandStr(12);
        _sdp_track = sdpParser.getAvailableTrack();
        if (_sdp_track.empty()) {
            //sdp无效
            static constexpr auto err = "sdp中无有效track";
            sendRtspResponse("403 Forbidden", {"Content-Type", "text/plain"}, err);
            shutdown(SockException(Err_shutdown, StrPrinter << err << ":" << full_url));
            return;
        }
        _rtcp_context.clear();
        for (auto &track : _sdp_track) {
            _rtcp_context.emplace_back(std::make_shared<RtcpContextForRecv>());
        }
        auto push_src = std::make_shared<RtspMediaSourceImp>(_media_info._vhost, _media_info._app, _media_info._streamid);
        push_src->setListener(dynamic_pointer_cast<MediaSourceEvent>(shared_from_this()));
        push_src->setProtocolTranslation(enableHls, enableMP4);
        push_src->setSdp(parser.Content());
        _push_src = std::move(push_src);
        sendRtspResponse("200 OK");
    };

    weak_ptr<RtspSession> weakSelf = dynamic_pointer_cast<RtspSession>(shared_from_this());
    Broadcast::PublishAuthInvoker invoker = [weakSelf, onRes](const string &err, bool enableHls, bool enableMP4) {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            return;
        }
        strongSelf->async([weakSelf, onRes, err, enableHls, enableMP4]() {
            auto strongSelf = weakSelf.lock();
            if (!strongSelf) {
                return;
            }
            onRes(err, enableHls, enableMP4);
        });
    };

    //rtsp推流需要鉴权
    auto flag = NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastMediaPublish, _media_info, invoker, static_cast<SockInfo &>(*this));
    if (!flag) {
        //该事件无人监听,默认不鉴权
        GET_CONFIG(bool, toHls, General::kPublishToHls);
        GET_CONFIG(bool, toMP4, General::kPublishToMP4);
        onRes("", toHls, toMP4);
    }
}

void RtspSession::handleReq_RECORD(const Parser &parser){
    if (_sdp_track.empty() || parser["Session"] != _sessionid) {
        send_SessionNotFound();
        throw SockException(Err_shutdown, _sdp_track.empty() ? "can not find any availabe track when record" : "session not found when record");
    }

    _StrPrinter rtp_info;
    for (auto &track : _sdp_track) {
        if (track->_inited == false) {
            //还有track没有setup
            shutdown(SockException(Err_shutdown, "track not setuped"));
            return;
        }
        rtp_info << "url=" << track->getControlUrl(_content_base) << ",";
    }
    rtp_info.pop_back();
    sendRtspResponse("200 OK", {"RTP-Info", rtp_info});
    if (_rtp_type == Rtsp::RTP_TCP) {
        //如果是rtsp推流服务器，并且是TCP推流，设置socket flags,，这样能提升接收性能
        setSocketFlags();
    }
}

void RtspSession::emitOnPlay(){
    weak_ptr<RtspSession> weakSelf = dynamic_pointer_cast<RtspSession>(shared_from_this());
    //url鉴权回调
    auto onRes = [weakSelf](const string &err) {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            return;
        }
        if (!err.empty()) {
            //播放url鉴权失败
            strongSelf->sendRtspResponse("401 Unauthorized", {"Content-Type", "text/plain"}, err);
            strongSelf->shutdown(SockException(Err_shutdown, StrPrinter << "401 Unauthorized:" << err));
            return;
        }
        strongSelf->onAuthSuccess();
    };

    Broadcast::AuthInvoker invoker = [weakSelf, onRes](const string &err) {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            return;
        }
        strongSelf->async([onRes, err, weakSelf]() {
            onRes(err);
        });
    };

    //广播通用播放url鉴权事件
    auto flag = _emit_on_play ? false : NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastMediaPlayed, _media_info, invoker, static_cast<SockInfo &>(*this));
    if (!flag) {
        //该事件无人监听,默认不鉴权
        onRes("");
    }
    //已经鉴权过了
    _emit_on_play = true;
}

void RtspSession::handleReq_Describe(const Parser &parser) {
    //该请求中的认证信息
    auto authorization = parser["Authorization"];
    weak_ptr<RtspSession> weakSelf = dynamic_pointer_cast<RtspSession>(shared_from_this());
    //rtsp专属鉴权是否开启事件回调
    onGetRealm invoker = [weakSelf, authorization](const string &realm) {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            //本对象已经销毁
            return;
        }
        //切换到自己的线程然后执行
        strongSelf->async([weakSelf, realm, authorization]() {
            auto strongSelf = weakSelf.lock();
            if (!strongSelf) {
                //本对象已经销毁
                return;
            }
            if (realm.empty()) {
                //无需rtsp专属认证, 那么继续url通用鉴权认证(on_play)
                strongSelf->emitOnPlay();
                return;
            }
            //该流需要rtsp专属认证，开启rtsp专属认证后，将不再触发url通用鉴权认证(on_play)
            strongSelf->_rtsp_realm = realm;
            strongSelf->onAuthUser(realm, authorization);
        });
    };

    if(_rtsp_realm.empty()){
        //广播是否需要rtsp专属认证事件
        if (!NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastOnGetRtspRealm, _media_info, invoker, static_cast<SockInfo &>(*this))) {
            //无人监听此事件，说明无需认证
            invoker("");
        }
    }else{
        invoker(_rtsp_realm);
    }
}

void RtspSession::onAuthSuccess() {
    TraceP(this);
    weak_ptr<RtspSession> weakSelf = dynamic_pointer_cast<RtspSession>(shared_from_this());
    MediaSource::findAsync(_media_info, weakSelf.lock(), [weakSelf](const MediaSource::Ptr &src){
        auto strongSelf = weakSelf.lock();
        if(!strongSelf){
            return;
        }
        auto rtsp_src = dynamic_pointer_cast<RtspMediaSource>(src);
        if (!rtsp_src) {
            //未找到相应的MediaSource
            string err = StrPrinter << "no such stream:" << strongSelf->_media_info._vhost << " " << strongSelf->_media_info._app << " " << strongSelf->_media_info._streamid;
            strongSelf->send_StreamNotFound();
            strongSelf->shutdown(SockException(Err_shutdown,err));
            return;
        }
        //找到了相应的rtsp流
        strongSelf->_sdp_track = SdpParser(rtsp_src->getSdp()).getAvailableTrack();
        if (strongSelf->_sdp_track.empty()) {
            //该流无效
            WarnL << "sdp中无有效track，该流无效:" << rtsp_src->getSdp();
            strongSelf->send_StreamNotFound();
            strongSelf->shutdown(SockException(Err_shutdown,"can not find any available track in sdp"));
            return;
        }
        strongSelf->_rtcp_context.clear();
        for (auto &track : strongSelf->_sdp_track) {
            strongSelf->_rtcp_context.emplace_back(std::make_shared<RtcpContextForSend>());
        }
        strongSelf->_sessionid = makeRandStr(12);
        strongSelf->_play_src = rtsp_src;
        for(auto &track : strongSelf->_sdp_track){
            track->_ssrc = rtsp_src->getSsrc(track->_type);
            track->_seq = rtsp_src->getSeqence(track->_type);
            track->_time_stamp = rtsp_src->getTimeStamp(track->_type);
        }

        strongSelf->sendRtspResponse("200 OK",
                                     {"Content-Base", strongSelf->_content_base + "/",
                                      "x-Accept-Retransmit","our-retransmit",
                                      "x-Accept-Dynamic-Rate","1"
                                     },rtsp_src->getSdp());
    });
}

void RtspSession::onAuthFailed(const string &realm,const string &why,bool close) {
    GET_CONFIG(bool,authBasic,Rtsp::kAuthBasic);
    if (!authBasic) {
        //我们需要客户端优先以md5方式认证
        _auth_nonce = makeRandStr(32);
        sendRtspResponse("401 Unauthorized",
                         {"WWW-Authenticate",
                          StrPrinter << "Digest realm=\"" << realm << "\",nonce=\"" << _auth_nonce << "\"" });
    }else {
        //当然我们也支持base64认证,但是我们不建议这样做
        sendRtspResponse("401 Unauthorized",
                         {"WWW-Authenticate",
                          StrPrinter << "Basic realm=\"" << realm << "\"" });
    }
    if(close){
        shutdown(SockException(Err_shutdown,StrPrinter << "401 Unauthorized:" << why));
    }
}

void RtspSession::onAuthBasic(const string &realm,const string &auth_base64){
    //base64认证
    char user_pwd_buf[512];
    av_base64_decode((uint8_t *) user_pwd_buf, auth_base64.data(), (int)auth_base64.size());
    auto user_pwd_vec = split(user_pwd_buf, ":");
    if (user_pwd_vec.size() < 2) {
        //认证信息格式不合法，回复401 Unauthorized
        onAuthFailed(realm, "can not find user and passwd when basic64 auth");
        return;
    }
    auto user = user_pwd_vec[0];
    auto pwd = user_pwd_vec[1];
    weak_ptr<RtspSession> weakSelf = dynamic_pointer_cast<RtspSession>(shared_from_this());
    onAuth invoker = [pwd, realm, weakSelf](bool encrypted, const string &good_pwd) {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            //本对象已经销毁
            return;
        }
        //切换到自己的线程执行
        strongSelf->async([weakSelf, good_pwd, pwd, realm]() {
            auto strongSelf = weakSelf.lock();
            if (!strongSelf) {
                //本对象已经销毁
                return;
            }
            //base64忽略encrypted参数，上层必须传入明文密码
            if (pwd == good_pwd) {
                //提供的密码且匹配正确
                strongSelf->onAuthSuccess();
                return;
            }
            //密码错误
            strongSelf->onAuthFailed(realm, StrPrinter << "password mismatch when base64 auth:" << pwd << " != " << good_pwd);
        });
    };

    //此时必须提供明文密码
    if (!NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastOnRtspAuth, _media_info, realm, user, true, invoker, static_cast<SockInfo &>(*this))) {
        //表明该流需要认证却没监听请求密码事件，这一般是大意的程序所为，警告之
        WarnP(this) << "请监听kBroadcastOnRtspAuth事件！";
        //但是我们还是忽略认证以便完成播放
        //我们输入的密码是明文
        invoker(false, pwd);
    }
}

void RtspSession::onAuthDigest(const string &realm,const string &auth_md5){
    DebugP(this) << auth_md5;
    auto mapTmp = Parser::parseArgs(auth_md5, ",", "=");
    decltype(mapTmp) map;
    for(auto &pr : mapTmp){
        map[trim(string(pr.first)," \"")] = trim(pr.second," \"");
    }
    //check realm
    if(realm != map["realm"]){
        onAuthFailed(realm,StrPrinter << "realm not mached:" << realm << " != " << map["realm"]);
        return ;
    }
    //check nonce
    auto nonce = map["nonce"];
    if(_auth_nonce != nonce){
        onAuthFailed(realm,StrPrinter << "nonce not mached:" << nonce << " != " << _auth_nonce);
        return ;
    }
    //check username and uri
    auto username = map["username"];
    auto uri = map["uri"];
    auto response = map["response"];
    if(username.empty() || uri.empty() || response.empty()){
        onAuthFailed(realm,StrPrinter << "username/uri/response empty:" << username << "," << uri << "," << response);
        return ;
    }

    auto realInvoker = [this,realm,nonce,uri,username,response](bool ignoreAuth,bool encrypted,const string &good_pwd){
        if(ignoreAuth){
            //忽略认证
            TraceP(this) << "auth ignored";
            onAuthSuccess();
            return;
        }
        /*
        response计算方法如下：
        RTSP客户端应该使用username + password并计算response如下:
        (1)当password为MD5编码,则
            response = md5( password:nonce:md5(public_method:url)  );
        (2)当password为ANSI字符串,则
            response= md5( md5(username:realm:password):nonce:md5(public_method:url) );
         */
        auto encrypted_pwd = good_pwd;
        if(!encrypted){
            //提供的是明文密码
            encrypted_pwd = MD5(username+ ":" + realm + ":" + good_pwd).hexdigest();
        }

        auto good_response = MD5( encrypted_pwd + ":" + nonce + ":" + MD5(string("DESCRIBE") + ":" + uri).hexdigest()).hexdigest();
        if(strcasecmp(good_response.data(),response.data()) == 0){
            //认证成功！md5不区分大小写
            onAuthSuccess();
        }else{
            //认证失败！
            onAuthFailed(realm, StrPrinter << "password mismatch when md5 auth:" << good_response << " != " << response );
        }
    };

    weak_ptr<RtspSession> weakSelf = dynamic_pointer_cast<RtspSession>(shared_from_this());
    onAuth invoker = [realInvoker,weakSelf](bool encrypted,const string &good_pwd){
        auto strongSelf = weakSelf.lock();
        if(!strongSelf){
            return;
        }
        //切换到自己的线程确保realInvoker执行时，this指针有效
        strongSelf->async([realInvoker,weakSelf,encrypted,good_pwd](){
            auto strongSelf = weakSelf.lock();
            if(!strongSelf){
                return;
            }
            realInvoker(false,encrypted,good_pwd);
        });
    };

    //此时可以提供明文或md5加密的密码
    if(!NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastOnRtspAuth, _media_info, realm, username, false, invoker, static_cast<SockInfo &>(*this))){
        //表明该流需要认证却没监听请求密码事件，这一般是大意的程序所为，警告之
        WarnP(this) << "请监听kBroadcastOnRtspAuth事件！";
        //但是我们还是忽略认证以便完成播放
        realInvoker(true,true,"");
    }
}

void RtspSession::onAuthUser(const string &realm,const string &authorization){
    if(authorization.empty()){
        onAuthFailed(realm,"", false);
        return;
    }
    //请求中包含认证信息
    auto authType = FindField(authorization.data(),NULL," ");
    auto authStr = FindField(authorization.data()," ",NULL);
    if(authType.empty() || authStr.empty()){
        //认证信息格式不合法，回复401 Unauthorized
        onAuthFailed(realm,"can not find auth type or auth string");
        return;
    }
    if(authType == "Basic"){
        //base64认证，需要明文密码
        onAuthBasic(realm,authStr);
    }else if(authType == "Digest"){
        //md5认证
        onAuthDigest(realm,authStr);
    }else{
        //其他认证方式？不支持！
        onAuthFailed(realm,StrPrinter << "unsupported auth type:" << authType);
    }
}

void RtspSession::send_StreamNotFound() {
    sendRtspResponse("404 Stream Not Found",{"Connection","Close"});
}

void RtspSession::send_UnsupportedTransport() {
    sendRtspResponse("461 Unsupported Transport",{"Connection","Close"});
}

void RtspSession::send_SessionNotFound() {
    sendRtspResponse("454 Session Not Found",{"Connection","Close"});
}

void RtspSession::handleReq_Setup(const Parser &parser) {
    //处理setup命令，该函数可能进入多次
    int trackIdx = getTrackIndexByControlUrl(parser.FullUrl());
    SdpTrack::Ptr &trackRef = _sdp_track[trackIdx];
    if (trackRef->_inited) {
        //已经初始化过该Track
        throw SockException(Err_shutdown, "can not setup one track twice");
    }
    trackRef->_inited = true; //现在初始化

    if(_rtp_type == Rtsp::RTP_Invalid){
        auto &strTransport = parser["Transport"];
        if(strTransport.find("TCP") != string::npos){
            _rtp_type = Rtsp::RTP_TCP;
        }else if(strTransport.find("multicast") != string::npos){
            _rtp_type = Rtsp::RTP_MULTICAST;
        }else{
            _rtp_type = Rtsp::RTP_UDP;
        }
    }

    //允许接收rtp、rtcp包
    RtspSplitter::enableRecvRtp(_rtp_type == Rtsp::RTP_TCP);

    switch (_rtp_type) {
    case Rtsp::RTP_TCP: {
        if(_push_src){
            //rtsp推流时，interleaved由推流者决定
            auto key_values =  Parser::parseArgs(parser["Transport"],";","=");
            int interleaved_rtp = -1 , interleaved_rtcp = -1;
            if(2 == sscanf(key_values["interleaved"].data(),"%d-%d",&interleaved_rtp,&interleaved_rtcp)){
                trackRef->_interleaved = interleaved_rtp;
            }else{
                throw SockException(Err_shutdown, "can not find interleaved when setup of rtp over tcp");
            }
        }else{
            //rtsp播放时，由于数据共享分发，所以interleaved必须由服务器决定
            trackRef->_interleaved = 2 * trackRef->_type;
        }
        sendRtspResponse("200 OK",
                         {"Transport", StrPrinter << "RTP/AVP/TCP;unicast;"
                                                  << "interleaved=" << (int) trackRef->_interleaved << "-"
                                                  << (int) trackRef->_interleaved + 1 << ";"
                                                  << "ssrc=" << printSSRC(trackRef->_ssrc),
                          "x-Transport-Options", "late-tolerance=1.400000",
                          "x-Dynamic-Rate", "1"
                         });
    }
        break;

    case Rtsp::RTP_UDP: {
        std::pair<Socket::Ptr, Socket::Ptr> pr = std::make_pair(createSocket(),createSocket());
        try {
            makeSockPair(pr, get_local_ip());
        } catch (std::exception &ex) {
            //分配端口失败
            send_NotAcceptable();
            throw SockException(Err_shutdown, ex.what());
        }

        _rtp_socks[trackIdx] = pr.first;
        _rtcp_socks[trackIdx] = pr.second;

        //设置客户端内网端口信息
        string strClientPort = FindField(parser["Transport"].data(), "client_port=", NULL);
        uint16_t ui16RtpPort = atoi(FindField(strClientPort.data(), NULL, "-").data());
        uint16_t ui16RtcpPort = atoi(FindField(strClientPort.data(), "-", NULL).data());

        struct sockaddr_in peerAddr;
        //设置rtp发送目标地址
        peerAddr.sin_family = AF_INET;
        peerAddr.sin_port = htons(ui16RtpPort);
        peerAddr.sin_addr.s_addr = inet_addr(get_peer_ip().data());
        bzero(&(peerAddr.sin_zero), sizeof peerAddr.sin_zero);
        pr.first->bindPeerAddr((struct sockaddr *) (&peerAddr));

        //设置rtcp发送目标地址
        peerAddr.sin_family = AF_INET;
        peerAddr.sin_port = htons(ui16RtcpPort);
        peerAddr.sin_addr.s_addr = inet_addr(get_peer_ip().data());
        bzero(&(peerAddr.sin_zero), sizeof peerAddr.sin_zero);
        pr.second->bindPeerAddr((struct sockaddr *) (&peerAddr));

        //尝试获取客户端nat映射地址
        startListenPeerUdpData(trackIdx);
        //InfoP(this) << "分配端口:" << srv_port;

        sendRtspResponse("200 OK",
                         {"Transport", StrPrinter << "RTP/AVP/UDP;unicast;"
                                                  << "client_port=" << strClientPort << ";"
                                                  << "server_port=" << pr.first->get_local_port() << "-"
                                                  << pr.second->get_local_port() << ";"
                                                  << "ssrc=" << printSSRC(trackRef->_ssrc)
                         });
    }
        break;
    case Rtsp::RTP_MULTICAST: {
        if(!_multicaster){
            _multicaster = RtpMultiCaster::get(*this, get_local_ip(), _media_info._vhost, _media_info._app, _media_info._streamid);
            if (!_multicaster) {
                send_NotAcceptable();
                throw SockException(Err_shutdown, "can not get a available udp multicast socket");
            }
            weak_ptr<RtspSession> weakSelf = dynamic_pointer_cast<RtspSession>(shared_from_this());
            _multicaster->setDetachCB(this, [weakSelf]() {
                auto strongSelf = weakSelf.lock();
                if(!strongSelf) {
                    return;
                }
                strongSelf->safeShutdown(SockException(Err_shutdown,"ring buffer detached"));
            });
        }
        int iSrvPort = _multicaster->getMultiCasterPort(trackRef->_type);
        //我们用trackIdx区分rtp和rtcp包
        //由于组播udp端口是共享的，而rtcp端口为组播udp端口+1，所以rtcp端口需要改成共享端口
        auto pSockRtcp = UDPServer::Instance().getSock(*this, get_local_ip().data(), 2 * trackIdx + 1, iSrvPort + 1);
        if (!pSockRtcp) {
            //分配端口失败
            send_NotAcceptable();
            throw SockException(Err_shutdown, "open shared rtcp socket failed");
        }
        startListenPeerUdpData(trackIdx);
        GET_CONFIG(uint32_t,udpTTL,MultiCast::kUdpTTL);

        sendRtspResponse("200 OK",
                         {"Transport", StrPrinter << "RTP/AVP;multicast;"
                                                  << "destination=" << _multicaster->getMultiCasterIP() << ";"
                                                  << "source=" << get_local_ip() << ";"
                                                  << "port=" << iSrvPort << "-" << pSockRtcp->get_local_port() << ";"
                                                  << "ttl=" << udpTTL << ";"
                                                  << "ssrc=" << printSSRC(trackRef->_ssrc)
                         });
    }
        break;
    default:
        break;
    }
}

void RtspSession::handleReq_Play(const Parser &parser) {
    if (_sdp_track.empty() || parser["Session"] != _sessionid) {
        send_SessionNotFound();
        throw SockException(Err_shutdown, _sdp_track.empty() ? "can not find any available track when play" : "session not found when play");
    }
    auto play_src = _play_src.lock();
    if(!play_src){
        send_StreamNotFound();
        shutdown(SockException(Err_shutdown,"rtsp stream released"));
        return;
    }

    bool useGOP = true;
    auto &strScale = parser["Scale"];
    auto &strRange = parser["Range"];
    StrCaseMap res_header;
    if (!strScale.empty()) {
        //这是设置播放速度
        res_header.emplace("Scale", strScale);
        auto speed = atof(strScale.data());
        play_src->speed(speed);
        InfoP(this) << "rtsp set play speed:" << speed;
    }

    if (!strRange.empty()) {
        //这是seek操作
        res_header.emplace("Range", strRange);
        auto strStart = FindField(strRange.data(), "npt=", "-");
        if (strStart == "now") {
            strStart = "0";
        }
        auto iStartTime = 1000 * (float) atof(strStart.data());
        useGOP = !play_src->seekTo((uint32_t) iStartTime);
        InfoP(this) << "rtsp seekTo(ms):" << iStartTime;
    }

    _StrPrinter rtp_info;
    for (auto &track : _sdp_track) {
        if (track->_inited == false) {
            //还有track没有setup
            shutdown(SockException(Err_shutdown, "track not setuped"));
            return;
        }
        track->_ssrc = play_src->getSsrc(track->_type);
        track->_seq = play_src->getSeqence(track->_type);
        track->_time_stamp = play_src->getTimeStamp(track->_type);

        rtp_info << "url=" << track->getControlUrl(_content_base) << ";"
                 << "seq=" << track->_seq << ";"
                 << "rtptime=" << (int) (track->_time_stamp * (track->_samplerate / 1000)) << ",";
    }

    rtp_info.pop_back();

    res_header.emplace("RTP-Info", rtp_info);
    //已存在Range时不覆盖
    res_header.emplace("Range", StrPrinter << "npt=" << setiosflags(ios::fixed) << setprecision(2) << play_src->getTimeStamp(TrackInvalid) / 1000.0);
    sendRtspResponse("200 OK", res_header);

    //在回复rtsp信令后再恢复播放
    play_src->pause(false);

    setSocketFlags();

    if (!_play_reader && _rtp_type != Rtsp::RTP_MULTICAST) {
        weak_ptr<RtspSession> weakSelf = dynamic_pointer_cast<RtspSession>(shared_from_this());
        _play_reader = play_src->getRing()->attach(getPoller(), useGOP);
        _play_reader->setDetachCB([weakSelf]() {
            auto strongSelf = weakSelf.lock();
            if (!strongSelf) {
                return;
            }
            strongSelf->shutdown(SockException(Err_shutdown, "rtsp ring buffer detached"));
        });
        _play_reader->setReadCB([weakSelf](const RtspMediaSource::RingDataType &pack) {
            auto strongSelf = weakSelf.lock();
            if (!strongSelf) {
                return;
            }
            strongSelf->sendRtpPacket(pack);
        });
    }
}

void RtspSession::handleReq_Pause(const Parser &parser) {
    if (parser["Session"] != _sessionid) {
        send_SessionNotFound();
        throw SockException(Err_shutdown, "session not found when pause");
    }

    sendRtspResponse("200 OK");
    auto play_src = _play_src.lock();
    if (play_src) {
        play_src->pause(true);
    }
}

void RtspSession::handleReq_Teardown(const Parser &parser) {
    sendRtspResponse("200 OK");
    throw SockException(Err_shutdown,"recv teardown request");
}

void RtspSession::handleReq_Get(const Parser &parser) {
    _http_x_sessioncookie = parser["x-sessioncookie"];
    sendRtspResponse("200 OK",
                     {"Cache-Control","no-store",
                      "Pragma","no-store",
                      "Content-Type","application/x-rtsp-tunnelled",
                     },"","HTTP/1.0");

    //注册http getter，以便http poster绑定
    lock_guard<recursive_mutex> lock(g_mtxGetter);
    g_mapGetter[_http_x_sessioncookie] = dynamic_pointer_cast<RtspSession>(shared_from_this());
}

void RtspSession::handleReq_Post(const Parser &parser) {
    lock_guard<recursive_mutex> lock(g_mtxGetter);
    string sessioncookie = parser["x-sessioncookie"];
    //Poster 找到 Getter
    auto it = g_mapGetter.find(sessioncookie);
    if (it == g_mapGetter.end()) {
        throw SockException(Err_shutdown,"can not find http getter by x-sessioncookie");
    }

    //Poster 找到Getter的SOCK
    auto httpGetterWeak = it->second;
    //移除http getter的弱引用记录
    g_mapGetter.erase(sessioncookie);

    //http poster收到请求后转发给http getter处理
    _on_recv = [this,httpGetterWeak](const Buffer::Ptr &buf){
        auto httpGetterStrong = httpGetterWeak.lock();
        if(!httpGetterStrong){
            shutdown(SockException(Err_shutdown,"http getter released"));
            return;
        }

        //切换到http getter的线程
        httpGetterStrong->async([buf,httpGetterWeak](){
            auto httpGetterStrong = httpGetterWeak.lock();
            if(!httpGetterStrong){
                return;
            }
            httpGetterStrong->onRecv(std::make_shared<BufferString>(decodeBase64(string(buf->data(), buf->size()))));
        });
    };

    if(!parser.Content().empty()){
        //http poster后面的粘包
        _on_recv(std::make_shared<BufferString>(parser.Content()));
    }

    sendRtspResponse("200 OK",
                     {"Cache-Control","no-store",
                      "Pragma","no-store",
                      "Content-Type","application/x-rtsp-tunnelled",
                     },"","HTTP/1.0");
}

void RtspSession::handleReq_SET_PARAMETER(const Parser &parser) {
    //TraceP(this) <<endl;
    sendRtspResponse("200 OK");
}

void RtspSession::send_NotAcceptable() {
    sendRtspResponse("406 Not Acceptable",{"Connection","Close"});
}

void RtspSession::onRtpSorted(RtpPacket::Ptr rtp, int track_idx) {
    _push_src->onWrite(std::move(rtp), false);
}

void RtspSession::onRcvPeerUdpData(int interleaved, const Buffer::Ptr &buf, const struct sockaddr &addr) {
    //这是rtcp心跳包，说明播放器还存活
    _alive_ticker.resetTime();

    if (interleaved % 2 == 0) {
        if (_push_src) {
            //这是rtsp推流上来的rtp包
            auto &ref = _sdp_track[interleaved / 2];
            handleOneRtp(interleaved / 2, ref->_type, ref->_samplerate, (uint8_t *) buf->data(), buf->size());
        } else if (!_udp_connected_flags.count(interleaved)) {
            //这是rtsp播放器的rtp打洞包
            _udp_connected_flags.emplace(interleaved);
            _rtp_socks[interleaved / 2]->bindPeerAddr(&addr);
        }
    } else {
        //rtcp包
        if (!_udp_connected_flags.count(interleaved)) {
            _udp_connected_flags.emplace(interleaved);
            _rtcp_socks[(interleaved - 1) / 2]->bindPeerAddr(&addr);
        }
        onRtcpPacket((interleaved - 1) / 2, _sdp_track[(interleaved - 1) / 2], buf->data(), buf->size());
    }
}

void RtspSession::startListenPeerUdpData(int track_idx) {
    weak_ptr<RtspSession> weakSelf = dynamic_pointer_cast<RtspSession>(shared_from_this());
    auto srcIP = inet_addr(get_peer_ip().data());
    auto onUdpData = [weakSelf,srcIP](const Buffer::Ptr &buf, struct sockaddr *peer_addr, int interleaved){
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            return false;
        }

        if (((struct sockaddr_in *) peer_addr)->sin_addr.s_addr != srcIP) {
            WarnP(strongSelf.get()) << ((interleaved % 2 == 0) ? "收到其他地址的rtp数据:" : "收到其他地址的rtcp数据:")
                                    << SockUtil::inet_ntoa(((struct sockaddr_in *) peer_addr)->sin_addr);
            return true;
        }

        struct sockaddr addr = *peer_addr;
        strongSelf->async([weakSelf, buf, addr, interleaved]() {
            auto strongSelf = weakSelf.lock();
            if (!strongSelf) {
                return;
            }
            try {
                strongSelf->onRcvPeerUdpData(interleaved, buf, addr);
            } catch (SockException &ex) {
                strongSelf->shutdown(ex);
            } catch (std::exception &ex) {
                strongSelf->shutdown(SockException(Err_other, ex.what()));
            }
        });
        return true;
    };

    switch (_rtp_type){
        case Rtsp::RTP_MULTICAST:{
            //组播使用的共享rtcp端口
            UDPServer::Instance().listenPeer(get_peer_ip().data(), this,
                    [onUdpData]( int interleaved, const Buffer::Ptr &buf, struct sockaddr *peer_addr) {
                return onUdpData(buf, peer_addr, interleaved);
            });
        }
            break;
        case Rtsp::RTP_UDP:{
            auto setEvent = [&](Socket::Ptr &sock,int interleaved){
                if(!sock){
                    WarnP(this) << "udp端口为空:" << interleaved;
                    return;
                }
                sock->setOnRead([onUdpData,interleaved](const Buffer::Ptr &pBuf, struct sockaddr *pPeerAddr , int addr_len){
                    onUdpData(pBuf, pPeerAddr, interleaved);
                });
            };
            setEvent(_rtp_socks[track_idx], 2 * track_idx );
            setEvent(_rtcp_socks[track_idx], 2 * track_idx + 1 );
        }
            break;

        default:
            break;
    }

}

static string dateStr(){
    char buf[64];
    time_t tt = time(NULL);
    strftime(buf, sizeof buf, "%a, %b %d %Y %H:%M:%S GMT", gmtime(&tt));
    return buf;
}

bool RtspSession::sendRtspResponse(const string &res_code, const StrCaseMap &header_const, const string &sdp, const char *protocol){
    auto header = header_const;
    header.emplace("CSeq",StrPrinter << _cseq);
    if(!_sessionid.empty()){
        header.emplace("Session", _sessionid);
    }

    header.emplace("Server",kServerName);
    header.emplace("Date",dateStr());

    if(!sdp.empty()){
        header.emplace("Content-Length",StrPrinter << sdp.size());
        header.emplace("Content-Type","application/sdp");
    }

    _StrPrinter printer;
    printer << protocol << " " << res_code << "\r\n";
    for (auto &pr : header){
        printer << pr.first << ": " << pr.second << "\r\n";
    }

    printer << "\r\n";

    if(!sdp.empty()){
        printer << sdp;
    }
//	DebugP(this) << printer;
    return send(std::make_shared<BufferString>(std::move(printer))) > 0 ;
}

ssize_t RtspSession::send(Buffer::Ptr pkt){
//	if(!_enableSendRtp){
//		DebugP(this) << pkt->data();
//	}
    _bytes_usage += pkt->size();
    return TcpSession::send(std::move(pkt));
}

bool RtspSession::sendRtspResponse(const string &res_code, const std::initializer_list<string> &header, const string &sdp, const char *protocol) {
    string key;
    StrCaseMap header_map;
    int i = 0;
    for(auto &val : header){
        if(++i % 2 == 0){
            header_map.emplace(key,val);
        }else{
            key = val;
        }
    }
    return sendRtspResponse(res_code,header_map,sdp,protocol);
}

int RtspSession::getTrackIndexByTrackType(TrackType type) {
    for (unsigned int i = 0; i < _sdp_track.size(); i++) {
        if (type == _sdp_track[i]->_type) {
            return i;
        }
    }
    if(_sdp_track.size() == 1){
        return 0;
    }
    throw SockException(Err_shutdown, StrPrinter << "no such track with type:" << (int) type);
}

int RtspSession::getTrackIndexByControlUrl(const string &control_url) {
    for (unsigned int i = 0; i < _sdp_track.size(); i++) {
        if (control_url == _sdp_track[i]->getControlUrl(_content_base)) {
            return i;
        }
    }
    if(_sdp_track.size() == 1){
        return 0;
    }
    throw SockException(Err_shutdown, StrPrinter << "no such track with control url:" << control_url);
}

int RtspSession::getTrackIndexByInterleaved(int interleaved){
    for (unsigned int i = 0; i < _sdp_track.size(); i++) {
        if (_sdp_track[i]->_interleaved == interleaved) {
            return i;
        }
    }
    if(_sdp_track.size() == 1){
        return 0;
    }
    throw SockException(Err_shutdown, StrPrinter << "no such track with interleaved:" << interleaved);
}

bool RtspSession::close(MediaSource &sender, bool force) {
    //此回调在其他线程触发
    if(!_push_src || (!force && _push_src->totalReaderCount())){
        return false;
    }
    string err = StrPrinter << "close media:" << sender.getSchema() << "/" << sender.getVhost() << "/" << sender.getApp() << "/" << sender.getId() << " " << force;
    safeShutdown(SockException(Err_shutdown,err));
    return true;
}

int RtspSession::totalReaderCount(MediaSource &sender) {
    return _push_src ? _push_src->totalReaderCount() : sender.readerCount();
}

MediaOriginType RtspSession::getOriginType(MediaSource &sender) const{
    return MediaOriginType::rtsp_push;
}

string RtspSession::getOriginUrl(MediaSource &sender) const {
    return _media_info._full_url;
}

std::shared_ptr<SockInfo> RtspSession::getOriginSock(MediaSource &sender) const {
    return const_cast<RtspSession *>(this)->shared_from_this();
}

void RtspSession::onBeforeRtpSorted(const RtpPacket::Ptr &rtp, int track_index){
    updateRtcpContext(rtp);
}

void RtspSession::updateRtcpContext(const RtpPacket::Ptr &rtp){
    int track_index = getTrackIndexByTrackType(rtp->type);
    auto &rtcp_ctx = _rtcp_context[track_index];
    rtcp_ctx->onRtp(rtp->getSeq(), rtp->getStamp(), rtp->ntp_stamp, rtp->sample_rate, rtp->size() - RtpPacket::kRtpTcpHeaderSize);

    auto &ticker = _rtcp_send_tickers[track_index];
    //send rtcp every 5 second
    if (ticker.elapsedTime() > 5 * 1000 || (_send_sr_rtcp[track_index] && !_push_src)) {
        //确保在发送rtp前，先发送一次sender report rtcp(用于播放器同步音视频)
        ticker.resetTime();
        _send_sr_rtcp[track_index] = false;

        static auto send_rtcp = [](RtspSession *thiz, int index, Buffer::Ptr ptr) {
            if (thiz->_rtp_type == Rtsp::RTP_TCP) {
                auto &track = thiz->_sdp_track[index];
                thiz->send(makeRtpOverTcpPrefix((uint16_t)(ptr->size()), track->_interleaved + 1));
                thiz->send(std::move(ptr));
            } else {
                thiz->_rtcp_socks[index]->send(std::move(ptr));
            }
        };

        auto ssrc = rtp->getSSRC();
        auto rtcp = _push_src ?  rtcp_ctx->createRtcpRR(ssrc + 1, ssrc) : rtcp_ctx->createRtcpSR(ssrc);
        auto rtcp_sdes = RtcpSdes::create({kServerName});
        rtcp_sdes->chunks.type = (uint8_t)SdesType::RTCP_SDES_CNAME;
        rtcp_sdes->chunks.ssrc = htonl(ssrc);
        send_rtcp(this, track_index, std::move(rtcp));
        send_rtcp(this, track_index, RtcpHeader::toBuffer(rtcp_sdes));
    }
}

void RtspSession::sendRtpPacket(const RtspMediaSource::RingDataType &pkt) {
    switch (_rtp_type) {
        case Rtsp::RTP_TCP: {
            size_t i = 0;
            auto size = pkt->size();
            setSendFlushFlag(false);
            pkt->for_each([&](const RtpPacket::Ptr &rtp) {
                updateRtcpContext(rtp);
                if (++i == size) {
                    setSendFlushFlag(true);
                }
                send(rtp);
            });
        }
            break;
        case Rtsp::RTP_UDP: {
            size_t i = 0;
            auto size = pkt->size();
            pkt->for_each([&](const RtpPacket::Ptr &rtp) {
                updateRtcpContext(rtp);
                int track_index = getTrackIndexByTrackType(rtp->type);
                auto &pSock = _rtp_socks[track_index];
                if (!pSock) {
                    shutdown(SockException(Err_shutdown, "udp sock not opened yet"));
                    return;
                }
                _bytes_usage += rtp->size() - RtpPacket::kRtpTcpHeaderSize;
                pSock->send(std::make_shared<BufferRtp>(rtp, RtpPacket::kRtpTcpHeaderSize), nullptr, 0, ++i == size);
            });
        }
            break;
        default:
            break;
    }
}

void RtspSession::setSocketFlags(){
    GET_CONFIG(int, mergeWriteMS, General::kMergeWriteMS);
    if(mergeWriteMS > 0) {
        //推流模式下，关闭TCP_NODELAY会增加推流端的延时，但是服务器性能将提高
        SockUtil::setNoDelay(getSock()->rawFD(), false);
        //播放模式下，开启MSG_MORE会增加延时，但是能提高发送性能
        setSendFlags(SOCKET_DEFAULE_FLAGS | FLAG_MORE);
    }
}

}
/* namespace mediakit */
