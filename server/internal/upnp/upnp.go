// Package upnp maps a local UDP port on an Internet Gateway Device so peers
// can reach a symmetric-NAT host through a stable external port. It implements
// only the small IGD subset chast needs: SSDP discovery, WANIPConnection or
// WANPPPConnection AddPortMapping/DeletePortMapping and GetExternalIPAddress.
//
// The mapping is a hint, not a trust boundary: agents report the port to the
// rendezvous server, which hands it to peers alongside the observed address.
// If the gateway refuses or disappears, direct punching keeps working through
// the observed mapping and relaying remains the fallback.
package upnp

import (
	"bytes"
	"context"
	"encoding/xml"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

const (
	ssdpAddr      = "239.255.255.250:1900"
	ssdpSearchFor = "urn:schemas-upnp-org:device:InternetGatewayDevice:1"
	// DefaultLease is renewed by the caller well before it expires. A finite
	// lease keeps a crashed agent from owning the port forever.
	DefaultLease = 1800 * time.Second
	maxPortTries = 20
)

// ErrNotFound reports that no gateway answered the SSDP search.
var ErrNotFound = errors.New("no UPnP Internet Gateway Device answered")

// Gateway is one discovered WAN connection service.
type Gateway struct {
	// ControlURL is absolute, resolved against the device description.
	ControlURL string
	// Service is the service type used in the SOAPAction header.
	Service string
	// LocalIP is the local address the gateway is reachable from; it is the
	// address the mapping must point at.
	LocalIP string
}

// Discover sends an SSDP M-SEARCH and returns the first gateway that exposes a
// WANIPConnection or WANPPPConnection service. Discovery itself is IPv4-only,
// matching the IGD specification.
func Discover(ctx context.Context, timeout time.Duration) (Gateway, error) {
	if timeout <= 0 {
		timeout = 3 * time.Second
	}
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{})
	if err != nil {
		return Gateway{}, fmt.Errorf("upnp discovery socket: %w", err)
	}
	defer conn.Close()
	search := []byte("M-SEARCH * HTTP/1.1\r\n" +
		"HOST: " + ssdpAddr + "\r\n" +
		"MAN: \"ssdp:discover\"\r\n" +
		"MX: 2\r\n" +
		"ST: " + ssdpSearchFor + "\r\n\r\n")
	destination, err := net.ResolveUDPAddr("udp4", ssdpAddr)
	if err != nil {
		return Gateway{}, err
	}
	// SSDP is lossy; three datagrams make discovery reliable on most LANs.
	for i := 0; i < 3; i++ {
		if _, err := conn.WriteToUDP(search, destination); err != nil {
			return Gateway{}, fmt.Errorf("upnp search: %w", err)
		}
		time.Sleep(100 * time.Millisecond)
	}
	deadline := time.Now().Add(timeout)
	buf := make([]byte, 4096)
	var lastErr error
	for {
		if err := conn.SetReadDeadline(deadline); err != nil {
			return Gateway{}, err
		}
		n, _, err := conn.ReadFromUDP(buf)
		if err != nil {
			if lastErr != nil {
				return Gateway{}, lastErr
			}
			if errors.Is(err, net.ErrClosed) || isTimeout(err) {
				return Gateway{}, ErrNotFound
			}
			return Gateway{}, fmt.Errorf("upnp discovery read: %w", err)
		}
		location := searchLocation(buf[:n])
		if location == "" {
			continue
		}
		gateway, err := gatewayFromLocation(ctx, location)
		if err != nil {
			lastErr = err
			continue
		}
		return gateway, nil
	}
}

func isTimeout(err error) bool {
	var netErr net.Error
	return errors.As(err, &netErr) && netErr.Timeout()
}

// searchLocation extracts LOCATION from an SSDP response. Header names are
// case-insensitive in practice.
func searchLocation(response []byte) string {
	for _, line := range strings.Split(string(response), "\n") {
		line = strings.TrimSpace(strings.TrimSuffix(line, "\r"))
		key, value, ok := strings.Cut(line, ":")
		if !ok || !strings.EqualFold(strings.TrimSpace(key), "LOCATION") {
			continue
		}
		address := strings.TrimSpace(value)
		if parsed, err := url.Parse(address); err == nil && parsed.IsAbs() {
			return address
		}
	}
	return ""
}

// gatewayFromLocation fetches and parses a device description. It is separated
// from Discover so the XML handling can be tested without multicast.
func gatewayFromLocation(ctx context.Context, location string) (Gateway, error) {
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, location, nil)
	if err != nil {
		return Gateway{}, err
	}
	client := &http.Client{Timeout: 5 * time.Second}
	response, err := client.Do(request)
	if err != nil {
		return Gateway{}, fmt.Errorf("upnp description: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return Gateway{}, fmt.Errorf("upnp description: %s", response.Status)
	}
	body, err := io.ReadAll(io.LimitReader(response.Body, 1<<20))
	if err != nil {
		return Gateway{}, err
	}
	return gatewayFromDescription(location, body)
}

