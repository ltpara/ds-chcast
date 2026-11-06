package agent

import (
	"bufio"
	"bytes"
	"context"
	"fmt"
	"net"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func TestMuxCarrierSlowLocalPeerDoesNotBlockOtherViewer(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func() {
				defer conn.Close()
				closeOnCancel := context.AfterFunc(ctx, func() { conn.Close() })
				defer closeOnCancel()
				conn.(*net.TCPConn).SetReadBuffer(1024)
				reader := bufio.NewReader(conn)
				magic, err := carrier.ReadLine(reader, 64)
				if err != nil || magic+"\n" != carrier.MagicRaw {
					return
				}
				payload, err := carrier.NewFrameReader(reader).ReadFrame()
				if err != nil {
					return
				}
				if string(payload) == "fast" {
					carrier.NewFrameWriter(conn).WriteFrame([]byte("fast-reply"))
				} else {
					// Deliberately stop reading this local carrier.
					<-ctx.Done()
				}
			}()
		}
	}()
	remote, local := net.Pipe()
	defer remote.Close()
	host := &HostAgent{ctx: ctx, carrier: listener.Addr().(*net.TCPAddr)}
	finished := make(chan error, 1)
	go func() {
		finished <- host.runMuxCarrier(local, carrier.NewFrameReader(local), carrier.NewFrameWriter(local), "test")
	}()
	replies := make(chan []byte, 1)
	go func() {
		reader := carrier.NewFrameReader(remote)
		for {
			frame, err := reader.ReadFrame()
			if err != nil {
				return
			}
			id, payload, ok := carrier.DecodeMux(frame)
			if ok && id == 2 && len(payload) != 0 {
				replies <- payload
				return
			}
		}
	}()
	remote.SetDeadline(time.Now().Add(5 * time.Second))
	writer := carrier.NewFrameWriter(remote)
	large := bytes.Repeat([]byte{'s'}, 60*1024)
	for i := 0; i < 80; i++ {
		if err := writer.WriteFrame(carrier.EncodeMux(1, large)); err != nil {
			t.Fatal(err)
		}
	}
	if err := writer.WriteFrame(carrier.EncodeMux(2, []byte("fast"))); err != nil {
		t.Fatal(err)
	}
	select {
	case reply := <-replies:
		if string(reply) != "fast-reply" {
			t.Fatalf("wrong viewer reply %q", reply)
		}
	case <-ctx.Done():
		t.Fatal("a blocked local carrier stalled the other viewer")
	}
	remote.Close()
	select {
	case <-finished:
	case <-ctx.Done():
		t.Fatal("multiplex link did not stop")
	}
}

type routedRequest struct {
	text, route string
	reply       func([]byte) error
}

// Deliberately postpone replies until both viewers have sent data. A shared
// last-remote pointer would send both replies to the second viewer.
func TestConcurrentViewersThroughAgents(t *testing.T) {
	for _, mode := range []string{ModeDirect, ModeRelay, ModeCarrier} {
		t.Run(mode, func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			requests := make(chan routedRequest, 64)
			backend, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
			if err != nil {
				t.Fatal(err)
			}
			defer backend.Close()
			go func() {
				buffer := make([]byte, carrier.MaxPayload)
				for {
					n, remote, err := backend.ReadFromUDP(buffer)
					if err != nil {
						return
					}
					request := routedRequest{string(buffer[:n]), remote.String(), func(data []byte) error {
						_, err := backend.WriteToUDP(data, remote)
						return err
					}}
					select {
					case requests <- request:
					case <-ctx.Done():
						return
					}
				}
			}()
			local, err := net.Listen("tcp", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			defer local.Close()
			go func() {
				for {
					conn, err := local.Accept()
					if err != nil {
						return
					}
					go func() {
						defer conn.Close()
						br := bufio.NewReader(conn)
						line, err := carrier.ReadLine(br, 64)
						if err != nil || line+"\n" != carrier.MagicRaw {
							return
						}
						reader, writer := carrier.NewFrameReader(br), carrier.NewFrameWriter(conn)
						for {
							data, err := reader.ReadFrame()
							if err != nil {
								return
							}
							if len(data) == 0 {
								continue
							}
							select {
							case requests <- routedRequest{string(data), conn.RemoteAddr().String(), writer.WriteFrame}:
							case <-ctx.Done():
								return
							}
						}
					}()
				}
			}()
			s := testutil.Start(t)
			startHostAgentCarrier(t, ctx, s.URL, backend.LocalAddr().String(), local.Addr().String())
			if mode == ModeCarrier {
				waitForSharedHost(t, s)
			}
			connect := func() (*ViewerAgent, *net.UDPConn) {
				viewer, err := StartViewer(ctx, ViewerConfig{ServerURL: s.URL, ServerKey: testutil.ServerKey,
					HostID: "devbox", ListenAddr: "127.0.0.1:0", Mode: mode, DirectTimeout: time.Second, Logger: quietLogger()})
				if err != nil {
					t.Fatal(err)
				}
				t.Cleanup(viewer.Close)
				client, err := net.DialUDP("udp", nil, viewer.LocalAddr().(*net.UDPAddr))
				if err != nil {
					t.Fatal(err)
				}
				t.Cleanup(func() { client.Close() })
				return viewer, client
			}
			first, a := connect()
			_, b := connect()
			exchange := func(round int, a, b *net.UDPConn) {
				t.Helper()
				names := []string{fmt.Sprintf("first-%d", round), fmt.Sprintf("second-%d", round)}
				clients := []*net.UDPConn{a, b}
				pending := map[string]routedRequest{}
				deadline := time.After(6 * time.Second)
				retry := time.NewTicker(150 * time.Millisecond)
				defer retry.Stop()
				for len(pending) < 2 {
					select {
					case <-retry.C:
						for i, name := range names {
							if _, ok := pending[name]; !ok {
								clients[i].Write([]byte(name))
							}
						}
					case request := <-requests:
						if request.text == names[0] || request.text == names[1] {
							pending[request.text] = request
						}
					case <-deadline:
						t.Fatalf("only %d viewers reached the backend", len(pending))
					}
				}
				if pending[names[0]].route == pending[names[1]].route {
					t.Fatal("viewers share a local backend route")
				}
				for i := 1; i >= 0; i-- {
					if err := pending[names[i]].reply([]byte("reply-" + names[i])); err != nil {
						t.Fatal(err)
					}
				}
				for i, client := range clients {
					client.SetReadDeadline(time.Now().Add(3 * time.Second))
					buffer := make([]byte, 1024)
					n, err := client.Read(buffer)
					if err != nil || string(buffer[:n]) != "reply-"+names[i] {
						t.Fatalf("viewer %d got %q, %v", i, buffer[:n], err)
					}
				}
			}
			exchange(1, a, b)
			first.Close()
			_, c := connect()
			exchange(2, b, c)
		})
	}
}
