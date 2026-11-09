package main

import (
	"bufio"
	"fmt"
	"io"
	"strings"
)

type stdinSecret struct {
	name    string
	value   *string
	enabled bool
	limit   int
}

// One reader must own the entire sequence: separate buffered readers would
// discard the next field when the parent writes all credentials at once.
func readStdinSecrets(input io.Reader, fields ...stdinSecret) error {
	reader := bufio.NewReaderSize(input, 1024)
	for _, field := range fields {
		if !field.enabled {
			continue
		}
		line, err := reader.ReadSlice('\n')
		if err != nil && err != io.EOF {
			return fmt.Errorf("read %s from stdin: unavailable or too long", field.name)
		}
		value := strings.TrimSuffix(strings.TrimSuffix(string(line), "\n"), "\r")
		if len(value) == 0 || len(value) > field.limit || strings.ContainsAny(value, "\r\n\x00") {
			return fmt.Errorf("read %s from stdin: invalid length or characters", field.name)
		}
		*field.value = value
	}
	return nil
}
