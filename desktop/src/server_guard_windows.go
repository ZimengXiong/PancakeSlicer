package main

import (
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"syscall"
	"time"
	"unsafe"
)

const (
	jobObjectExtendedLimitInformation = 9
	jobObjectLimitKillOnJobClose      = 0x2000
	createNewConsole                  = 0x10
	ctrlBreakEvent                    = 1
)

type jobLimits struct {
	basic struct {
		perProcessUserTime, perJobUserTime int64
		flags                              uint32
		minWorkingSet, maxWorkingSet       uintptr
		activeProcessLimit                 uint32
		affinity                           uintptr
		priorityClass, schedulingClass     uint32
	}
	ioCounters                                     [6]uint64
	processMemory, jobMemory, peakProcess, peakJob uintptr
}

var kernel32 = syscall.NewLazyDLL("kernel32.dll")

func joinJob() error {
	job, _, err := kernel32.NewProc("CreateJobObjectW").Call(0, 0)
	if job == 0 {
		return fmt.Errorf("create server job: %w", err)
	}
	var limits jobLimits
	limits.basic.flags = jobObjectLimitKillOnJobClose
	if ok, _, err := kernel32.NewProc("SetInformationJobObject").Call(job, jobObjectExtendedLimitInformation, uintptr(unsafe.Pointer(&limits)), unsafe.Sizeof(limits)); ok == 0 {
		return fmt.Errorf("set server job limits: %w", err)
	}
	self, _ := syscall.GetCurrentProcess()
	if ok, _, err := kernel32.NewProc("AssignProcessToJobObject").Call(job, uintptr(self)); ok == 0 {
		return fmt.Errorf("join server job: %w", err)
	}
	return nil
}

func interrupt(pid int) error {
	kernel32.NewProc("FreeConsole").Call()
	if ok, _, err := kernel32.NewProc("AttachConsole").Call(uintptr(pid)); ok == 0 {
		return err
	}
	handler := syscall.NewCallback(func(event uint32) uintptr {
		if event == ctrlBreakEvent {
			return 1
		}
		return 0
	})
	if ok, _, err := kernel32.NewProc("SetConsoleCtrlHandler").Call(handler, 1); ok == 0 {
		return err
	}
	if ok, _, err := kernel32.NewProc("GenerateConsoleCtrlEvent").Call(ctrlBreakEvent, 0); ok == 0 {
		return err
	}
	return nil
}

func main() {
	if len(os.Args) < 2 {
		log.Fatal("server executable is required")
	}
	if err := joinJob(); err != nil {
		log.Printf("server job: %v", err)
	}
	cmd := exec.Command(os.Args[1], os.Args[2:]...)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNewConsole}
	cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
	if err := cmd.Start(); err != nil {
		log.Fatal(err)
	}
	exited := make(chan error, 1)
	go func() { exited <- cmd.Wait() }()
	closing := make(chan struct{})
	go func() {
		io.Copy(io.Discard, os.Stdin)
		close(closing)
	}()
	select {
	case err := <-exited:
		if err != nil {
			log.Fatal(err)
		}
	case <-closing:
		if err := interrupt(cmd.Process.Pid); err != nil {
			log.Printf("request server shutdown: %v", err)
		}
		select {
		case <-exited:
		case <-time.After(5 * time.Second):
			log.Print("server shutdown timed out")
		}
	}
}
