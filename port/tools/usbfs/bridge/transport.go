package main

// The way to usbfsd: frames ("UFS1", see usbfsd.c) over the loader's host pipe, which the loader
// serves as fastboot commands while a kernel runs ("oem pipe-w:HEX" + DATA, "oem pipe-r:HEX").
// The phone is shared with ./xl: every stretch of talking to it holds xl's lock
// (/tmp/hackios-phone.lock, flock) and the USB interface, and both are let go once it has been
// quiet for a moment, so that `xl run`'s own fastboot calls get their turn.

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/google/gousb"
)

const frameMagic = 0x31534655

type transport interface {
	// one request, its answer's payload (after the errno) and the errno
	call(op uint16, payload []byte) ([]byte, syscall.Errno, error)
}

// ---- framing, the same whatever carries it ----

type framer struct {
	mu     sync.Mutex
	nextID uint32
	rx     []byte
	send   func([]byte) error
	recv   func(wait time.Duration) ([]byte, error) // whatever has come, possibly nothing
}

func (f *framer) call(op uint16, payload []byte) ([]byte, syscall.Errno, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.nextID++
	id := f.nextID
	frame := make([]byte, 16+len(payload))
	binary.LittleEndian.PutUint32(frame[0:], frameMagic)
	binary.LittleEndian.PutUint32(frame[4:], id)
	binary.LittleEndian.PutUint32(frame[8:], uint32(op))
	binary.LittleEndian.PutUint32(frame[12:], uint32(len(payload)))
	copy(frame[16:], payload)
	if err := f.send(frame); err != nil {
		return nil, 0, err
	}
	deadline := time.Now().Add(60 * time.Second)
	wait := time.Duration(0)
	for {
		// a whole answer in what has come?
		for {
			i := bytes.Index(f.rx, []byte{0x55, 0x46, 0x53, 0x31})
			if i < 0 {
				if len(f.rx) > 3 {
					f.rx = f.rx[len(f.rx)-3:]
				}
				break
			}
			f.rx = f.rx[i:]
			if len(f.rx) < 16 {
				break
			}
			gotID := binary.LittleEndian.Uint32(f.rx[4:])
			n := binary.LittleEndian.Uint32(f.rx[12:])
			if n > 8<<20 {
				f.rx = f.rx[4:] // not a frame
				continue
			}
			if uint32(len(f.rx)) < 16+n {
				break
			}
			body := f.rx[16 : 16+n]
			f.rx = f.rx[16+n:]
			if gotID != id || n < 4 {
				continue // an answer to a request given up on
			}
			errno := syscall.Errno(binary.LittleEndian.Uint32(body))
			out := make([]byte, len(body)-4)
			copy(out, body[4:])
			return out, errno, nil
		}
		if time.Now().After(deadline) {
			return nil, 0, errors.New("no answer from the phone")
		}
		got, err := f.recv(wait)
		if err != nil {
			return nil, 0, err
		}
		if len(got) == 0 {
			if wait < 20*time.Millisecond {
				wait += time.Millisecond
			}
		} else {
			wait = 0
			f.rx = append(f.rx, got...)
		}
	}
}

// ---- over USB, through the loader ----

type usbPipe struct {
	mu       sync.Mutex
	ctx      *gousb.Context
	dev      *gousb.Device
	cfg      *gousb.Config
	intf     *gousb.Interface
	in       *gousb.InEndpoint
	out      *gousb.OutEndpoint
	lock     *os.File
	lastUse  time.Time
	buf      []byte
}

func newUSBPipe() *framer {
	u := &usbPipe{ctx: gousb.NewContext(), buf: make([]byte, 1<<20)}
	go u.idleCloser()
	return &framer{send: u.write, recv: u.read}
}

func (u *usbPipe) idleCloser() {
	for {
		time.Sleep(100 * time.Millisecond)
		u.mu.Lock()
		if u.intf != nil && time.Since(u.lastUse) > 300*time.Millisecond {
			u.release()
		}
		u.mu.Unlock()
	}
}

func (u *usbPipe) release() {
	if u.intf != nil {
		u.intf.Close()
		u.intf = nil
	}
	if u.cfg != nil {
		u.cfg.Close()
		u.cfg = nil
	}
	if u.dev != nil {
		u.dev.Close()
		u.dev = nil
	}
	if u.lock != nil {
		syscall.Flock(int(u.lock.Fd()), syscall.LOCK_UN)
		u.lock.Close()
		u.lock = nil
	}
}

// the phone, its interface claimed and xl's lock held (u.mu held)
func (u *usbPipe) acquire() error {
	u.lastUse = time.Now()
	if u.intf != nil {
		return nil
	}
	dir := os.Getenv("XDG_RUNTIME_DIR")
	if dir == "" {
		dir = "/tmp"
	}
	lf, err := os.OpenFile(dir+"/hackios-phone.lock", os.O_CREATE|os.O_RDWR, 0666)
	if err != nil {
		return err
	}
	if err := syscall.Flock(int(lf.Fd()), syscall.LOCK_EX); err != nil {
		lf.Close()
		return err
	}
	u.lock = lf
	dev, err := u.ctx.OpenDeviceWithVIDPID(0x18d1, 0xd00d)
	if err != nil || dev == nil {
		u.release()
		if err == nil {
			err = errors.New("the phone's loader is not on the USB (is a run going? ./xl run)")
		}
		return err
	}
	u.dev = dev
	if u.cfg, err = dev.Config(1); err != nil {
		u.release()
		return err
	}
	if u.intf, err = u.cfg.Interface(0, 0); err != nil {
		u.release()
		return err
	}
	if u.in, err = u.intf.InEndpoint(1); err != nil {
		u.release()
		return err
	}
	if u.out, err = u.intf.OutEndpoint(1); err != nil {
		u.release()
		return err
	}
	return nil
}