type descriptionService struct {
	ServiceType string `xml:"serviceType"`
	ControlURL  string `xml:"controlURL"`
}

type descriptionDevice struct {
	URLBase     string               `xml:"URLBase"`
	ServiceList []descriptionService `xml:"serviceList>service"`
	Devices     []descriptionDevice  `xml:"deviceList>device"`
}

func (d descriptionDevice) walkServices() []descriptionService {
	services := append([]descriptionService{}, d.ServiceList...)
	for _, child := range d.Devices {
		services = append(services, child.walkServices()...)
	}
	return services
}

// gatewayFromDescription walks the device tree and prefers WANIPConnection over
// WANPPPConnection, then the highest service version.
func gatewayFromDescription(location string, body []byte) (Gateway, error) {
	var document struct {
		URLBase string            `xml:"URLBase"`
		Device  descriptionDevice `xml:"device"`
	}
	if err := xml.Unmarshal(body, &document); err != nil {
		return Gateway{}, fmt.Errorf("upnp description xml: %w", err)
	}
	baseText := document.URLBase
	if baseText == "" {
		baseText = location
	}
	base, err := url.Parse(baseText)
	if err != nil {
		return Gateway{}, fmt.Errorf("upnp URLBase: %w", err)
	}
	best := -1
	var chosen descriptionService
	for _, service := range document.Device.walkServices() {
		rank := serviceRank(service.ServiceType)
		if rank < 0 || rank <= best {
			continue
		}
		best = rank
		chosen = service
	}
	if best < 0 || chosen.ControlURL == "" {
		return Gateway{}, errors.New("upnp device has no WANIPConnection or WANPPPConnection service")
	}
	control, err := base.Parse(chosen.ControlURL)
	if err != nil {
		return Gateway{}, fmt.Errorf("upnp control URL: %w", err)
	}
	localIP, err := localIPFor(control.Host)
	if err != nil {
		return Gateway{}, err
	}
	return Gateway{ControlURL: control.String(), Service: chosen.ServiceType, LocalIP: localIP}, nil
}

// serviceRank orders candidates: WANIPConnection before WANPPPConnection, and
// version 1 before 2 (both are widely implemented; v1 is the safe default).
func serviceRank(serviceType string) int {
	switch {
	case strings.Contains(serviceType, "WANIPConnection:1"):
		return 4
	case strings.Contains(serviceType, "WANIPConnection:2"):
		return 3
	case strings.Contains(serviceType, "WANPPPConnection:1"):
		return 2
	case strings.Contains(serviceType, "WANPPPConnection:2"):
		return 1
	default:
		return -1
	}
}

// localIPFor finds the source address used to reach the gateway without
// sending a packet.
func localIPFor(hostPort string) (string, error) {
	host, _, err := net.SplitHostPort(hostPort)
	if err != nil {
		host = hostPort
		hostPort = net.JoinHostPort(host, "80")
	}
	conn, err := net.Dial("udp4", hostPort)
	if err != nil {
		return "", fmt.Errorf("upnp local address: %w", err)
	}
	defer conn.Close()
	local, ok := conn.LocalAddr().(*net.UDPAddr)
	if !ok || local.IP == nil {
		return "", errors.New("upnp local address unavailable")
	}
	return local.IP.String(), nil
}

// Mapper owns one leased external UDP port.
type Mapper struct {
	gateway      Gateway
	client       *http.Client
	externalPort int
	internalPort int
	lease        time.Duration
	description  string
}

// Map discovers a gateway and maps internalPort. The external port is normally
// the same number; if the gateway already uses it, the next free port in a
// small range is taken.
func Map(ctx context.Context, internalPort int, description string) (*Mapper, error) {
	gateway, err := Discover(ctx, 3*time.Second)
	if err != nil {
		return nil, err
	}
	return MapWithGateway(ctx, gateway, internalPort, description)
}

// MapWithGateway is Map for a caller that already discovered (or was given) a
// gateway. Tests use it with a local fake IGD.
func MapWithGateway(ctx context.Context, gateway Gateway, internalPort int, description string) (*Mapper, error) {
	if gateway.ControlURL == "" || gateway.LocalIP == "" {
		return nil, errors.New("upnp gateway needs a control URL and a local address")
	}
	if internalPort <= 0 || internalPort > 65535 {
		return nil, fmt.Errorf("upnp internal port %d is out of range", internalPort)
	}
	if description == "" {
		description = "chast"
	}
	m := &Mapper{
		gateway:      gateway,
		client:       &http.Client{Timeout: 5 * time.Second},
		internalPort: internalPort,
		lease:        DefaultLease,
		description:  description,
	}
	var lastErr error
	for offset := 0; offset < maxPortTries; offset++ {
		port := internalPort + offset
		if port > 65535 {
			break
		}
		err := m.addMapping(ctx, port, description)
		if err == nil {
			m.externalPort = port
			return m, nil
		}
		if !errors.Is(err, errPortConflict) {
			return nil, err
		}
		lastErr = err
	}
	if lastErr == nil {
		lastErr = errPortConflict
	}
	return nil, fmt.Errorf("upnp could not map port %d: %w", internalPort, lastErr)
}

