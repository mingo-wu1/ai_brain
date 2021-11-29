﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_DECODER_H
#define ZLMEDIAKIT_DECODER_H

#include <stdint.h>
#include <memory>
#include <functional>
#include "Common/MediaSink.h"

using namespace std;
namespace mediakit {

class Decoder {
public:
    typedef std::shared_ptr<Decoder> Ptr;
    typedef std::function<void(int stream, int codecid, int flags, int64_t pts, int64_t dts, const void *data, size_t bytes)> onDecode;
    typedef std::function<void(int stream, int codecid, const void *extra, size_t bytes, int finish)> onStream;

    virtual ssize_t input(const uint8_t *data, size_t bytes) = 0;
    virtual void setOnDecode(onDecode cb) = 0;
    virtual void setOnStream(onStream cb) = 0;

protected:
    Decoder() = default;
    virtual ~Decoder() = default;
};

class DecoderImp{
public:
    typedef enum {
        decoder_ts = 0,
        decoder_ps
    }Type;

    typedef std::shared_ptr<DecoderImp> Ptr;
    ~DecoderImp() = default;

    static Ptr createDecoder(Type type, MediaSinkInterface *sink);
    ssize_t input(const uint8_t *data, size_t bytes);

protected:
    void onTrack(const Track::Ptr &track);
    void onFrame(const Frame::Ptr &frame);

private:
    DecoderImp(const Decoder::Ptr &decoder, MediaSinkInterface *sink);
    void onDecode(int stream, int codecid, int flags, int64_t pts, int64_t dts, const void *data, size_t bytes);
    void onStream(int stream, int codecid, const void *extra, size_t bytes, int finish);

private:
    Decoder::Ptr _decoder;
    MediaSinkInterface *_sink;
    FrameMerger _merger{FrameMerger::none};
    Ticker _last_unsported_print;
    Track::Ptr _tracks[TrackMax];
};

}//namespace mediakit
#endif //ZLMEDIAKIT_DECODER_H
