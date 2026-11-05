// Package dbmetric wraps a database/sql driver to record statement latency and
// exports the numbers in Prometheus text format together with the pool's wait
// statistics. The wait counters are the queue-depth signal for SQLite: while
// the store is limited to one connection, every concurrent writer waits in
// database/sql, and that queue is otherwise invisible.
package dbmetric

import (
	"context"
	"database/sql"
	"database/sql/driver"
	"fmt"
	"io"
	"sort"
	"strings"
	"sync"
	"time"

	"modernc.org/sqlite"
)

// slowQueryThreshold marks a statement slow enough to investigate. The counter
// keeps its own total so a dashboard can alert without a full histogram.
const slowQueryThreshold = 100 * time.Millisecond

type opStats struct {
	count      uint64
	errors     uint64
	slow       uint64
	durationNs uint64
	maxNs      uint64
}

type tracked struct {
	name string
	db   *sql.DB
}

// Registry collects statement and pool metrics for one process.
type Registry struct {
	mu   sync.Mutex
	ops  map[string]*opStats
	dbs  []tracked
	slow time.Duration
}

// Default receives every statement executed through OpenSQLite.
var Default = &Registry{ops: make(map[string]*opStats), slow: slowQueryThreshold}

func (r *Registry) observe(op string, elapsed time.Duration, err error) {
	ns := uint64(elapsed)
	if elapsed < 0 {
		ns = 0
	}
	r.mu.Lock()
	stats := r.ops[op]
	if stats == nil {
		stats = &opStats{}
		r.ops[op] = stats
	}
	stats.count++
	stats.durationNs += ns
	if ns > stats.maxNs {
		stats.maxNs = ns
	}
	if err != nil {
		stats.errors++
	}
	if elapsed >= r.slow {
		stats.slow++
	}
	r.mu.Unlock()
}

// Track registers a pool so WritePrometheus reports its wait statistics.
func (r *Registry) Track(name string, db *sql.DB) {
	if r == nil || db == nil {
		return
	}
	r.mu.Lock()
	r.dbs = append(r.dbs, tracked{name: name, db: db})
	r.mu.Unlock()
}

// OpenSQLite opens path through a timing wrapper around the modernc driver.
// The returned pool has the driver's normal semantics; only observation is
// added.
func OpenSQLite(name, path string) (*sql.DB, error) {
	db := sql.OpenDB(&connector{
		base:     &sqlite.Driver{},
		dsn:      path,
		registry: Default,
	})
	Default.Track(name, db)
	return db, nil
}

type connector struct {
	base     driver.Driver
	dsn      string
	registry *Registry
}

func (c *connector) Connect(context.Context) (driver.Conn, error) {
	base, err := c.base.Open(c.dsn)
	if err != nil {
		return nil, err
	}
	return &conn{base: base, registry: c.registry}, nil
}

func (c *connector) Driver() driver.Driver { return c.base }

type conn struct {
	base     driver.Conn
	registry *Registry
}

func (c *conn) Prepare(query string) (driver.Stmt, error) {
	base, err := c.base.Prepare(query)
	if err != nil {
		c.registry.observe(operation(query), 0, err)
		return nil, err
	}
	return &stmt{base: base, registry: c.registry, op: operation(query)}, nil
}

func (c *conn) Close() error { return c.base.Close() }

func (c *conn) Begin() (driver.Tx, error) {
	start := time.Now()
	base, err := c.base.Begin()
	c.registry.observe("BEGIN", time.Since(start), err)
	if err != nil {
		return nil, err
	}
	return &tx{base: base, registry: c.registry}, nil
}

func (c *conn) ResetSession(ctx context.Context) error {
	if resetter, ok := c.base.(driver.SessionResetter); ok {
		return resetter.ResetSession(ctx)
	}
	return nil
}

func (c *conn) IsValid() bool {
	if valid, ok := c.base.(driver.Validator); ok {
		return valid.IsValid()
	}
	return true
}

type tx struct {
	base     driver.Tx
	registry *Registry
}

func (t *tx) Commit() error {
	start := time.Now()
	err := t.base.Commit()
	t.registry.observe("COMMIT", time.Since(start), err)
	return err
}

func (t *tx) Rollback() error {
	start := time.Now()
	err := t.base.Rollback()
	t.registry.observe("ROLLBACK", time.Since(start), err)
	return err
}

type stmt struct {
	base     driver.Stmt
	registry *Registry
	op       string
}

func (s *stmt) Close() error  { return s.base.Close() }
func (s *stmt) NumInput() int { return s.base.NumInput() }

func (s *stmt) Exec(args []driver.Value) (driver.Result, error) {
	start := time.Now()
	result, err := s.base.Exec(args)
	s.registry.observe(s.op, time.Since(start), err)
	return result, err
}

func (s *stmt) Query(args []driver.Value) (driver.Rows, error) {
	start := time.Now()
	rows, err := s.base.Query(args)
	s.registry.observe(s.op, time.Since(start), err)
	return rows, err
}

