package main

import (
	"strings"
	"testing"
)

func TestHostStdinSecrets(t *testing.T) {
	for _, role := range []string{"host key", "server key", ""} {
		t.Run(role, func(t *testing.T) {
			registration, device, proxy := "old-role", "old-device", "old-proxy"
			input := " device secret \nsocks5://user:password@127.0.0.1:1080\r\n"
			if role != "" {
				input = " role secret \r\n" + input
			}
			err := readStdinSecrets(strings.NewReader(input),
				stdinSecret{role, &registration, role != "", 256},
				stdinSecret{"device key", &device, true, 128},
				stdinSecret{"proxy", &proxy, true, 512})
			if err != nil {
				t.Fatal(err)
			}
			if role != "" && registration != " role secret " {
				t.Fatal("registration was not preserved")
			}
			if role == "" && registration != "old-role" {
				t.Fatal("disabled field changed")
			}
			if device != " device secret " || proxy != "socks5://user:password@127.0.0.1:1080" {
				t.Fatal("buffered input lost a subsequent field")
			}
		})
	}
}

func TestStdinSecretRejectsInvalidWithoutEcho(t *testing.T) {
	for _, input := range []string{"", "\n", "fake-secret\rINJECT\n", "fake-secret\x00\n", strings.Repeat("fake-secret", 200) + "\n"} {
		value := "previous"
		err := readStdinSecrets(strings.NewReader(input), stdinSecret{"device key", &value, true, 128})
		if err == nil || strings.Contains(err.Error(), "fake-secret") || value != "previous" {
			t.Fatal("invalid input accepted, echoed, or persisted")
		}
	}
	var first, second string
	if readStdinSecrets(strings.NewReader("first\n"), stdinSecret{"first", &first, true, 128}, stdinSecret{"second", &second, true, 128}) == nil {
		t.Fatal("missing second line accepted")
	}
}

func TestHostStdinRoleFlagsExclusive(t *testing.T) {
	if err := runHost([]string{"--host-key-stdin", "--server-key-stdin"}); err == nil {
		t.Fatal("conflicting role credentials accepted")
	}
}
