// Package sqlitex wraps the SQLite driver used by the registry and account
// stores with per-statement timing. Wrapping at the driver level means
// statements inside transactions are measured too, and no call site changes.
// Pool wait counters from database/sql provide the queue-depth signal.
package sqlitex

import (
	"context"
	"database/sql"
	"database/sql/driver"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	sqlite "modernc.org/sqlite"
)

const (
	// Registry and Account are the store labels exposed in metrics.
	Registry = "registry"
	Account  = "account"

	// RegistryDriver and AccountDriver are database/sql driver names for the
	// instrumented SQLite connections.
	RegistryDriver = "chast-sqlite-registry"
	AccountDriver  = "chast-sqlite-account"

	// SlowQuery is the threshold above which a statement counts as slow.
	SlowQuery = 50 * time.Millisecond
)

type counters struct {
	queries  atomic.Uint64
	errors   atomic.Uint64
	slow     atomic.Uint64
	nanos    atomic.Uint64
	maxNanos atomic.Uint64
	inFlight atomic.Int64

	mu  sync.Mutex
	ops map[string]*opCounters
}

type opCounters struct {
	queries  atomic.Uint64
	errors   atomic.Uint64
	slow     atomic.Uint64
	nanos    atomic.Uint64
	maxNanos atomic.Uint64
}

var (
	stores = map[string]*counters{
		Registry: {ops: make(map[string]*opCounters)},
		Account:  {ops: make(map[string]*opCounters)},
	}
	opCache sync.Map
)

func init() {
	sql.Register(RegistryDriver, &wrappedDriver{inner: &sqlite.Driver{}, store: Registry})
	sql.Register(AccountDriver, &wrappedDriver{inner: &sqlite.Driver{}, store: Account})
}

// OpStat is one verb+table bucket.
type OpStat struct {
	Op         string  `json:"op"`
	Queries    uint64  `json:"queries"`
	Errors     uint64  `json:"errors"`
	Slow       uint64  `json:"slow"`
	Seconds    float64 `json:"seconds"`
	MaxSeconds float64 `json:"max_seconds"`
}

// PoolStats mirrors the database/sql counters that show connection queueing.
type PoolStats struct {
	OpenConnections    int     `json:"open_connections"`
	InUse              int     `json:"in_use"`
	Idle               int     `json:"idle"`
	MaxOpenConnections int     `json:"max_open_connections"`
	WaitCount          int64   `json:"wait_count"`
	WaitSeconds        float64 `json:"wait_seconds"`
}

// Pool reads the current pool counters. A nil database yields a zero value.
func Pool(db *sql.DB) PoolStats {
	if db == nil {
		return PoolStats{}
	}
	stats := db.Stats()
	return PoolStats{
		OpenConnections:    stats.OpenConnections,
		InUse:              stats.InUse,
		Idle:               stats.Idle,
		MaxOpenConnections: stats.MaxOpenConnections,
		WaitCount:          stats.WaitCount,
		WaitSeconds:        stats.WaitDuration.Seconds(),
	}
}

// StoreStats summarises one store since process start.
type StoreStats struct {
	Store      string   `json:"store"`
	Queries    uint64   `json:"queries"`
	Errors     uint64   `json:"errors"`
	Slow       uint64   `json:"slow"`
	Seconds    float64  `json:"seconds"`
	MaxSeconds float64  `json:"max_seconds"`
	InFlight   int64    `json:"in_flight"`
	Ops        []OpStat `json:"ops,omitempty"`
}

// Snapshot returns query metrics for every instrumented store, sorted by name.
func Snapshot() []StoreStats {
	out := make([]StoreStats, 0, len(stores))
	for _, name := range []string{Account, Registry} {
		out = append(out, StoreStatsFor(name))
	}
	return out
}

// StoreStatsFor returns the counters of one store.
func StoreStatsFor(name string) StoreStats {
	c := stores[name]
	if c == nil {
		return StoreStats{}
	}
	out := StoreStats{
		Store:      name,
		Queries:    c.queries.Load(),
		Errors:     c.errors.Load(),
		Slow:       c.slow.Load(),
		Seconds:    float64(c.nanos.Load()) / float64(time.Second),
		MaxSeconds: float64(c.maxNanos.Load()) / float64(time.Second),
		InFlight:   c.inFlight.Load(),
	}
	c.mu.Lock()
	out.Ops = make([]OpStat, 0, len(c.ops))
	for op, oc := range c.ops {
		out.Ops = append(out.Ops, OpStat{
			Op:         op,
			Queries:    oc.queries.Load(),
			Errors:     oc.errors.Load(),
			Slow:       oc.slow.Load(),
			Seconds:    float64(oc.nanos.Load()) / float64(time.Second),
			MaxSeconds: float64(oc.maxNanos.Load()) / float64(time.Second),
		})
	}
	c.mu.Unlock()
	sort.Slice(out.Ops, func(i, j int) bool { return out.Ops[i].Op < out.Ops[j].Op })
	return out
}

