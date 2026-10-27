package upnp

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
)

// fakeGateway records SOAP calls and lets each test decide how the gateway
// answers. It is an HTTP endpoint on loopback, standing in for a router's
// control URL.
type fakeGateway struct {
	t       *testing.T
	mu      sync.Mutex
	calls   []string
	addFail int // number of AddPortMapping calls answered with a 718 conflict
}

func (f *fakeGateway) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	action := ""
	if header := r.Header.Get("SOAPAction"); header != "" {
		action = header[strings.LastIndex(header, "#")+1:]
		action = strings.Trim(action, `"`)
	}
	body, _ := io.ReadAll(r.Body)
	request := string(body)
	f.mu.Lock()
	f.calls = append(f.calls, action)
	conflict := action == "AddPortMapping" && f.addFail > 0
	if conflict {
		f.addFail--
	}
	f.mu.Unlock()
	if conflict {
		w.WriteHeader(http.StatusInternalServerError)
		fmt.Fprint(w, `<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><s:Fault><faultcode>s:Client</faultcode><detail><UPnPError xmlns="urn:schemas-upnp-org:control-1-0"><errorCode>718</errorCode><errorDescription>ConflictInMappingEntry</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>`)
		return
	}
	switch action {
	case "GetExternalIPAddress":
		fmt.Fprint(w, `<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:GetExternalIPAddressResponse xmlns:u="urn:schemas-upnp-org:service:WANIPConnection:1"><NewExternalIPAddress>203.0.113.7</NewExternalIPAddress></u:GetExternalIPAddressResponse></s:Body></s:Envelope>`)
	default:
		if !strings.Contains(request, "<NewProtocol>UDP</NewProtocol>") {
			f.t.Errorf("%s did not request UDP: %s", action, request)
		}
		fmt.Fprint(w, `<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:`+action+`Response xmlns:u="urn:schemas-upnp-org:service:WANIPConnection:1"></u:`+action+`Response></s:Body></s:Envelope>`)
	}
}

func (f *fakeGateway) called(action string) int {
	f.mu.Lock()
	defer f.mu.Unlock()
	count := 0
	for _, call := range f.calls {
		if call == action {
			count++
		}
	}
	return count
}

func TestGatewayFromDescriptionFindsWanConnection(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/xml")
		fmt.Fprintf(w, `<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <URLBase>http://%s/</URLBase>
  <device>
    <deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
    <deviceList>
      <device>
        <deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
        <deviceList>
          <device>
            <deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
            <serviceList>
              <service>
                <serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>
                <controlURL>/ppp/control</controlURL>
              </service>
              <service>
                <serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
                <controlURL>ip/control</controlURL>
              </service>
            </serviceList>
          </device>
        </deviceList>
      </device>
    </deviceList>
  </device>
</root>`, r.Host)
	}))
	defer server.Close()

	gateway, err := gatewayFromLocation(context.Background(), server.URL+"/rootDesc.xml")
	if err != nil {
		t.Fatal(err)
	}
	if gateway.Service != "urn:schemas-upnp-org:service:WANIPConnection:1" {
		t.Fatalf("service = %q", gateway.Service)
	}
	if gateway.ControlURL != server.URL+"/ip/control" {
		t.Fatalf("control URL = %q", gateway.ControlURL)
	}
	if gateway.LocalIP != "127.0.0.1" {
		t.Fatalf("local IP = %q", gateway.LocalIP)
	}
}

func TestGatewayFromDescriptionRequiresWanService(t *testing.T) {
	_, err := gatewayFromDescription("http://192.0.2.1/root.xml",
		[]byte(`<root><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType><controlURL>/x</controlURL></service></serviceList></device></root>`))
	if err == nil {
		t.Fatal("a device without a WAN connection service was accepted")
	}
}

