package dbmetric

import (
	"database/sql"
	"errors"
	"regexp"
	"strconv"
	"strings"

	"github.com/jackc/pgx/v5/stdlib"
)

// OpenPostgres opens dsn through the pgx stdlib driver and the same timing
// wrapper used for SQLite. Store queries stay written with database/sql's
// positional '?' placeholders; Prepare rebinds them to PostgreSQL's $n form
// and translates the few SQLite spellings the stores rely on.
func OpenPostgres(name, dsn string) (*sql.DB, error) {
	if strings.TrimSpace(dsn) == "" {
		return nil, errors.New("postgres dsn is empty")
	}
	db := sql.OpenDB(&connector{
		base:     stdlib.GetDefaultDriver(),
		dsn:      dsn,
		registry: Default,
		rewrite:  RewritePostgres,
	})
	Default.Track(name, db)
	return db, nil
}

var (
	collateNoCase = regexp.MustCompile(`(?i)\s+COLLATE\s+NOCASE\b`)
	// Boolean columns are INTEGER 0/1 in SQLite and BOOLEAN in PostgreSQL;
	// statements compare them with numeric literals that need a spelling
	// change. Timestamp columns such as used_at are not in this set.
	boolFalse = regexp.MustCompile(`(?i)\b(banned|deleted|success|totp_enabled)\s*=\s*0\b`)
	boolTrue  = regexp.MustCompile(`(?i)\b(banned|deleted|success|totp_enabled)\s*=\s*1\b`)
)

// RewritePostgres converts a query written for the SQLite stores into the
// PostgreSQL equivalent. The account schema declares case-insensitive
// columns with a non-deterministic ICU collation, so dropping COLLATE NOCASE
// keeps the SQLite comparison and ordering semantics.
func RewritePostgres(query string) string {
	query = collateNoCase.ReplaceAllString(query, "")
	query = boolFalse.ReplaceAllString(query, "${1} = false")
	query = boolTrue.ReplaceAllString(query, "${1} = true")
	return rebindDollar(query)
}

// rebindDollar numbers '?' placeholders outside string literals and comments.
func rebindDollar(query string) string {
	var b strings.Builder
	b.Grow(len(query) + 8)
	n := 0
	for i := 0; i < len(query); i++ {
		switch c := query[i]; c {
		case '\'', '"':
			end := skipQuoted(query, i, c)
			b.WriteString(query[i:end])
			i = end - 1
		case '-':
			if i+1 < len(query) && query[i+1] == '-' {
				end := strings.IndexByte(query[i:], '\n')
				if end < 0 {
					b.WriteString(query[i:])
					return b.String()
				}
				b.WriteString(query[i : i+end])
				i += end - 1
				continue
			}
			b.WriteByte(c)
		case '/':
			if i+1 < len(query) && query[i+1] == '*' {
				end := strings.Index(query[i+2:], "*/")
				if end < 0 {
					b.WriteString(query[i:])
					return b.String()
				}
				end += i + 4
				b.WriteString(query[i:end])
				i = end - 1
				continue
			}
			b.WriteByte(c)
		case '?':
			n++
			b.WriteByte('$')
			b.WriteString(strconv.Itoa(n))
		default:
			b.WriteByte(c)
		}
	}
	return b.String()
}

// skipQuoted returns the index just past a quoted section starting at start.
func skipQuoted(query string, start int, quote byte) int {
	for i := start + 1; i < len(query); i++ {
		if query[i] != quote {
			continue
		}
		if i+1 < len(query) && query[i+1] == quote {
			i++
			continue
		}
		return i + 1
	}
	return len(query)
}