func observe(store, query string, elapsed time.Duration, err error) {
	c := stores[store]
	if c == nil {
		return
	}
	op := operation(query)
	c.queries.Add(1)
	c.nanos.Add(uint64(elapsed))
	raiseMax(&c.maxNanos, uint64(elapsed))
	if err != nil {
		c.errors.Add(1)
	}
	if elapsed >= SlowQuery {
		c.slow.Add(1)
	}
	c.mu.Lock()
	oc := c.ops[op]
	if oc == nil {
		oc = &opCounters{}
		c.ops[op] = oc
	}
	c.mu.Unlock()
	oc.queries.Add(1)
	oc.nanos.Add(uint64(elapsed))
	raiseMax(&oc.maxNanos, uint64(elapsed))
	if err != nil {
		oc.errors.Add(1)
	}
	if elapsed >= SlowQuery {
		oc.slow.Add(1)
	}
}

func raiseMax(target *atomic.Uint64, value uint64) {
	for {
		current := target.Load()
		if value <= current || target.CompareAndSwap(current, value) {
			return
		}
	}
}

func track[T any](store, query string, fn func() (T, error)) (T, error) {
	c := stores[store]
	if c == nil {
		return fn()
	}
	c.inFlight.Add(1)
	start := time.Now()
	value, err := fn()
	elapsed := time.Since(start)
	c.inFlight.Add(-1)
	observe(store, query, elapsed, err)
	return value, err
}

func trackErr(store, query string, fn func() error) error {
	_, err := track(store, query, func() (struct{}, error) {
		return struct{}{}, fn()
	})
	return err
}

// operation reduces a statement to a low-cardinality verb+table label.
func operation(query string) string {
	if cached, ok := opCache.Load(query); ok {
		return cached.(string)
	}
	op := classify(query)
	opCache.Store(query, op)
	return op
}

func classify(query string) string {
	fields := strings.Fields(strings.ToUpper(query))
	if len(fields) == 0 {
		return "other"
	}
	verb := fields[0]
	switch verb {
	case "BEGIN", "COMMIT", "ROLLBACK", "SAVEPOINT", "RELEASE":
		return strings.ToLower(verb)
	case "PRAGMA":
		if len(fields) > 1 {
			if name := identifier(fields[1]); name != "" {
				return "pragma " + name
			}
		}
		return "pragma"
	case "CREATE", "ALTER", "DROP":
		for i := 1; i < len(fields)-1; i++ {
			switch fields[i] {
			case "TABLE", "INDEX", "VIEW", "TRIGGER":
				if name := nextIdentifier(fields[i+1:]); name != "" {
					return strings.ToLower(verb) + " " + name
				}
			}
		}
		return strings.ToLower(verb)
	case "UPDATE":
		offset := 1
		if len(fields) > 3 && fields[1] == "OR" {
			offset = 3
		}
		if len(fields) > offset {
			if name := identifier(fields[offset]); name != "" {
				return "update " + name
			}
		}
		return "update"
	default:
		for i := 1; i < len(fields)-1; i++ {
			switch fields[i] {
			case "FROM", "INTO", "JOIN":
				if table := identifier(fields[i+1]); table != "" {
					return strings.ToLower(verb) + " " + table
				}
			}
		}
		return strings.ToLower(verb)
	}
}

// nextIdentifier skips DDL modifiers such as "IF NOT EXISTS" so the label
// names the object being created.
func nextIdentifier(fields []string) string {
	for _, field := range fields {
		switch field {
		case "IF", "NOT", "EXISTS", "TEMP", "TEMPORARY", "UNIQUE":
			continue
		}
		if name := identifier(field); name != "" {
			return name
		}
		return ""
	}
	return ""
}

func identifier(token string) string {
	if i := strings.IndexAny(token, "(\"'`;"); i >= 0 {
		token = token[:i]
	}
	token = strings.Trim(token, "\"'`")
	for _, r := range token {
		if !(r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || r == '_') {
			return ""
		}
	}
	return strings.ToLower(token)
}

type wrappedDriver struct {
	inner driver.Driver
	store string
}