func TestSearchLocation(t *testing.T) {
	response := "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nlocation: http://192.168.1.1:5000/rootDesc.xml\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n"
	if got := searchLocation([]byte(response)); got != "http://192.168.1.1:5000/rootDesc.xml" {
		t.Fatalf("location = %q", got)
	}
	if got := searchLocation([]byte("HTTP/1.1 200 OK\r\nSERVER: x\r\n\r\n")); got != "" {
		t.Fatalf("missing location = %q", got)
	}
}

func TestMapperAddsRenewsAndDeletes(t *testing.T) {
	fake := &fakeGateway{t: t}
	server := httptest.NewServer(fake)
	defer server.Close()
	gateway := Gateway{
		ControlURL: server.URL + "/control",
		Service:    "urn:schemas-upnp-org:service:WANIPConnection:1",
		LocalIP:    "127.0.0.1",
	}
	mapper, err := MapWithGateway(context.Background(), gateway, 40000, "chast host")
	if err != nil {
		t.Fatal(err)
	}
	if mapper.ExternalPort() != 40000 {
		t.Fatalf("external port = %d", mapper.ExternalPort())
	}
	if err := mapper.Renew(context.Background()); err != nil {
		t.Fatal(err)
	}
	address, err := mapper.ExternalIP(context.Background())
	if err != nil || address != "203.0.113.7" {
		t.Fatalf("external address = %q, %v", address, err)
	}
	if err := mapper.Close(); err != nil {
		t.Fatal(err)
	}
	if mapper.ExternalPort() != 0 {
		t.Fatalf("external port after close = %d", mapper.ExternalPort())
	}
	if fake.called("AddPortMapping") != 2 || fake.called("DeletePortMapping") != 1 || fake.called("GetExternalIPAddress") != 1 {
		t.Fatalf("calls = %v", fake.calls)
	}
}

func TestMapperTakesNextPortOnConflict(t *testing.T) {
	fake := &fakeGateway{t: t, addFail: 1}
	server := httptest.NewServer(fake)
	defer server.Close()
	gateway := Gateway{
		ControlURL: server.URL + "/control",
		Service:    "urn:schemas-upnp-org:service:WANIPConnection:1",
		LocalIP:    "127.0.0.1",
	}
	mapper, err := MapWithGateway(context.Background(), gateway, 40000, "chast")
	if err != nil {
		t.Fatal(err)
	}
	defer mapper.Close()
	if mapper.ExternalPort() != 40001 {
		t.Fatalf("external port after a conflict = %d, want 40001", mapper.ExternalPort())
	}
	if fake.called("AddPortMapping") != 2 {
		t.Fatalf("calls = %v", fake.calls)
	}
}

func TestMapperRejectsInvalidPort(t *testing.T) {
	if _, err := MapWithGateway(context.Background(),
		Gateway{ControlURL: "http://127.0.0.1/control", LocalIP: "127.0.0.1"}, 0, "chast"); err == nil {
		t.Fatal("port 0 was accepted")
	}
}

func TestMapperCloseTreatsMissingEntryAsDone(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusInternalServerError)
		fmt.Fprint(w, `<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><s:Fault><detail><UPnPError><errorCode>714</errorCode></UPnPError></detail></s:Fault></s:Body></s:Envelope>`)
	}))
	defer server.Close()
	mapper := &Mapper{
		gateway: Gateway{
			ControlURL: server.URL,
			Service:    "urn:schemas-upnp-org:service:WANIPConnection:1",
			LocalIP:    "127.0.0.1",
		},
		client:       server.Client(),
		externalPort: 40000,
	}
	if err := mapper.Close(); err != nil {
		t.Fatalf("close with a missing entry: %v", err)
	}
}

func TestFaultCode(t *testing.T) {
	body := []byte(`<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><s:Fault><detail><UPnPError xmlns="urn:schemas-upnp-org:control-1-0"><errorCode>718</errorCode></UPnPError></detail></s:Fault></s:Body></s:Envelope>`)
	if code := faultCode(body); code != "718" {
		t.Fatalf("fault code = %q", code)
	}
	if code := faultCode([]byte("not xml")); code != "" {
		t.Fatalf("unexpected fault code %q", code)
	}
}
