package dbmetric

import "testing"

func TestRewritePostgres(t *testing.T) {
	cases := []struct{ in, want string }{
		{
			`SELECT * FROM users WHERE username = ? COLLATE NOCASE`,
			`SELECT * FROM users WHERE username = $1`,
		},
		{
			`SELECT * FROM audit_events WHERE username = ? COLLATE NOCASE AND success = 1`,
			`SELECT * FROM audit_events WHERE username = $1 AND success = true`,
		},
		{
			`UPDATE users SET banned = 0 WHERE id = ?`,
			`UPDATE users SET banned = false WHERE id = $1`,
		},
		{
			`UPDATE address_book SET deleted = 1 WHERE user_id = ? AND peer_id = ?`,
			`UPDATE address_book SET deleted = true WHERE user_id = $1 AND peer_id = $2`,
		},
		{
			`SELECT * FROM address_book WHERE user_id = ? AND deleted = 0 ORDER BY position, name, group_id`,
			`SELECT * FROM address_book WHERE user_id = $1 AND deleted = false ORDER BY position, name, group_id`,
		},
		{
			`SELECT * FROM address_book_groups WHERE g.user_id = ? AND g.deleted = 0 ORDER BY u.username COLLATE NOCASE`,
			`SELECT * FROM address_book_groups WHERE g.user_id = $1 AND g.deleted = false ORDER BY u.username`,
		},
		{
			`SELECT '?' AS literal, "a?b" AS quoted, id FROM peers WHERE id = ?`,
			`SELECT '?' AS literal, "a?b" AS quoted, id FROM peers WHERE id = $1`,
		},
		{
			`SELECT 'it''s ?' AS literal WHERE id = ?`,
			`SELECT 'it''s ?' AS literal WHERE id = $1`,
		},
		{
			"SELECT 1 -- ? in a comment\nWHERE id = ?",
			"SELECT 1 -- ? in a comment\nWHERE id = $1",
		},
		{
			`SELECT /* ? in a block comment */ 1 WHERE used_at = 0 AND failures = 0`,
			`SELECT /* ? in a block comment */ 1 WHERE used_at = 0 AND failures = 0`,
		},
		{
			`INSERT INTO login_locks (login_key) VALUES (?) ON CONFLICT(login_key) DO UPDATE SET failures = excluded.failures`,
			`INSERT INTO login_locks (login_key) VALUES ($1) ON CONFLICT(login_key) DO UPDATE SET failures = excluded.failures`,
		},
	}
	for _, tc := range cases {
		if got := RewritePostgres(tc.in); got != tc.want {
			t.Errorf("RewritePostgres(%q) = %q, want %q", tc.in, got, tc.want)
		}
	}
}
