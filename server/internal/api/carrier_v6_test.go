package api_test

import (
	"encoding/json"
	"net"
	"net/http"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func TestLocalCarrierV6Endpoints(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{CarrierV6: true})
	if s.CarrierV6 == "" {
		t.Fatal("IPv6 carrier listener was not bound")
	}
	_, wantPort, err := net.SplitHostPort(s.Carrier)
	if err != nil {
		t.Fatalf("carrier address: %v", err)
	}

	now := time.Now().Unix()
	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "correct horse", "AB12", now), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	var reg struct {
		Carrier   string `json:"carrier"`
		CarrierV6 string `json:"carrier_v6"`
	}
	if err := json.Unmarshal(body, &reg); err != nil {
		t.Fatal(err)
	}
	host, port, err := net.SplitHostPort(reg.CarrierV6)
	if err != nil {
		t.Fatalf("carrier_v6 %q: %v", reg.CarrierV6, err)
	}
	if host != "::1" || port != wantPort {
		t.Fatalf("carrier_v6 = %q, want [::1]:%s", reg.CarrierV6, wantPort)
	}

	lease, ok := s.Registry.Lookup("devbox")
	if !ok || lease.RelayPort == 0 {
		t.Fatalf("registry lease: %+v (found=%v)", lease, ok)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/server", "", "")
	if status != http.StatusOK {
		t.Fatalf("server info: %d %s", status, body)
	}
	var info struct {
		Carrier   string `json:"carrier"`
		CarrierV6 string `json:"carrier_v6"`
	}
	json.Unmarshal(body, &info)
	if info.CarrierV6 != reg.CarrierV6 || info.Carrier != s.Carrier {
		t.Fatalf("server info carrier endpoints: %+v", info)
	}

	if err := s.Registry.Touch("devbox", "198.51.100.9:5000", nil, ""); err != nil {
		t.Fatal(err)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("lookup: %d %s", status, body)
	}
	var live hostReply
	json.Unmarshal(body, &live)
	if !live.Online || live.CarrierV6 != reg.CarrierV6 || live.Carrier != reg.Carrier {
		t.Fatalf("lookup carrier endpoints: %+v", live)
	}
}

func TestCarrierV6HiddenWithoutListener(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "correct horse", "", now), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	var reg struct {
		Carrier   string `json:"carrier"`
		CarrierV6 string `json:"carrier_v6"`
	}
	json.Unmarshal(body, &reg)
	if reg.CarrierV6 != "" {
		t.Fatalf("carrier_v6 without an IPv6 listener: %+v", reg)
	}

	if err := s.Registry.Touch("devbox", "198.51.100.9:5000", nil, ""); err != nil {
		t.Fatal(err)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("lookup: %d %s", status, body)
	}
	var live hostReply
	json.Unmarshal(body, &live)
	if live.CarrierV6 != "" {
		t.Fatalf("lookup leaked carrier_v6: %+v", live)
	}
	_, body = request(t, http.MethodGet, s.URL+"/v1/server", "", "")
	var info struct {
		CarrierV6 string `json:"carrier_v6"`
	}
	json.Unmarshal(body, &info)
	if info.CarrierV6 != "" {
		t.Fatalf("server info leaked carrier_v6: %+v", info)
	}
}
