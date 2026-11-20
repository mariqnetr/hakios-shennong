package main

// usbfs: the files of HackiOS on the phone (shennong), mounted on this Mac, read and write.
//
//   usbfs [-mount DIR] [-port N] [-local PATH-TO-usbfsd]
//
// Talks to usbfsd on the phone through the loader's USB pipe (a kernel must be running: ./xl run)
// and serves what it finds to macOS as NFS on 127.0.0.1, then mounts that on DIR (default
// ~/iPhoneX; it shows up in Finder). Ctrl-C unmounts. -local runs a usbfsd on this Mac instead
// of the phone's, which tries everything here but the phone.

import (
	"context"
	"flag"
	"fmt"
	"net"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"github.com/go-git/go-billy/v5"
	nfs "github.com/willscott/go-nfs"
	nfshelper "github.com/willscott/go-nfs/helpers"
)

// the NullAuthHandler, with the space the phone's data volume really has
type handler struct {
	*nfshelper.NullAuthHandler
	fs *phoneFS
}

func (h handler) FSStat(ctx context.Context, f billy.Filesystem, s *nfs.FSStat) error {
	bsize, blocks, bfree, bavail, files, ffree, err := h.fs.statfs()
	if err != nil {
		return nil // the defaults
	}
	s.TotalSize, s.FreeSize, s.AvailableSize = bsize*blocks, bsize*bfree, bsize*bavail
	s.TotalFiles, s.FreeFiles, s.AvailableFiles = files, ffree, ffree
	return nil
}

func main() {
	home, _ := os.UserHomeDir()
	mountAt := flag.String("mount", filepath.Join(home, "iPhoneX"), "where to mount the phone's files (\"\": serve only)")
	port := flag.Int("port", 11120, "the NFS server's port on 127.0.0.1")
	local := flag.String("local", "", "a usbfsd on this Mac to talk to instead of the phone (tests)")
	flag.Parse()

	var t transport
	if *local != "" {
		f, err := newLocalPipe(*local)
		if err != nil {
			fmt.Fprintln(os.Stderr, "usbfs:", err)
			os.Exit(1)
		}
		t = f
	} else {
		t = newUSBPipe()
	}
	// is anyone there?
	for {
		out, _, err := t.call(opPing, nil)
		if err == nil {
			fmt.Printf("usbfs: the phone answers: %s\n", (&rd{b: out}).str())
			break
		}
		fmt.Fprintf(os.Stderr, "usbfs: %v; trying again\n", err)
		time.Sleep(2 * time.Second)
	}

	fs := newPhoneFS(t)
	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", *port))
	if err != nil {
		fmt.Fprintln(os.Stderr, "usbfs:", err)
		os.Exit(1)
	}
	h := handler{nfshelper.NewNullAuthHandler(fs).(*nfshelper.NullAuthHandler), fs}
	go func() {
		err := nfs.Serve(ln, nfshelper.NewCachingHandler(h, 1<<20))
		fmt.Fprintln(os.Stderr, "usbfs: the server ended:", err)
		os.Exit(1)
	}()
	fmt.Printf("usbfs: NFS on 127.0.0.1:%d\n", *port)

	if *mountAt == "" {
		select {}
	}
	os.MkdirAll(*mountAt, 0755)
	opts := fmt.Sprintf("port=%d,mountport=%d,vers=3,tcp,nolocks,locallocks,soft,intr,timeo=600,retrans=2,rsize=524288,wsize=524288,readahead=4,actimeo=1,nonamedattr", *port, *port)
	cmd := exec.Command("/sbin/mount_nfs", "-o", opts, "127.0.0.1:/", *mountAt)
	if out, err := cmd.CombinedOutput(); err != nil {
		fmt.Fprintf(os.Stderr, "usbfs: mount_nfs: %v: %s\n", err, strings.TrimSpace(string(out)))
		fmt.Fprintf(os.Stderr, "usbfs: the server stays up; mount by hand: mount_nfs -o %s 127.0.0.1:/ %s\n", opts, *mountAt)
	} else {
		fmt.Printf("usbfs: the phone's files are in %s (Ctrl-C to unmount)\n", *mountAt)
		exec.Command("/usr/bin/open", *mountAt).Run()
	}
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	<-sig
	fmt.Println("usbfs: unmounting")
	if err := exec.Command("/sbin/umount", *mountAt).Run(); err != nil {
		exec.Command("/usr/sbin/diskutil", "unmount", "force", *mountAt).Run()
	}
}
