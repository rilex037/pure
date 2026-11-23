#!/bin/bash
export LD_LIBRARY_PATH=/home/linuxbrew/.linuxbrew/Cellar/ffmpeg/7.1.1_1/lib:$LD_LIBRARY_PATH
exec /home/rilex/Downloads/PurePlayer-1.0/PurePlayer "$@"
