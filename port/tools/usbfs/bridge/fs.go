package main

// The phone's whole filesystem as a billy.Filesystem (what go-nfs serves), each call one or more
// usbfsd requests. Stats are kept for a second: Finder asks for the same ones over and over.

import (
	"encoding/binary"
	"errors"
	"io"
	"os"
	pathpkg "path"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/go-git/go-billy/v5"
	nfsfile "github.com/willscott/go-nfs/file"
)

const (
	opPing = 1 + iota
	opStat
	opReaddir
	opRead
	opWrite
	opCreate
	opMkdir
	opRemove
	opRename
	opTruncate
	opChmod
	opUtimes
	opSymlink
	opReadlink
	opStatfs
	opChown
	opLink
)

const maxIO = 1 << 20

// ---- requests ----

type req struct{ b []byte }

func (r *req) u32(v uint32) *req { r.b = binary.LittleEndian.AppendUint32(r.b, v); return r }
func (r *req) u64(v uint64) *req { r.b = binary.LittleEndian.AppendUint64(r.b, v); return r }
func (r *req) str(s string) *req { r.u32(uint32(len(s))); r.b = append(r.b, s...); return r }
func (r *req) raw(b []byte) *req { r.u32(uint32(len(b))); r.b = append(r.b, b...); return r }

type rd struct {
	b   []byte
	bad bool
}

func (r *rd) u32() uint32 {
	if len(r.b) < 4 {
		r.bad = true
		return 0
	}
	v := binary.LittleEndian.Uint32(r.b)
	r.b = r.b[4:]
	return v
}
func (r *rd) u64() uint64 {
	if len(r.b) < 8 {
		r.bad = true
		return 0
	}
	v := binary.LittleEndian.Uint64(r.b)
	r.b = r.b[8:]
	return v
}
func (r *rd) str() string {
	n := r.u32()
	if r.bad || int(n) > len(r.b) {
		r.bad = true
		return ""
	}
	s := string(r.b[:n])
	r.b = r.b[n:]
	return s
}

// ---- what a stat says ----

type info struct {
	name                         string
	mode, nlink, uid, gid        uint32
	size, ino                    uint64
	atime, mtime, ctime, btime   time.Time
}

func (i *info) Name() string       { return i.name }
func (i *info) Size() int64        { return int64(i.size) }
func (i *info) ModTime() time.Time { return i.mtime }
func (i *info) IsDir() bool        { return i.mode&syscall.S_IFMT == syscall.S_IFDIR }
func (i *info) Sys() any {
	return nfsfile.FileInfo{Nlink: i.nlink, UID: i.uid, GID: i.gid, Fileid: i.ino}
}
func (i *info) Mode() os.FileMode {
	m := os.FileMode(i.mode & 0777)
	switch i.mode & syscall.S_IFMT {
	case syscall.S_IFDIR:
		m |= os.ModeDir
	case syscall.S_IFLNK:
		m |= os.ModeSymlink
	case syscall.S_IFCHR:
		m |= os.ModeDevice | os.ModeCharDevice
	case syscall.S_IFBLK:
		m |= os.ModeDevice
	case syscall.S_IFIFO:
		m |= os.ModeNamedPipe
	case syscall.S_IFSOCK:
		m |= os.ModeSocket
	}
	if i.mode&syscall.S_ISUID != 0 {
		m |= os.ModeSetuid
	}
	if i.mode&syscall.S_ISGID != 0 {
		m |= os.ModeSetgid
	}
	if i.mode&syscall.S_ISVTX != 0 {
		m |= os.ModeSticky
	}
	return m
}

func readStat(r *rd, name string) *info {
	i := &info{name: name}
	i.mode, i.nlink, i.uid, i.gid = r.u32(), r.u32(), r.u32(), r.u32()
	i.size, i.ino = r.u64(), r.u64()
	ts := func() time.Time { s := int64(r.u64()); ns := int64(r.u32()); return time.Unix(s, ns) }
	i.atime, i.mtime, i.ctime = ts(), ts(), ts()
	i.btime = time.Unix(int64(r.u64()), 0)
	return i
}

// ---- the filesystem ----

type phoneFS struct {
	t     transport
	mu    sync.Mutex
	cache map[string]cached
}

type cached struct {
	i  *info
	at time.Time
}

func newPhoneFS(t transport) *phoneFS { return &phoneFS{t: t, cache: map[string]cached{}} }

func clean(p string) string { return pathpkg.Clean("/" + p) }

func perr(op, p string, e syscall.Errno) error { return &os.PathError{Op: op, Path: p, Err: e} }

// a request whose answer is only its errno
func (fs *phoneFS) do(op uint16, name string, r *req) ([]byte, error) {
	out, e, err := fs.t.call(op, r.b)
	if err != nil {
		return nil, &os.PathError{Op: "usbfs", Path: name, Err: err}
	}
	if e != 0 {
		return nil, perr("usbfs", name, e)
	}
	return out, nil
}

