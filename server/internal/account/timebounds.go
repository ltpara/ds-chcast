package account

// TimeBounds narrows a report query to an inclusive window in unix
// milliseconds. A zero edge leaves that side open, so callers that do not ask
// for a range pass the zero value.
type TimeBounds struct {
	Since int64
	Until int64
}

// addTimeBounds appends the predicates for the non-zero edges of bounds.
func addTimeBounds(clauses []string, args []any, column string, bounds TimeBounds) ([]string, []any) {
	if bounds.Since > 0 {
		clauses = append(clauses, column+" >= ?")
		args = append(args, bounds.Since)
	}
	if bounds.Until > 0 {
		clauses = append(clauses, column+" <= ?")
		args = append(args, bounds.Until)
	}
	return clauses, args
}
