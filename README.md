Compile: 

1 - Move to folder: decoder->debug: then run make
2 - Yet run the decoder: example: ./decoder udp://@:8888 10
3 - In other terminal Move to folder: encoder->debug: the run make
4 - Yet run the sender: example: ./sender d.avi video udp://127.0.0.1:8888
5 - In Qjackctl, conect output FogoAudio in input of system
6 - It work, but not reproduce sound.