func (fs *phoneFS) forget(p string) {
	fs.mu.Lock()
	delete(fs.cache, p)
	delete(fs.cache, pathpkg.Dir(p))
	fs.mu.Unlock()
}

func (fs *phoneFS) remember(p string, i *info) {
	fs.mu.Lock()
	if len(fs.cache) > 100000 {
		fs.cache = map[string]cached{}
	}
	fs.cache[p] = cached{i, time.Now()}
	fs.mu.Unlock()
}

func (fs *phoneFS) Lstat(name string) (os.FileInfo, error) {
	p := clean(name)
	fs.mu.Lock()
	c, ok := fs.cache[p]
	fs.mu.Unlock()
	if ok && time.Since(c.at) < time.Second {
		return c.i, nil
	}
	out, err := fs.do(opStat, p, (&req{}).str(p))
	if err != nil {
		return nil, err
	}
	i := readStat(&rd{b: out}, pathpkg.Base(p))
	fs.remember(p, i)
	return i, nil
}

// Stat follows symbolic links (a few deep)
func (fs *phoneFS) Stat(name string) (os.FileInfo, error) {
	p := clean(name)
	for n := 0; n < 8; n++ {
		i, err := fs.Lstat(p)
		if err != nil || i.Mode()&os.ModeSymlink == 0 {
			return i, err
		}
		t, err := fs.Readlink(p)
		if err != nil {
			return nil, err
		}
		if !strings.HasPrefix(t, "/") {
			t = pathpkg.Join(pathpkg.Dir(p), t)
		}
		p = clean(t)
	}
	return nil, perr("stat", name, syscall.ELOOP)
}

func (fs *phoneFS) ReadDir(name string) ([]os.FileInfo, error) {
	p := clean(name)
	out, err := fs.do(opReaddir, p, (&req{}).str(p))
	if err != nil {
		return nil, err
	}
	r := &rd{b: out}
	n := r.u32()
	list := make([]os.FileInfo, 0, n)
	for k := uint32(0); k < n && !r.bad; k++ {
		nm := r.str()
		i := readStat(r, nm)
		if r.bad {
			break
		}
		fs.remember(pathpkg.Join(p, nm), i)
		list = append(list, i)
	}
	return list, nil
}

func (fs *phoneFS) OpenFile(name string, flag int, perm os.FileMode) (billy.File, error) {
	p := clean(name)
	if flag&os.O_CREATE != 0 {
		excl := uint32(0)
		if flag&os.O_EXCL != 0 {
			excl = 1
		}
		fs.forget(p)
		if _, err := fs.do(opCreate, p, (&req{}).str(p).u32(uint32(perm.Perm())).u32(excl)); err != nil {
			return nil, err
		}
	} else if _, err := fs.Lstat(p); err != nil {
		return nil, err
	}
	if flag&os.O_TRUNC != 0 {
		if err := fs.truncate(p, 0); err != nil {
			return nil, err
		}
	}
	f := &phoneFile{fs: fs, path: p, flag: flag}
	if flag&os.O_APPEND != 0 {
		if i, err := fs.Lstat(p); err == nil {
			f.pos = i.Size()
		}
	}
	return f, nil
}

