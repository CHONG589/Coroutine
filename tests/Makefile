CXX = g++
CFLAGS =  -Wall -g 

TARGET = test_iomanager
OBJS = log.cpp fiber.cc test_iomanager.cc thread.cc buffer.cpp mutex.cc scheduler.cc iomanager.cc timer.cc

all: $(OBJS)
	$(CXX) $(CFLAGS) $(OBJS) -o $(TARGET)  -pthread 
# -lmysqlclient

# clean:
#	rm -rf ../bin/$(OBJS) $(TARGET)




