rm tracking
rm *Frame*.jpg
rm output.*
g++ -std=c++11 *.cpp -lxml2 -I/usr/include/libxml2/ -L/usr/lib/x86_64-linux-gnu/ -L/usr/local/lib -L/usr/lib -lopencv_core -lopencv_highgui -lopencv_imgproc -lopencv_gpu -lopencv_video -lopencv_features2d -lopencv_objdetect -lavformat -lavutil -lavcodec -lavformat -ldl -lpthread -lz -lswscale -lm -D__STDC_CONSTANT_MACROS -p -o tracking
./tracking ../../test.avi 0 1 355

 