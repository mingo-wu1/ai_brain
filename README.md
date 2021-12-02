# ai_brain
ai brain with big data serve

# live serve

## test example rtsp

ffplay -i rtsp://0.0.0.0:554/live/dog -fflags nobuffer -analyzeduration 10

sudo ffmpeg -re -f v4l2 -s 320x240 -r 24 -i /dev/video4 -vcodec h264 -max_delay 0 -preset:v ultrafast -tune zerolatency -g 5 -b 70000k -f rtsp rtsp://0.0.0.0:554/live/dog

## test example rtmp

ffplay -i rtmp://0.0.0.0:1935/live/dog -fflags nobuffer -analyzeduration 10

sudo ffmpeg -re -f v4l2 -s 320x240 -r 24 -i /dev/video4 -vcodec h264 -max_delay 0 -preset:v ultrafast -tune zerolatency -g 5 -b 70000k -f flv rtmp://0.0.0.0:1935/live/dog

## test example rtp

sudo ffmpeg -re -f v4l2 -s 320x240 -i /dev/video4 -vcodec h264 -max_delay 0 -preset:v ultrafast -tune zerolatency -g 5 -b 700k -f rtp rtp://0.0.0.0:10003/live/dog > test.sdp

ffplay -protocol_whitelist "file,udp,rtp" -i test.sdp -fflags nobuffer -analyzeduration 10

## test example rtp

sudo ffmpeg -re -i /dev/video4 -f rtp rtp://0.0.0.0:10002/live/dog > test.sdp

sudo ffplay -protocol_whitelist "file,udp,rtp" -i test.sdp

## test example http

### http ffmpeg

ffmpeg -f v4l2 -i /dev/video4 -vcodec h264 -max_delay 0 -preset:v ultrafast -tune zerolatency -g 5 -b 700k -fflags nobuffer -flags low_delay http://192.168.12.1:8090/feed1.ffm

### http ffserver

ffserver -f ffserver.conf

ffserver.conf

HTTPPort 8090
HTTPBindAddress 0.0.0.0
MaxHTTPConnections 2000
MaxClients 1000
MaxBandwidth 10000000
CustomLog -
NoDaemon

<Feed feed1.ffm>
File /tmp/feed1.ffm
FileMaxSize 20M
ACL allow 1921.168.12.1
</Feed>

<Stream test.flv>
Format flv
Feed feed1.ffm
VideoFrameRate 40
VideoBitRate 100000
VideoSize 360x240
AVOptionVideo flags +global_header
NoAudio
ACL allow 192.168.0.0 192.168.255.255
</Stream>

<Stream stat.html>
Format status
ACL allow localhost
ACL allow 192.168.0.0 192.168.255.255
</Stream>

### http ffplay

ffplay -fflags nobuffer -flags low_delay http://192.168.12.1:8090/test.flv
ffplay -fflags nobuffer http://127.0.0.1:8090/test.flv

## test example udp

### udp ffmpeg good

route -n
sudo route add -net 224.1.1.0 netmask 255.255.255.0 dev wlan0
sudo ffmpeg -f v4l2 -i /dev/video2 -vcodec h264 -max_delay 0 -preset:v ultrafast -tune zerolatency -g 5 -b 700k -fflags nobuffer -flags low_delay -f h264 -pix_fmt yuv420p udp://224.1.1.1:6666

### udp ffplay good

sudo route add -net 224.1.1.0 netmask 255.255.255.0 dev wlp60s0
ffplay -f h264 udp://224.1.1.1:6666
ffplay -fflags nobuffer -flags low_delay -f h264 udp://224.1.1.1:6666
