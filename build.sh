#!/bin/bash

g++ -g example.c -Inode_modules/native-video-deps/include -Lnode_modules/native-video-deps/lib/linux/ -lm -lpthread -lswscale -lavdevice -lavformat -lavcodec -lavutil -lopus
