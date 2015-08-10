
#!/bin/bash
IP=192.168.10.88
#localhost
STORE=/home/zhangbin/alex/Downloads/testVideos/vod
NAME=play_trailer.html-rtmp_video_gz3goij33q2.flv
FILE=$STORE/$NAME
APP=myapp
#live_server
#myapp

for((;;)); do
        ffmpeg -re -i $FILE -vcodec copy -acodec copy -f flv -y rtmp://$IP/$APP
        #/liveStream
        sleep 1
done
