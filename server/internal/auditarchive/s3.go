package auditarchive

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"sort"
	"strings"
	"time"
)

// S3Options selects native AWS Signature Version 4 delivery to an
// S3-compatible endpoint. Addressing is "path" (the default,
// <endpoint>/<bucket>/<file>, accepted by MinIO and most gateways without
// DNS changes) or "virtual" (<bucket>.<endpoint-host>/<file>), which AWS
// requires for buckets created after September 2020. Endpoint may carry a
// path prefix for a reverse proxy.
type S3Options struct {
	Endpoint     string
	Region       string
	Bucket       string
	AccessKey    string
	SecretKey    string
	SessionToken string
	Addressing   string
}

// s3Signer holds the credentials used to sign every upload. The secret never
// appears in a URL or a delivered marker.
type s3Signer struct {
	endpoint     *url.URL
	region       string
	bucket       string
	accessKey    string
	secretKey    string
	sessionToken string
	virtual      bool
}

func newS3Signer(opts S3Options) (*s3Signer, error) {
	raw := strings.TrimSpace(opts.Endpoint)
	if raw == "" {
		return nil, fmt.Errorf("audit archive S3 endpoint is empty")
	}
	endpoint, err := url.Parse(raw)
	if err != nil {
		return nil, fmt.Errorf("audit archive S3 endpoint: %w", err)
	}
	if endpoint.Scheme != "http" && endpoint.Scheme != "https" {
		return nil, fmt.Errorf("audit archive S3 endpoint must be http or https")
	}
	if endpoint.Host == "" {
		return nil, fmt.Errorf("audit archive S3 endpoint has no host")
	}
	if endpoint.User != nil || endpoint.RawQuery != "" || endpoint.Fragment != "" {
		return nil, fmt.Errorf("audit archive S3 endpoint must not carry credentials, a query or a fragment")
	}
	region := strings.TrimSpace(opts.Region)
	if region == "" {
		return nil, fmt.Errorf("audit archive S3 region is empty")
	}
	bucket := strings.TrimSpace(opts.Bucket)
	if bucket == "" || strings.ContainsAny(bucket, "/\\ \t\r\n") {
		return nil, fmt.Errorf("audit archive S3 bucket %q is not a bucket name", bucket)
	}
	accessKey := strings.TrimSpace(opts.AccessKey)
	secretKey := strings.TrimSpace(opts.SecretKey)
	if accessKey == "" || secretKey == "" {
		return nil, fmt.Errorf("audit archive S3 access key and secret key are required")
	}
	var virtual bool
	switch strings.ToLower(strings.TrimSpace(opts.Addressing)) {
	case "", "path":
	case "virtual":
		virtual = true
		// A virtual host is formed by prefixing the bucket to the endpoint
		// host, so an IP endpoint would produce a name that never resolves.
		host := endpoint.Host
		if h, _, err := net.SplitHostPort(host); err == nil {
			host = h
		}
		if net.ParseIP(host) != nil {
			return nil, fmt.Errorf("audit archive S3 virtual-hosted addressing needs a DNS endpoint, not %q", endpoint.Host)
		}
	default:
		return nil, fmt.Errorf("audit archive S3 addressing must be path or virtual")
	}
	return &s3Signer{
		endpoint:     endpoint,
		region:       region,
		bucket:       bucket,
		accessKey:    accessKey,
		secretKey:    secretKey,
		sessionToken: strings.TrimSpace(opts.SessionToken),
		virtual:      virtual,
	}, nil
}

// bucketURL is the base shown to operators and stored in logs. It never
// carries the secret key.
func (s *s3Signer) bucketURL() string {
	if s.virtual {
		return s.endpoint.Scheme + "://" + s.bucket + "." + s.endpoint.Host +
			strings.TrimRight(s.endpoint.EscapedPath(), "/")
	}
	return s.endpoint.Scheme + "://" + s.endpoint.Host +
		strings.TrimRight(s.endpoint.EscapedPath(), "/") + "/" + awsURIEncode(s.bucket)
}