// operation reduces a statement to a bounded label. Dynamic SQL in this
// codebase starts with a small set of keywords; anything else is OTHER.
func operation(query string) string {
	query = strings.TrimSpace(query)
	if query == "" {
		return "OTHER"
	}
	end := strings.IndexAny(query, " \t\r\n(")
	word := query
	if end >= 0 {
		word = query[:end]
	}
	switch strings.ToUpper(word) {
	case "SELECT", "INSERT", "UPDATE", "DELETE", "WITH", "CREATE", "ALTER",
		"DROP", "PRAGMA", "ATTACH", "DETACH", "REPLACE", "BEGIN", "COMMIT",
		"ROLLBACK", "VACUUM", "ANALYZE":
		return strings.ToUpper(word)
	}
	return "OTHER"
}

// WritePrometheus appends the statement and pool metrics.
func (r *Registry) WritePrometheus(w io.Writer) {
	if r == nil {
		return
	}
	r.mu.Lock()
	ops := make([]string, 0, len(r.ops))
	snapshot := make(map[string]opStats, len(r.ops))
	for op, stats := range r.ops {
		ops = append(ops, op)
		snapshot[op] = *stats
	}
	dbs := append([]tracked(nil), r.dbs...)
	r.mu.Unlock()
	sort.Strings(ops)

	fmt.Fprintf(w, "# HELP chast_db_queries_total SQL statements executed.\n# TYPE chast_db_queries_total counter\n")
	for _, op := range ops {
		fmt.Fprintf(w, "chast_db_queries_total{op=%q} %d\n", op, snapshot[op].count)
	}
	fmt.Fprintf(w, "# HELP chast_db_query_errors_total SQL statements that returned an error.\n# TYPE chast_db_query_errors_total counter\n")
	for _, op := range ops {
		fmt.Fprintf(w, "chast_db_query_errors_total{op=%q} %d\n", op, snapshot[op].errors)
	}
	fmt.Fprintf(w, "# HELP chast_db_slow_queries_total SQL statements at or above the slow threshold.\n# TYPE chast_db_slow_queries_total counter\n")
	for _, op := range ops {
		fmt.Fprintf(w, "chast_db_slow_queries_total{op=%q} %d\n", op, snapshot[op].slow)
	}
	fmt.Fprintf(w, "# HELP chast_db_query_duration_seconds_total Time spent executing SQL statements.\n# TYPE chast_db_query_duration_seconds_total counter\n")
	for _, op := range ops {
		fmt.Fprintf(w, "chast_db_query_duration_seconds_total{op=%q} %.9f\n",
			op, float64(snapshot[op].durationNs)/float64(time.Second))
	}
	fmt.Fprintf(w, "# HELP chast_db_query_duration_seconds_max Longest single statement.\n# TYPE chast_db_query_duration_seconds_max gauge\n")
	for _, op := range ops {
		fmt.Fprintf(w, "chast_db_query_duration_seconds_max{op=%q} %.9f\n",
			op, float64(snapshot[op].maxNs)/float64(time.Second))
	}

	if len(dbs) == 0 {
		return
	}
	fmt.Fprintf(w, "# HELP chast_db_pool_open_connections Open database connections.\n# TYPE chast_db_pool_open_connections gauge\n")
	for _, entry := range dbs {
		stats := entry.db.Stats()
		fmt.Fprintf(w, "chast_db_pool_open_connections{db=%q} %d\n", entry.name, stats.OpenConnections)
	}
	fmt.Fprintf(w, "# HELP chast_db_pool_in_use Connections currently executing a statement.\n# TYPE chast_db_pool_in_use gauge\n")
	for _, entry := range dbs {
		stats := entry.db.Stats()
		fmt.Fprintf(w, "chast_db_pool_in_use{db=%q} %d\n", entry.name, stats.InUse)
	}
	fmt.Fprintf(w, "# HELP chast_db_pool_idle Connections waiting in the pool.\n# TYPE chast_db_pool_idle gauge\n")
	for _, entry := range dbs {
		stats := entry.db.Stats()
		fmt.Fprintf(w, "chast_db_pool_idle{db=%q} %d\n", entry.name, stats.Idle)
	}
	fmt.Fprintf(w, "# HELP chast_db_pool_max_open Configured connection ceiling.\n# TYPE chast_db_pool_max_open gauge\n")
	for _, entry := range dbs {
		stats := entry.db.Stats()
		fmt.Fprintf(w, "chast_db_pool_max_open{db=%q} %d\n", entry.name, stats.MaxOpenConnections)
	}
	fmt.Fprintf(w, "# HELP chast_db_pool_wait_total Statements that waited for a free connection.\n# TYPE chast_db_pool_wait_total counter\n")
	for _, entry := range dbs {
		stats := entry.db.Stats()
		fmt.Fprintf(w, "chast_db_pool_wait_total{db=%q} %d\n", entry.name, stats.WaitCount)
	}
	fmt.Fprintf(w, "# HELP chast_db_pool_wait_seconds_total Time statements waited for a free connection.\n# TYPE chast_db_pool_wait_seconds_total counter\n")
	for _, entry := range dbs {
		stats := entry.db.Stats()
		fmt.Fprintf(w, "chast_db_pool_wait_seconds_total{db=%q} %.9f\n",
			entry.name, stats.WaitDuration.Seconds())
	}
}
