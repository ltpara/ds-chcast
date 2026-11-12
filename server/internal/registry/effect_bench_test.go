package registry

import (
	"strconv"
	"testing"
	"time"
)

// BenchmarkIssueChallengeScale exposes per-issue cost as the replay map grows
// with the online fleet. Every verified proof is remembered until its
// challenge expires, so N dual-stack agents keeping alive every 10 s against
// the 30 s challenge TTL hold about 6N live entries. A cleanup pass on every
// issue scans that whole map; a gated sweep makes the amortized cost constant.
func BenchmarkIssueChallengeScale(b *testing.B) {
	const proofsPerHost = 6
	for _, hosts := range []int{0, 1000, 10000, 30000} {
		b.Run("hosts="+strconv.Itoa(hosts), func(b *testing.B) {
			reg := New(time.Minute)
			now := time.Now()
			live := now.Add(challengeTTL)
			reg.usedChallenges = make(map[string]time.Time, hosts*proofsPerHost)
			for i := 0; i < hosts; i++ {
				id := "bench-" + strconv.Itoa(i)
				reg.hosts[id] = &Host{ID: id, ExpiresAt: now.Add(time.Minute)}
				for j := 0; j < proofsPerHost; j++ {
					reg.usedChallenges[id+"."+strconv.Itoa(j)] = live
				}
			}
			// The measured host exists even in the empty-fleet baseline; the
			// benchmark isolates the sweep, not registration.
			reg.hosts["bench-0"] = &Host{ID: "bench-0", ExpiresAt: now.Add(time.Minute)}
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				if _, err := reg.IssueChallenge("bench-0", "keepalive"); err != nil {
					b.Fatal(err)
				}
			}
		})
	}
}