func (u *usbPipe) xfer(f func(ctx context.Context) (int, error)) (int, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	return f(ctx)
}

// one fastboot command; returns the DATA length when the loader asks for or offers data, or -1
func (u *usbPipe) command(cmd string) (int, string, error) {
	if _, err := u.xfer(func(c context.Context) (int, error) { return u.out.WriteContext(c, []byte(cmd)) }); err != nil {
		return 0, "", err
	}
	for {
		resp := make([]byte, 512)
		n, err := u.xfer(func(c context.Context) (int, error) { return u.in.ReadContext(c, resp) })
		if err != nil {
			return 0, "", err
		}
		s := string(resp[:n])
		switch {
		case strings.HasPrefix(s, "INFO"):
			continue
		case strings.HasPrefix(s, "DATA"):
			v, err := strconv.ParseUint(s[4:], 16, 32)
			return int(v), "", err
		case strings.HasPrefix(s, "OKAY"):
			return -1, s[4:], nil
		case strings.HasPrefix(s, "FAIL"):
			return -1, "", fmt.Errorf("loader: %s", s[4:])
		default:
			return 0, "", fmt.Errorf("loader said %q", s)
		}
	}
}

func (u *usbPipe) okay() error {
	resp := make([]byte, 512)
	n, err := u.xfer(func(c context.Context) (int, error) { return u.in.ReadContext(c, resp) })
	if err != nil {
		return err
	}
	if !strings.HasPrefix(string(resp[:n]), "OKAY") {
		return fmt.Errorf("loader said %q", resp[:n])
	}
	return nil
}

func (u *usbPipe) write(frame []byte) error {
	u.mu.Lock()
	defer u.mu.Unlock()
	deadline := time.Now().Add(30 * time.Second)
	for len(frame) > 0 {
		if err := u.acquire(); err != nil {
			return err
		}
		chunk := frame
		if len(chunk) > 256<<10 {
			chunk = chunk[:256<<10]
		}
		n, _, err := u.command(fmt.Sprintf("oem pipe-w:%x", len(chunk)))
		if err != nil {
			if strings.Contains(err.Error(), "full") && time.Now().Before(deadline) {
				u.mu.Unlock()
				time.Sleep(2 * time.Millisecond)
				u.mu.Lock()
				continue
			}
			u.release()
			return err
		}
		if n != len(chunk) {
			u.release()
			return fmt.Errorf("loader wants %d bytes, not %d", n, len(chunk))
		}
		if _, err := u.xfer(func(c context.Context) (int, error) { return u.out.WriteContext(c, chunk) }); err != nil {
			u.release()
			return err
		}
		if err := u.okay(); err != nil {
			u.release()
			return err
		}
		frame = frame[len(chunk):]
	}
	return nil
}

func (u *usbPipe) read(wait time.Duration) ([]byte, error) {
	if wait > 0 {
		time.Sleep(wait)
	}
	u.mu.Lock()
	defer u.mu.Unlock()
	if err := u.acquire(); err != nil {
		return nil, err
	}
	n, _, err := u.command(fmt.Sprintf("oem pipe-r:%x", len(u.buf)))
	if err != nil {
		u.release()
		return nil, err
	}
	if n <= 0 {
		return nil, u.okay()
	}
	got := 0
	for got < n {
		m, err := u.xfer(func(c context.Context) (int, error) { return u.in.ReadContext(c, u.buf[got:n]) })
		if err != nil {
			u.release()
			return nil, err
		}
		got += m
	}
	if err := u.okay(); err != nil {
		u.release()
		return nil, err
	}
	out := make([]byte, n)
	copy(out, u.buf[:n])
	return out, nil
}

// ---- a usbfsd on this Mac, for trying the rest without the phone ----

func newLocalPipe(server string) (*framer, error) {
	cmd := exec.Command(server, "-", "-f")
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	ch := make(chan []byte, 64)
	go func() {
		for {
			b := make([]byte, 1<<20)
			n, err := stdout.Read(b)
			if n > 0 {
				ch <- b[:n]
			}
			if err != nil {
				if err != io.EOF {
					fmt.Fprintln(os.Stderr, "local usbfsd:", err)
				}
				close(ch)
				return
			}
		}
	}()
	return &framer{
		send: func(b []byte) error { _, err := stdin.Write(b); return err },
		recv: func(wait time.Duration) ([]byte, error) {
			select {
			case b, ok := <-ch:
				if !ok {
					return nil, errors.New("local usbfsd ended")
				}
				return b, nil
			case <-time.After(wait + time.Millisecond):
				return nil, nil
			}
		},
	}, nil
}