func (d *wrappedDriver) Open(name string) (driver.Conn, error) {
	c, err := d.inner.Open(name)
	if err != nil {
		return nil, err
	}
	return &conn{Conn: c, store: d.store}, nil
}

type conn struct {
	driver.Conn
	store string
}

func (c *conn) Prepare(query string) (driver.Stmt, error) {
	stmt, err := c.Conn.Prepare(query)
	if err != nil {
		return nil, err
	}
	return &statement{Stmt: stmt, store: c.store, query: query}, nil
}

func (c *conn) PrepareContext(ctx context.Context, query string) (driver.Stmt, error) {
	if preparer, ok := c.Conn.(driver.ConnPrepareContext); ok {
		stmt, err := preparer.PrepareContext(ctx, query)
		if err != nil {
			return nil, err
		}
		return &statement{Stmt: stmt, store: c.store, query: query}, nil
	}
	return c.Prepare(query)
}

func (c *conn) ExecContext(ctx context.Context, query string, args []driver.NamedValue) (driver.Result, error) {
	if execer, ok := c.Conn.(driver.ExecerContext); ok {
		return track(c.store, query, func() (driver.Result, error) {
			return execer.ExecContext(ctx, query, args)
		})
	}
	return nil, driver.ErrSkip
}

func (c *conn) QueryContext(ctx context.Context, query string, args []driver.NamedValue) (driver.Rows, error) {
	if queryer, ok := c.Conn.(driver.QueryerContext); ok {
		return track(c.store, query, func() (driver.Rows, error) {
			return queryer.QueryContext(ctx, query, args)
		})
	}
	return nil, driver.ErrSkip
}

func (c *conn) BeginTx(ctx context.Context, opts driver.TxOptions) (driver.Tx, error) {
	beginner, ok := c.Conn.(driver.ConnBeginTx)
	if !ok {
		return nil, driver.ErrSkip
	}
	tx, err := track(c.store, "BEGIN", func() (driver.Tx, error) {
		return beginner.BeginTx(ctx, opts)
	})
	if err != nil {
		return nil, err
	}
	return &instrumentedTx{Tx: tx, store: c.store}, nil
}

// instrumentedTx measures the implicit BEGIN/COMMIT/ROLLBACK statements the
// driver runs itself; a slow COMMIT is the clearest sign of WAL contention.
type instrumentedTx struct {
	driver.Tx
	store string
}

func (t *instrumentedTx) Commit() error {
	return trackErr(t.store, "COMMIT", t.Tx.Commit)
}

func (t *instrumentedTx) Rollback() error {
	return trackErr(t.store, "ROLLBACK", t.Tx.Rollback)
}

func (c *conn) Ping(ctx context.Context) error {
	if pinger, ok := c.Conn.(driver.Pinger); ok {
		return pinger.Ping(ctx)
	}
	return nil
}

func (c *conn) CheckNamedValue(value *driver.NamedValue) error {
	if checker, ok := c.Conn.(driver.NamedValueChecker); ok {
		return checker.CheckNamedValue(value)
	}
	return driver.ErrSkip
}

func (c *conn) ResetSession(ctx context.Context) error {
	if resetter, ok := c.Conn.(driver.SessionResetter); ok {
		return resetter.ResetSession(ctx)
	}
	return nil
}

func (c *conn) IsValid() bool {
	if validator, ok := c.Conn.(driver.Validator); ok {
		return validator.IsValid()
	}
	return true
}

type statement struct {
	driver.Stmt
	store string
	query string
}

func (s *statement) Exec(args []driver.Value) (driver.Result, error) {
	return track(s.store, s.query, func() (driver.Result, error) {
		return s.Stmt.Exec(args)
	})
}

func (s *statement) Query(args []driver.Value) (driver.Rows, error) {
	return track(s.store, s.query, func() (driver.Rows, error) {
		return s.Stmt.Query(args)
	})
}

func (s *statement) ExecContext(ctx context.Context, args []driver.NamedValue) (driver.Result, error) {
	if execer, ok := s.Stmt.(driver.StmtExecContext); ok {
		return track(s.store, s.query, func() (driver.Result, error) {
			return execer.ExecContext(ctx, args)
		})
	}
	return nil, driver.ErrSkip
}

func (s *statement) QueryContext(ctx context.Context, args []driver.NamedValue) (driver.Rows, error) {
	if queryer, ok := s.Stmt.(driver.StmtQueryContext); ok {
		return track(s.store, s.query, func() (driver.Rows, error) {
			return queryer.QueryContext(ctx, args)
		})
	}
	return nil, driver.ErrSkip
}
