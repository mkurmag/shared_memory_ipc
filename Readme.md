Shared Memory IPC
=================

Example of inter-process communication of multiple processes through shared memory.

Processes are mostly independent and communicate from time to time.
Communication is asynchronous.
At random time intervals process updates message for other processes and reads their messages.

Processes could be killed and must be able to recover after that.

Design
------
For `N` processes `N` shared memory objects are created.
Each shared memory object has a data structure with single producer and multiple consumers.

Shared memory is used because it allows to reread same data after process was killed and recovered.

When producer writes new message, the old message is deleted if there is no consumer
reading the old message.

While reading, consumers lock the message to prevent it from being deleted.
When consumer tries to lock the message the most recent message is locked.
Producer constantly updates the pointer to the most recent message.

Single consumer can lock only one message at the same time.
Single message can be locked by several consumers.

Even when current message is not locked by any consumer, the producer is still required to
write message to some empty slot, to prevent partially written state if producer
is killed during write.
After write is complete producer atomically moves the pointer to the most recent message.

Since in the worst case all consumers can lock different messages,
then N+1 slots are required for producer to always be able to write new messages.

When consumer is killed during read, then when restored, the consumer releases the lock it made early.

Prerequisites
-------------

* CMake
* Boost
* Catch2 (only for tests)


Run script
----------

Interactive script `run.sh` builds project and runs it.
The script provides the ability to kill any process and to restart it.

The script takes a number of processes as a parameter.
This command start 3 processes with indices 0,1,2:

```
./run.sh 3
```
Each process prints value it is going to write and values it read from other process.

```
0: read info from 1: empty
0: read info from 2: empty
0: write 1
1: read info from 0: 1
1: read info from 2: empty
1: write 1
2: ready
2: read info from 0: 1
2: read info from 1: 1
2: write 1
2: read info from 0: 1
2: read info from 1: 1
...
```

Entering digits from `0` to `N-1` (where `N` is the parameter) allows to control processes with corresponding index.
If process is running, then entering digit kills the process, otherwise entering digit start the process.

Building manually
-----------------

CMake takes number of processes as a parameter.

```
mkdir build
cd build
cmake ../ -DPROC_COUNT=4
make
```

Running manually
-----------------

Building process creates executable `./build/Proc`.
Program takes index of the process as a parameter.
N copies of program should be started with indices from `0` to `N-1`, where N is a number of processes passed to CMake.
Program waits until all N copies have been started.

After that programs will start to communicate. Program executes indefinitely.
Each program can be manually killed and started again.

Since program executes indefinitely, it doesn't clear allocated resources.
Shared memory objects should be cleared manually.
This could be done with the following command:

```
rm -f /dev/shm/shared_memory*
```


Testing
-------

Project has a few unit tests. Tests can be run with the following commands:

```
cd build
cmake ../ -DTESTS=1
make
```
