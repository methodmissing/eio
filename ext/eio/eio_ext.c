#define EIO_REQ_MEMBERS short int complete;

#include "../libeio/config.h"
#include "../libeio/eio.h"
#include "../libeio/xthread.h"
#include "../libeio/eio.c"
#include "ruby.h"

/*
 *  Ruby 1.9 specific macros
 */
#ifdef RUBY_VM
#include <ruby/encoding.h>
#include <ruby/io.h>
#define NO_CB_ARGS 0
#define EioEncode(str) rb_enc_associate(str, rb_default_internal_encoding())
#define DONT_GC(obj) rb_gc_register_mark_object(obj)
#define TRAP_BEG
#define TRAP_END
static size_t
stat_memsize(const void *p)
{
   return p ? sizeof(struct stat) : 0;
}

static const rb_data_type_t stat_data_type = {
    "stat",
    {NULL, RUBY_TYPED_DEFAULT_FREE, stat_memsize,},
};
#else
#ifndef RSTRING_PTR
#define RSTRING_PTR(str) RSTRING(str)->ptr
#endif
#ifndef RSTRING_LEN
#define RSTRING_LEN(s) (RSTRING(s)->len)
#endif
#include "rubyio.h"
#include "rubysig.h"
#define NO_CB_ARGS -1
#define EioEncode(str) str
#define DONT_GC(obj) rb_gc_register_address(&obj)
#endif

/*
 *  Synchronous I/O fallback
 */
#define s_fsync fsync
#define s_open open
#define s_close close
#define s_readahead(fd, len, off) read(fd, off, len)
#define s_sendfile eio_sendfile_sync
#define s_mkdir mkdir
#define s_rmdir rmdir
#define s_unlink unlink
#define s_rename rename
#define s_chmod chmod
#define s_fchmod fchmod
#define s_ftruncate ftruncate
#define s_truncate truncate
#define s_chown chown
#define s_fchown fchown
#define s_link link
#define s_symlink symlink

VALUE mEio;
VALUE cEioReq;

/*
 *  Symbols
 */
static VALUE sym_call;
static VALUE sym_arity;
static VALUE sym_pipe;
static VALUE sym_readlink;
static VALUE sym_stat;
static VALUE sym_pipe_r_fd;
static VALUE sym_pipe_w_fd;

/*
 *  Common fixnums
 */
static VALUE eio_zero;
static VALUE eio_default_bufsize;
static VALUE eio_default_mode;

/*
 *  Pipe specific
 */

static VALUE eio_pipe;
static VALUE pipe_r_fd;
static VALUE pipe_w_fd;
static int eio_pipe_r_fd;
static int eio_pipe_w_fd;

static void rb_eio_recreate_pipe(void);
static VALUE rb_eio_wrap_request(eio_req *r);

/*
 *  Assert a valid Proc and arity as callback
 */
#define AssertCallback(cb, arity) \
    if (NIL_P(cb)) cb = proc; \
    if (!NIL_P(cb)){ \
        if (rb_class_of(cb) != rb_cProc) \
            rb_raise(rb_eTypeError, "Expected a Proc callback"); \
        if (rb_funcall(cb, sym_arity, 0) != INT2NUM(arity)) \
          rb_raise(rb_eArgError, "Callback expects %d argument(s), got %d", arity, FIX2INT(rb_funcall(cb, sym_arity, 0))); \
    }

/*
 *  Wrap the eio_req struct for a given object. Raises TypeError if the struct has been recycled by
 *  a libeio finish callback.
 */
#define GetRequest(obj) \
    eio_req * req; \
    Data_Get_Struct(obj, eio_req, req); \
    if (!req) rb_raise(rb_eTypeError, "uninitialized EIO::Request");

/*
 *  libeio callback handler. Respects cancelled requests and bubbles up any errors (-1 results).
 */
#define EioCallback(req, statements) \
    VALUE cb; \
    assert(req); \
    cb = (VALUE)req->data; \
    if EIO_CANCELLED(req){ \
        rb_gc_unregister_address(&cb); \
        return 0; \
    } \
    if (req->result == -1){ \
         rb_gc_unregister_address(&cb); \
         errno = req->errorno; \
         req->complete = 1; \
         (!req->ptr1) ? rb_sys_fail(0) : rb_sys_fail(req->ptr1); \
         return -1; \
    } else { \
        if (!NIL_P(cb)){ \
            statements; \
            req->complete = 1; \
            rb_gc_unregister_address(&cb); \
        } \
    } \
    return 0;

/*
 *  Synchronous I/O request
 */
#define SyncRequest(statements) \
    if (!rb_block_given_p()){ \
        TRAP_BEG; \
        statements; \
        TRAP_END; \
    }

/*
 *  Asynchronous I/O request
 */