// ExternalPort is the public UDP port the gateway forwards. Zero means no
// mapping.
func (m *Mapper) ExternalPort() int {
	if m == nil {
		return 0
	}
	return m.externalPort
}

// Gateway returns the discovered gateway, mainly for logging.
func (m *Mapper) Gateway() Gateway {
	if m == nil {
		return Gateway{}
	}
	return m.gateway
}

// Renew extends the lease. Callers should renew before DefaultLease elapses.
func (m *Mapper) Renew(ctx context.Context) error {
	if m == nil || m.externalPort == 0 {
		return errors.New("upnp mapper has no lease")
	}
	return m.addMapping(ctx, m.externalPort, m.description)
}

// Close removes the mapping. A gateway that forgot the entry (714) is treated
// as success so shutdown is idempotent.
func (m *Mapper) Close() error {
	if m == nil || m.externalPort == 0 {
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, err := m.soap(ctx, "DeletePortMapping", [][2]string{
		{"NewRemoteHost", ""},
		{"NewExternalPort", strconv.Itoa(m.externalPort)},
		{"NewProtocol", "UDP"},
	})
	if err != nil && !errors.Is(err, errNoSuchEntry) {
		return err
	}
	m.externalPort = 0
	return nil
}

// ExternalIP asks the gateway for its public address, which is useful in logs
// and diagnostics. Mapping itself does not require it.
func (m *Mapper) ExternalIP(ctx context.Context) (string, error) {
	body, err := m.soap(ctx, "GetExternalIPAddress", nil)
	if err != nil {
		return "", err
	}
	var reply struct {
		Address string `xml:"Body>GetExternalIPAddressResponse>NewExternalIPAddress"`
	}
	if err := xml.Unmarshal(body, &reply); err != nil {
		return "", fmt.Errorf("upnp external address: %w", err)
	}
	if reply.Address == "" {
		return "", errors.New("upnp gateway returned an empty external address")
	}
	return reply.Address, nil
}

var (
	errPortConflict = errors.New("upnp external port is already mapped")
	errNoSuchEntry  = errors.New("upnp mapping does not exist")
)

func (m *Mapper) addMapping(ctx context.Context, externalPort int, description string) error {
	_, err := m.soap(ctx, "AddPortMapping", [][2]string{
		{"NewRemoteHost", ""},
		{"NewExternalPort", strconv.Itoa(externalPort)},
		{"NewProtocol", "UDP"},
		{"NewInternalPort", strconv.Itoa(m.internalPort)},
		{"NewInternalClient", m.gateway.LocalIP},
		{"NewEnabled", "1"},
		{"NewPortMappingDescription", description},
		{"NewLeaseDuration", strconv.Itoa(int(m.lease / time.Second))},
	})
	return err
}

func (m *Mapper) soap(ctx context.Context, action string, arguments [][2]string) ([]byte, error) {
	var body bytes.Buffer
	body.WriteString(`<?xml version="1.0"?>`)
	body.WriteString(`<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>`)
	fmt.Fprintf(&body, `<u:%s xmlns:u="%s">`, action, m.gateway.Service)
	for _, argument := range arguments {
		fmt.Fprintf(&body, "<%s>%s</%s>", argument[0], xmlEscape(argument[1]), argument[0])
	}
	fmt.Fprintf(&body, `</u:%s></s:Body></s:Envelope>`, action)
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, m.gateway.ControlURL, bytes.NewReader(body.Bytes()))
	if err != nil {
		return nil, err
	}
	request.Header.Set("Content-Type", `text/xml; charset="utf-8"`)
	request.Header.Set("SOAPAction", fmt.Sprintf(`"%s#%s"`, m.gateway.Service, action))
	response, err := m.client.Do(request)
	if err != nil {
		return nil, fmt.Errorf("upnp %s: %w", action, err)
	}
	defer response.Body.Close()
	reply, err := io.ReadAll(io.LimitReader(response.Body, 1<<20))
	if err != nil {
		return nil, err
	}
	if response.StatusCode != http.StatusOK {
		code := faultCode(reply)
		switch code {
		case "718":
			return reply, errPortConflict
		case "714":
			return reply, errNoSuchEntry
		}
		if code != "" {
			return reply, fmt.Errorf("upnp %s: error %s", action, code)
		}
		return reply, fmt.Errorf("upnp %s: %s", action, response.Status)
	}
	return reply, nil
}

func faultCode(body []byte) string {
	var fault struct {
		Code string `xml:"Body>Fault>detail>UPnPError>errorCode"`
	}
	if err := xml.Unmarshal(body, &fault); err == nil && fault.Code != "" {
		return fault.Code
	}
	return ""
}

func xmlEscape(value string) string {
	var escaped bytes.Buffer
	_ = xml.EscapeText(&escaped, []byte(value))
	return escaped.String()
}
