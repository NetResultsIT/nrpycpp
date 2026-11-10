# Functions called by tst_nrpycpp.cpp to test the nrpycpp module (how a script talks back to C++)
import sys
import threading
import time
import _thread

import nrpycpp


def send(text):
    nrpycpp.send_message(text)
    return nrpycpp.runner_id()


def send_bad():
    nrpycpp.send_message(123)


def lines():
    print("a\nb")


def partial():
    print("x", end="")


def to_stderr():
    print("oops", file=sys.stderr)


def stream_info():
    # what libraries look at in a stream
    return [type(sys.stdout).__name__, sys.stdout.encoding, sys.stdout.errors, sys.stdout.isatty(),
            sys.stdout.write("12345"), sys.stdout.fileno() == sys.__stdout__.fileno()]


def write_bad():
    sys.stdout.write(b"bytes")


def chatter(tag, n):
    for i in range(n):
        print("%s-%d" % (tag, i))
        if i % 50 == 0:
            sys.stderr.write("%s-err-%d\n" % (tag, i))
    return n


def thread_talks():
    def work():
        print("from-thread")
        nrpycpp.send_message("msg-from-thread")
        _thread_result.append(nrpycpp.runner_id())
    t = threading.Thread(target=work)
    t.start()


_thread_result = []


def thread_runner_id():
    return list(_thread_result)


def thread_partial():
    threading.Thread(target=lambda: print("tail", end="")).start()


def thread_fails():
    def work():
        raise ValueError("thread-boom")
    threading.Thread(target=work).start()


def pool_talks():
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(2) as pool:
        list(pool.map(lambda i: print("pool-%d" % i), range(4)))


_raw_result = []


def raw_thread():
    def work():
        try:
            nrpycpp.send_message("never")
            _raw_result.append("sent")
        except Exception as e:
            _raw_result.append(type(e).__name__)
    _thread.start_new_thread(work, ())


def raw_thread_result():
    return list(_raw_result)


def everything():
    print("line-1")
    print("err-1", file=sys.stderr)
    nrpycpp.send_message("hello")
    threading.Thread(target=lambda: 1 / 0).start()


def ghost(seconds):
    def work():
        end = time.time() + seconds
        while time.time() < end:
            print("ghost")
            time.sleep(0.02)
    threading.Thread(target=work, daemon=True).start()


def no_runner_at_import_time():
    return nrpycpp.runner_id()