// target builds the object URL. Every segment is AWS-encoded, so the request
// path is already the canonical URI used for signing.
func (s *s3Signer) target(name string) string {
	return s.bucketURL() + "/" + awsURIEncode(name)
}

// sign adds the SigV4 headers to a request. signingTime is passed explicitly
// so the archiver's clock (and tests) own the timestamp; the payload hash is
// the file's SHA-256, already computed for the delivered marker, so the body
// is never read twice.
func (s *s3Signer) sign(request *http.Request, payloadHash string, signingTime time.Time) {
	amzDate := signingTime.UTC().Format("20060102T150405Z")
	day := signingTime.UTC().Format("20060102")
	request.Header.Set("X-Amz-Date", amzDate)
	request.Header.Set("X-Amz-Content-Sha256", payloadHash)
	if s.sessionToken != "" {
		request.Header.Set("X-Amz-Security-Token", s.sessionToken)
	}

	values := map[string]string{"host": request.URL.Host}
	names := []string{"host"}
	for name, header := range request.Header {
		lower := strings.ToLower(name)
		// Every x-amz-* header must be signed; Date is optional but common,
		// and signing it matches the published AWS examples.
		if strings.HasPrefix(lower, "x-amz-") || lower == "date" {
			names = append(names, lower)
			values[lower] = canonicalHeaderValue(header)
		}
	}
	sort.Strings(names)
	var headers strings.Builder
	for _, name := range names {
		headers.WriteString(name)
		headers.WriteByte(':')
		headers.WriteString(values[name])
		headers.WriteByte('\n')
	}
	signedHeaders := strings.Join(names, ";")

	canonicalURI := request.URL.EscapedPath()
	if canonicalURI == "" {
		canonicalURI = "/"
	}
	canonicalRequest := request.Method + "\n" + canonicalURI + "\n\n" + headers.String() +
		"\n" + signedHeaders + "\n" + payloadHash
	scope := day + "/" + s.region + "/s3/aws4_request"
	stringToSign := "AWS4-HMAC-SHA256\n" + amzDate + "\n" + scope + "\n" +
		hashHex([]byte(canonicalRequest))
	key := hmacSHA256([]byte("AWS4"+s.secretKey), day)
	key = hmacSHA256(key, s.region)
	key = hmacSHA256(key, "s3")
	key = hmacSHA256(key, "aws4_request")
	signature := hex.EncodeToString(hmacSHA256(key, stringToSign))
	request.Header.Set("Authorization", "AWS4-HMAC-SHA256 Credential="+s.accessKey+"/"+scope+
		", SignedHeaders="+signedHeaders+", Signature="+signature)
}

// canonicalHeaderValue trims and collapses whitespace the way SigV4 requires:
// values with different spacing must produce the same canonical header.
func canonicalHeaderValue(values []string) string {
	trimmed := make([]string, len(values))
	for i, value := range values {
		trimmed[i] = strings.Join(strings.Fields(value), " ")
	}
	return strings.Join(trimmed, ",")
}

// awsURIEncode escapes one path segment with the SigV4 alphabet: unreserved
// characters stay plain and everything else is percent-encoded in upper case.
func awsURIEncode(value string) string {
	const upperHex = "0123456789ABCDEF"
	var out strings.Builder
	for i := 0; i < len(value); i++ {
		c := value[i]
		if (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			c == '-' || c == '_' || c == '.' || c == '~' {
			out.WriteByte(c)
			continue
		}
		out.WriteByte('%')
		out.WriteByte(upperHex[c>>4])
		out.WriteByte(upperHex[c&0x0f])
	}
	return out.String()
}

func hashHex(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

func hmacSHA256(key []byte, data string) []byte {
	mac := hmac.New(sha256.New, key)
	mac.Write([]byte(data))
	return mac.Sum(nil)
}
