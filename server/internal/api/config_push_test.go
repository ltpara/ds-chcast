package api_test

import (
	"encoding/json"
	"net/http"
	"reflect"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/configpush"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type configReply struct {
	ConfigSerial      int64    `json:"config_serial"`
	RendezvousServers []string `json:"rendezvous_servers"`
}

func startPushedServer(t *testing.T, servers ...string) (*testutil.Server, *configpush.Config) {
	t.Helper()
	push, err := configpush.New(1, servers)
	if err != nil {
		t.Fatalf("config: %v", err)
	}
	return testutil.StartWith(t, testutil.Options{RendezvousConfig: push}), push
}

func TestRegisterAndLookupCarryRendezvousConfig(t *testing.T) {
	want := []string{"https://one.example", "https://two.example"}
	s, _ := startPushedServer(t, want...)
	now := time.Now().Unix()

	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "correct horse", "AB12", now), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	var reg configReply
	if err := json.Unmarshal(body, &reg); err != nil {
		t.Fatal(err)
	}
	if reg.ConfigSerial != 1 || !reflect.DeepEqual(reg.RendezvousServers, want) {
		t.Fatalf("register config = %+v", reg)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("lookup: %d %s", status, body)
	}
	var host configReply
	json.Unmarshal(body, &host)
	if host.ConfigSerial != 1 || !reflect.DeepEqual(host.RendezvousServers, want) {
		t.Fatalf("lookup config = %+v", host)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/hosts?ids=devbox", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("batch lookup: %d %s", status, body)
	}
	var batch configReply
	json.Unmarshal(body, &batch)
	if batch.ConfigSerial != 1 || !reflect.DeepEqual(batch.RendezvousServers, want) {
		t.Fatalf("batch config = %+v", batch)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/server", "", "")
	if status != http.StatusOK {
		t.Fatalf("server info: %d %s", status, body)
	}
	var info configReply
	json.Unmarshal(body, &info)
	if info.ConfigSerial != 1 || !reflect.DeepEqual(info.RendezvousServers, want) {
		t.Fatalf("server info config = %+v", info)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/stats", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("stats: %d %s", status, body)
	}
	var stats configReply
	json.Unmarshal(body, &stats)
	if stats.ConfigSerial != 1 || !reflect.DeepEqual(stats.RendezvousServers, want) {
		t.Fatalf("stats config = %+v", stats)
	}
}

func TestRendezvousConfigSerialMovesForward(t *testing.T) {
	s, push := startPushedServer(t, "https://one.example")
	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "correct horse", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}

	next, err := push.Set([]string{"https://three.example"})
	if err != nil {
		t.Fatalf("set: %v", err)
	}
	_, body := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
	var host configReply
	json.Unmarshal(body, &host)
	if host.ConfigSerial != next || !reflect.DeepEqual(host.RendezvousServers, []string{"https://three.example"}) {
		t.Fatalf("lookup after update = %+v", host)
	}
}

func TestRendezvousConfigAbsentWithoutPush(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	_, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "correct horse", "AB12", now), testutil.ServerKey)
	var reg configReply
	json.Unmarshal(body, &reg)
	if reg.ConfigSerial != 0 || len(reg.RendezvousServers) != 0 {
		t.Fatalf("register config = %+v", reg)
	}

	_, body = request(t, http.MethodGet, s.URL+"/v1/server", "", "")
	var info map[string]any
	json.Unmarshal(body, &info)
	if _, ok := info["config_serial"]; ok {
		t.Fatalf("server info advertises a config serial: %s", body)
	}
}
