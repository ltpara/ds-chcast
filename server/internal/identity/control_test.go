package identity

import "testing"

func TestRendezvousSignaturesBindRolesConnectionAndPolicy(t *testing.T) {
	server, err := GenerateServer()
	if err != nil {
		t.Fatal(err)
	}
	pub, _ := ParsePublicKey(server.PublicKeyText())
	sig := server.SignRendezvousChallenge("host", "client", "challenge")
	if Verify(pub, RendezvousControlMessage("host", "host", "client", "challenge"), sig) {
		t.Fatal("server challenge can be replayed as host proof")
	}
	sig = server.SignRendezvousTrigger("host", "connection", "punch", "198.51.100.1:4000", "asymmetric", false, 100)
	for _, msg := range [][]byte{
		RendezvousTriggerMessage("host", "other-connection", "punch", "198.51.100.1:4000", "asymmetric", false, 100),
		RendezvousTriggerMessage("host", "connection", "punch", "198.51.100.2:4000", "asymmetric", false, 100),
		RendezvousTriggerMessage("host", "connection", "punch", "198.51.100.1:4000", "symmetric", false, 100),
		RendezvousTriggerMessage("host", "connection", "punch", "198.51.100.1:4000", "asymmetric", true, 100),
	} {
		if Verify(pub, msg, sig) {
			t.Fatal("altered connection/address/NAT/local policy verified")
		}
	}
}
