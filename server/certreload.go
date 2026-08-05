package main

import (
	"crypto/tls"
	"fmt"
	"log"
	"os"
	"sync"
	"time"
)

// certReloader serves the TLS certificate to crypto/tls and transparently
// picks up on-disk changes, so an ACME/Let's Encrypt renewal takes effect
// without restarting the process.
type certReloader struct {
	certPath string
	keyPath  string

	mu        sync.RWMutex
	cert      *tls.Certificate
	certMod   time.Time
	keyMod    time.Time
	lastCheck time.Time
}

// checkInterval bounds how often we stat the key pair; handshakes can be
// frequent and there is no point hitting the filesystem for every one.
const certCheckInterval = 30 * time.Second

func newCertReloader(certPath, keyPath string) *certReloader {
	r := &certReloader{certPath: certPath, keyPath: keyPath}
	if err := r.reload(); err != nil {
		// Fatal here is appropriate: the operator explicitly asked for TLS
		// and we cannot serve a single request without a usable key pair.
		log.Fatalf("tls: %v", err)
	}
	return r
}

// GetCertificate satisfies tls.Config.GetCertificate.
func (r *certReloader) GetCertificate(*tls.ClientHelloInfo) (*tls.Certificate, error) {
	r.maybeReload()
	r.mu.RLock()
	defer r.mu.RUnlock()
	if r.cert == nil {
		return nil, fmt.Errorf("tls: no certificate loaded")
	}
	return r.cert, nil
}

func (r *certReloader) maybeReload() {
	r.mu.RLock()
	last := r.lastCheck
	certMod, keyMod := r.certMod, r.keyMod
	r.mu.RUnlock()

	if time.Since(last) < certCheckInterval {
		return
	}

	r.mu.Lock()
	r.lastCheck = time.Now()
	r.mu.Unlock()

	cs, err := os.Stat(r.certPath)
	if err != nil {
		log.Printf("tls: stat cert: %v (keeping previous certificate)", err)
		return
	}
	ks, err := os.Stat(r.keyPath)
	if err != nil {
		log.Printf("tls: stat key: %v (keeping previous certificate)", err)
		return
	}
	if cs.ModTime().Equal(certMod) && ks.ModTime().Equal(keyMod) {
		return
	}
	if err := r.reload(); err != nil {
		// Renewal may be mid-write (cert updated, key not yet). Keep serving
		// the old pair and retry on the next interval.
		log.Printf("tls: reload failed: %v (keeping previous certificate)", err)
		return
	}
	log.Printf("tls: reloaded certificate from %s", r.certPath)
}

func (r *certReloader) reload() error {
	cert, err := tls.LoadX509KeyPair(r.certPath, r.keyPath)
	if err != nil {
		return fmt.Errorf("load key pair (%s, %s): %w", r.certPath, r.keyPath, err)
	}
	var certMod, keyMod time.Time
	if st, serr := os.Stat(r.certPath); serr == nil {
		certMod = st.ModTime()
	}
	if st, serr := os.Stat(r.keyPath); serr == nil {
		keyMod = st.ModTime()
	}

	r.mu.Lock()
	defer r.mu.Unlock()
	r.cert = &cert
	r.certMod = certMod
	r.keyMod = keyMod
	r.lastCheck = time.Now()
	return nil
}
