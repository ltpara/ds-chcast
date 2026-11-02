package api_test

import (
	"encoding/json"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

// signedMetadataBody builds a registration whose hostname/platform/version are
// signed with the host key. signedFor signs under another host id and
// sentHostname overrides the sent hostname; either models tampering because the
// signature stays over the original value.
func signedMetadataBody(t *testing.T, id, secret, fingerprint, hostname, platform, version string,
	timestamp int64, signedFor, sentHostname string) string {
	t.Helper()
	priv, pub := hostKey(secret)
	signID := id
	if signedFor != "" {
		signID = signedFor
	}
	body := map[string]any{
		"id":          id,
		"public_key":  pub,
		"fingerprint": fingerprint,
		"hostname":    hostname,
		"platform":    platform,
		"version":     version,
		"timestamp":   timestamp,
		"signature":   identity.Sign(priv, identity.RegisterMessage(id, fingerprint, timestamp)),
		"metadata_sig": identity.Sign(priv,
			identity.HostMetadataMessage(signID, hostname, platform, version, timestamp)),
	}
	if sentHostname != "" {
		body["hostname"] = sentHostname
	}
	data, err := json.Marshal(body)
	if err != nil {
		t.Fatal(err)
	}
	return string(data)
}

func TestRegisterPersistsSignedMetadata(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		signedMetadataBody(t, "devbox", "correct horse", "AB12",
			"STUDIO-PC", "windows/amd64", "0.36.0", now, "", ""), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("signed registration: %d %s", status, body)
	}
	record, ok := s.Registry.Identity("devbox")
	if !ok || !record.MetadataSigned || record.Hostname != "STUDIO-PC" ||
		record.Platform != "windows/amd64" || record.Version != "0.36.0" {
		t.Fatalf("stored identity = %+v (found %v)", record, ok)
	}

	status, list := listIdentities(t, s.URL, "", testutil.ServerKey)
	if status != http.StatusOK || len(list.Identities) != 1 || !list.Identities[0].MetadataSigned {
		t.Fatalf("identity list = %d %+v", status, list)
	}
}

func TestRegisterRejectsTamperedMetadata(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		signedMetadataBody(t, "devbox", "correct horse", "AB12",
			"STUDIO-PC", "windows/amd64", "0.36.0", now, "", "ATTACKER-PC"), testutil.ServerKey)
	if status != http.StatusUnauthorized || !strings.Contains(string(body), "metadata signature") {
		t.Fatalf("tampered registration = %d %s", status, body)
	}
	if _, ok := s.Registry.Identity("devbox"); ok {
		t.Fatal("tampered registration created an identity")
	}
}

func TestRegisterRejectsMetadataSignatureForAnotherID(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		signedMetadataBody(t, "devbox", "correct horse", "AB12",
			"STUDIO-PC", "windows/amd64", "0.36.0", now, "otherbox", ""), testutil.ServerKey)
	if status != http.StatusUnauthorized {
		t.Fatalf("foreign metadata signature = %d %s", status, body)
	}
}

func TestRegisterAllowsUnsignedMetadataByDefault(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBodyWithInfo(t, "oldbox", "legacy secret", "CD34",
			"LEGACY-PC", "windows/amd64", "0.20.0", now), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("unsigned registration = %d %s", status, body)
	}
	record, ok := s.Registry.Identity("oldbox")
	if !ok || record.MetadataSigned || record.Hostname != "LEGACY-PC" {
		t.Fatalf("stored identity = %+v (found %v)", record, ok)
	}
	// A signed renewal flips the status for the new report.
	status, body = request(t, http.MethodPost, s.URL+"/v1/hosts",
		signedMetadataBody(t, "oldbox", "legacy secret", "CD34",
			"LEGACY-PC", "windows/amd64", "0.36.0", now, "", ""), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("signed renewal = %d %s", status, body)
	}
	if record, _ := s.Registry.Identity("oldbox"); !record.MetadataSigned {
		t.Fatalf("renewal did not record the signature: %+v", record)
	}
}

func TestRegisterRequiresSignedMetadataWhenConfigured(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{RequireHostMetadataSig: true})
	now := time.Now().Unix()
	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBodyWithInfo(t, "oldbox", "legacy secret", "CD34",
			"LEGACY-PC", "windows/amd64", "0.20.0", now), testutil.ServerKey)
	if status != http.StatusUnauthorized || !strings.Contains(string(body), "required") {
		t.Fatalf("unsigned registration under enforcement = %d %s", status, body)
	}
	status, body = request(t, http.MethodPost, s.URL+"/v1/hosts",
		signedMetadataBody(t, "devbox", "correct horse", "AB12",
			"STUDIO-PC", "windows/amd64", "0.36.0", now, "", ""), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("signed registration under enforcement: %d %s", status, body)
	}
	record, ok := s.Registry.Identity("devbox")
	if !ok || !record.MetadataSigned {
		t.Fatalf("stored identity = %+v (found %v)", record, ok)
	}
}