#define AsyncRequest(syscall, callback, ...) \
    if (rb_thread_current() != rb_thread_main()) \
        rb_raise(rb_eThreadError, "EIO requests can only be submitted on the main thread."); \
    DONT_GC(cb); \
    return rb_eio_wrap_request(eio_ ## syscall(__VA_ARGS__, EIO_PRI_DEFAULT, callback, (void*)cb)); \

/*
 *  Abstraction for conditional sync / async I/O
 */
#define SubmitRequest(syscall, callback, ...) \
    if (rb_block_given_p()){ \
        AsyncRequest(syscall, callback, ##__VA_ARGS__); \
    } else { \
        TRAP_BEG; \
        ret = s_ ## syscall(__VA_ARGS__); \
        TRAP_END; \
        if (ret == -1) rb_sys_fail(#syscall); \
        return INT2NUM(ret); \
    }

#define CloseOnExec(fd) \
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) \
        rb_sys_fail("could not set FD_CLOEXEC flag on descriptor");

/*
 *  Callback for when libeio wants attention. Writes a char to pipe to wake up the event loop.
 */
static void
want_poll(void)
{
    char heartbeat;
    assert(write(eio_pipe_w_fd, &heartbeat, 1) == 1);
}

/*
 *  Callback invoked when all pending work's been done. Reads a char from the pipe.
 */
static void
done_poll(void)
{
    char heartbeat;
    assert(read(eio_pipe_r_fd, &heartbeat, 1) == 1);
}

/*
 *  Drain all pending libeio requests
 */
static void
rb_eio_s_wait0()
{
    fd_set rdset;
    int fd, size;
    fd = eio_pipe_r_fd;

    while (eio_nreqs())
    {
        X_LOCK(reslock);
        size = res_queue.size;
        X_UNLOCK(reslock);

        if (size) return;

        etp_maybe_start_thread();

        FD_ZERO(&rdset);
        FD_SET(fd, &rdset);
        if (rb_thread_select(fd + 1, &rdset, NULL, NULL, NULL) < 0) rb_sys_fail(0);
    }
}

/*
 *  Generic callback, invoked with no args
 */
int
rb_eio_generic_cb(eio_req *req)
{
    EioCallback(req,{
        rb_funcall(cb, sym_call, 0);
    });
}

/*
 *  Open callback, invoked with a single Fixnum arg
 */
int
rb_eio_open_cb(eio_req *req)
{
    int fd;
    EioCallback(req,{
        fd = EIO_RESULT(req);
        CloseOnExec(fd);
        rb_funcall(cb, sym_call, 1, INT2NUM(fd));
    });
}

/*
 *  Read callback, invoked with a single String arg
 */
int
rb_eio_read_cb(eio_req *req)
{
    EioCallback(req,{
        rb_funcall(cb, sym_call, 1, EioEncode(rb_str_new((const char*)EIO_BUF(req), EIO_RESULT(req))));
    });
}

/*
 *  Readdir callback, invoked with a single Array arg
 */
int
rb_eio_readdir_cb(eio_req *req)
{
    int ret;
    VALUE result;
    char *entries;
    EioCallback(req, {
        result = rb_ary_new2(EIO_RESULT(req));
        entries = (char *)EIO_BUF(req);
        while (EIO_RESULT(req)--)
        {
            rb_ary_push(result, EioEncode(rb_str_new2(entries)));
            entries += strlen(entries) + 1;
        }
        rb_funcall(cb, sym_call, 1, result);
    });
}

/*
 *  Write callback, invoked with a single Fixnum arg
 */
int
rb_eio_write_cb(eio_req *req)
{
    EioCallback(req,{
        rb_funcall(cb, sym_call, 1, INT2NUM(EIO_RESULT(req)));
    });
}

/*
 *  Stat callback, invoked with a single File::Stat arg
 */
int
rb_eio_stat_cb(eio_req *req)
{
    EioCallback(req,{
#ifdef RUBY_VM
        rb_funcall(cb, sym_call, 1, TypedData_Wrap_Struct(rb_cStat, &stat_data_type, EIO_BUF(req)));
#else
        rb_funcall(cb, sym_call, 1, Data_Wrap_Struct(rb_cStat, NULL, NULL, EIO_BUF(req)));
#endif
    });
}

/*
 *  call-seq:
 *     EIO.wait                    =>  nil
 * 
 *  Drain / flush all pending requests - BLOCKS
 *
 */
static VALUE
rb_eio_s_wait(VALUE eio)
{
    while (eio_nreqs())
    {
        rb_eio_s_wait0();
        if (eio_poll() > 0) rb_sys_fail("eio_poll");
    }
    return Qnil;
}

/*
 *  call-seq:
 *     EIO.poll                    =>  Fixnum
 *
 *  Called when pending requests need finishing
 *
 */
static VALUE
rb_eio_s_poll(VALUE eio)
{
    int res;
    res = eio_poll();
    if (res > 0) rb_sys_fail("eio_poll");
    return INT2NUM(res);
}

/*
 *  call-seq:
 *     EIO.requests                 =>  Fixnum
 *
 *  Number of requests currently in the ready, execute or pending states
 *
 */
static VALUE
rb_eio_s_requests(VALUE eio)
{
    return INT2NUM(eio_nreqs());
}

/*
 *  call-seq:
 *     EIO.ready                 =>  Fixnum
 *
 *  Number of requests currently in the ready state (not yet executed)
 *
 */
static VALUE
rb_eio_s_ready(VALUE eio)
{
    return INT2NUM(eio_nready());
}

/*
 *  call-seq:
 *     EIO.pending                 =>  Fixnum
 *
 *  Number of requests currently in the pending state
 *
 */
static VALUE
rb_eio_s_pending(VALUE eio)
{
    return INT2NUM(eio_npending());
}

/*
 *  call-seq:
 *     EIO.threads                 =>  Fixnum
 *
 *  Number of worker threads spawned
 *
 */
static VALUE
rb_eio_s_threads(VALUE eio)
{
    return INT2NUM(eio_nthreads());
}

/*
 *  call-seq:
 *     EIO.fd                 =>  Fixnum
 *
 *  Read end of the pipe an event loop can monitor for readability
 *
 */
static VALUE
rb_eio_s_fd(VALUE eio)
{
    return INT2NUM(eio_pipe_r_fd);
}

/*
 *  call-seq:
 *     EIO.max_poll_time = x                 =>  Fixnum
 *
 *  Set the maximum amount of time spent in each eio_poll() invocation
 *
 */
static VALUE
rb_eio_s_set_max_poll_time(VALUE eio, VALUE seconds)
{
    eio_set_max_poll_time(FIX2LONG(seconds));
    return seconds;
}

/*
 *  call-seq:
 *     EIO.max_poll_reqs = x                 =>  Fixnum
 *
 *  Set the maximum number of requests by each eio_poll() invocation
 *
 */
static VALUE
rb_eio_s_set_max_poll_reqs(VALUE eio, VALUE requests)
{
    eio_set_max_poll_reqs(FIX2INT(requests));
    return requests;
}

/*
 *  call-seq:
 *     EIO.min_parallel = x                 =>  Fixnum
 *
 *  Set the minimum number of libeio threads to run in parallel. default: 8
 *
 */
static VALUE
rb_eio_s_set_min_parallel(VALUE eio, VALUE threads)
{
    eio_set_min_parallel(FIX2INT(threads));
    return threads;
}

/*
 *  call-seq:
 *     EIO.max_parallel = x                 =>  Fixnum
 *
 *  Set the maximum number of AIO threads to run in parallel. default: 8
 *
 */
static VALUE
rb_eio_s_set_max_parallel(VALUE eio, VALUE threads)
{
    eio_set_max_parallel(FIX2INT(threads));
    return threads;
}

/*
 *  call-seq:
 *     EIO.max_idle = x                 =>  Fixnum
 *
 *  Limit the number of threads allowed to be idle
 *
 */
static VALUE
rb_eio_s_set_max_idle(VALUE eio, VALUE threads)
{
    eio_set_max_idle(FIX2INT(threads));
    return threads;
}

/*
 *  call-seq:
 *     EIO.idle_timeout = x                 =>  Fixnum
 *
 *  Set the minimum idle timeout before a thread is allowed to exit
 *
 */
static VALUE
rb_eio_s_set_idle_timeout(VALUE eio, VALUE seconds)
{
    eio_set_idle_timeout(FIX2INT(seconds));
    return seconds;
}

/*
 *  call-seq:
 *     EIO.open('/path/file'){|fd| p fd }                    =>  EIO::Request
 *
 *  Asynchronously open or create a file and call the callback with a newly created file handle
 *  for the file.
 *
 * === Examples
 *     EIO.open('/path/file', EIO::RDONLY){|fd| p fd }       =>  EIO::Request
 *     EIO.open('/path/file', EIO::RDWR, 0777){|fd| p fd }   =>  EIO::Request
 *     cb = Proc.new{|fd| p fd }
 *     EIO.open('/path/file', EIO::RDWR, 0777, cb)           =>  EIO::Request
 *
 *     EIO.open('/path/file')                                =>  Fixnum
 *     EIO.open('/path/file', EIO::RDWR)                     =>  Fixnum
 *     EIO.open('/path/file', EIO::RDWR, 0777)               =>  Fixnum
 *
 */
static VALUE
rb_eio_s_open(int argc, VALUE *argv, VALUE eio)
{
    int ret, fd;
    VALUE path, flags, mode, proc, cb;
    rb_scan_args(argc, argv, "13&", &path, &flags, &mode, &proc, &cb);
    AssertCallback(cb, 1);
    Check_Type(path, T_STRING);
    if (NIL_P(flags)) flags = INT2NUM(O_RDONLY);
    Check_Type(flags, T_FIXNUM);
    if (NIL_P(mode)) mode = eio_default_mode;
    Check_Type(mode, T_FIXNUM);
    SyncRequest({
        fd = open(StringValueCStr(path), FIX2INT(flags), FIX2INT(mode));
        if (fd < 0) rb_sys_fail("open");
        CloseOnExec(fd);
        return INT2NUM(fd);
    });
    AsyncRequest(open, rb_eio_open_cb, StringValueCStr(path), FIX2INT(flags), FIX2INT(mode));
}

/*
 *  call-seq:
 *     EIO.close(fd){ p :closed }  =>  EIO::Request
 *
 *  Asynchronously close a file and call the callback with the result code.
 *
 * === Examples
 *     cb = Proc.new{ p :closed }
 *     EIO.close(fd, cb)           =>  EIO::Request
 *
 *     EIO.close(fd)               =>  Fixnum
 *
 */
static VALUE
rb_eio_s_close(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE fd, proc, cb;
    rb_scan_args(argc, argv, "11&", &fd, &proc, &cb);
    AssertCallback(cb, NO_CB_ARGS);
    Check_Type(fd, T_FIXNUM);
    SubmitRequest(close, rb_eio_generic_cb, FIX2INT(fd));
}

/*
 *  call-seq:
 *     EIO.fsync(fd){ p :synced }  =>  EIO::Request
 *
 *  Asynchronously call fsync on the given filehandle and call the callback with the result
 *  code.
 *
 * === Examples
 *     cb = Proc.new{ p :synced }
 *     EIO.fsync(fd, cb)           =>  EIO::Request
 *
 *     EIO.fsync(fd)               =>  Fixnum
 *
 */
static VALUE
rb_eio_s_fsync(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE fd, proc, cb;
    rb_scan_args(argc, argv, "11&", &fd, &proc, &cb);
    AssertCallback(cb, NO_CB_ARGS);
    Check_Type(fd, T_FIXNUM);
    SubmitRequest(fsync, rb_eio_generic_cb, FIX2INT(fd));
}

/*
 *  call-seq:
 *     EIO.fdatasync(fd){ p :synced }  =>  EIO::Request
 *
 *  Asynchronously call fdatasync on the given filehandle and call the callback with the result
 *  code.
 *
 * === Examples
 *     cb = Proc.new{ p :synced }
 *     EIO.fdatasync(fd, cb)           =>  EIO::Request
 *
 *     EIO.fdatasync(fd)               =>  Fixnum
 *
 */
static VALUE
rb_eio_s_fdatasync(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE fd, proc, cb;
    rb_scan_args(argc, argv, "11&", &fd, &proc, &cb);
    AssertCallback(cb, NO_CB_ARGS);
    Check_Type(fd, T_FIXNUM);
    SyncRequest({
#if HAVE_FDATASYNC
       ret = fdatasync(FIX2INT(fd));
#else
       ret = fsync(FIX2INT(fd));
#endif
       if (ret == -1) rb_sys_fail("fdatasync");
       return INT2NUM(ret);
    });
    AsyncRequest(fdatasync, rb_eio_generic_cb, FIX2INT(fd));
}

/*
 *  call-seq:
 *     EIO.read(fd){|d| p d }           =>  EIO::Request
 *
 *  Asynchronously reads length bytes from a specified offset into a buffer.
 *
 * === Examples
 *     EIO.read(fd, 100){|d| p d }      =>  EIO::Request
 *     EIO.read(fd, 100, 50){|d| p d }  =>  EIO::Request
 *     cb = Proc.new{|d| p d }
 *     EIO.read(fd, 100, 50, cb)        =>  EIO::Request
 *
 *     EIO.read(fd)                     =>  String
 *     EIO.read(fd, 100)                =>  String
 *     EIO.read(fd, 100, 50)            =>  String
 *
 */
static VALUE
rb_eio_s_read(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE fd, len, offset, proc, cb;
    VALUE buf;
    rb_scan_args(argc, argv, "13&", &fd, &len, &offset, &proc, &cb);
    AssertCallback(cb, 1);
    Check_Type(fd, T_FIXNUM);
    if (NIL_P(len)) len = eio_default_bufsize;
    Check_Type(len, T_FIXNUM);
    if (len == eio_zero) len = eio_default_bufsize;
    if (NIL_P(offset)) offset = eio_zero;
    Check_Type(offset, T_FIXNUM);
    SyncRequest({
        buf = rb_str_new(0, FIX2INT(len));
        if (offset == eio_zero){
            ret = read(FIX2INT(fd), RSTRING_PTR(buf), FIX2INT(len));
        } else {
            ret = pread(FIX2INT(fd), RSTRING_PTR(buf), FIX2INT(len), FIX2INT(offset));
        }
        if (ret == -1) rb_sys_fail("read");
        return buf;
    });
    AsyncRequest(read, rb_eio_read_cb, FIX2INT(fd), 0, FIX2INT(len), FIX2INT(offset));
}

/*
 *  call-seq:
 *     EIO.readahead(fd){|d| p :read }        =>  EIO::Request
 *
 *  Populates the page cache with data from a file so that subsequent reads from that file will
 *  not block on disk I/O.
 *
 * === Examples
 *     EIO.readahead(fd, 100){|d| p :read }   =>  EIO::Request
 *     EIO.readahead(fd, 100, 50){ p :read }  =>  EIO::Request
 *     cb = Proc.new{ p :read }
 *     EIO.readahead(fd, 100, 50, cb)         =>  EIO::Request
 *
 *     EIO.readahead(fd)                      =>  Fixnum
 *     EIO.readahead(fd, 100)                 =>  Fixnum
 *     EIO.readahead(fd, 100, 50)             =>  Fixnum
 *
 */
static VALUE
rb_eio_s_readahead(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE fd, len, offset, proc, cb;
    rb_scan_args(argc, argv, "13&", &fd, &len, &offset, &proc, &cb);
    AssertCallback(cb, NO_CB_ARGS);
    Check_Type(fd, T_FIXNUM);
    if (NIL_P(len)) len = eio_default_bufsize;
    Check_Type(len, T_FIXNUM);
    if (len == eio_zero) len = eio_default_bufsize;
    if (NIL_P(offset)) offset = eio_zero;
    Check_Type(offset, T_FIXNUM);
    SubmitRequest(readahead, rb_eio_generic_cb, FIX2INT(fd), FIX2INT(offset), FIX2INT(len));
}

/*
 *  call-seq:
 *     EIO.write(fd, buf){|b| p b }           =>  EIO::Request
 *
 *  Asynchronously writes length bytes from a specified offset into a buffer.
 *
 * === Examples
 *     EIO.write(fd, buf, 100){|b| p b }      =>  EIO::Request
 *     EIO.write(fd, buf, 100, 50){|b| p b }  =>  EIO::Request
 *     cb = Proc.new{|b| p b }
 *     EIO.write(fd, buf, 100, 50, cb)        =>  EIO::Request
 *
 *     EIO.write(fd, buf)                     =>  Fixnum
 *     EIO.write(fd, buf, 100)                =>  Fixnum
 *     EIO.write(fd, buf, 100, 50)            =>  Fixnum
 *
 */
static VALUE
rb_eio_s_write(int argc, VALUE *argv, VALUE eio)
{
    int ret, i_len, i_offset;
    VALUE fd, buf, len, offset, proc, cb, buf_len;
    rb_scan_args(argc, argv, "23&", &fd, &buf, &len, &offset, &proc, &cb);
    AssertCallback(cb, 1);
    Check_Type(fd, T_FIXNUM);
    Check_Type(buf, T_STRING);
    if (NIL_P(len)) len = INT2NUM(RSTRING_LEN(buf));
    Check_Type(len, T_FIXNUM);
    if (NIL_P(offset)) offset = eio_zero;
    Check_Type(offset, T_FIXNUM);
    i_offset = FIX2INT(offset);
    i_len = FIX2INT(len);
    if (i_offset >= RSTRING_LEN(buf)) rb_raise(rb_eArgError, "out of bounds offset");
    if ((i_offset + i_len) > RSTRING_LEN(buf)) rb_raise(rb_eArgError, "length extends beyond buffer");
    SyncRequest({
        if (offset == eio_zero){
            ret = write(FIX2INT(fd), StringValueCStr(buf), i_len);
        } else {
            ret = pwrite(FIX2INT(fd), StringValueCStr(buf), i_len, i_offset);
        }
        if (ret == -1) rb_sys_fail("write");
        return INT2NUM(ret);
    });
    AsyncRequest(write, rb_eio_write_cb, FIX2INT(fd), StringValueCStr(buf), i_len, i_offset);
}

/*
 *  call-seq:
 *     EIO.sendfile(in_fd, out_fd){|b| p b }           =>  EIO::Request
 *
 *  Tries to copy length bytes from in fd to out fd, starting at a given offset.
 *
 * === Examples
 *     EIO.sendfile(in_fd, out_fd, 100){|b| p b }      =>  EIO::Request
 *     EIO.sendfile(in_fd, out_fd, 100, 50){|b| p b }  =>  EIO::Request
 *     cb = Proc.new{|b| p b }
 *     EIO.sendfile(in_fd, out_fd, 100, 50, cb)        =>  EIO::Request
 *
 *     EIO.sendfile(in_fd, out_fd)                     =>  Fixnum
 *     EIO.sendfile(in_fd, out_fd, 100)                =>  Fixnum
 *     EIO.sendfile(in_fd, out_fd, 100, 50)            =>  Fixnum
 *
 */
static VALUE
rb_eio_s_sendfile(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE out_fd, in_fd, offset, len, proc, cb;
    rb_scan_args(argc, argv, "23&", &out_fd, &in_fd, &offset, &len, &proc, &cb);
    AssertCallback(cb, 1);
    Check_Type(in_fd, T_FIXNUM);
    Check_Type(out_fd, T_FIXNUM);
    if (NIL_P(len)) len = eio_default_bufsize;
    Check_Type(len, T_FIXNUM);
    if (len == eio_zero) len = eio_default_bufsize;
    if (NIL_P(offset)) offset = eio_zero;
    Check_Type(offset, T_FIXNUM);
    SubmitRequest(sendfile, rb_eio_write_cb, FIX2INT(out_fd), FIX2INT(in_fd), FIX2INT(offset), FIX2INT(len));
}

/*
 *  call-seq:
 *     EIO.readdir('/path'){|fs| p fs }   =>  EIO::Request
 *
 *  Unlike the POSIX call of the same name, aio_readdir reads an entire directory (i.e.
 *  opendir + readdir + closedir). The entries will not be sorted, and will NOT include the
 *  . and .. entries.
 *
 * === Examples
 *     cb = Proc.new{|b| p b }
 *     EIO.readdir('/path', cb)           =>  EIO::Request
 *
 *     EIO.readdir('/path')               =>  Array
 *
 */
static VALUE
rb_eio_s_readdir(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE path, proc, cb;
    VALUE files;
    char *name;
    struct dirent *ent;
    rb_scan_args(argc, argv, "11&", &path, &proc, &cb);
    AssertCallback(cb, 1);
    Check_Type(path, T_STRING);
    SyncRequest({
        DIR *dir = opendir(StringValueCStr(path));
        if (!dir) rb_sys_fail(StringValueCStr(path));

        files = rb_ary_new();

        while ((ent = readdir(dir))) {
          name = ent->d_name;
          if (name[0] != '.' || (name[1] && (name[1] != '.' || name[2]))) {
              rb_ary_push(files, rb_str_new2(name));
          }
        }
        ret = closedir(dir);
        if (ret == -1) rb_sys_fail("closedir");
        return(files);
    });
    AsyncRequest(readdir, rb_eio_readdir_cb, StringValueCStr(path), EIO_READDIR_STAT_ORDER);
}

/*
 *  call-seq:
 *     EIO.mkdir('/path'){ p :created }         =>  EIO::Request
 *
 *  Asynchronously mkdir (create) a directory and call the callback with the result code.
 *
 * === Examples
 *     EIO.mkdir('/path', 0777){ p :created }   =>  EIO::Request
 *     cb = Proc.new{ p :created }
 *     EIO.mkdir('/path', 0777, cb)             =>  EIO::Request
 *
 *     EIO.mkdir('/path')                       =>  Fixnum
 *     EIO.mkdir('/path', 0777)                 =>  Fixnum
 *
 */
static VALUE
rb_eio_s_mkdir(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE path, mode, proc, cb;
    rb_scan_args(argc, argv, "12&", &path, &mode, &proc, &cb);
    AssertCallback(cb, NO_CB_ARGS);
    Check_Type(path, T_STRING);
    if (NIL_P(mode)) mode = eio_default_mode;
    Check_Type(mode, T_FIXNUM);
    SubmitRequest(mkdir, rb_eio_generic_cb, StringValueCStr(path), FIX2INT(mode));
}

/*
 *  call-seq:
 *     EIO.rmdir('/path'){ p :removed }   =>  EIO::Request
 *
 *  Asynchronously rmdir (delete) a directory and call the callback with the result code.
 *
 * === Examples
 *     cb = Proc.new{ p :removed }
 *     EIO.rmdir('/path', cb)             =>  EIO::Request
 *
 *     EIO.rmdir('/path')                 =>  Fixnum
 *
 */
static VALUE
rb_eio_s_rmdir(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE path, proc, cb;
    rb_scan_args(argc, argv, "11&", &path, &proc, &cb);
    AssertCallback(cb, NO_CB_ARGS);
    Check_Type(path, T_STRING);
    SubmitRequest(rmdir, rb_eio_generic_cb, StringValueCStr(path));
}

/*
 *  call-seq:
 *     EIO.unlink('/path/file'){ p :removed }   =>  EIO::Request
 *
 *  Asynchronously unlink (delete) a file and call the callback with the result code.
 *
 * === Examples
 *     cb = Proc.new{ p :removed }
 *     EIO.unlink('/path/file', cb)             =>  EIO::Request
 *
 *     EIO.unlink('/path/file')                 =>  Fixnum
 *
 */
static VALUE
rb_eio_s_unlink(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE path, proc, cb;
    rb_scan_args(argc, argv, "11&", &path, &proc, &cb);
    AssertCallback(cb, NO_CB_ARGS);
    Check_Type(path, T_STRING);
    SubmitRequest(unlink, rb_eio_generic_cb, StringValueCStr(path));
}

/*
 *  call-seq:
 *     EIO.readlink('/path/link'){|l| p l }   =>  EIO::Request
 *
 *  Asynchronously read the symlink specified by path and pass it to the callback.
 *
 * === Examples
 *     cb = Proc.new{|l| p l }
 *     EIO.readlink('/path/link', cb)         =>  EIO::Request
 *
 *     EIO.readlink('/path/link')             =>  Fixnum
 *
 */
static VALUE
rb_eio_s_readlink(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE path, proc, cb;
    rb_scan_args(argc, argv, "11&", &path, &proc, &cb);
    AssertCallback(cb, 1);
    Check_Type(path, T_STRING);
    SyncRequest({
        return rb_funcall(rb_cFile, sym_readlink, 1, path);
    });
    AsyncRequest(readlink, rb_eio_read_cb, StringValueCStr(path));
}

/*
 *  call-seq:
 *     EIO.stat('/path/file'){|s| p s }   =>  EIO::Request
 *
 *  Works like Ruby's stat. The callback will be called after the stat.
 *
 * === Examples
 *     cb = Proc.new{|s| p s }
 *     EIO.stat('/path/file', cb)         =>  EIO::Request
 *
 *     EIO.stat('/path/file')             =>  File::Stat
 *
 */
static VALUE
rb_eio_s_stat(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE path, proc, cb;
    rb_scan_args(argc, argv, "11&", &path, &proc, &cb);
    AssertCallback(cb, 1);
    Check_Type(path, T_STRING);
    SyncRequest({
        return rb_funcall(rb_cFile, sym_stat, 1, path);
    });
    AsyncRequest(stat, rb_eio_stat_cb, StringValueCStr(path));
}

/*
 *  call-seq:
 *     EIO.rename('/path/a', '/path/b'){ p :renamed }  =>  EIO::Request
 *
 *  Asynchronously rename the object at source path to destination path
 *
 * === Examples
 *     cb = Proc.new{ p :renamed }
 *     EIO.rename('/path/a', '/path/b', cb)            =>  EIO::Request
 *
 *     EIO.rename('/path/a', '/path/b')                =>  Fixnum
 *
 */
static VALUE
rb_eio_s_rename(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE path, new_path, proc, cb;
    rb_scan_args(argc, argv, "21&", &path, &new_path, &proc, &cb);
    AssertCallback(cb, NO_CB_ARGS);
    Check_Type(path, T_STRING);
    Check_Type(new_path, T_STRING);
    SubmitRequest(rename, rb_eio_generic_cb, StringValueCStr(path), StringValueCStr(new_path));
}

/*
 *  call-seq:
 *     EIO.chmod('/path/file'){ p :chmodded }        =>  EIO::Request
 *
 *  Asynchronously change permissions for a given file path.
 *
 * === Examples
 *     EIO.chmod('/path/file', 0777){ p :chmodded }  =>  EIO::Request
 *     cb = Proc.new{ p :chmodded }
 *     EIO.chmod('/path/file', 0777, cb)             =>  EIO::Request
 *
 *     EIO.chmod('/path/file')                       =>  Fixnum
 *     EIO.chmod('/path/file', 0777)                 =>  Fixnum
 *
 */
static VALUE
rb_eio_s_chmod(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE path, mode, proc, cb;
    rb_scan_args(argc, argv, "12&", &path, &mode, &proc, &cb);
    AssertCallback(cb, NO_CB_ARGS);
    Check_Type(path, T_STRING);
    if (NIL_P(mode)) mode = eio_default_mode;
    Check_Type(mode, T_FIXNUM);
    SubmitRequest(chmod, rb_eio_generic_cb, StringValueCStr(path), FIX2INT(mode));
}

/*
 *  call-seq:
 *     EIO.fchmod(fd){ p :chmodded }        =>  EIO::Request
 *
 *  Asynchronously change ownership for a given file descriptor.
 *
 * === Examples
 *     EIO.fchmod(fd, 0777){ p :chmodded }  =>  EIO::Request
 *     cb = Proc.new{ p :chmodded }
 *     EIO.fchmod(fd, 0777, cb)             =>  EIO::Request
 *
 *     EIO.fchmod(fd)                       =>  Fixnum
 *     EIO.fchmod(fd, 0777)                 =>  Fixnum
 *
 */
static VALUE
rb_eio_s_fchmod(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE fd, mode, proc, cb;
    rb_scan_args(argc, argv, "12&", &fd, &mode, &proc, &cb);
    AssertCallback(cb, NO_CB_ARGS);
    Check_Type(fd, T_FIXNUM);
    if (NIL_P(mode)) mode = eio_default_mode;
    Check_Type(mode, T_FIXNUM);
    SubmitRequest(fchmod, rb_eio_generic_cb, FIX2INT(fd), FIX2INT(mode));
}

/*
 *  call-seq:
 *     EIO.truncate('/path/file'){ p :truncated }       =>  EIO::Request
 *
 *  Asynchronously truncates a given file path.
 *
 * === Examples
 *     EIO.truncate('/path/file', 100){ p :truncated }  =>  EIO::Request
 *     cb = Proc.new{ p :truncated }
 *     EIO.truncate('/path/file', 100, cb)              =>  EIO::Request
 *
 *     EIO.truncate('/path/file')                       =>  Fixnum
 *     EIO.truncate('/path/file', 100)                  =>  Fixnum
 *
 */
static VALUE
rb_eio_s_truncate(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE path, offset, proc, cb;
    rb_scan_args(argc, argv, "12&", &path, &offset, &proc, &cb);
    AssertCallback(cb, NO_CB_ARGS);
    Check_Type(path, T_STRING);
    if (NIL_P(offset)) offset = eio_zero;
    Check_Type(offset, T_FIXNUM);
    SubmitRequest(truncate, rb_eio_generic_cb, StringValueCStr(path), FIX2INT(offset));
}

/*
 *  call-seq:
 *     EIO.ftruncate(fd){ p :truncated }       =>  EIO::Request
 *
 *  Asynchronously truncates a given file descriptor.
 *
 * === Examples
 *     EIO.ftruncate(fd, 100){ p :truncated }  =>  EIO::Request
 *     cb = Proc.new{ p :truncated }
 *     EIO.ftruncate(fd, 100, cb)              =>  EIO::Request
 *
 *     EIO.ftruncate(fd)                       =>  Fixnum
 *     EIO.ftruncate(fd, 100)                  =>  Fixnum
 *
 */
static VALUE
rb_eio_s_ftruncate(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE fd, offset, proc, cb;
    rb_scan_args(argc, argv, "12&", &fd, &offset, &proc, &cb);
    AssertCallback(cb, NO_CB_ARGS);
    Check_Type(fd, T_FIXNUM);
    if (NIL_P(offset)) offset = eio_zero;
    Check_Type(offset, T_FIXNUM);
    SubmitRequest(ftruncate, rb_eio_generic_cb, FIX2INT(fd), FIX2INT(offset));
}

/*
 *  call-seq:
 *     EIO.chown('/path/file'){ p :chowned }            =>  EIO::Request
 *
 *  Asynchronously changes ownership for a given file path.
 *
 * === Examples
 *     EIO.chown('/path/file', 500){ p :chowned }       =>  EIO::Request
 *     EIO.chown('/path/file', 500, 500){ p :chowned }  =>  EIO::Request
 *     cb = Proc.new{ p :chowned }
 *     EIO.chown('/path/file', 500, 500, cb)            =>  EIO::Request
 *
 *     EIO.chown('/path/file', 500)                     =>  Fixnum
 *     EIO.chown('/path/file', 500, 500)                =>  Fixnum
 *
 */
static VALUE
rb_eio_s_chown(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE path, uid, gid, proc, cb;
    rb_scan_args(argc, argv, "13&", &path, &uid, &gid, &proc, &cb);
    AssertCallback(cb, NO_CB_ARGS);
    Check_Type(path, T_STRING);
    if (NIL_P(uid)) uid = INT2NUM(getuid());
    Check_Type(uid, T_FIXNUM);
    if (NIL_P(gid)) gid = INT2NUM(getgid());
    Check_Type(gid, T_FIXNUM);
    SubmitRequest(chown, rb_eio_generic_cb, StringValueCStr(path), FIX2INT(uid), FIX2INT(gid));
}

/*
 *  call-seq:
 *     EIO.fchown(fd){ p :chowned }            =>  EIO::Request
 *
 *  Asynchronously changes ownership for a given file descriptor.
 *
 * === Examples
 *     EIO.fchown(fd, 500){ p :chowned }       =>  EIO::Request
 *     EIO.fchown(fd, 500, 500){ p :chowned }  =>  EIO::Request
 *     cb = Proc.new{ p :chowned }
 *     EIO.fchown(fd, 500, 500, cb)            =>  EIO::Request
 *
 *     EIO.fchown(fd, 500)                     =>  Fixnum
 *     EIO.fchown(fd, 500, 500)                =>  Fixnum
 *
 */
static VALUE
rb_eio_s_fchown(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE fd, uid, gid, proc, cb;
    rb_scan_args(argc, argv, "13&", &fd, &uid, &gid, &proc, &cb);
    AssertCallback(cb, NO_CB_ARGS);
    Check_Type(fd, T_FIXNUM);
    if (NIL_P(uid)) uid = INT2NUM(getuid());
    Check_Type(uid, T_FIXNUM);
    if (NIL_P(gid)) gid = INT2NUM(getgid());
    Check_Type(gid, T_FIXNUM);
    SubmitRequest(fchown, rb_eio_generic_cb, FIX2INT(fd), FIX2INT(uid), FIX2INT(gid));
}

/*
 *  call-seq:
 *     EIO.link('/path/a', '/path/b'){ p :linked }  =>  EIO::Request
 *
 *  Asynchronously create a new link to the existing object at source path at the destination
 *  path and call the callback with the result code.
 *
 * === Examples
 *     cb = Proc.new{ p :linked }
 *     EIO.link('/path/a', '/path/b', cb)           =>  EIO::Request
 *
 *     EIO.link('/path/a', '/path/b')               =>  Fixnum
 *
 */
static VALUE
rb_eio_s_link(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE path, new_path, proc, cb;
    rb_scan_args(argc, argv, "21&", &path, &new_path, &proc, &cb);
    AssertCallback(cb, NO_CB_ARGS);
    Check_Type(path, T_STRING);
    Check_Type(new_path, T_STRING);
    SubmitRequest(link, rb_eio_generic_cb, StringValueCStr(path), StringValueCStr(new_path));
}

/*
 *  call-seq:
 *     EIO.symlink('/path/a', '/path/b'){ p :linked }  =>  EIO::Request
 *
 *  Asynchronously create a new symbolic link to the existing object at sourc path at the
 *  destination path and call the callback with the result code.
 *
 * === Examples
 *     cb = Proc.new{ p :linked }
 *     EIO.symlink('/path/a', '/path/b', cb)           =>  EIO::Request
 *
 *     EIO.symlink('/path/a', '/path/b')               =>  Fixnum
 *
 */
static VALUE
rb_eio_s_symlink(int argc, VALUE *argv, VALUE eio)
{
    int ret;
    VALUE path, new_path, proc, cb;
    rb_scan_args(argc, argv, "21&", &path, &new_path, &proc, &cb);
    AssertCallback(cb, NO_CB_ARGS);
    Check_Type(path, T_STRING);
    Check_Type(new_path, T_STRING);
    SubmitRequest(symlink, rb_eio_generic_cb, StringValueCStr(path), StringValueCStr(new_path));
}

/*
 * Mark an EIO::Request instance
 */
static void
rb_eio_mark_request(void *ptr)
{
}

/*
 * Free an EIO::Request instance
 */
static void
rb_eio_free_request(void *ptr)
{
}

/*
 *  Wraps an eio_req struct
 */
static VALUE
rb_eio_wrap_request(eio_req *r)
{
    VALUE obj;
    obj = Data_Wrap_Struct(cEioReq, rb_eio_mark_request, rb_eio_free_request, r);
    rb_obj_call_init(obj, 0, NULL);
    return obj;
}

/*
 *  call-seq:
 *     req.errno  =>  Fixnum
 *
 *  Request error number, if any.
 *
 */
static VALUE
rb_eio_req_errno(VALUE obj)
{
    GetRequest(obj);
    return INT2NUM(req->errorno);
}

/*
 *  call-seq:
 *     req.type  =>  Fixnum
 *
 *  Request type
 *
 */
static VALUE
rb_eio_req_type(VALUE obj)
{
    GetRequest(obj);
    return INT2NUM(req->type);
}

/*
 *  call-seq:
 *     req.priority  =>  Fixnum
 *
 *  Request priority
 *
 */
static VALUE
rb_eio_req_priority(VALUE obj)
{
    GetRequest(obj);
    return INT2NUM(req->pri);
}

/*
 *  call-seq:
 *     req.cancel  =>  nil
 *
 *  Attempt to cancel an in flight libeio request - no guarantees.
 *
 */
static VALUE
rb_eio_req_cancel(VALUE obj)
{
    GetRequest(obj);
    eio_cancel(req);
    return Qnil;
}

/*
 *  call-seq:
 *     req.complete?  =>  Boolean
 *
 *  True if the Ruby callback for this request already fired.
 *
 */
static VALUE
rb_eio_req_complete_p(VALUE obj)
{
    GetRequest(obj);
    return (req->complete == 1) ? Qtrue : Qfalse;
}

/*
 * Get the fd from a given I/O instance
 */
static int
rb_eio_pipe_fd(VALUE io)
{
    int fd;
    rb_io_t *fptr;
    GetOpenFile(io, fptr);
#ifdef RUBY_VM
    fd = fptr->fd;
#else
    fd = fileno(fptr->f);
#endif
    assert(fcntl(fd, F_SETFD, FD_CLOEXEC) != -1);
    return fd;
}

/*
 * create the libeio notify pipe
 */
static void
rb_eio_create_pipe(void)
{
    DONT_GC(eio_pipe);
    eio_pipe = rb_funcall(rb_cIO, sym_pipe, 0);

    DONT_GC(pipe_r_fd);
    pipe_r_fd = rb_ary_entry(eio_pipe, 0);

    DONT_GC(pipe_w_fd);
    pipe_w_fd = rb_ary_entry(eio_pipe, 1);

    rb_ivar_set(mEio, sym_pipe, eio_pipe);
    rb_ivar_set(mEio, sym_pipe_r_fd, pipe_r_fd);
    rb_ivar_set(mEio, sym_pipe_w_fd, pipe_w_fd);

    eio_pipe_r_fd = rb_eio_pipe_fd(pipe_r_fd);
    eio_pipe_w_fd = rb_eio_pipe_fd(pipe_w_fd);
}

/* recreate the libeio notify pipe */
static void
rb_eio_recreate_pipe(void)
{
    rb_eio_create_pipe();
}

/* recreate the libeio notify pipe on fork */
static void
rb_eio_atfork_child(void)
{
    rb_eio_recreate_pipe();
}

static void
rb_eio_atfork_prepare(void)
{
}

static void
rb_eio_atfork_parent(void)
{
}

void
Init_eio_ext()
{
    /* Initializes libeio */
    if (eio_init(want_poll, done_poll) < 0) rb_sys_fail("EIO init failed!");

    mEio = rb_define_module("EIO");

    /* Init symbols ahead of time */
    sym_call = rb_intern("call");
    sym_arity = rb_intern("arity");
    sym_pipe = rb_intern("pipe");
    sym_readlink = rb_intern("readlink");
    sym_stat = rb_intern("stat");
    sym_pipe_r_fd = rb_intern("pipe_r_fd");
    sym_pipe_w_fd = rb_intern("pipe_w_fd");

    /* Common fixnum defaults */
    eio_default_mode = INT2NUM(0777);
    eio_zero = INT2NUM(0);
    eio_default_bufsize = INT2NUM(BUFSIZ);

    /* Setup a communication pipe between libeio and other I/O frameworks */
    rb_eio_create_pipe();

    /* Recreate pipe on fork */
    X_THREAD_ATFORK(rb_eio_atfork_prepare, rb_eio_atfork_parent, rb_eio_atfork_child);

    rb_define_const(mEio, "PRI_MIN", INT2NUM(EIO_PRI_MIN));
    rb_define_const(mEio, "PRI_MAX", INT2NUM(EIO_PRI_MAX));
    rb_define_const(mEio, "PRI_DEFAULT", INT2NUM(EIO_PRI_DEFAULT));

    rb_define_const(mEio, "RDONLY", INT2NUM(O_RDONLY));
    rb_define_const(mEio, "WRONLY", INT2NUM(O_WRONLY));
    rb_define_const(mEio, "RDWR", INT2NUM(O_RDWR));
    rb_define_const(mEio, "APPEND", INT2NUM(O_APPEND));
    rb_define_const(mEio, "CREAT", INT2NUM(O_CREAT));
    rb_define_const(mEio, "EXCL", INT2NUM(O_EXCL));

    rb_define_module_function(mEio, "poll", rb_eio_s_poll, 0);
    rb_define_module_function(mEio, "wait", rb_eio_s_wait, 0);
    rb_define_module_function(mEio, "requests", rb_eio_s_requests, 0);
    rb_define_module_function(mEio, "ready", rb_eio_s_ready, 0);
    rb_define_module_function(mEio, "pending", rb_eio_s_pending, 0);
    rb_define_module_function(mEio, "threads", rb_eio_s_threads, 0);
    rb_define_module_function(mEio, "fd", rb_eio_s_fd, 0);

    rb_define_module_function(mEio, "max_poll_time=", rb_eio_s_set_max_poll_time, 1);
    rb_define_module_function(mEio, "max_poll_reqs=", rb_eio_s_set_max_poll_reqs, 1);
    rb_define_module_function(mEio, "min_parallel=", rb_eio_s_set_min_parallel, 1);
    rb_define_module_function(mEio, "max_parallel=", rb_eio_s_set_max_parallel, 1);
    rb_define_module_function(mEio, "max_idle=", rb_eio_s_set_max_idle, 1);
    rb_define_module_function(mEio, "idle_timeout=", rb_eio_s_set_idle_timeout, 1);

    rb_define_module_function(mEio, "fsync", rb_eio_s_fsync, -1);
    rb_define_module_function(mEio, "fdatasync", rb_eio_s_fdatasync, -1);
    rb_define_module_function(mEio, "open", rb_eio_s_open, -1);
    rb_define_module_function(mEio, "close", rb_eio_s_close, -1);
    rb_define_module_function(mEio, "read", rb_eio_s_read, -1);
    rb_define_module_function(mEio, "readahead", rb_eio_s_readahead, -1);
    rb_define_module_function(mEio, "write", rb_eio_s_write, -1);
    rb_define_module_function(mEio, "sendfile", rb_eio_s_sendfile, -1);
    rb_define_module_function(mEio, "mkdir", rb_eio_s_mkdir, -1);
    rb_define_module_function(mEio, "rmdir", rb_eio_s_rmdir, -1);
    rb_define_module_function(mEio, "unlink", rb_eio_s_unlink, -1);
    rb_define_module_function(mEio, "rename", rb_eio_s_rename, -1);
    rb_define_module_function(mEio, "chmod", rb_eio_s_chmod, -1);
    rb_define_module_function(mEio, "fchmod", rb_eio_s_fchmod, -1);
    rb_define_module_function(mEio, "truncate", rb_eio_s_truncate, -1);
    rb_define_module_function(mEio, "ftruncate", rb_eio_s_ftruncate, -1);
    rb_define_module_function(mEio, "chown", rb_eio_s_chown, -1);
    rb_define_module_function(mEio, "fchown", rb_eio_s_fchown, -1);
    rb_define_module_function(mEio, "link", rb_eio_s_link, -1);
    rb_define_module_function(mEio, "readlink", rb_eio_s_readlink, -1);
    rb_define_module_function(mEio, "symlink", rb_eio_s_symlink, -1);
    rb_define_module_function(mEio, "readdir", rb_eio_s_readdir, -1);
    rb_define_module_function(mEio, "stat", rb_eio_s_stat, -1);

    cEioReq  = rb_define_class_under(mEio, "Request", rb_cObject);

    rb_define_method(cEioReq, "errno", rb_eio_req_errno, 0);
    rb_define_method(cEioReq, "type", rb_eio_req_type, 0);
    rb_define_method(cEioReq, "priority", rb_eio_req_priority, 0);
    rb_define_method(cEioReq, "cancel", rb_eio_req_cancel, 0);
    rb_define_method(cEioReq, "complete?", rb_eio_req_complete_p, 0);

    rb_define_const(cEioReq, "OPEN", INT2NUM(EIO_OPEN));
    rb_define_const(cEioReq, "CLOSE", INT2NUM(EIO_CLOSE));
    rb_define_const(cEioReq, "READ", INT2NUM(EIO_READ));
    rb_define_const(cEioReq, "WRITE", INT2NUM(EIO_WRITE));
    rb_define_const(cEioReq, "READAHEAD", INT2NUM(EIO_READAHEAD));
    rb_define_const(cEioReq, "SENDFILE", INT2NUM(EIO_SENDFILE));
    rb_define_const(cEioReq, "STAT", INT2NUM(EIO_STAT));
    rb_define_const(cEioReq, "TRUNCATE", INT2NUM(EIO_TRUNCATE));
    rb_define_const(cEioReq, "FTRUNCATE", INT2NUM(EIO_FTRUNCATE));
    rb_define_const(cEioReq, "CHMOD", INT2NUM(EIO_CHMOD));
    rb_define_const(cEioReq, "FCHMOD", INT2NUM(EIO_FCHMOD));
    rb_define_const(cEioReq, "CHOWN", INT2NUM(EIO_CHOWN));
    rb_define_const(cEioReq, "FCHOWN", INT2NUM(EIO_FCHOWN));
    rb_define_const(cEioReq, "SYNC", INT2NUM(EIO_SYNC));
    rb_define_const(cEioReq, "FSYNC", INT2NUM(EIO_FSYNC));
    rb_define_const(cEioReq, "FDATASYNC", INT2NUM(EIO_FDATASYNC));
    rb_define_const(cEioReq, "UNLINK", INT2NUM(EIO_UNLINK));
    rb_define_const(cEioReq, "RMDIR", INT2NUM(EIO_RMDIR));
    rb_define_const(cEioReq, "MKDIR", INT2NUM(EIO_MKDIR));
    rb_define_const(cEioReq, "RENAME", INT2NUM(EIO_RENAME));
    rb_define_const(cEioReq, "READDIR", INT2NUM(EIO_READDIR));
    rb_define_const(cEioReq, "LINK", INT2NUM(EIO_LINK));
    rb_define_const(cEioReq, "SYMLINK", INT2NUM(EIO_SYMLINK));
    rb_define_const(cEioReq, "READLINK", INT2NUM(EIO_READLINK));
} 