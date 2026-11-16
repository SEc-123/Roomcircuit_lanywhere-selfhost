// SPDX-License-Identifier: AGPL-3.0-only
// Transparent TLS routing. TLS and device authentication remain at each backend.
package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

type route struct {
	Address string `json:"address"`
	ProxyV1 bool   `json:"proxy_v1"`
}
type configuration struct {
	Listen string           `json:"listen"`
	NoSNI  route            `json:"no_sni"`
	Routes map[string]route `json:"routes"`
}

func validName(name string) bool {
	if len(name) == 0 || len(name) > 253 || strings.ToLower(name) != name {
		return false
	}
	for _, label := range strings.Split(name, ".") {
		if len(label) == 0 || len(label) > 63 || label[0] == '-' || label[len(label)-1] == '-' {
			return false
		}
		for _, c := range label {
			if !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '-') {
				return false
			}
		}
	}
	return true
}

func validate(c configuration) error {
	if len(c.Routes) == 0 || len(c.Routes) > 32 {
		return errors.New("bounded exact SNI routes required")
	}
	listenHost, listenPortText, err := net.SplitHostPort(c.Listen)
	if err != nil {
		return err
	}
	listenIP := net.ParseIP(listenHost)
	listenPort, err := numericPort(listenPortText, true)
	if err != nil || (listenHost != "" && listenIP == nil) {
		return errors.New("listener requires a literal address and numeric port")
	}
	check := func(r route) error {
		h, p, err := net.SplitHostPort(r.Address)
		ip := net.ParseIP(h)
		if err != nil || !ip.IsLoopback() {
			return errors.New("backend must use a fixed loopback address")
		}
		port, err := numericPort(p, false)
		if err != nil {
			return err
		}
		if port == listenPort && (listenHost == "" || listenIP.IsUnspecified() || listenIP.Equal(ip)) {
			return errors.New("backend must not route into the ingress listener")
		}
		return nil
	}
	if err := check(c.NoSNI); err != nil {
		return err
	}
	for name, r := range c.Routes {
		if !validName(name) {
			return errors.New("invalid exact SNI name")
		}
		if err := check(r); err != nil {
			return err
		}
	}
	return nil
}

func numericPort(value string, allowZero bool) (int, error) {
	if value == "" || len(value) > 5 {
		return 0, errors.New("numeric TCP port required")
	}
	for _, b := range []byte(value) {
		if b < '0' || b > '9' {
			return 0, errors.New("numeric TCP port required")
		}
	}
	n, err := strconv.Atoi(value)
	if err != nil || n > 65535 || (!allowZero && n == 0) {
		return 0, errors.New("invalid TCP port")
	}
	return n, nil
}

// Read a bounded, possibly record-fragmented ClientHello and retain every wire byte.
func hello(r io.Reader) (string, []byte, error) {
	var wire, handshake bytes.Buffer
	for wire.Len() < 65536 {
		var head [5]byte
		if _, err := io.ReadFull(r, head[:]); err != nil {
			return "", nil, err
		}
		n := int(binary.BigEndian.Uint16(head[3:]))
		if head[0] != 22 || head[1] != 3 || n == 0 || n > 16384 || wire.Len()+5+n > 65536 {
			return "", nil, errors.New("invalid TLS record")
		}
		payload := make([]byte, n)
		if _, err := io.ReadFull(r, payload); err != nil {
			return "", nil, err
		}
		wire.Write(head[:])
		wire.Write(payload)
		handshake.Write(payload)
		b := handshake.Bytes()
		if len(b) < 4 {
			continue
		}
		length := int(b[1])<<16 | int(b[2])<<8 | int(b[3])
		if b[0] != 1 || length > 60000 || length < 38 {
			return "", nil, errors.New("invalid ClientHello")
		}
		if len(b) >= 4+length {
			name, err := serverName(b[4 : 4+length])
			return name, wire.Bytes(), err
		}
	}
	return "", nil, errors.New("ClientHello exceeds limit")
}