func (fs *phoneFS) Create(name string) (billy.File, error) {
	return fs.OpenFile(name, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
}
func (fs *phoneFS) Open(name string) (billy.File, error) { return fs.OpenFile(name, os.O_RDONLY, 0) }

func (fs *phoneFS) Rename(from, to string) error {
	a, b := clean(from), clean(to)
	fs.forget(a)
	fs.forget(b)
	_, err := fs.do(opRename, a, (&req{}).str(a).str(b))
	return err
}

func (fs *phoneFS) Remove(name string) error {
	p := clean(name)
	fs.forget(p)
	_, err := fs.do(opRemove, p, (&req{}).str(p))
	return err
}

func (fs *phoneFS) Join(elem ...string) string { return pathpkg.Join(elem...) }

func (fs *phoneFS) TempFile(dir, prefix string) (billy.File, error) {
	for n := 0; n < 100; n++ {
		p := pathpkg.Join(clean(dir), prefix+time.Now().Format("150405.000000000"))
		f, err := fs.OpenFile(p, os.O_RDWR|os.O_CREATE|os.O_EXCL, 0600)
		if !errors.Is(err, os.ErrExist) {
			return f, err
		}
	}
	return nil, os.ErrExist
}

func (fs *phoneFS) MkdirAll(name string, perm os.FileMode) error {
	p := clean(name)
	if i, err := fs.Lstat(p); err == nil {
		if i.IsDir() {
			return nil
		}
		return perr("mkdir", p, syscall.ENOTDIR)
	}
	if p != "/" {
		if err := fs.MkdirAll(pathpkg.Dir(p), perm); err != nil {
			return err
		}
	}
	fs.forget(p)
	_, err := fs.do(opMkdir, p, (&req{}).str(p).u32(uint32(perm.Perm())))
	if errors.Is(err, os.ErrExist) {
		return nil
	}
	return err
}

func (fs *phoneFS) Symlink(target, link string) error {
	p := clean(link)
	fs.forget(p)
	_, err := fs.do(opSymlink, p, (&req{}).str(target).str(p))
	return err
}

func (fs *phoneFS) Readlink(link string) (string, error) {
	p := clean(link)
	out, err := fs.do(opReadlink, p, (&req{}).str(p))
	if err != nil {
		return "", err
	}
	return (&rd{b: out}).str(), nil
}

func (fs *phoneFS) Chroot(string) (billy.Filesystem, error) { return nil, errors.New("not here") }
func (fs *phoneFS) Root() string                           { return "/" }

// billy.Change
func (fs *phoneFS) Chmod(name string, mode os.FileMode) error {
	p := clean(name)
	fs.forget(p)
	m := uint32(mode.Perm())
	if mode&os.ModeSetuid != 0 {
		m |= syscall.S_ISUID
	}
	if mode&os.ModeSetgid != 0 {
		m |= syscall.S_ISGID
	}
	if mode&os.ModeSticky != 0 {
		m |= syscall.S_ISVTX
	}
	_, err := fs.do(opChmod, p, (&req{}).str(p).u32(m))
	return err
}
func (fs *phoneFS) Lchown(name string, uid, gid int) error {
	p := clean(name)
	fs.forget(p)
	_, err := fs.do(opChown, p, (&req{}).str(p).u32(uint32(uid)).u32(uint32(gid)))
	return err
}
func (fs *phoneFS) Chown(name string, uid, gid int) error { return fs.Lchown(name, uid, gid) }
func (fs *phoneFS) Chtimes(name string, atime, mtime time.Time) error {
	p := clean(name)
	fs.forget(p)
	_, err := fs.do(opUtimes, p, (&req{}).str(p).u64(uint64(atime.Unix())).u32(uint32(atime.Nanosecond())).
		u64(uint64(mtime.Unix())).u32(uint32(mtime.Nanosecond())))
	return err
}

func (fs *phoneFS) truncate(p string, size int64) error {
	fs.forget(p)
	_, err := fs.do(opTruncate, p, (&req{}).str(p).u64(uint64(size)))
	return err
}

func (fs *phoneFS) statfs() (bsize, blocks, bfree, bavail, files, ffree uint64, err error) {
	out, err := fs.do(opStatfs, "/", (&req{}).str("/private/var"))
	if err != nil {
		return
	}
	r := &rd{b: out}
	return r.u64(), r.u64(), r.u64(), r.u64(), r.u64(), r.u64(), nil
}

// ---- a file: its path and a position; every read and write a request ----

type phoneFile struct {
	fs   *phoneFS
	path string
	flag int
	pos  int64
}

func (f *phoneFile) Name() string { return f.path }

func (f *phoneFile) ReadAt(b []byte, off int64) (int, error) {
	got := 0
	for got < len(b) {
		want := len(b) - got
		if want > maxIO {
			want = maxIO
		}
		out, err := f.fs.do(opRead, f.path, (&req{}).str(f.path).u64(uint64(off+int64(got))).u32(uint32(want)))
		if err != nil {
			return got, err
		}
		r := &rd{b: out}
		n := r.u32()
		if r.bad || int(n) > len(r.b) {
			return got, io.ErrUnexpectedEOF
		}
		copy(b[got:], r.b[:n])
		got += int(n)
		if int(n) < want {
			return got, io.EOF
		}
	}
	return got, nil
}

func (f *phoneFile) Read(b []byte) (int, error) {
	n, err := f.ReadAt(b, f.pos)
	f.pos += int64(n)
	if err == io.EOF && n > 0 {
		err = nil
	}
	return n, err
}

func (f *phoneFile) WriteAt(b []byte, off int64) (int, error) {
	f.fs.forget(f.path)
	done := 0
	for done < len(b) {
		chunk := b[done:]
		if len(chunk) > maxIO/2 {
			chunk = chunk[:maxIO/2]
		}
		out, err := f.fs.do(opWrite, f.path, (&req{}).str(f.path).u64(uint64(off+int64(done))).raw(chunk))
		if err != nil {
			return done, err
		}
		r := &rd{b: out}
		n := r.u32()
		if r.bad || n == 0 {
			return done, io.ErrShortWrite
		}
		done += int(n)
	}
	return done, nil
}

func (f *phoneFile) Write(b []byte) (int, error) {
	n, err := f.WriteAt(b, f.pos)
	f.pos += int64(n)
	return n, err
}

func (f *phoneFile) Seek(off int64, whence int) (int64, error) {
	switch whence {
	case io.SeekStart:
		f.pos = off
	case io.SeekCurrent:
		f.pos += off
	case io.SeekEnd:
		i, err := f.fs.Lstat(f.path)
		if err != nil {
			return 0, err
		}
		f.pos = i.Size() + off
	}
	return f.pos, nil
}

func (f *phoneFile) Close() error               { return nil }
func (f *phoneFile) Lock() error                { return nil }
func (f *phoneFile) Unlock() error              { return nil }
func (f *phoneFile) Truncate(size int64) error { return f.fs.truncate(f.path, size) }