func serverName(b []byte) (string, error) {
	bad := errors.New("malformed ClientHello")
	if len(b) < 35 {
		return "", bad
	}
	i := 34
	n := int(b[i])
	i++
	if n > 32 || i+n+2 > len(b) {
		return "", bad
	}
	i += n
	n = int(binary.BigEndian.Uint16(b[i:]))
	i += 2
	if n == 0 || n%2 != 0 || i+n+1 > len(b) {
		return "", bad
	}
	i += n
	n = int(b[i])
	i++
	if n == 0 || i+n > len(b) {
		return "", bad
	}
	i += n
	if i == len(b) {
		return "", nil
	}
	if i+2 > len(b) {
		return "", bad
	}
	n = int(binary.BigEndian.Uint16(b[i:]))
	i += 2
	if i+n != len(b) {
		return "", bad
	}
	name := ""
	seen := false
	for i < len(b) {
		if i+4 > len(b) {
			return "", bad
		}
		kind := binary.BigEndian.Uint16(b[i:])
		size := int(binary.BigEndian.Uint16(b[i+2:]))
		i += 4
		if i+size > len(b) {
			return "", bad
		}
		if kind == 0 {
			if seen || size < 5 {
				return "", bad
			}
			seen = true
			x := b[i : i+size]
			if int(binary.BigEndian.Uint16(x)) != len(x)-2 {
				return "", bad
			}
			for p := 2; p < len(x); {
				if p+3 > len(x) {
					return "", bad
				}
				typ := x[p]
				l := int(binary.BigEndian.Uint16(x[p+1:]))
				p += 3
				if p+l > len(x) || l == 0 {
					return "", bad
				}
				if typ == 0 {
					if name != "" {
						return "", bad
					}
					for _, b := range x[p : p+l] {
						if b >= 128 {
							return "", bad
						}
					}
					name = strings.ToLower(string(x[p : p+l]))
					if !validName(name) {
						return "", bad
					}
				}
				p += l
			}
			if name == "" {
				return "", bad
			}
		}
		i += size
	}
	return name, nil
}

func copyFully(w io.Writer, b []byte) error {
	for len(b) > 0 {
		n, err := w.Write(b)
		if err != nil {
			return err
		}
		if n == 0 {
			return io.ErrShortWrite
		}
		b = b[n:]
	}
	return nil
}

func bridge(client net.Conn, c configuration) {
	defer client.Close()
	client.SetDeadline(time.Now().Add(5 * time.Second))
	name, prefix, err := hello(client)
	if err != nil {
		return
	}
	r, ok := c.Routes[name]
	if name == "" {
		r, ok = c.NoSNI, true
	}
	if !ok {
		return
	}
	upstream, err := net.DialTimeout("tcp", r.Address, 3*time.Second)
	if err != nil {
		return
	}
	defer upstream.Close()
	upstream.SetDeadline(time.Now().Add(5 * time.Second))
	if r.ProxyV1 {
		src, ok1 := client.RemoteAddr().(*net.TCPAddr)
		dst, ok2 := client.LocalAddr().(*net.TCPAddr)
		if !ok1 || !ok2 || src.IP.To4() == nil || dst.IP.To4() == nil {
			return
		}
		line := fmt.Sprintf("PROXY TCP4 %s %s %d %d\r\n", src.IP.String(), dst.IP.String(), src.Port, dst.Port)
		if err = copyFully(upstream, []byte(line)); err != nil {
			return
		}
	}
	if err = copyFully(upstream, prefix); err != nil {
		return
	}
	client.SetDeadline(time.Time{})
	upstream.SetDeadline(time.Time{})
	var wg sync.WaitGroup
	wg.Add(2)
	copyOne := func(to, from net.Conn) {
		defer wg.Done()
		if _, err := io.Copy(to, from); err != nil {
			to.Close()
			from.Close()
			return
		}
		if tcp, ok := to.(*net.TCPConn); ok {
			tcp.CloseWrite()
		} else {
			to.Close()
		}
		// Preserve the response after a half-close, but bound shutdown.
		to.SetDeadline(time.Now().Add(5 * time.Second))
		from.SetDeadline(time.Now().Add(5 * time.Second))
	}
	go copyOne(upstream, client)
	go copyOne(client, upstream)
	wg.Wait()
}

func serve(listener net.Listener, c configuration) error {
	active := make(chan struct{}, 128)
	for {
		conn, err := listener.Accept()
		if err != nil {
			return err
		}
		select {
		case active <- struct{}{}:
			go func() { defer func() { <-active }(); bridge(conn, c) }()
		default:
			conn.Close()
		}
	}
}

func main() {
	path := flag.String("config", "", "private routing configuration")
	flag.Parse()
	f, err := os.Open(*path)
	if err != nil {
		log.Fatal(err)
	}
	info, err := f.Stat()
	if err != nil || !info.Mode().IsRegular() || info.Size() > 16384 {
		log.Fatal("configuration must be a bounded regular file")
	}
	decoder := json.NewDecoder(io.LimitReader(f, 16385))
	decoder.DisallowUnknownFields()
	var c configuration
	if err = decoder.Decode(&c); err != nil {
		log.Fatal(err)
	}
	if decoder.Decode(new(any)) != io.EOF {
		log.Fatal("trailing configuration data")
	}
	f.Close()
	if err = validate(c); err != nil {
		log.Fatal(err)
	}
	listener, err := net.Listen("tcp", c.Listen)
	if err != nil {
		log.Fatal(err)
	}
	c.Listen = listener.Addr().String()
	if err = validate(c); err != nil {
		listener.Close()
		log.Fatal(err)
	}
	log.Fatal(serve(listener, c))
}
